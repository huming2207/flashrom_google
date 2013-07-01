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

#ifndef __GEC_H_
#define __GEC_H_

/* FIXME: We should be able to forward declare enum ec_current_image here
 * instead of including gec_ec_commands.h */
#include "gec_ec_commands.h"

struct gec_priv {
	int detected;
	int (*ec_command)(int command, int ver, const void *indata, int insize,
			  void *outdata, int outsize);
};

int gec_probe_size(struct flashchip *flash);
int gec_block_erase(struct flashchip *flash,
                    unsigned int blockaddr, unsigned int len);
int gec_read(struct flashchip *flash, uint8_t *readarr,
             unsigned int blockaddr, unsigned int readcnt);
int gec_write(struct flashchip *flash, uint8_t *buf, unsigned int addr,
                    unsigned int nbytes);

#endif	/* __GEC_H_ */
