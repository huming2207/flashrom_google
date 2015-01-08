/*
 * This file is part of the flashrom project.
 *
 * Copyright (C) 2010 Carl-Daniel Hailfinger
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
#include <usb.h>
#include "flash.h"
#include "chipdrivers.h"
#include "programmer.h"
#include "spi.h"

#define FIRMWARE_VERSION(x,y,z) ((x << 16) | (y << 8) | z)
#define DEFAULT_TIMEOUT 3000
static usb_dev_handle *dediprog_handle;
static int dediprog_firmwareversion;
static int dediprog_endpoint;

enum cmd_t {
	CMD_TRANSCEIVE			= 0x1,
	CMD_SET_FLASH_TYPE		= 0x4,
	CMD_SET_IO_LED			= 0x7,
	CMD_READ_PROGRAMMER_INFO	= 0x8,
	CMD_SET_TARGET_FLASH_VCC	= 0x9,
	CMD_INIT			= 0xb,
	CMD_GET_UID			= 0x12,
	CMD_READ			= 0x20,
	CMD_WRITE			= 0x30,
	CMD_SET_SPI_CLK			= 0x61,
};

enum flash_type {
	FLASH_TYPE_APPLICATION_FLASH_1	= 0,
	FLASH_TYPE_FLASH_CARD,
	FLASH_TYPE_APPLICATION_FLASH_2,
};

/* Set/clear LEDs on dediprog */
enum {
	LED_PASS		= 1 << 0,
	LED_BUSY		= 1 << 1,
	LED_ERROR		= 1 << 2,
	LED_ALL			= 7,
};

/* IO bits for CMD_SET_IO_LED message */
enum {
	IO1			= 1 << 0,
	IO2			= 1 << 1,
	IO3			= 1 << 2,
	IO4			= 1 << 3,
};

enum {
	READ_MODE_STD		= 1,
	READ_FAST,
	READ_MODE_ATMEL45,
	READ_MODE_4BYTE_ADDR_MODE_FAST,
	READ_MODE_4BYTE_ADDR_MODE_FAST_WITH_0C_CMD,
};

enum {
	WRITE_MODE_PAGE_PGM = 1,
	WRITE_MODE_PAGE_WRITE,
	WRITE_MODE_1BYTE_AAI,
	WRITE_MODE_2BYTE_AAI,
	WRITE_MODE_128BYTE_PAGE,
	WRITE_MODE_PAGE_AT26DF041,
	WRITE_MODE_SILICON_BLUE_FPGA,
	WRITE_MODE_64_BYTE_PAGE_NUMONYX_PCM,	/* unit of length 512 bytes */
	WRITE_MODE_4BYTE_ADDR_MODE_256BYTE_PAGE_PGM,
	WRITE_MODE_32BYTE_PAGE_PGM_MXIC_512K,	/* unit of length 512 bytes */
	WRITE_MODE_4BYTE_ADDR_MODE_256BYTE_PAGE_PGM_12_COMMAND,
	WRITE_MODE_4BYTE_ADDR_MODE_256BYTE_PAGE_PGM_CHECKING_FLAGS,
};

static int current_led_status = -1;

enum {
	SPEED_24M,
	SPEED_8M,
	SPEED_12M,
	SPEED_3M,
	SPEED_2_18M,
	SPEED_1_5M,
	SPEED_750K,
	SPEED_375K,

	SPEED_COUNT,
	SPEED_UNKNOWN,
};

static const char *const speeds[SPEED_COUNT] = {
	"24",
	"8",
	"12",
	"3",
	"2.18",
	"1.5",
	".750",
	".375",
};

#if 0
/* Might be useful for other pieces of code as well. */
static void print_hex(void *buf, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++)
		msg_pdbg(" %02x", ((uint8_t *)buf)[i]);
}
#endif

/* Might be useful for other USB devices as well. static for now. */
static struct usb_device *get_device_by_vid_pid(uint16_t vid, uint16_t pid)
{
	struct usb_bus *bus;
	struct usb_device *dev;

	for (bus = usb_get_busses(); bus; bus = bus->next)
		for (dev = bus->devices; dev; dev = dev->next)
			if ((dev->descriptor.idVendor == vid) &&
			    (dev->descriptor.idProduct == pid))
				return dev;

	return NULL;
}

static int dediprog_set_leds(int leds)
{
	int ret, target_leds;

	if (leds < 0 || leds > LED_ALL)
		leds = LED_ALL;
	if (leds == current_led_status)
		return 0;

	/* Older Dediprogs with 2.x.x and 3.x.x firmware only had
	 * two LEDs, and they were reversed. So map them around if 
	 * we have an old device. On those devices the LEDs map as
	 * follows:
	 *   bit 2 == 0: green light is on.
	 *   bit 0 == 0: red light is on. 
	 */
	if (dediprog_firmwareversion < FIRMWARE_VERSION(5,0,0)) {
		target_leds = ((leds & LED_ERROR) >> 2) |
			((leds & LED_PASS) << 2);
	} else {
		target_leds = leds;
	}

	target_leds ^= 7;
	ret = usb_control_msg(dediprog_handle, 0x42, 0x07, 0x09, target_leds,
			      NULL, 0x0, DEFAULT_TIMEOUT);
	if (ret != 0x0) {
		msg_perr("Command Set LED 0x%x failed (%s)!\n",
			 leds, usb_strerror());
		return 1;
	}

	current_led_status = leds;

	return 0;
}

static int dediprog_set_spi_voltage(int millivolt)
{
	int ret;
	uint16_t voltage_selector;

	switch (millivolt) {
	case 0:
		/* Admittedly this one is an assumption. */
		voltage_selector = 0x0;
		break;
	case 1800:
		voltage_selector = 0x12;
		break;
	case 2500:
		voltage_selector = 0x11;
		break;
	case 3500:
		voltage_selector = 0x10;
		break;
	default:
		msg_perr("Unknown voltage %i mV! Aborting.\n", millivolt);
		return 1;
	}
	msg_pdbg("Setting SPI voltage to %u.%03u V\n", millivolt / 1000,
		 millivolt % 1000);

	ret = usb_control_msg(dediprog_handle, 0x42, CMD_SET_TARGET_FLASH_VCC,
			      voltage_selector, 0xff, NULL, 0x0,
			      DEFAULT_TIMEOUT);
	if (ret != 0x0) {
		msg_perr("Command Set SPI Voltage 0x%x failed!\n",
			 voltage_selector);
		return 1;
	}
	return 0;
}

static int dediprog_set_spi_speed(uint16_t speed)
{
	int ret;

	msg_pdbg("Setting SPI speed to %u kHz\n",
                 (int)(atof(speeds[speed]) * 1000));

	ret = usb_control_msg(dediprog_handle, 0x42, CMD_SET_SPI_CLK, speed,
			      0x0, NULL, 0x0, DEFAULT_TIMEOUT);
	if (ret != 0x0) {
		msg_perr("Command Set SPI Speed 0x%x failed!\n", speed);
		return 1;
	}
	return 0;
}

/* Bulk read interface, will read multiple 512 byte chunks aligned to 512 bytes.
 * @start	start address
 * @len		length
 * @return	0 on success, 1 on failure
 */
static int dediprog_spi_bulk_read(struct flashchip *flash, uint8_t *buf,
				  unsigned int start, unsigned int len)
{
	int ret;
	unsigned int i;
	/* chunksize must be 512, other sizes will NOT work at all. */
	const unsigned int chunksize = 0x200;
	const unsigned int count = len / chunksize;
	const char count_and_chunk[] = {count & 0xff,
					(count >> 8) & 0xff,
					chunksize & 0xff,
					(chunksize >> 8) & 0xff};

	if ((start % chunksize) || (len % chunksize)) {
		msg_perr("%s: Unaligned start=%i, len=%i! Please report a bug "
			 "at flashrom@flashrom.org\n", __func__, start, len);
		return 1;
	}

	/* No idea if the hardware can handle empty reads, so chicken out. */
	if (!len)
		return 0;
	/* Command Read SPI Bulk. No idea which read command is used on the
	 * SPI side.
	 */
	ret = usb_control_msg(dediprog_handle, 0x42, 0x20, start % 0x10000,
			      start / 0x10000, (char *)count_and_chunk,
			      sizeof(count_and_chunk), DEFAULT_TIMEOUT);
	if (ret != sizeof(count_and_chunk)) {
		msg_perr("Command Read SPI Bulk failed, %i %s!\n", ret,
			 usb_strerror());
		return 1;
	}

	for (i = 0; i < count; i++) {
		ret = usb_bulk_read(dediprog_handle, 0x80 | dediprog_endpoint,
				    (char *)buf + i * chunksize, chunksize,
				    DEFAULT_TIMEOUT);
		if (ret != chunksize) {
			msg_perr("SPI bulk read %i failed, expected %i, got %i "
				 "%s!\n", i, chunksize, ret, usb_strerror());
			return 1;
		}
	}

	return 0;
}

static int dediprog_spi_read(struct flashchip *flash, uint8_t *buf,
			     unsigned int start, unsigned int len)
{
	int ret;
	/* chunksize must be 512, other sizes will NOT work at all. */
	const unsigned int chunksize = 0x200;
	unsigned int residue = start % chunksize ? chunksize - start % chunksize : 0;
	unsigned int bulklen;

	dediprog_set_leds(LED_BUSY);

	if (residue) {
		msg_pdbg("Slow read for partial block from 0x%x, length 0x%x\n",
			 start, residue);
		ret = spi_read_chunked(flash, buf, start, residue, 16);
		if (ret)
			goto err;
	}

	/* Round down. */
	bulklen = (len - residue) / chunksize * chunksize;
	ret = dediprog_spi_bulk_read(flash, buf + residue, start + residue,
				     bulklen);
	if (ret)
		goto err;

	len -= residue + bulklen;
	if (len) {
		msg_pdbg("Slow read for partial block from 0x%x, length 0x%x\n",
			 start, len);
		ret = spi_read_chunked(flash, buf + residue + bulklen,
				       start + residue + bulklen, len, 16);
		if (ret)
			goto err;
	}

	dediprog_set_leds(LED_PASS);
	return 0;
err:
	dediprog_set_leds(LED_ERROR);
	return ret;
}

static int dediprog_spi_write_256(struct flashchip *flash, uint8_t *buf,
				  unsigned int start, unsigned int len)
{
	int ret;

	dediprog_set_leds(LED_BUSY);

	/* No idea about the real limit. Maybe 12, maybe more, maybe less. */
	ret = spi_write_chunked(flash, buf, start, len, 12);

	if (ret)
		dediprog_set_leds(LED_ERROR);
	else
		dediprog_set_leds(LED_PASS);

	return ret;
}

static int dediprog_spi_send_command(unsigned int writecnt, unsigned int readcnt,
			const unsigned char *writearr, unsigned char *readarr)
{
	int ret;

	msg_pspew("%s, writecnt=%i, readcnt=%i\n", __func__, writecnt, readcnt);
	ret = usb_control_msg(dediprog_handle, 0x42, CMD_TRANSCEIVE, 0,
			      readcnt ? 0x1 : 0x0, (char *)writearr, writecnt,
			      DEFAULT_TIMEOUT);
	if (ret != writecnt) {
		msg_perr("Send SPI failed, expected %i, got %i %s!\n",
			 writecnt, ret, usb_strerror());
		return 1;
	}
	if (!readcnt)
		return 0;
	memset(readarr, 0, readcnt);
	ret = usb_control_msg(dediprog_handle, 0xc2, CMD_TRANSCEIVE, 0, 0,
			     (char *)readarr, readcnt, DEFAULT_TIMEOUT);
	if (ret != readcnt) {
		msg_perr("Receive SPI failed, expected %i, got %i %s!\n",
			 readcnt, ret, usb_strerror());
		return 1;
	}
	return 0;
}

static int dediprog_check_devicestring(void)
{
	int ret;
	int fw[3];
	char buf[0x11];

	/* Command Receive Device String. */
	memset(buf, 0, sizeof(buf));
	ret = usb_control_msg(dediprog_handle, 0xc2, CMD_READ_PROGRAMMER_INFO,
			      0, 0, buf, 0x10, DEFAULT_TIMEOUT);
	if (ret != 0x10) {
		msg_perr("Incomplete/failed Command Receive Device String!\n");
		return 1;
	}
	buf[0x10] = '\0';
	msg_pdbg("Found a %s\n", buf);
	if (memcmp(buf, "SF100", 0x5)) {
		msg_perr("Device not a SF100!\n");
		return 1;
	}
	if (sscanf(buf, "SF100 V:%d.%d.%d ", &fw[0], &fw[1], &fw[2]) != 3) {
		msg_perr("Unexpected firmware version string!\n");
		return 1;
	}
	/* Only these versions were tested. */
	if (fw[0] < 2 || fw[0] > 5) {
		msg_perr("Unexpected firmware version %d.%d.%d!\n", fw[0],
			 fw[1], fw[2]);
		return 1;
	}
	dediprog_firmwareversion = FIRMWARE_VERSION(fw[0], fw[1], fw[2]);
	return 0;
}

static int dediprog_device_init(void)
{
	int ret;
	char buf[0x1];

	memset(buf, 0, sizeof(buf));
	ret = usb_control_msg(dediprog_handle, 0xc3, CMD_INIT, 0x0, 0x0, buf,
			      0x1, DEFAULT_TIMEOUT);
	if (ret < 0) {
		msg_perr("Command A failed (%s)!\n", usb_strerror());
		return 1;
	}
	if ((ret != 0x1) || (buf[0] != 0x6f)) {
		msg_perr("Unexpected response to init!\n");
		return 1;
	}
	return 0;
}

static int set_target_flash(enum flash_type type)
{
	int ret;

	ret = usb_control_msg(dediprog_handle, 0x42, CMD_SET_FLASH_TYPE, type,
			      0x0, NULL, 0, DEFAULT_TIMEOUT);
	if (ret != 0x0) {
		msg_perr("set_target_flash failed (%s)!\n", usb_strerror());
		return 1;
	}
	return 0;
}

static int parse_speed(char *speed_str)
{
	int i;

	for (i = 0; i < SPEED_COUNT; i++) {
		if (!strcmp(speed_str, speeds[i]))
			return i;
	}

	return SPEED_UNKNOWN;
}

static void list_speeds(void)
{
	int i;

	for (i = 0; i < SPEED_COUNT; i++)
		msg_perr("%s%s", speeds[i], i == SPEED_COUNT - 1 ? "" : ", ");
	msg_perr("\n");
}

static int parse_voltage(char *voltage)
{
	char *tmp = NULL;
	int i;
	int millivolt = 0, fraction = 0;

	if (!voltage || !strlen(voltage)) {
		msg_perr("Empty voltage= specified.\n");
		return -1;
	}
	millivolt = (int)strtol(voltage, &tmp, 0);
	voltage = tmp;
	/* Handle "," and "." as decimal point. Everything after it is assumed
	 * to be in decimal notation.
	 */
	if ((*voltage == '.') || (*voltage == ',')) {
		voltage++;
		for (i = 0; i < 3; i++) {
			fraction *= 10;
			/* Don't advance if the current character is invalid,
			 * but continue multiplying.
			 */
			if ((*voltage < '0') || (*voltage > '9'))
				continue;
			fraction += *voltage - '0';
			voltage++;
		}
		/* Throw away remaining digits. */
		voltage += strspn(voltage, "0123456789");
	}
	/* The remaining string must be empty or "mV" or "V". */
	tolower_string(voltage);

	/* No unit or "V". */
	if ((*voltage == '\0') || !strncmp(voltage, "v", 1)) {
		millivolt *= 1000;
		millivolt += fraction;
	} else if (!strncmp(voltage, "mv", 2) ||
		   !strncmp(voltage, "milliv", 6)) {
		/* No adjustment. fraction is discarded. */
	} else {
		/* Garbage at the end of the string. */
		msg_perr("Garbage voltage= specified.\n");
		return -1;
	}
	return millivolt;
}

static const struct spi_programmer spi_programmer_dediprog = {
	.type		= SPI_CONTROLLER_DEDIPROG,
	.max_data_read	= MAX_DATA_UNSPECIFIED,
	.max_data_write	= MAX_DATA_UNSPECIFIED,
	.command	= dediprog_spi_send_command,
	.multicommand	= default_spi_send_multicommand,
	.read		= dediprog_spi_read,
	.write_256	= dediprog_spi_write_256,
};

static int dediprog_shutdown(void *data)
{
	msg_pspew("%s\n", __func__);

	/* URB 28. Command Set SPI Voltage to 0. */
	if (dediprog_set_spi_voltage(0x0))
		return 1;

	if (usb_release_interface(dediprog_handle, 0)) {
		msg_perr("Could not release USB interface!\n");
		return 1;
	}
	if (usb_close(dediprog_handle)) {
		msg_perr("Could not close USB device!\n");
		return 1;
	}
	return 0;
}

/* URB numbers refer to the first log ever captured. */
int dediprog_init(void)
{
	struct usb_device *dev;
	char *voltage;
	int millivolt = 3500;
	int ret;
	char *speed_str;
	int speed = SPEED_12M;

	msg_pspew("%s\n", __func__);

	voltage = extract_programmer_param("voltage");
	if (voltage) {
		millivolt = parse_voltage(voltage);
		free(voltage);
		if (millivolt < 0)
			return 1;
		msg_pinfo("Setting voltage to %i mV\n", millivolt);
	}
	speed_str = extract_programmer_param("speed");
	if (speed_str) {
		speed = parse_speed(speed_str);
		if (speed == SPEED_UNKNOWN) {
			msg_perr("Invalid speed '%s', valid speeds in MHz are: ",
				 speed_str);
			list_speeds();
			free(speed_str);
			return 1;
		}
		free(speed_str);
	}

	/* Here comes the USB stuff. */
	usb_init();
	usb_find_busses();
	usb_find_devices();
	dev = get_device_by_vid_pid(0x0483, 0xdada);
	if (!dev) {
		msg_perr("Could not find a Dediprog SF100 on USB!\n");
		return 1;
	}
	msg_pdbg("Found USB device (%04x:%04x).\n",
		 dev->descriptor.idVendor, dev->descriptor.idProduct);
	dediprog_handle = usb_open(dev);
	ret = usb_set_configuration(dediprog_handle, 1);
	if (ret < 0) {
		msg_perr("Could not set USB device configuration: %i %s\n",
			 ret, usb_strerror());
		if (usb_close(dediprog_handle))
			msg_perr("Could not close USB device!\n");
		return 1;
	}
	ret = usb_claim_interface(dediprog_handle, 0);
	if (ret < 0) {
		msg_perr("Could not claim USB device interface %i: %i %s\n",
			 0, ret, usb_strerror());
		if (usb_close(dediprog_handle))
			msg_perr("Could not close USB device!\n");
		return 1;
	}
	dediprog_endpoint = 2;
	
	if (register_shutdown(dediprog_shutdown, NULL))
		return 1;

	if (dediprog_device_init())
		return 1;
	if (dediprog_check_devicestring())
		return 1;
	if (set_target_flash(FLASH_TYPE_APPLICATION_FLASH_1)) {
		dediprog_set_leds(LED_ERROR);
		return 1;
	}
	/* URB 11. Command Set SPI Voltage. */
	if (dediprog_set_spi_voltage(millivolt)) {
		dediprog_set_leds(LED_ERROR);
		return 1;
	}
	if (dediprog_set_spi_speed(speed)) {
		dediprog_set_leds(LED_ERROR);
		return 1;
	}

	register_spi_programmer(&spi_programmer_dediprog);

	dediprog_set_leds(0);

	return 0;
}
