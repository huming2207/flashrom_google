/*
 * This file is part of the flashrom project.
 *
 * Copyright (C) 2011 Sven Schnelle <svens@stackframe.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <unistd.h>
#include <linux/spi/spidev.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "file.h"
#include "flash.h"
#include "chipdrivers.h"
#include "programmer.h"
#include "spi.h"



/* TODO: this information should come from the SPI master driver. */
#define SPI_DMA_SIZE 64

#ifndef SPIDEV_MAJOR
#define SPIDEV_MAJOR 153  /* refer to kernel/files/drivers/spi/spidev.c */
#endif

#define MODALIAS_FILE		"modalias"
#define LINUX_SPI_SYSFS_ROOT	"/sys/bus/spi/devices"

static int fd = -1;

static int linux_spi_shutdown(struct flashctx *flash, void *data);
static int linux_spi_send_command(const struct flashctx *flash, unsigned int writecnt, unsigned int readcnt,
			const unsigned char *txbuf, unsigned char *rxbuf);
static int linux_spi_read(struct flashctx *flash, uint8_t *buf,
			  unsigned int start, unsigned int len);
static int linux_spi_write_256(struct flashctx *flash, uint8_t *buf,
			       unsigned int start, unsigned int len);

static const struct spi_programmer spi_programmer_linux = {
	.type		= SPI_CONTROLLER_LINUX,
	.max_data_read	= MAX_DATA_UNSPECIFIED, /* TODO? */
	.max_data_write	= MAX_DATA_UNSPECIFIED, /* TODO? */
	.command	= linux_spi_send_command,
	.multicommand	= default_spi_send_multicommand,
	.read		= linux_spi_read,
	.write_256	= linux_spi_write_256,
};

static char devfs_path[32]; 	/* at least big enough to fit /dev/spidevX.Y */
static char *check_sysfs(void)
{
	int i;
	const char *sysfs_path = NULL;
	char *p;
	char *modalias[] = {
		"spi:spidev",	/* raw access over SPI bus (newer kernels) */
		"spidev",	/* raw access over SPI bus (older kernels) */
		"m25p80",	/* generic MTD device */
	};

	for (i = 0; i < ARRAY_SIZE(modalias); i++) {
		int major, minor;

		/* Path should look like: /sys/blah/spiX.Y/modalias */
		sysfs_path = scanft(LINUX_SPI_SYSFS_ROOT,
				MODALIAS_FILE, modalias[i], 1);
		if (!sysfs_path)
			continue;

		p = (char *)sysfs_path + strlen(LINUX_SPI_SYSFS_ROOT);
		if (p[0] == '/')
			p++;

		if (sscanf(p, "spi%u.%u", &major, &minor) == 2) {
			msg_pdbg("Found SPI device %s on spi%u.%u\n",
				modalias[i], major, minor);
			sprintf(devfs_path, "/dev/spidev%u.%u", major, minor);
			free((void *)sysfs_path);
			break;
		}
		free((void *)sysfs_path);
	}

	if (i == ARRAY_SIZE(modalias))
		return NULL;
	return devfs_path;
}

static char *check_fdt(void)
{
	unsigned int bus, cs;

	if (fdt_find_spi_nor_flash(&bus, &cs) < 0)
		return NULL;

	sprintf(devfs_path, "/dev/spidev%u.%u", bus, cs);
	return devfs_path;
}

static char *linux_spi_probe(void)
{
	char *ret;

	ret = check_fdt();
	if (ret)
		return ret;

	ret = check_sysfs();
	if (ret)
		return ret;

	return NULL;
}

/*
 * This is used when /dev/spidevX.Y is not created yet, for example, when
 * udev is not started.
 */
static int manual_mknod(const char *dev)
{
	char cmd[256];

	msg_pdbg("Creating SPI device node %s...\n", dev);
	strcpy(cmd, "modprobe spidev");
	msg_pdbg("CMD: [%s]\n", cmd);
	if (system(cmd)) {
		msg_perr("%s: failed to run '%s': %s\n", __func__,
			 cmd, strerror(errno));
		return -1;
	}
	snprintf(cmd, sizeof(cmd), "mknod %s c %d 0", dev, SPIDEV_MAJOR);
	msg_pdbg("CMD: [%s]\n", cmd);
	if (system(cmd)) {
		msg_perr("%s: failed to run '%s': %s\n", __func__,
			 cmd, strerror(errno));
		return -1;
	}

	if ((fd = open(dev, O_RDWR)) == -1) {
		msg_perr("%s: failed to open %s: %s\n", __func__,
			 dev, strerror(errno));
	}

	return fd;
}


int linux_spi_init(struct flashctx *flash)
{
	char *p, *endp, *dev;
	uint32_t speed = 0;

	/*
	 * FIXME: There might be other programmers with flash memory (such as
	 * an EC) connected via SPI. For now we rely on the device's driver to
	 * distinguish it and assume generic SPI implies host.
	 */
	if (alias && alias->type != ALIAS_HOST)
		return 1;

	dev = extract_programmer_param("dev");
	if (!dev)
		dev = linux_spi_probe();
	if (!dev || !strlen(dev)) {
		msg_perr("No SPI device found. Use flashrom -p "
			 "linux_spi:dev=/dev/spidevX.Y\n");
		return 1;
	}

	p = extract_programmer_param("speed");
	if (p && strlen(p)) {
		speed = (uint32_t)strtoul(p, &endp, 10) * 1024;
		if (p == endp) {
			msg_perr("%s: invalid clock: %s kHz\n", __func__, p);
			return 1;
		}
	}

	msg_pdbg("Using device %s\n", dev);
	if ((fd = open(dev, O_RDWR)) == -1) {
		msg_pdbg("%s: failed to open %s: %s\n", __func__,
			 dev, strerror(errno));

		if (manual_mknod(dev) == -1) {  // global fd is effected.
			return 1;
		}
	}

	if (speed > 0) {
		if (ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) == -1) {
			msg_perr("%s: failed to set speed %dHz: %s\n",
				 __func__, speed, strerror(errno));
			close(fd);
			return 1;
		}

		msg_pdbg("Using %d kHz clock\n", speed);
	}

	if (register_shutdown(linux_spi_shutdown, NULL))
		return 1;

	register_spi_programmer(&spi_programmer_linux);

	return 0;
}

static int linux_spi_shutdown(struct flashctx *flash, void *data)
{
	if (fd != -1) {
		close(fd);
		fd = -1;
	}
	return 0;
}

static int linux_spi_send_command(const struct flashctx *flash, unsigned int writecnt, unsigned int readcnt,
			const unsigned char *txbuf, unsigned char *rxbuf)
{
	int msg_start = 0, msg_count = 0;
	struct spi_ioc_transfer msg[2] = {
		{
			.tx_buf = (uint64_t)(uintptr_t)txbuf,
			.len = writecnt,
		},
		{
			.rx_buf = (uint64_t)(uintptr_t)rxbuf,
			.len = readcnt,
		},
	};

	if (fd == -1)
		return -1;

	/* Only pass necessary msg[] to ioctl() to avoid the empty message
	 * which drives an un-expected CS line and clocks. */
	if (writecnt) {
		msg_start = 0;  /* tx: msg[0] */
		msg_count++;
		if (readcnt) {
			msg_count++;
		}
	} else if (readcnt) {
		msg_start = 1;  /* rx: msg[1] */
		msg_count++;
	} else {
		msg_cerr("%s: both writecnt and readcnt are 0.\n",
			 __func__);
		return -1;
	}

	if (ioctl(fd, SPI_IOC_MESSAGE(msg_count), &msg[msg_start]) == -1) {
		msg_cerr("%s: ioctl: %s\n", __func__, strerror(errno));
		return -1;
	}
	return 0;
}

static int linux_spi_read(struct flashctx *flash, uint8_t *buf,
			  unsigned int start, unsigned int len)
{
	return spi_read_chunked(flash, buf, start, len, SPI_DMA_SIZE);
}

static int linux_spi_write_256(struct flashctx *flash, uint8_t *buf,
			       unsigned int start, unsigned int len)
{
	return spi_write_chunked(flash, buf, start, len,
				 SPI_DMA_SIZE - 5 /* WREN, Page Program */);
}
