/*
 * This file is part of the flashrom project.
 *
 * Copyright 2015 Google Inc.
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

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <mtd/mtd-user.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "file.h"
#include "flash.h"
#include "programmer.h"
#include "writeprotect.h"

#define LINUX_DEV_ROOT			"/dev"
#define LINUX_MTD_SYSFS_ROOT		"/sys/class/mtd"

/* enough space for LINUX_MTD_SYSFS_ROOT + directory name + filename */
static char sysfs_path[PATH_MAX];

static int dev_fd = -1;

static int mtd_device_is_writeable;

static int mtd_no_erase;

/* Size info is presented in bytes in sysfs. */
static unsigned long int mtd_total_size;
static unsigned long int mtd_numeraseregions;
static unsigned long int mtd_erasesize;	/* only valid if numeraseregions is 0 */

static struct wp wp_mtd;	/* forward declaration */

static int stat_mtd_files(char *dev_path)
{
	struct stat s;

	errno = 0;
	if (stat(dev_path, &s) < 0) {
		msg_pdbg("Cannot stat \"%s\": %s\n", dev_path, strerror(errno));
		return 1;
	}

	if (lstat(sysfs_path, &s) < 0) {
		msg_pdbg("Cannot stat \"%s\" : %s\n",
				sysfs_path, strerror(errno));
		return 1;
	}

	return 0;
}

/* read a string from a sysfs file and sanitize it */
static int read_sysfs_string(const char *filename, char *buf, int len)
{
	int fd, bytes_read, i;
	char path[strlen(LINUX_MTD_SYSFS_ROOT) + 32];

	snprintf(path, sizeof(path), "%s/%s", sysfs_path, filename);

	if ((fd = open(path, O_RDONLY)) < 0) {
		msg_perr("Cannot open %s\n", path);
		return 1;
	}

	if ((bytes_read = read(fd, buf, len - 1)) < 0) {
		msg_perr("Cannot read %s\n", path);
		close(fd);
		return 1;
	}

	buf[bytes_read] = '\0';

	/*
	 * Files from sysfs sometimes contain a newline or other garbage that
	 * can confuse functions like strtoul() and ruin formatting in print
	 * statements. Replace the first non-printable character (space is
	 * considered printable) with a proper string terminator.
	 */
	for (i = 0; i < len; i++) {
		if (!isprint(buf[i])) {
			buf[i] = '\0';
			break;
		}
	}

	close(fd);
	return 0;
}

static int read_sysfs_int(const char *filename, unsigned long int *val)
{
	char buf[32];
	char *endptr;

	if (read_sysfs_string(filename, buf, sizeof(buf)))
		return 1;

	errno = 0;
	*val = strtoul(buf, &endptr, 0);
	if (endptr != &buf[strlen(buf)]) {
		msg_perr("Error reading %s\n", filename);
		return 1;
	}

	if (errno) {
		msg_perr("Error reading %s: %s\n", filename, strerror(errno));
		return 1;
	}

	return 0;
}

/* returns 0 to indicate success, non-zero to indicate error */
static int get_mtd_info(void)
{
	unsigned long int tmp;
	char mtd_device_name[32];

	/* Flags */
	if (read_sysfs_int("flags", &tmp))
		return 1;
	if (tmp & MTD_WRITEABLE) {
		/* cache for later use by write function */
		mtd_device_is_writeable = 1;
	}
	if (tmp & MTD_NO_ERASE) {
		mtd_no_erase = 1;
	}

	/* Device name */
	if (read_sysfs_string("name", mtd_device_name, sizeof(mtd_device_name)))
		return 1;

	/* Total size */
	if (read_sysfs_int("size", &mtd_total_size))
		return 1;
	if (__builtin_popcount(mtd_total_size) != 1) {
		msg_perr("MTD size is not a power of 2\n");
		return 1;
	}

	/* Erase size */
	if (read_sysfs_int("erasesize", &mtd_erasesize))
		return 1;
	if (__builtin_popcount(mtd_erasesize) != 1) {
		msg_perr("MTD erase size is not a power of 2\n");
		return 1;
	}

	/* Erase regions */
	if (read_sysfs_int("numeraseregions", &mtd_numeraseregions))
		return 1;
	if (mtd_numeraseregions != 0) {
		msg_perr("Non-uniform eraseblock size is unsupported.\n");
		return 1;
	}

	msg_pspew("%s: device_name: \"%s\", is_writeable: %d, "
		"numeraseregions: %lu, total_size: %lu, erasesize: %lu\n",
		__func__, mtd_device_name, mtd_device_is_writeable,
		mtd_numeraseregions, mtd_total_size, mtd_erasesize);

	return 0;
}

static int linux_mtd_probe(struct flashctx *flash)
{
	flash->chip->wp = &wp_mtd;
	if (mtd_no_erase)
		flash->chip->feature_bits |= FEATURE_NO_ERASE;
	flash->chip->tested = TEST_OK_PREW;
	flash->chip->total_size = mtd_total_size / 1024;	/* bytes -> kB */
	flash->chip->block_erasers[0].eraseblocks[0].size = mtd_erasesize;
	flash->chip->block_erasers[0].eraseblocks[0].count =
				mtd_total_size / mtd_erasesize;
	return 1;
}

static int linux_mtd_read(struct flashctx *flash, uint8_t *buf,
			  unsigned int start, unsigned int len)
{
	unsigned int eb_size = flash->chip->block_erasers[0].eraseblocks[0].size;
	unsigned int i;

	if (lseek(dev_fd, start, SEEK_SET) != start) {
		msg_perr("Cannot seek to 0x%06x: %s\n", start, strerror(errno));
		return 1;
	}

	for (i = 0; i < len; ) {
		/* Try to align reads to eraseblock size */
		unsigned int step = eb_size - ((start + i) % eb_size);
		step = min(step, len - i);

		if (read(dev_fd, buf + i, step) != step) {
			msg_perr("Cannot read 0x%06x bytes at 0x%06x: %s\n",
					step, start + i, strerror(errno));
			return 1;
		}

		i += step;
	}

	return 0;
}

/* this version assumes we must divide the write request into pages ourselves */
static int linux_mtd_write(struct flashctx *flash, const uint8_t *buf,
				unsigned int start, unsigned int len)
{
	unsigned int page;
	unsigned int chunksize, page_size;

	chunksize = page_size = flash->chip->page_size;

	if (!mtd_device_is_writeable)
		return 1;

	for (page = start / page_size;
		page <= (start + len - 1) / page_size; page++) {
		unsigned int i, starthere, lenhere;

		starthere = max(start, page * page_size);
		lenhere = min(start + len, (page + 1) * page_size) - starthere;
		for (i = 0; i < lenhere; i += chunksize) {
			unsigned int towrite = min(chunksize, lenhere - i);

			if (lseek(dev_fd, starthere, SEEK_SET) != starthere) {
				msg_perr("Cannot seek to 0x%06x: %s\n",
						start, strerror(errno));
				return 1;
			}

			if (write(dev_fd, &buf[starthere - start], towrite) != towrite) {
				msg_perr("Cannot read 0x%06x bytes at 0x%06x: "
					"%s\n", start, len, strerror(errno));
				return 1;
			}
		}
	}

	return 0;
}

static int linux_mtd_erase(struct flashctx *flash,
			unsigned int start, unsigned int len)
{
	uint32_t u;

	if (mtd_no_erase) {
		msg_perr("%s: device does not support erasing. Please file a "
				"bug report at flashrom@flashrom.org\n", __func__);
		return 1;
	}

	if (mtd_numeraseregions != 0) {
		/* TODO: Support non-uniform eraseblock size using
		   use MEMGETREGIONCOUNT/MEMGETREGIONINFO ioctls */
	}

	for (u = 0; u < len; u += mtd_erasesize) {
		struct erase_info_user erase_info = {
			.start = start + u,
			.length = mtd_erasesize,
		};

		if (ioctl(dev_fd, MEMERASE, &erase_info) == -1) {
			msg_perr("%s: ioctl: %s\n", __func__, strerror(errno));
			return 1;
		}
	}

	return 0;
}

static struct opaque_programmer programmer_linux_mtd = {
	/* max_data_{read,write} don't have any effect for this programmer */
	.max_data_read	= MAX_DATA_UNSPECIFIED,
	.max_data_write	= MAX_DATA_UNSPECIFIED,
	.probe		= linux_mtd_probe,
	.read		= linux_mtd_read,
	.write		= linux_mtd_write,
	.erase		= linux_mtd_erase,
};

/* Returns 0 if setup is successful, non-zero to indicate error */
static int linux_mtd_setup(int dev_num)
{
	char dev_path[16];	/* "/dev/mtdN" */
	int ret = 1;

	if (dev_num < 0) {
		char *tmp, *p;

		tmp = (char *)scanft(LINUX_MTD_SYSFS_ROOT, "type", "nor", 1);
		if (!tmp) {
			msg_pdbg("%s: NOR type device not found.\n", __func__);
			goto linux_mtd_setup_exit;
		}

		/* "tmp" should be something like "/sys/blah/mtdN/type" */
		p = tmp + strlen(LINUX_MTD_SYSFS_ROOT);
		while (p[0] == '/')
			p++;

		if (sscanf(p, "mtd%d", &dev_num) != 1) {
			msg_perr("Can't obtain device number from \"%s\"\n", p);
			free(tmp);
			goto linux_mtd_setup_exit;
		}
		free(tmp);
	}

	snprintf(sysfs_path, sizeof(sysfs_path), "%s/mtd%d",
				LINUX_MTD_SYSFS_ROOT, dev_num);
	snprintf(dev_path, sizeof(dev_path), "%s/mtd%d",
				LINUX_DEV_ROOT, dev_num);
	msg_pdbg("%s: sysfs_path: \"%s\", dev_path: \"%s\"\n",
			__func__, sysfs_path, dev_path);

	if (stat_mtd_files(dev_path))
		goto linux_mtd_setup_exit;

	if (get_mtd_info())
		goto linux_mtd_setup_exit;

	if ((dev_fd = open(dev_path, O_RDWR)) == -1) {
		msg_pdbg("%s: failed to open %s: %s\n", __func__,
			 dev_path, strerror(errno));
		goto linux_mtd_setup_exit;
	}

	ret = 0;
linux_mtd_setup_exit:
	return ret;
}

static int linux_mtd_shutdown(void *data)
{
	if (dev_fd != -1) {
		close(dev_fd);
		dev_fd = -1;
	}

	return 0;
}

int linux_mtd_init(void)
{
	char *param;
	int dev_num = -1;	/* linux_mtd_setup will search if dev_num < 0 */
	int ret = 1;

	if (alias && alias->type != ALIAS_HOST)
		return 1;

	param = extract_programmer_param("dev");
	if (param) {
		char *endptr;

		dev_num = strtol(param, &endptr, 0);
		if ((param == endptr) || (dev_num < 0)) {
			msg_perr("Invalid device number %s. Use flashrom -p "
				"linux_mtd:dev=N where N is a valid MTD "
				"device number\n", param);
			goto linux_mtd_init_exit;
		}
	}

	if (linux_mtd_setup(dev_num))
		goto linux_mtd_init_exit;

	if (register_shutdown(linux_mtd_shutdown, NULL))
		goto linux_mtd_init_exit;

	register_opaque_programmer(&programmer_linux_mtd);

	ret = 0;
linux_mtd_init_exit:
	msg_pdbg("%s: %s\n", __func__, ret == 0 ? "success." : "failed.");
	return ret;
}

/*
 * Write-protect functions.
 */
static int mtd_wp_list_ranges(const struct flashctx *flash)
{
	/* TODO: implement this */
	msg_perr("--wp-list is not currently implemented for MTD.\n");
	return 1;
}

/*
 * We only have MEMLOCK to enable write-protection for a particular block,
 * so we need to do force the user to use --wp-range and --wp-enable
 * command-line arguments simultaneously. (Fortunately, CrOS factory
 * installer does this already).
 *
 * The --wp-range argument is processed first and will set these variables
 * which --wp-enable will use afterward.
 */
static unsigned int wp_range_start;
static unsigned int wp_range_len;
static int wp_set_range_called = 0;

static int mtd_wp_set_range(const struct flashctx *flash,
			unsigned int start, unsigned int len)
{
	wp_range_start = start;
	wp_range_len = len;

	wp_set_range_called = 1;
	return 0;
}

static int mtd_wp_enable_writeprotect(const struct flashctx *flash, enum wp_mode mode)
{
	struct erase_info_user entire_chip = {
		.start = 0,
		.length = mtd_total_size,
	};
	struct erase_info_user desired_range = {
		.start = wp_range_start,
		.length = wp_range_len,
	};

	if (!wp_set_range_called) {
		msg_perr("For MTD, --wp-range and --wp-enable must be "
			"used simultaneously.\n");
		return 1;
	}

	/*
	 * MTD handles write-protection additively, so whatever new range is
	 * specified is added to the range which is currently protected. To be
	 * consistent with flashrom behavior with other programmer interfaces,
	 * we need to disable the current write protection and then enable
	 * it for the desired range.
	 */
	if (ioctl(dev_fd, MEMUNLOCK, &entire_chip) == -1) {
		msg_perr("%s: Failed to disable write-protection, ioctl: %s\n",
				__func__, strerror(errno));
		msg_perr("Did you disable WP#?\n");
		return 1;
	}

	if (ioctl(dev_fd, MEMLOCK, &desired_range) == -1) {
		msg_perr("%s: Failed to enable write-protection, ioctl: %s\n",
				__func__, strerror(errno));
		return 1;
	}

	return 0;
}

static int mtd_wp_disable_writeprotect(const struct flashctx *flash)
{
	struct erase_info_user erase_info;

	if (wp_set_range_called) {
		erase_info.start = wp_range_start;
		erase_info.length = wp_range_len;
	} else {
		erase_info.start = 0;
		erase_info.length = mtd_total_size;
	}

	if (ioctl(dev_fd, MEMUNLOCK, &erase_info) == -1) {
		msg_perr("%s: ioctl: %s\n", __func__, strerror(errno));
		msg_perr("Did you disable WP#?\n");
		return 1;
	}

	return 0;
}

static int mtd_wp_status(const struct flashctx *flash)
{
	uint32_t start = 0, len = 0;
	int start_found = 0;
	unsigned int u;

	/* For now, assume only one contiguous region can be locked (NOR) */
	/* FIXME: use flash struct members instead of raw MTD values here */
	for (u = 0; u < mtd_total_size; u += mtd_erasesize) {
		int rc;
		struct erase_info_user erase_info = {
			.start = u,
			.length = mtd_erasesize,
		};

		rc = ioctl(dev_fd, MEMISLOCKED, &erase_info);
		if (rc < 0) {
			msg_perr("%s: ioctl: %s\n", __func__, strerror(errno));
			return 1;
		} else if (rc == 1) {
			if (!start_found) {
				start = erase_info.start;
				start_found = 1;
			}
			len += mtd_erasesize;
		} else if (rc == 0) {
			if (start_found) {
				/* TODO: changes required for supporting non-contiguous locked regions */
				break;
			}
		}

	}

	msg_cinfo("WP: write protect is %s.\n",
			start_found ? "enabled": "disabled");
	msg_pinfo("WP: write protect range: start=0x%08x, "
			"len=0x%08x\n", start, len);

	return 0;
}

static struct wp wp_mtd = {
	.list_ranges	= mtd_wp_list_ranges,
	.set_range	= mtd_wp_set_range,
	.enable		= mtd_wp_enable_writeprotect,
	.disable	= mtd_wp_disable_writeprotect,
	.wp_status	= mtd_wp_status,
};
