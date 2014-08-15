/*
 * This file is part of the flashrom project.
 *
 * Copyright 2013 Google Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
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

#ifndef __CROS_EC_H_
#define __CROS_EC_H_

/* FIXME: We should be able to forward declare enum ec_current_image here
 * instead of including cros_ec_ec_commands.h */
#include "cros_ec_commands.h"

struct cros_ec_priv {
	int detected;
	enum ec_current_image current_image;
	struct ec_response_flash_region_info *region;
	int (*ec_command)(int command, int ver, const void *indata, int insize,
			  void *outdata, int outsize);

	/*
	 * Starting in version 3.0, the EC can act as a bridge to pass along
	 * messages from the host to up to 3 attached devices (device 0 is the
	 * EC itself) which also use the CrOS EC protocol. The devices can be
	 * attached to any bus on the EC and the EC will abstract bus details.
	 * Bits 14 and 15 in the command word indicate the device index.
	 */
	unsigned int dev_index;
};

int cros_ec_test(struct cros_ec_priv *priv);
int cros_ec_parse_param(struct cros_ec_priv *priv);
int cros_ec_probe_size(struct flashchip *flash);
int cros_ec_block_erase(struct flashchip *flash,
                    unsigned int blockaddr, unsigned int len);
int cros_ec_read(struct flashchip *flash, uint8_t *readarr,
             unsigned int blockaddr, unsigned int readcnt);
int cros_ec_write(struct flashchip *flash, uint8_t *buf, unsigned int addr,
                    unsigned int nbytes);

#endif	/* __CROS_EC_H_ */
