/*
 * This file is part of the flashrom project.
 *
 * Copyright 2014, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *    * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *    * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Alternatively, this software may be distributed under the terms of the
 * GNU General Public License ("GPL") version 2 as published by the Free
 * Software Foundation.
 */

/*
 * This SPI flash programming interface is designed to talk to a Chromium OS
 * device over a Raiden USB connection.  The USB connection is routed to a
 * microcontroller running an image compiled from:
 *
 *     https://chromium.googlesource.com/chromiumos/platform/ec
 *
 * The protocol for the USB-SPI bridge is documented in the following file in
 * that respository:
 *
 *     chip/stm32/usb_spi.c
 */

#include <stdio.h>
#include <string.h>
#include "programmer.h"
#include "spi.h"

#include <libusb.h>
#include <stdlib.h>

#define GOOGLE_VID		0x18D1
#define GOOGLE_RAIDEN_PID	0x500f
#define GOOGLE_RAIDEN_ENDPOINT	2

#define PACKET_HEADER_SIZE	2
#define MAX_PACKET_SIZE		64

/*
 * This timeout is so large because the Raiden SPI timeout is 800ms.
 */
#define TRANSFER_TIMEOUT_MS	1000

#define CHECK(expression, string...)					\
	({								\
		int error__ = (expression);				\
									\
		if (error__ != 0) {					\
			msg_perr("Raiden: libusb error: %s:%d %s\n",	\
				 __FILE__,				\
				 __LINE__,				\
				 libusb_error_name(error__));		\
			msg_perr(string);				\
			return 0x20000 | -error__;			\
		}							\
	})

static libusb_context       *context = NULL;
static libusb_device_handle *device  = NULL;
static uint8_t              endpoint = GOOGLE_RAIDEN_ENDPOINT;

static int send_command(unsigned int write_count,
			unsigned int read_count,
			const unsigned char *write_buffer,
			unsigned char *read_buffer)
{
	uint8_t  buffer[MAX_PACKET_SIZE];
	int      transferred;

	if (write_count > MAX_PACKET_SIZE - PACKET_HEADER_SIZE) {
		msg_perr("Raiden: invalid write_count of %d\n", write_count);
		return SPI_INVALID_LENGTH;
	}

	if (read_count > MAX_PACKET_SIZE - PACKET_HEADER_SIZE) {
		msg_perr("Raiden: invalid read_count of %d\n", read_count);
		return SPI_INVALID_LENGTH;
	}

	buffer[0] = write_count;
	buffer[1] = read_count;

	memcpy(buffer + PACKET_HEADER_SIZE, write_buffer, write_count);

	CHECK(libusb_bulk_transfer(device,
				   LIBUSB_ENDPOINT_OUT |
				   GOOGLE_RAIDEN_ENDPOINT,
				   buffer,
				   write_count + PACKET_HEADER_SIZE,
				   &transferred,
				   TRANSFER_TIMEOUT_MS),
	      "Raiden: OUT transfer failed\n"
	      "    write_count = %d\n"
	      "    read_count  = %d\n",
	      write_count,
	      read_count);

	if (transferred != write_count + PACKET_HEADER_SIZE) {
		msg_perr("Raiden: Write failure (wrote %d, expected %d)\n",
			 transferred, write_count + PACKET_HEADER_SIZE);
		return 0x10001;
	}

	CHECK(libusb_bulk_transfer(device,
				   LIBUSB_ENDPOINT_IN |
				   GOOGLE_RAIDEN_ENDPOINT,
				   buffer,
				   read_count + PACKET_HEADER_SIZE,
				   &transferred,
				   TRANSFER_TIMEOUT_MS),
	      "Raiden: IN transfer failed\n"
	      "    write_count = %d\n"
	      "    read_count  = %d\n",
	      write_count,
	      read_count);

	if (transferred != read_count + PACKET_HEADER_SIZE) {
		msg_perr("Raiden: Read failure (read %d, expected %d)\n",
			 transferred, read_count + PACKET_HEADER_SIZE);
		return 0x10002;
	}

	memcpy(read_buffer, buffer + PACKET_HEADER_SIZE, read_count);

	return buffer[0] | (buffer[1] << 8);
}

/*
 * Unfortunately there doesn't seem to be a way to specify the maximum number
 * of bytes that your SPI device can read/write, these values are the maximum
 * data chunk size that flashrom will package up with an additional four bytes
 * of command for the flash device, resulting in a 62 byte packet, that we then
 * add two bytes to in either direction, making our way up to the 64 byte
 * maximum USB packet size for the device.
 *
 * The largest command that flashrom generates is the byte program command, so
 * we use that command header maximum size here.  The definition of
 * JEDEC_BYTE_PROGRAM_OUTSIZE includes enough space for a single byte of data
 * to write, so we add one byte of space back.
 */
#define MAX_DATA_SIZE	(MAX_PACKET_SIZE -			\
			 PACKET_HEADER_SIZE -			\
			 JEDEC_BYTE_PROGRAM_OUTSIZE +		\
			 1)

static const struct spi_programmer spi_programmer_raiden_debug = {
	.type		= SPI_CONTROLLER_RAIDEN_DEBUG,
	.max_data_read	= MAX_DATA_SIZE,
	.max_data_write	= MAX_DATA_SIZE,
	.command	= send_command,
	.multicommand	= default_spi_send_multicommand,
	.read		= default_spi_read,
	.write_256	= default_spi_write_256,
};

static long int get_parameter(char const * name, long int default_value)
{
	char *   string = extract_programmer_param(name);
	long int value  = default_value;

	if (string)
		value = strtol(string, NULL, 0);

	free(string);

	return value;
}

int raiden_debug_spi_init(void)
{
	uint16_t vid = get_parameter("vid", GOOGLE_VID);
	uint16_t pid = get_parameter("pid", GOOGLE_RAIDEN_PID);

	CHECK(libusb_init(&context), "Raiden: libusb_init failed\n");

	endpoint = get_parameter("endpoint", GOOGLE_RAIDEN_ENDPOINT);
	device   = libusb_open_device_with_vid_pid(context, vid, pid);

	if (device == NULL) {
		msg_perr("Unable to find device 0x%04x:0x%04x\n", vid, pid);
		return 1;
	}

	CHECK(libusb_set_auto_detach_kernel_driver(device, 1), "");
	CHECK(libusb_claim_interface(device, 1), "");

	register_spi_programmer(&spi_programmer_raiden_debug);

	return 0;
}
