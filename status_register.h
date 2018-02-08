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

enum wp_mode {
	WP_MODE_UNKNOWN = -1,
	WP_MODE_HARDWARE,	/* hardware WP pin determines status */
	WP_MODE_POWER_CYCLE,	/* WP active until power off/on cycle */
	WP_MODE_PERMANENT,	/* status register permanently locked,
				   WP permanently enabled */
};

struct wp {
	int (*list_ranges)(const struct flashctx *flash);
	int (*set_range)(const struct flashctx *flash,
			 unsigned int start, unsigned int len);
	int (*enable)(const struct flashctx *flash, enum wp_mode mode);
	int (*disable)(const struct flashctx *flash);
	int (*wp_status)(const struct flashctx *flash);
};


/* winbond w25-series */
extern struct wp wp_w25;	/* older winbond chips (w25p, w25x, etc) */
extern struct wp wp_w25q;
extern struct wp wp_w25r;

extern struct wp wp_generic;
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
} __attribute__ ((packed));

struct w25q_status_2 {
	unsigned char srp1 : 1;
	unsigned char qe : 1;
	unsigned char rsvd : 1;
	unsigned char lb1 : 1;
	unsigned char lb2 : 1;
	unsigned char lb3 : 1;
	unsigned char cmp : 1;
	unsigned char sus : 1;
} __attribute__ ((packed));

// Ref: https://www.winbond.com/resource-files/w25q256fv_revg1_120214_qpi_website_rev_g.pdf
// Figure 4c, Page 18
struct w25q_status_3 {
	unsigned char ads : 1;
	unsigned char adp : 1;
	unsigned char wps : 1;
	unsigned char rsvd : 2;
	unsigned char drv0 : 1;
	unsigned char drv1 : 1;
	unsigned char hold : 1;
} __attribute__ ((packed));

int w25_range_to_status(const struct flashctx *flash,
                        unsigned int start, unsigned int len,
                        struct w25q_status *status);
int w25_status_to_range(const struct flashctx *flash,
                        const struct w25q_status *status,
                        unsigned int *start, unsigned int *len);
enum wp_mode get_wp_mode(const char *mode_str);
int w25q_get_adp_status(const struct flashctx *flash);
int w25q_set_adp_status(const struct flashctx *flash, int enable);

/*
 * Generic write-protect stuff
 */

/* For now, use one struct for all modifier bits on all devices. We can get
 * more specific to certain chips if needed later on. */
struct generic_modifier_bits {
		int tb;			/* value of top/bottom bit */
};

#endif				/* !__WRITEPROTECT_H__ */
