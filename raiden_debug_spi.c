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

#include "check.h"
#include "programmer.h"
#include "spi.h"
#include "usb_device.h"

#include <libusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GOOGLE_VID                 0x18D1
#define GOOGLE_RAIDEN_SPI_SUBCLASS 0x51
#define GOOGLE_RAIDEN_SPI_PROTOCOL 0x01

enum raiden_debug_spi_request {
	RAIDEN_DEBUG_SPI_REQ_ENABLE  = 0x0000,
	RAIDEN_DEBUG_SPI_REQ_DISABLE = 0x0001,
};

#define PACKET_HEADER_SIZE	2
#define MAX_PACKET_SIZE		64

/*
 * This timeout is so large because the Raiden SPI timeout is 800ms.
 */
#define TRANSFER_TIMEOUT_MS	1000

struct usb_device *device       = NULL;
uint8_t            in_endpoint  = 0;
uint8_t            out_endpoint = 0;

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

	CHECK(LIBUSB(libusb_bulk_transfer(device->handle,
					  out_endpoint,
					  buffer,
					  write_count + PACKET_HEADER_SIZE,
					  &transferred,
					  TRANSFER_TIMEOUT_MS)),
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

	CHECK(LIBUSB(libusb_bulk_transfer(device->handle,
					  in_endpoint,
					  buffer,
					  read_count + PACKET_HEADER_SIZE,
					  &transferred,
					  TRANSFER_TIMEOUT_MS)),
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

static int match_endpoint(struct libusb_endpoint_descriptor const *descriptor,
			  enum libusb_endpoint_direction direction)
{
	return (((descriptor->bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) ==
		 direction) &&
		((descriptor->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) ==
		 LIBUSB_TRANSFER_TYPE_BULK));
}

static int find_endpoints(struct usb_device *device)
{
	int i;
	int in_count  = 0;
	int out_count = 0;

	for (i = 0; i < device->interface_descriptor->bNumEndpoints; i++) {
		struct libusb_endpoint_descriptor const  *endpoint =
			&device->interface_descriptor->endpoint[i];

		if (match_endpoint(endpoint, LIBUSB_ENDPOINT_IN)) {
			in_count++;
			in_endpoint = endpoint->bEndpointAddress;
		} else if (match_endpoint(endpoint, LIBUSB_ENDPOINT_OUT)) {
			out_count++;
			out_endpoint = endpoint->bEndpointAddress;
		}
	}

	if (in_count != 1 || out_count != 1) {
		msg_perr("Raiden: Failed to find one IN and one OUT endpoint\n"
			 "        found %d IN and %d OUT endpoints\n",
			 in_count,
			 out_count);
		return 1;
	}

	msg_pdbg("Raiden: Found IN  endpoint = 0x%02x\n",  in_endpoint);
	msg_pdbg("Raiden: Found OUT endpoint = 0x%02x\n", out_endpoint);

	return 0;
}

static int shutdown(void * data)
{
	CHECK(LIBUSB(libusb_control_transfer(
			     device->handle,
			     LIBUSB_ENDPOINT_OUT |
			     LIBUSB_REQUEST_TYPE_VENDOR |
			     LIBUSB_RECIPIENT_INTERFACE,
			     RAIDEN_DEBUG_SPI_REQ_DISABLE,
			     0,
			     device->interface_descriptor->bInterfaceNumber,
			     NULL,
			     0,
			     TRANSFER_TIMEOUT_MS)),
		"Raiden: Failed to disable SPI bridge\n");

	usb_device_free(device);

	device = NULL;
	libusb_exit(NULL);

	return 0;
}

int raiden_debug_spi_init(void)
{
	struct usb_match match;

	usb_match_init(&match);

	usb_match_value_default(&match.vid,      GOOGLE_VID);
	usb_match_value_default(&match.class,    LIBUSB_CLASS_VENDOR_SPEC);
	usb_match_value_default(&match.subclass, GOOGLE_RAIDEN_SPI_SUBCLASS);
	usb_match_value_default(&match.protocol, GOOGLE_RAIDEN_SPI_PROTOCOL);

	CHECK(LIBUSB(libusb_init(NULL)), "Raiden: libusb_init failed\n");

	CHECK(usb_device_find(&match, &device),
	      "Raiden: Failed to find devices\n");

	if (device->next != NULL) {
		struct usb_device *current;

		msg_perr("Raiden: Found too many compatible devices\n");
		msg_perr("        Use parameters to specify desired device\n");

		for (current = device;
		     current != NULL;
		     current = usb_device_free(current))
			usb_device_show("        ", current);

		device = NULL;

		return 1;
	}

	CHECK(find_endpoints(device),
	      "Raiden: Failed to find valid endpoints\n");

	CHECK(usb_device_claim(device),
	      "Raiden: Failed to claim USB device\n");

	CHECK(LIBUSB(libusb_control_transfer(
			     device->handle,
			     LIBUSB_ENDPOINT_OUT |
			     LIBUSB_REQUEST_TYPE_VENDOR |
			     LIBUSB_RECIPIENT_INTERFACE,
			     RAIDEN_DEBUG_SPI_REQ_ENABLE,
			     0,
			     device->interface_descriptor->bInterfaceNumber,
			     NULL,
			     0,
			     TRANSFER_TIMEOUT_MS)),
		"Raiden: Failed to enable SPI bridge\n");

	register_spi_programmer(&spi_programmer_raiden_debug);
	register_shutdown(shutdown, NULL);

	return 0;
}
