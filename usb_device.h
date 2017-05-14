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

/*
 * USB device matching framework
 *
 * This can be used to match a USB device by a number of different parameters.
 * The parameters can be passed on the command line and defaults can be set
 * by the programmer.
 */

#include "check.h"

#include <libusb.h>
#include <stdint.h>

/*
 * The LIBUSB macro converts a libusb failure code into an error code that
 * flashrom recognizes.  It also displays additional libusb specific
 * information about the failure.
 */
#define LIBUSB(expression)				      		\
	({						      		\
		int libusb_error__ = (expression);		      	\
							      		\
		if (libusb_error__ < 0) {			      	\
			msg_perr("libusb error: %s:%d %s\n",  		\
				 __FILE__,		      		\
				 __LINE__,		      		\
				 libusb_error_name(libusb_error__)); 	\
			libusb_error__ = 0x20000 | -libusb_error__;	\
		} else {				      		\
			libusb_error__ = 0;			      	\
		}					      		\
							      		\
		libusb_error__;				      		\
	})

/*
 * A USB match and associated value struct are used to encode the information
 * about a device against which we wish to match.  If the value of a
 * usb_match_value has been set then a device must match that value.  The name
 * of the usb_match_value is used to fetch the programmer parameter from the
 * flashrom command line and is the same as the name of the corresponding
 * field in usb_match.
 */
struct usb_match_value {
	char const *name;
	int         value;
	int         set;
};

struct usb_match {
	struct usb_match_value bus;
	struct usb_match_value address;
	struct usb_match_value vid;
	struct usb_match_value pid;
	struct usb_match_value serial;
	struct usb_match_value config;
	struct usb_match_value interface;
	struct usb_match_value altsetting;
	struct usb_match_value class;
	struct usb_match_value subclass;
	struct usb_match_value protocol;
};

/*
 * Initialize a usb_match structure so that each value's name matches the
 * values name in the usb_match structure (so bus.name == "bus"...), and
 * look for each value in the flashrom command line via
 * extract_programmer_param.  If the value is found convert it to an integer
 * using strtol, accepting hex, decimal and octal encoding.
 */
void usb_match_init(struct usb_match *match);

/*
 * Add a default value to a usb_match_value.  This must be done after calling
 * usb_match_init.  If usb_match_init already set the value of a usb_match_value
 * we do nothing, otherwise set the value to default_value.  This ensures that
 * parameters passed on the command line override defaults.
 */
void usb_match_value_default(struct usb_match_value *match,
			     long int default_value);

/*
 * The usb_device structure is an entry in a linked list of devices that were
 * matched by usb_device_find.
 */
struct usb_device {
	struct libusb_device                     *device;
	struct libusb_config_descriptor          *config_descriptor;
	struct libusb_interface_descriptor const *interface_descriptor;

	/*
	 * Initially NULL, the libusb_device_handle is only valid once the
	 * usb_device has been successfully passed to usb_device_show or
	 * usb_device_claim.
	 */
	struct libusb_device_handle *handle;

	/*
	 * Link to next device, or NULL
	 */
	struct usb_device *next;
};

/*
 * Find and return a list of all compatible devices.  Each device is added to
 * the list with its first valid configuration and interface.  If an alternate
 * configuration (config, interface, altsetting...) is desired the specifics
 * can be supplied as programmer parameters.
 *
 * Return:
 *     0: At least one matching device was found.
 *     1: No matching devices were found.
 */
int usb_device_find(struct usb_match const *match, struct usb_device **devices);

/*
 * Display the devices bus and address as well as its product string.  The
 * underlying libusb device is opened if it is not already open.
 *
 * Return:
 *     0: The device information was displayed.
 *     non-zero: There was a failure while displaying the device information.
 */
int usb_device_show(char const *prefix, struct usb_device *device);

/*
 * Open the underlying libusb device, set its config, claim the interface and
 * select the correct alternate interface.
 *
 * Return:
 *     0: The device was successfully claimed.
 *     non-zero: There was a failure while trying to claim the device.
 */
int usb_device_claim(struct usb_device *device);

/*
 * Free a usb_device structure.
 *
 * This ensures that the libusb device is closed and that all allocated
 * handles and descriptors are freed.
 *
 * Return:
 *     The next device in the device list.
 */
struct usb_device *usb_device_free(struct usb_device *device);
