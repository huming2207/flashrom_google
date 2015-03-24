/*
 * This file is part of the flashrom project.
 *
 * Copyright 2015, Google Inc.
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

#include "programmer.h"
#include "spi.h"
#include "usb_device.h"

#include <assert.h>
#include <libusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Possibly extract a programmer parameter and use it to initialize the given
 * match value structure.
 */
static void usb_match_value_init(struct usb_match_value *match,
				 char const *parameter)
{
	char *string = extract_programmer_param(parameter);

	match->name = parameter;

	if (string) {
		match->set = 1;
		match->value = strtol(string, NULL, 0);
	} else {
		match->set = 0;
	}

	free(string);
}

#define USB_MATCH_VALUE_INIT(NAME)			\
	usb_match_value_init(&match->NAME, #NAME)

void usb_match_init(struct usb_match *match)
{
	USB_MATCH_VALUE_INIT(vid);
	USB_MATCH_VALUE_INIT(pid);
	USB_MATCH_VALUE_INIT(bus);
	USB_MATCH_VALUE_INIT(address);
	USB_MATCH_VALUE_INIT(config);
	USB_MATCH_VALUE_INIT(interface);
	USB_MATCH_VALUE_INIT(altsetting);
	USB_MATCH_VALUE_INIT(class);
	USB_MATCH_VALUE_INIT(subclass);
	USB_MATCH_VALUE_INIT(protocol);
}

void usb_match_value_default(struct usb_match_value *value,
			     long int default_value)
{
	if (value->set)
		return;

	value->set   = 1;
	value->value = default_value;
}

/*
 * Match the value against a possible user supplied parameter.
 *
 * Return:
 *     0: The user supplied the given parameter and it did not match the value.
 *     1: Either the user didn't supply the parameter, or they did and it
 *        matches the given value.
 */
static int check_match(struct usb_match_value const *match_value, int value)
{
	int reject = match_value->set && (match_value->value != value);

	if (reject)
		msg_pdbg("USB: Rejecting device because %s = %d != %d\n",
			 match_value->name,
			 value,
			 match_value->value);

	return !reject;
}

/*
 * Allocate a copy of device and add it to the head of the devices list.
 */
static void add_device(struct usb_device *device,
		       struct usb_device **devices)
{
	struct usb_device *copy = malloc(sizeof(struct usb_device));

	assert(copy != NULL);

	*copy = *device;

	copy->next = *devices;
	*devices = copy;

	libusb_ref_device(copy->device);
}

/*
 * Look through the interfaces of the current device config for a match. Stop
 * looking after the first valid match is found.
 *
 * Return:
 *     0: No matching interface was found.
 *     1: A complete match was found and added to the devices list.
 */
static int find_interface(struct usb_match const *match,
			  struct usb_device *current,
			  struct usb_device **devices)
{
	int i, j;

	for (i = 0; i < current->config_descriptor->bNumInterfaces; ++i) {
		struct libusb_interface const *interface;

		interface = &current->config_descriptor->interface[i];

		for (j = 0; j < interface->num_altsetting; ++j) {
			struct libusb_interface_descriptor const *descriptor;

			descriptor = &interface->altsetting[j];

			if (check_match(&match->interface,
					descriptor->bInterfaceNumber) &&
			    check_match(&match->altsetting,
					descriptor->bAlternateSetting) &&
			    check_match(&match->class,
					descriptor->bInterfaceClass) &&
			    check_match(&match->subclass,
					descriptor->bInterfaceSubClass) &&
			    check_match(&match->protocol,
					descriptor->bInterfaceProtocol)) {
				current->interface_descriptor = descriptor;
				add_device(current, devices);
				msg_pdbg("USB: Found matching device\n");
				return 1;
			}
		}
	}

	return 0;
}

/*
 * Look through the configs of the current device for a match.  Stop looking
 * after the first valid match is found.
 *
 * Return:
 *     0: All configurations successfully checked, one may have been added to
 *        the list.
 *     non-zero: There was a failure while checking for a match.
 */
static int find_config(struct usb_match const *match,
		       struct usb_device *current,
		       struct libusb_device_descriptor const *device_descriptor,
		       struct usb_device **devices)
{
	int i;

	for (i = 0; i < device_descriptor->bNumConfigurations; ++i) {
		CHECK(LIBUSB(libusb_get_config_descriptor(
				     current->device,
				     i,
				     &current->config_descriptor)),
		      "USB: Failed to get config descriptor");

		if (check_match(&match->config,
				current->config_descriptor->
				bConfigurationValue) &&
		    find_interface(match, current, devices))
			break;

		libusb_free_config_descriptor(current->config_descriptor);
	}

	return 0;
}

int usb_device_find(struct usb_match const *match, struct usb_device **devices)
{
	libusb_device **list;
	ssize_t         count;
	size_t          i;

	*devices = NULL;

	CHECK(LIBUSB(count = libusb_get_device_list(NULL, &list)),
	      "USB: Failed to get device list");

	for (i = 0; i < count; ++i) {
		struct libusb_device_descriptor descriptor;
		struct usb_device               current = {
			.device = list[i],
			.handle = NULL,
			.next   = NULL,
		};

		uint8_t bus     = libusb_get_bus_number(list[i]);
		uint8_t address = libusb_get_device_address(list[i]);

		msg_pdbg("USB: Inspecting device (Bus %d, Address %d)\n",
			 bus,
			 address);

		CHECK(LIBUSB(libusb_get_device_descriptor(list[i],
							  &descriptor)),
		      "USB: Failed to get device descriptor");

		if (check_match(&match->vid,     descriptor.idVendor) &&
		    check_match(&match->pid,     descriptor.idProduct) &&
		    check_match(&match->bus,     bus) &&
		    check_match(&match->address, address))
			CHECK(find_config(match,
					  &current,
					  &descriptor,
					  devices),
			      "USB: Failed to find config");
	}

	libusb_free_device_list(list, 1);

	return (*devices == NULL);
}

/*
 * If the underlying libusb device is not open, open it.
 *
 * Return:
 *     0: The device was already open or was successfully opened.
 *     non-zero: There was a failure while opening the device.
 */
static int usb_device_open(struct usb_device *device)
{
	int      current_config;

	if (device->handle == NULL)
		CHECK(LIBUSB(libusb_open(device->device, &device->handle)),
		      "USB: Failed to open device\n");

	return 0;
}

int usb_device_show(char const *prefix, struct usb_device *device)
{
	struct libusb_device_descriptor descriptor;
	unsigned char                   product[256];

	CHECK(usb_device_open(device), "");

	CHECK(LIBUSB(libusb_get_device_descriptor(device->device, &descriptor)),
	      "USB: Failed to get device descriptor\n");

	CHECK(LIBUSB(libusb_get_string_descriptor_ascii(
			     device->handle,
			     descriptor.iProduct,
			     product,
			     sizeof(product))),
	      "USB: Failed to get device product string\n");

	product[255] = '\0';

	msg_perr("%sbus=0x%02x,address=0x%02x | %s\n",
		 prefix,
		 libusb_get_bus_number(device->device),
		 libusb_get_device_address(device->device),
		 product);

	return 0;
}

int usb_device_claim(struct usb_device *device)
{
	int current_config;

	CHECK(usb_device_open(device), "");

	CHECK(LIBUSB(libusb_get_configuration(device->handle,
					      &current_config)),
	      "USB: Failed to get current device configuration\n");

	if (current_config != device->config_descriptor->bConfigurationValue)
		CHECK(LIBUSB(libusb_set_configuration(
				     device->handle,
				     device->
				     config_descriptor->
				     bConfigurationValue)),
		      "USB: Failed to set new configuration from %d to %d\n",
		      current_config,
		      device->config_descriptor->bConfigurationValue);

	CHECK(LIBUSB(libusb_set_auto_detach_kernel_driver(device->handle, 1)),
	      "USB: Failed to enable auto kernel driver detach\n");

	CHECK(LIBUSB(libusb_claim_interface(device->handle,
					    device->
					    interface_descriptor->
					    bInterfaceNumber)),
	      "USB: Could not claim device interface %d\n",
	      device->interface_descriptor->bInterfaceNumber);

	if (device->interface_descriptor->bAlternateSetting != 0)
		CHECK(LIBUSB(libusb_set_interface_alt_setting(
				     device->handle,
				     device->
				     interface_descriptor->
				     bInterfaceNumber,
				     device->
				     interface_descriptor->
				     bAlternateSetting)),
		      "USB: Failed to set alternate setting %d\n",
		      device->interface_descriptor->bAlternateSetting);

	return 0;
}

struct usb_device *usb_device_free(struct usb_device *device)
{
	struct usb_device *next = device->next;

	if (device->handle != NULL)
		libusb_close(device->handle);

	/*
	 * This unref balances the ref added in the add_device function.
	 */
	libusb_unref_device(device->device);
	libusb_free_config_descriptor(device->config_descriptor);

	free(device);

	return next;
}
