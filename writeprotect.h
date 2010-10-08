/*
 * This file is part of the flashrom project.
 *
 * Copyright (C) 2010 Google Inc.
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

#ifndef __WRITEPROTECT_H__
#define __WRITEPROTECT_H__ 1

struct wp {
	int (*list_ranges)(struct flashchip *flash);
	int (*set_range)(struct flashchip *flash,
			 unsigned int start, unsigned int len);
	int (*enable)(struct flashchip *flash);
	int (*disable)(struct flashchip *flash);
};

/* winbond w25-series */
extern struct wp wp_w25;
extern struct wp wp_wpce775x;

struct w25q_status {
	/* this maps to register layout -- do not change ordering */
	unsigned char busy : 1;
	unsigned char wel : 1;
	unsigned char bp0 : 1;
	unsigned char bp1 : 1;
	unsigned char bp2 : 1;
	unsigned char tb : 1;
	unsigned char sec : 1;
	unsigned char srp0 : 1;
	/* FIXME: what about the second status register? */
//	unsigned char srp1 : 1;
//	unsigned char qe : 1;
} __attribute__ ((packed));

int wp_get_status(const struct flashchip *flash,
                  unsigned int start, unsigned int len,
                  struct w25q_status *status);

#endif				/* !__WRITEPROTECT_H__ */
