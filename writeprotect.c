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

#include <stdlib.h>
#include <string.h>

#include "flash.h"
#include "flashchips.h"
#include "chipdrivers.h"
#include "spi.h"
#include "writeprotect.h"

/* When update flash's status register, it takes few time to erase register.
 * After surveying some flash vendor specs, such as Winbond, MXIC, EON,
 * all of their update time are less than 20ms. After refering the spi25.c,
 * use 100ms delay.
 */
#define WRITE_STATUS_REGISTER_DELAY 100 * 1000  /* unit: us */

/*
 * Mask to extract write-protect enable and range bits
 *   Status register 1:
 *     SRP0:           bit 7
 *     range(BP2-BP0): bit 4-2
 *   Status register 2:
 *     SRP1:           bit 1
 */
#define MASK_WP_AREA (0x9C)
#define MASK_WP2_AREA (0x01)

/*
 * The following procedures rely on look-up tables to match the user-specified
 * range with the chip's supported ranges. This turned out to be the most
 * elegant approach since diferent flash chips use different levels of
 * granularity and methods to determine protected ranges. In other words,
 * be stupid and simple since clever arithmetic will not for many chips.
 */

struct wp_range {
	unsigned int start;	/* starting address */
	unsigned int len;	/* len */
};

enum bit_state {
	OFF	= 0,
	ON	= 1,
	X	= -1	/* don't care. Must be bigger than max # of bp. */
};

struct w25q_range {
	enum bit_state sec;		/* if 1, bp[2:0] describe sectors */
	enum bit_state tb;		/* top/bottom select */
	int bp;				/* block protect bitfield */
	struct wp_range range;
};

struct w25q_range en25f40_ranges[] = {
	{ X, X, 0, {0, 0} },    /* none */
	{ 0, 0, 0x1, {0x000000, 504 * 1024} },
	{ 0, 0, 0x2, {0x000000, 496 * 1024} },
	{ 0, 0, 0x3, {0x000000, 480 * 1024} },
	{ 0, 0, 0x4, {0x000000, 448 * 1024} },
	{ 0, 0, 0x5, {0x000000, 384 * 1024} },
	{ 0, 0, 0x6, {0x000000, 256 * 1024} },
	{ 0, 0, 0x7, {0x000000, 512 * 1024} },
};

struct w25q_range en25q40_ranges[] = {
	{ 0, 0, 0, {0, 0} },    /* none */
	{ 0, 0, 0x1, {0x000000, 504 * 1024} },
	{ 0, 0, 0x2, {0x000000, 496 * 1024} },
	{ 0, 0, 0x3, {0x000000, 480 * 1024} },

	{ 0, 1, 0x0, {0x000000, 448 * 1024} },
	{ 0, 1, 0x1, {0x000000, 384 * 1024} },
	{ 0, 1, 0x2, {0x000000, 256 * 1024} },
	{ 0, 1, 0x3, {0x000000, 512 * 1024} },
};

struct w25q_range en25q80_ranges[] = {
	{ 0, 0, 0, {0, 0} },    /* none */
	{ 0, 0, 0x1, {0x000000, 1016 * 1024} },
	{ 0, 0, 0x2, {0x000000, 1008 * 1024} },
	{ 0, 0, 0x3, {0x000000, 992 * 1024} },
	{ 0, 0, 0x4, {0x000000, 960 * 1024} },
	{ 0, 0, 0x5, {0x000000, 896 * 1024} },
	{ 0, 0, 0x6, {0x000000, 768 * 1024} },
	{ 0, 0, 0x7, {0x000000, 1024 * 1024} },
};

struct w25q_range en25q32_ranges[] = {
	{ 0, 0, 0, {0, 0} },    /* none */
	{ 0, 0, 0x1, {0x000000, 4032 * 1024} },
	{ 0, 0, 0x2, {0x000000, 3968 * 1024} },
	{ 0, 0, 0x3, {0x000000, 3840 * 1024} },
	{ 0, 0, 0x4, {0x000000, 3584 * 1024} },
	{ 0, 0, 0x5, {0x000000, 3072 * 1024} },
	{ 0, 0, 0x6, {0x000000, 2048 * 1024} },
	{ 0, 0, 0x7, {0x000000, 4096 * 1024} },

	{ 0, 1, 0, {0, 0} },    /* none */
	{ 0, 1, 0x1, {0x010000, 4032 * 1024} },
	{ 0, 1, 0x2, {0x020000, 3968 * 1024} },
	{ 0, 1, 0x3, {0x040000, 3840 * 1024} },
	{ 0, 1, 0x4, {0x080000, 3584 * 1024} },
	{ 0, 1, 0x5, {0x100000, 3072 * 1024} },
	{ 0, 1, 0x6, {0x200000, 2048 * 1024} },
	{ 0, 1, 0x7, {0x000000, 4096 * 1024} },
};

struct w25q_range en25q64_ranges[] = {
	{ 0, 0, 0, {0, 0} },    /* none */
	{ 0, 0, 0x1, {0x000000, 8128 * 1024} },
	{ 0, 0, 0x2, {0x000000, 8064 * 1024} },
	{ 0, 0, 0x3, {0x000000, 7936 * 1024} },
	{ 0, 0, 0x4, {0x000000, 7680 * 1024} },
	{ 0, 0, 0x5, {0x000000, 7168 * 1024} },
	{ 0, 0, 0x6, {0x000000, 6144 * 1024} },
	{ 0, 0, 0x7, {0x000000, 8192 * 1024} },

	{ 0, 1, 0, {0, 0} },	/* none */
	{ 0, 1, 0x1, {0x010000, 8128 * 1024} },
	{ 0, 1, 0x2, {0x020000, 8064 * 1024} },
	{ 0, 1, 0x3, {0x040000, 7936 * 1024} },
	{ 0, 1, 0x4, {0x080000, 7680 * 1024} },
	{ 0, 1, 0x5, {0x100000, 7168 * 1024} },
	{ 0, 1, 0x6, {0x200000, 6144 * 1024} },
	{ 0, 1, 0x7, {0x000000, 8192 * 1024} },
};

struct w25q_range en25q128_ranges[] = {
	{ 0, 0, 0, {0, 0} },    /* none */
	{ 0, 0, 0x1, {0x000000, 16320 * 1024} },
	{ 0, 0, 0x2, {0x000000, 16256 * 1024} },
	{ 0, 0, 0x3, {0x000000, 16128 * 1024} },
	{ 0, 0, 0x4, {0x000000, 15872 * 1024} },
	{ 0, 0, 0x5, {0x000000, 15360 * 1024} },
	{ 0, 0, 0x6, {0x000000, 14336 * 1024} },
	{ 0, 0, 0x7, {0x000000, 16384 * 1024} },

	{ 0, 1, 0, {0, 0} },	/* none */
	{ 0, 1, 0x1, {0x010000, 16320 * 1024} },
	{ 0, 1, 0x2, {0x020000, 16256 * 1024} },
	{ 0, 1, 0x3, {0x040000, 16128 * 1024} },
	{ 0, 1, 0x4, {0x080000, 15872 * 1024} },
	{ 0, 1, 0x5, {0x100000, 15360 * 1024} },
	{ 0, 1, 0x6, {0x200000, 14336 * 1024} },
	{ 0, 1, 0x7, {0x000000, 16384 * 1024} },
};

struct w25q_range en25s64_ranges[] = {
	{ 0, 0, 0, {0, 0} },    /* none */
	{ 0, 0, 0x1, {0x000000, 8064 * 1024} },
	{ 0, 0, 0x2, {0x000000, 7936 * 1024} },
	{ 0, 0, 0x3, {0x000000, 7680 * 1024} },
	{ 0, 0, 0x4, {0x000000, 7168 * 1024} },
	{ 0, 0, 0x5, {0x000000, 6144 * 1024} },
	{ 0, 0, 0x6, {0x000000, 4096 * 1024} },
	{ 0, 0, 0x7, {0x000000, 8192 * 1024} },

	{ 0, 1, 0, {0, 0} },	/* none */
	{ 0, 1, 0x1, {0x7e0000, 128 * 1024} },
	{ 0, 1, 0x2, {0x7c0000, 256 * 1024} },
	{ 0, 1, 0x3, {0x780000, 512 * 1024} },
	{ 0, 1, 0x4, {0x700000, 1024 * 1024} },
	{ 0, 1, 0x5, {0x600000, 2048 * 1024} },
	{ 0, 1, 0x6, {0x400000, 4096 * 1024} },
	{ 0, 1, 0x7, {0x000000, 8192 * 1024} },
};

/* mx25l1005 ranges also work for the mx25l1005c */
static struct w25q_range mx25l1005_ranges[] = {
	{ X, X, 0, {0, 0} },	/* none */
	{ X, X, 0x1, {0x010000, 64 * 1024} },
	{ X, X, 0x2, {0x000000, 128 * 1024} },
	{ X, X, 0x3, {0x000000, 128 * 1024} },
};

static struct w25q_range mx25l2005_ranges[] = {
	{ X, X, 0, {0, 0} },	/* none */
	{ X, X, 0x1, {0x030000, 64 * 1024} },
	{ X, X, 0x2, {0x020000, 128 * 1024} },
	{ X, X, 0x3, {0x000000, 256 * 1024} },
};

static struct w25q_range mx25l4005_ranges[] = {
	{ X, X, 0, {0, 0} },	/* none */
	{ X, X, 0x1, {0x070000, 64 * 1 * 1024} },	/* block 7 */
	{ X, X, 0x2, {0x060000, 64 * 2 * 1024} },	/* blocks 6-7 */
	{ X, X, 0x3, {0x040000, 64 * 4 * 1024} },	/* blocks 4-7 */
	{ X, X, 0x4, {0x000000, 512 * 1024} },
	{ X, X, 0x5, {0x000000, 512 * 1024} },
	{ X, X, 0x6, {0x000000, 512 * 1024} },
	{ X, X, 0x7, {0x000000, 512 * 1024} },
};

static struct w25q_range mx25l8005_ranges[] = {
	{ X, X, 0, {0, 0} },	/* none */
	{ X, X, 0x1, {0x0f0000, 64 * 1 * 1024} },	/* block 15 */
	{ X, X, 0x2, {0x0e0000, 64 * 2 * 1024} },	/* blocks 14-15 */
	{ X, X, 0x3, {0x0c0000, 64 * 4 * 1024} },	/* blocks 12-15 */
	{ X, X, 0x4, {0x080000, 64 * 8 * 1024} },	/* blocks 8-15 */
	{ X, X, 0x5, {0x000000, 1024 * 1024} },
	{ X, X, 0x6, {0x000000, 1024 * 1024} },
	{ X, X, 0x7, {0x000000, 1024 * 1024} },
};

#if 0
/* FIXME: mx25l1605 has the same IDs as the mx25l1605d */
static struct w25q_range mx25l1605_ranges[] = {
	{ X, X, 0, {0, 0} },	/* none */
	{ X, X, 0x1, {0x1f0000, 64 * 1024} },	/* block 31 */
	{ X, X, 0x2, {0x1e0000, 128 * 1024} },	/* blocks 30-31 */
	{ X, X, 0x3, {0x1c0000, 256 * 1024} },	/* blocks 28-31 */
	{ X, X, 0x4, {0x180000, 512 * 1024} },	/* blocks 24-31 */
	{ X, X, 0x4, {0x100000, 1024 * 1024} },	/* blocks 16-31 */
	{ X, X, 0x6, {0x000000, 2048 * 1024} },
	{ X, X, 0x7, {0x000000, 2048 * 1024} },
};
#endif

#if 0
/* FIXME: mx25l6405 has the same IDs as the mx25l6405d */
static struct w25q_range mx25l6405_ranges[] = {
	{ X, 0, 0, {0, 0} },	/* none */
	{ X, 0, 0x1, {0x7f0000, 64 * 1 * 1024} },	/* block 127 */
	{ X, 0, 0x2, {0x7e0000, 64 * 2 * 1024} },	/* blocks 126-127 */
	{ X, 0, 0x3, {0x7c0000, 64 * 4 * 1024} },	/* blocks 124-127 */
	{ X, 0, 0x4, {0x780000, 64 * 8 * 1024} },	/* blocks 120-127 */
	{ X, 0, 0x5, {0x700000, 64 * 16 * 1024} },	/* blocks 112-127 */
	{ X, 0, 0x6, {0x600000, 64 * 32 * 1024} },	/* blocks 96-127 */
	{ X, 0, 0x7, {0x400000, 64 * 64 * 1024} },	/* blocks 64-127 */

	{ X, 1, 0x0, {0x000000, 8192 * 1024} },
	{ X, 1, 0x1, {0x000000, 8192 * 1024} },
	{ X, 1, 0x2, {0x000000, 8192 * 1024} },
	{ X, 1, 0x3, {0x000000, 8192 * 1024} },
	{ X, 1, 0x4, {0x000000, 8192 * 1024} },
	{ X, 1, 0x5, {0x000000, 8192 * 1024} },
	{ X, 1, 0x6, {0x000000, 8192 * 1024} },
	{ X, 1, 0x7, {0x000000, 8192 * 1024} },
};
#endif

static struct w25q_range mx25l1605d_ranges[] = {
	{ X, 0, 0, {0, 0} },	/* none */
	{ X, 0, 0x1, {0x1f0000, 64 * 1 * 1024} },	/* block 31 */
	{ X, 0, 0x2, {0x1e0000, 64 * 2 * 1024} },	/* blocks 30-31 */
	{ X, 0, 0x3, {0x1c0000, 64 * 4 * 1024} },	/* blocks 28-31 */
	{ X, 0, 0x4, {0x180000, 64 * 8 * 1024} },	/* blocks 24-31 */
	{ X, 0, 0x5, {0x100000, 64 * 16 * 1024} },	/* blocks 16-31 */
	{ X, 0, 0x6, {0x000000, 64 * 32 * 1024} },	/* blocks 0-31 */
	{ X, 0, 0x7, {0x000000, 64 * 32 * 1024} },	/* blocks 0-31 */

	{ X, 1, 0x0, {0x000000, 2048 * 1024} },
	{ X, 1, 0x1, {0x000000, 2048 * 1024} },
	{ X, 1, 0x2, {0x000000, 64 * 16 * 1024} },	/* blocks 0-15 */
	{ X, 1, 0x3, {0x000000, 64 * 24 * 1024} },	/* blocks 0-23 */
	{ X, 1, 0x4, {0x000000, 64 * 28 * 1024} },	/* blocks 0-27 */
	{ X, 1, 0x5, {0x000000, 64 * 30 * 1024} },	/* blocks 0-29 */
	{ X, 1, 0x6, {0x000000, 64 * 31 * 1024} },	/* blocks 0-30 */
	{ X, 1, 0x7, {0x000000, 64 * 32 * 1024} },	/* blocks 0-31 */
};

/* FIXME: Is there an mx25l3205 (without a trailing letter)? */
static struct w25q_range mx25l3205d_ranges[] = {
	{ X, 0, 0, {0, 0} },	/* none */
	{ X, 0, 0x1, {0x3f0000, 64 * 1024} },
	{ X, 0, 0x2, {0x3e0000, 128 * 1024} },
	{ X, 0, 0x3, {0x3c0000, 256 * 1024} },
	{ X, 0, 0x4, {0x380000, 512 * 1024} },
	{ X, 0, 0x5, {0x300000, 1024 * 1024} },
	{ X, 0, 0x6, {0x200000, 2048 * 1024} },
	{ X, 0, 0x7, {0x000000, 4096 * 1024} },

	{ X, 1, 0x0, {0x000000, 4096 * 1024} },
	{ X, 1, 0x1, {0x000000, 2048 * 1024} },
	{ X, 1, 0x2, {0x000000, 3072 * 1024} },
	{ X, 1, 0x3, {0x000000, 3584 * 1024} },
	{ X, 1, 0x4, {0x000000, 3840 * 1024} },
	{ X, 1, 0x5, {0x000000, 3968 * 1024} },
	{ X, 1, 0x6, {0x000000, 4032 * 1024} },
	{ X, 1, 0x7, {0x000000, 4096 * 1024} },
};

static struct w25q_range mx25u3235e_ranges[] = {
	{ X, 0, 0, {0, 0} },	/* none */
	{ 0, 0, 0x1, {0x3f0000, 64 * 1024} },
	{ 0, 0, 0x2, {0x3e0000, 128 * 1024} },
	{ 0, 0, 0x3, {0x3c0000, 256 * 1024} },
	{ 0, 0, 0x4, {0x380000, 512 * 1024} },
	{ 0, 0, 0x5, {0x300000, 1024 * 1024} },
	{ 0, 0, 0x6, {0x200000, 2048 * 1024} },
	{ 0, 0, 0x7, {0x000000, 4096 * 1024} },

	{ 0, 1, 0x0, {0x000000, 4096 * 1024} },
	{ 0, 1, 0x1, {0x000000, 2048 * 1024} },
	{ 0, 1, 0x2, {0x000000, 3072 * 1024} },
	{ 0, 1, 0x3, {0x000000, 3584 * 1024} },
	{ 0, 1, 0x4, {0x000000, 3840 * 1024} },
	{ 0, 1, 0x5, {0x000000, 3968 * 1024} },
	{ 0, 1, 0x6, {0x000000, 4032 * 1024} },
	{ 0, 1, 0x7, {0x000000, 4096 * 1024} },
};

#if 0
/* FIXME: MX25L6405D has same ID as MX25L6406 */
static struct w25q_range mx25l6405d_ranges[] = {
	{ X, 0, 0, {0, 0} },	/* none */
	{ X, 0, 0x1, {0x7e0000, 2 * 64 * 1024} },	/* blocks 126-127 */
	{ X, 0, 0x2, {0x7c0000, 4 * 64 * 1024} },	/* blocks 124-127 */
	{ X, 0, 0x3, {0x780000, 8 * 64 * 1024} },	/* blocks 120-127 */
	{ X, 0, 0x4, {0x700000, 16 * 64 * 1024} },	/* blocks 112-127 */
	{ X, 0, 0x5, {0x600000, 32 * 64 * 1024} },	/* blocks 96-127 */
	{ X, 0, 0x6, {0x400000, 64 * 64 * 1024} },	/* blocks 64-127 */
	{ X, 0, 0x7, {0x000000, 64 * 128 * 1024} },	/* blocks 0-127 */

	{ X, 1, 0x0, {0x000000, 8192 * 1024} },
	{ X, 1, 0x1, {0x000000, 64 * 64 * 1024} },	/* blocks 0-63 */
	{ X, 1, 0x2, {0x000000, 64 * 96 * 1024} },	/* blocks 0-95 */
	{ X, 1, 0x3, {0x000000, 64 * 112 * 1024} },	/* blocks 0-111 */
	{ X, 1, 0x4, {0x000000, 64 * 120 * 1024} },	/* blocks 0-119 */
	{ X, 1, 0x5, {0x000000, 64 * 124 * 1024} },	/* blocks 0-123 */
	{ X, 1, 0x6, {0x000000, 64 * 126 * 1024} },	/* blocks 0-125 */
	{ X, 1, 0x7, {0x000000, 64 * 128 * 1024} },	/* blocks 0-127 */
};
#endif

/* FIXME: MX25L6406 has same ID as MX25L6405D */
static struct w25q_range mx25l6406e_ranges[] = {
	{ X, 0, 0, {0, 0} },	/* none */
	{ X, 0, 0x1, {0x7e0000, 64 * 2 * 1024} },	/* blocks 126-127 */
	{ X, 0, 0x2, {0x7c0000, 64 * 4 * 1024} },	/* blocks 124-127 */
	{ X, 0, 0x3, {0x7a0000, 64 * 8 * 1024} },	/* blocks 120-127 */
	{ X, 0, 0x4, {0x700000, 64 * 16 * 1024} },	/* blocks 112-127 */
	{ X, 0, 0x5, {0x600000, 64 * 32 * 1024} },	/* blocks 96-127 */
	{ X, 0, 0x6, {0x400000, 64 * 64 * 1024} },	/* blocks 64-127 */
	{ X, 0, 0x7, {0x000000, 64 * 128 * 1024} },	/* all */

	{ X, 1, 0x0, {0x000000, 64 * 128 * 1024} },	/* all */
	{ X, 1, 0x1, {0x000000, 64 * 64 * 1024} },	/* blocks 0-63 */
	{ X, 1, 0x2, {0x000000, 64 * 96 * 1024} },	/* blocks 0-95 */
	{ X, 1, 0x3, {0x000000, 64 * 112 * 1024} },	/* blocks 0-111 */
	{ X, 1, 0x4, {0x000000, 64 * 120 * 1024} },	/* blocks 0-119 */
	{ X, 1, 0x5, {0x000000, 64 * 124 * 1024} },	/* blocks 0-123 */
	{ X, 1, 0x6, {0x000000, 64 * 126 * 1024} },	/* blocks 0-125 */
	{ X, 1, 0x7, {0x000000, 64 * 128 * 1024} },	/* all */
};

static struct w25q_range mx25u6435e_ranges[] = {
	{ X, 0, 0, {0, 0} },	/* none */
	{ 0, 0, 0x1, {0x7f0000,   1 * 64 * 1024} },	/* block 127 */
	{ 0, 0, 0x2, {0x7e0000,   2 * 64 * 1024} },	/* blocks 126-127 */
	{ 0, 0, 0x3, {0x7c0000,   4 * 64 * 1024} },	/* blocks 124-127 */
	{ 0, 0, 0x4, {0x780000,   8 * 64 * 1024} },	/* blocks 120-127 */
	{ 0, 0, 0x5, {0x700000,  16 * 64 * 1024} },	/* blocks 112-127 */
	{ 0, 0, 0x6, {0x600000,  32 * 64 * 1024} },	/* blocks 96-127 */
	{ 0, 0, 0x7, {0x400000,  64 * 64 * 1024} },	/* blocks 64-127 */

	{ 0, 1, 0x0, {0x000000,  64 * 64 * 1024} },	/* blocks 0-63 */
	{ 0, 1, 0x1, {0x000000,  96 * 64 * 1024} },	/* blocks 0-95 */
	{ 0, 1, 0x2, {0x000000, 112 * 64 * 1024} },	/* blocks 0-111 */
	{ 0, 1, 0x3, {0x000000, 120 * 64 * 1024} },	/* blocks 0-119 */
	{ 0, 1, 0x4, {0x000000, 124 * 64 * 1024} },	/* blocks 0-123 */
	{ 0, 1, 0x5, {0x000000, 126 * 64 * 1024} },	/* blocks 0-125 */
	{ 0, 1, 0x6, {0x000000, 127 * 64 * 1024} },	/* blocks 0-126 */
	{ 0, 1, 0x7, {0x000000, 128 * 64 * 1024} },	/* blocks 0-127 */
};

static struct w25q_range n25q064_ranges[] = {
	{ X, 0, 0, {0, 0} },	/* none */

	{ 0, 0, 0x1, {0x7f0000,       64 * 1024} },	/* block 127 */
	{ 0, 0, 0x2, {0x7e0000,   2 * 64 * 1024} },	/* blocks 126-127 */
	{ 0, 0, 0x3, {0x7c0000,   4 * 64 * 1024} },	/* blocks 124-127 */
	{ 0, 0, 0x4, {0x780000,   8 * 64 * 1024} },	/* blocks 120-127 */
	{ 0, 0, 0x5, {0x700000,  16 * 64 * 1024} },	/* blocks 112-127 */
	{ 0, 0, 0x6, {0x600000,  32 * 64 * 1024} },	/* blocks 96-127 */
	{ 0, 0, 0x7, {0x400000,  64 * 64 * 1024} },	/* blocks 64-127 */

	{ 1, 0, 0x1, {0x000000,       64 * 1024} },	/* block 0 */
	{ 1, 0, 0x2, {0x000000,   2 * 64 * 1024} },	/* blocks 0-1 */
	{ 1, 0, 0x3, {0x000000,   4 * 64 * 1024} },	/* blocks 0-3 */
	{ 1, 0, 0x4, {0x000000,   8 * 64 * 1024} },	/* blocks 0-7 */
	{ 1, 0, 0x5, {0x000000,  16 * 64 * 1024} },	/* blocks 0-15 */
	{ 1, 0, 0x6, {0x000000,  32 * 64 * 1024} },	/* blocks 0-31 */
	{ 1, 0, 0x7, {0x000000,  64 * 64 * 1024} },	/* blocks 0-63 */

	{ X, 1, 0x0, {0x000000, 128 * 64 * 1024} },	/* all */
	{ X, 1, 0x1, {0x000000, 128 * 64 * 1024} },	/* all */
	{ X, 1, 0x2, {0x000000, 128 * 64 * 1024} },	/* all */
	{ X, 1, 0x3, {0x000000, 128 * 64 * 1024} },	/* all */
	{ X, 1, 0x4, {0x000000, 128 * 64 * 1024} },	/* all */
	{ X, 1, 0x5, {0x000000, 128 * 64 * 1024} },	/* all */
	{ X, 1, 0x6, {0x000000, 128 * 64 * 1024} },	/* all */
	{ X, 1, 0x7, {0x000000, 128 * 64 * 1024} },	/* all */
};

static struct w25q_range w25q16_ranges[] = {
	{ X, X, 0, {0, 0} },	/* none */
	{ 0, 0, 0x1, {0x1f0000, 64 * 1024} },
	{ 0, 0, 0x2, {0x1e0000, 128 * 1024} },
	{ 0, 0, 0x3, {0x1c0000, 256 * 1024} },
	{ 0, 0, 0x4, {0x180000, 512 * 1024} },
	{ 0, 0, 0x5, {0x100000, 1024 * 1024} },

	{ 0, 1, 0x1, {0x000000, 64 * 1024} },
	{ 0, 1, 0x2, {0x000000, 128 * 1024} },
	{ 0, 1, 0x3, {0x000000, 256 * 1024} },
	{ 0, 1, 0x4, {0x000000, 512 * 1024} },
	{ 0, 1, 0x5, {0x000000, 1024 * 1024} },
	{ X, X, 0x6, {0x000000, 2048 * 1024} },
	{ X, X, 0x7, {0x000000, 2048 * 1024} },

	{ 1, 0, 0x1, {0x1ff000, 4 * 1024} },
	{ 1, 0, 0x2, {0x1fe000, 8 * 1024} },
	{ 1, 0, 0x3, {0x1fc000, 16 * 1024} },
	{ 1, 0, 0x4, {0x1f8000, 32 * 1024} },
	{ 1, 0, 0x5, {0x1f8000, 32 * 1024} }, 

	{ 1, 1, 0x1, {0x000000, 4 * 1024} },
	{ 1, 1, 0x2, {0x000000, 8 * 1024} },
	{ 1, 1, 0x3, {0x000000, 16 * 1024} },
	{ 1, 1, 0x4, {0x000000, 32 * 1024} },	
	{ 1, 1, 0x5, {0x000000, 32 * 1024} },
};

static struct w25q_range w25q32_ranges[] = {
	{ X, X, 0, {0, 0} },	/* none */
	{ 0, 0, 0x1, {0x3f0000, 64 * 1024} },
	{ 0, 0, 0x2, {0x3e0000, 128 * 1024} },
	{ 0, 0, 0x3, {0x3c0000, 256 * 1024} },
	{ 0, 0, 0x4, {0x380000, 512 * 1024} },
	{ 0, 0, 0x5, {0x300000, 1024 * 1024} },
	{ 0, 0, 0x6, {0x200000, 2048 * 1024} },

	{ 0, 1, 0x1, {0x000000, 64 * 1024} },
	{ 0, 1, 0x2, {0x000000, 128 * 1024} },
	{ 0, 1, 0x3, {0x000000, 256 * 1024} },
	{ 0, 1, 0x4, {0x000000, 512 * 1024} },
	{ 0, 1, 0x5, {0x000000, 1024 * 1024} },
	{ 0, 1, 0x6, {0x000000, 2048 * 1024} },
	{ X, X, 0x7, {0x000000, 4096 * 1024} },

	{ 1, 0, 0x1, {0x3ff000, 4 * 1024} },
	{ 1, 0, 0x2, {0x3fe000, 8 * 1024} },
	{ 1, 0, 0x3, {0x3fc000, 16 * 1024} },
	{ 1, 0, 0x4, {0x3f8000, 32 * 1024} },
	{ 1, 0, 0x5, {0x3f8000, 32 * 1024} }, 

	{ 1, 1, 0x1, {0x000000, 4 * 1024} },
	{ 1, 1, 0x2, {0x000000, 8 * 1024} },
	{ 1, 1, 0x3, {0x000000, 16 * 1024} },
	{ 1, 1, 0x4, {0x000000, 32 * 1024} },
	{ 1, 1, 0x5, {0x000000, 32 * 1024} },
};

static struct w25q_range w25q80_ranges[] = {
	{ X, X, 0, {0, 0} },	/* none */
	{ 0, 0, 0x1, {0x0f0000, 64 * 1024} },
	{ 0, 0, 0x2, {0x0e0000, 128 * 1024} },
	{ 0, 0, 0x3, {0x0c0000, 256 * 1024} },
	{ 0, 0, 0x4, {0x080000, 512 * 1024} },

	{ 0, 1, 0x1, {0x000000, 64 * 1024} },
	{ 0, 1, 0x2, {0x000000, 128 * 1024} },
	{ 0, 1, 0x3, {0x000000, 256 * 1024} },
	{ 0, 1, 0x4, {0x000000, 512 * 1024} },
	{ X, X, 0x6, {0x000000, 1024 * 1024} },
	{ X, X, 0x7, {0x000000, 1024 * 1024} },

	{ 1, 0, 0x1, {0x1ff000, 4 * 1024} },
	{ 1, 0, 0x2, {0x1fe000, 8 * 1024} },
	{ 1, 0, 0x3, {0x1fc000, 16 * 1024} },
	{ 1, 0, 0x4, {0x1f8000, 32 * 1024} },
	{ 1, 0, 0x5, {0x1f8000, 32 * 1024} },

	{ 1, 1, 0x1, {0x000000, 4 * 1024} },
	{ 1, 1, 0x2, {0x000000, 8 * 1024} },
	{ 1, 1, 0x3, {0x000000, 16 * 1024} },
	{ 1, 1, 0x4, {0x000000, 32 * 1024} },
	{ 1, 1, 0x5, {0x000000, 32 * 1024} },
};

static struct w25q_range w25q64_ranges[] = {
	{ X, X, 0, {0, 0} },	/* none */

	{ 0, 0, 0x1, {0x7e0000, 128 * 1024} },
	{ 0, 0, 0x2, {0x7c0000, 256 * 1024} },
	{ 0, 0, 0x3, {0x780000, 512 * 1024} },
	{ 0, 0, 0x4, {0x700000, 1024 * 1024} },
	{ 0, 0, 0x5, {0x600000, 2048 * 1024} },
	{ 0, 0, 0x6, {0x400000, 4096 * 1024} },

	{ 0, 1, 0x1, {0x000000, 128 * 1024} },
	{ 0, 1, 0x2, {0x000000, 256 * 1024} },
	{ 0, 1, 0x3, {0x000000, 512 * 1024} },
	{ 0, 1, 0x4, {0x000000, 1024 * 1024} },
	{ 0, 1, 0x5, {0x000000, 2048 * 1024} },
	{ 0, 1, 0x6, {0x000000, 4096 * 1024} },
	{ X, X, 0x7, {0x000000, 8192 * 1024} },

	{ 1, 0, 0x1, {0x7ff000, 4 * 1024} },
	{ 1, 0, 0x2, {0x7fe000, 8 * 1024} },
	{ 1, 0, 0x3, {0x7fc000, 16 * 1024} },
	{ 1, 0, 0x4, {0x7f8000, 32 * 1024} },
	{ 1, 0, 0x5, {0x7f8000, 32 * 1024} },

	{ 1, 1, 0x1, {0x000000, 4 * 1024} },
	{ 1, 1, 0x2, {0x000000, 8 * 1024} },
	{ 1, 1, 0x3, {0x000000, 16 * 1024} },
	{ 1, 1, 0x4, {0x000000, 32 * 1024} },
	{ 1, 1, 0x5, {0x000000, 32 * 1024} },
};

struct w25q_range w25x10_ranges[] = {
	{ X, X, 0, {0, 0} },    /* none */
	{ 0, 0, 0x1, {0x010000, 64 * 1024} },
	{ 0, 1, 0x1, {0x000000, 64 * 1024} },
	{ X, X, 0x2, {0x000000, 128 * 1024} },
	{ X, X, 0x3, {0x000000, 128 * 1024} },
};

struct w25q_range w25x20_ranges[] = {
	{ X, X, 0, {0, 0} },    /* none */
	{ 0, 0, 0x1, {0x030000, 64 * 1024} },
	{ 0, 0, 0x2, {0x020000, 128 * 1024} },
	{ 0, 1, 0x1, {0x000000, 64 * 1024} },
	{ 0, 1, 0x2, {0x000000, 128 * 1024} },
	{ 0, X, 0x3, {0x000000, 256 * 1024} },
};

struct w25q_range w25x40_ranges[] = {
	{ X, X, 0, {0, 0} },	/* none */
	{ 0, 0, 0x1, {0x070000, 64 * 1024} },
	{ 0, 0, 0x2, {0x060000, 128 * 1024} },
	{ 0, 0, 0x3, {0x040000, 256 * 1024} },
	{ 0, 1, 0x1, {0x000000, 64 * 1024} },
	{ 0, 1, 0x2, {0x000000, 128 * 1024} },
	{ 0, 1, 0x3, {0x000000, 256 * 1024} },
	{ 0, X, 0x4, {0x000000, 512 * 1024} },
};

struct w25q_range w25x80_ranges[] = {
	{ X, X, 0, {0, 0} },    /* none */
	{ 0, 0, 0x1, {0x0F0000, 64 * 1024} },
	{ 0, 0, 0x2, {0x0E0000, 128 * 1024} },
	{ 0, 0, 0x3, {0x0C0000, 256 * 1024} },
	{ 0, 0, 0x4, {0x080000, 512 * 1024} },
	{ 0, 1, 0x1, {0x000000, 64 * 1024} },
	{ 0, 1, 0x2, {0x000000, 128 * 1024} },
	{ 0, 1, 0x3, {0x000000, 256 * 1024} },
	{ 0, 1, 0x4, {0x000000, 512 * 1024} },
	{ 0, X, 0x5, {0x000000, 1024 * 1024} },
	{ 0, X, 0x6, {0x000000, 1024 * 1024} },
	{ 0, X, 0x7, {0x000000, 1024 * 1024} },
};

static struct w25q_range gd25q64_ranges[] = {
	{ X, X, 0, {0, 0} },	/* none */
	{ 0, 0, 0x1, {0x7e0000, 128 * 1024} },
	{ 0, 0, 0x2, {0x7c0000, 256 * 1024} },
	{ 0, 0, 0x3, {0x780000, 512 * 1024} },
	{ 0, 0, 0x4, {0x700000, 1024 * 1024} },
	{ 0, 0, 0x5, {0x600000, 2048 * 1024} },
	{ 0, 0, 0x6, {0x400000, 4096 * 1024} },

	{ 0, 1, 0x1, {0x000000, 128 * 1024} },
	{ 0, 1, 0x2, {0x000000, 256 * 1024} },
	{ 0, 1, 0x3, {0x000000, 512 * 1024} },
	{ 0, 1, 0x4, {0x000000, 1024 * 1024} },
	{ 0, 1, 0x5, {0x000000, 2048 * 1024} },
	{ 0, 1, 0x6, {0x000000, 4096 * 1024} },
	{ X, X, 0x7, {0x000000, 8192 * 1024} },

	{ 1, 0, 0x1, {0x7ff000, 4 * 1024} },
	{ 1, 0, 0x2, {0x7fe000, 8 * 1024} },
	{ 1, 0, 0x3, {0x7fc000, 16 * 1024} },
	{ 1, 0, 0x4, {0x7f8000, 32 * 1024} },
	{ 1, 0, 0x5, {0x7f8000, 32 * 1024} },
	{ 1, 0, 0x6, {0x7f8000, 32 * 1024} },

	{ 1, 1, 0x1, {0x000000, 4 * 1024} },
	{ 1, 1, 0x2, {0x000000, 8 * 1024} },
	{ 1, 1, 0x3, {0x000000, 16 * 1024} },
	{ 1, 1, 0x4, {0x000000, 32 * 1024} },
	{ 1, 1, 0x5, {0x000000, 32 * 1024} },
	{ 1, 1, 0x6, {0x000000, 32 * 1024} },
};

static struct w25q_range a25l040_ranges[] = {
	{ X, X, 0x0, {0, 0} },	/* none */
	{ X, X, 0x1, {0x70000, 64 * 1024} },
	{ X, X, 0x2, {0x60000, 128 * 1024} },
	{ X, X, 0x3, {0x40000, 256 * 1024} },
	{ X, X, 0x4, {0x00000, 512 * 1024} },
	{ X, X, 0x5, {0x00000, 512 * 1024} },
	{ X, X, 0x6, {0x00000, 512 * 1024} },
	{ X, X, 0x7, {0x00000, 512 * 1024} },
};

/* Given a flash chip, this function returns its range table. */
static int w25_range_table(const struct flashchip *flash,
                           struct w25q_range **w25q_ranges,
                           int *num_entries)
{
	*w25q_ranges = 0;
	*num_entries = 0;

	switch (flash->manufacture_id) {
	case WINBOND_NEX_ID:
		switch(flash->model_id) {
		case WINBOND_NEX_W25X10:
			*w25q_ranges = w25x10_ranges;
			*num_entries = ARRAY_SIZE(w25x10_ranges);
			break;
		case WINBOND_NEX_W25X20:
			*w25q_ranges = w25x20_ranges;
			*num_entries = ARRAY_SIZE(w25x20_ranges);
			break;
		case WINBOND_NEX_W25X40:
			*w25q_ranges = w25x40_ranges;
			*num_entries = ARRAY_SIZE(w25x40_ranges);
			break;
		case WINBOND_NEX_W25X80:
			*w25q_ranges = w25x80_ranges;
			*num_entries = ARRAY_SIZE(w25x80_ranges);
			break;
		case WINBOND_NEX_W25Q80:
			*w25q_ranges = w25q80_ranges;
			*num_entries = ARRAY_SIZE(w25q80_ranges);
			break;
		case WINBOND_NEX_W25Q16:
			*w25q_ranges = w25q16_ranges;
			*num_entries = ARRAY_SIZE(w25q16_ranges);
			break;
		case WINBOND_NEX_W25Q32:
		case WINBOND_NEX_W25Q32DW:
			*w25q_ranges = w25q32_ranges;
			*num_entries = ARRAY_SIZE(w25q32_ranges);
			break;
		case WINBOND_NEX_W25Q64:
                case WINBOND_NEX_W25Q64DW:
			*w25q_ranges = w25q64_ranges;
			*num_entries = ARRAY_SIZE(w25q64_ranges);
			break;
		default:
			msg_cerr("%s() %d: WINBOND flash chip mismatch (0x%04x)"
			         ", aborting\n", __func__, __LINE__,
			         flash->model_id);
			return -1;
		}
		break;
	case EON_ID_NOPREFIX:
		switch (flash->model_id) {
		case EON_EN25F40:
			*w25q_ranges = en25f40_ranges;
			*num_entries = ARRAY_SIZE(en25f40_ranges);
			break;
		case EON_EN25Q40:
			*w25q_ranges = en25q40_ranges;
			*num_entries = ARRAY_SIZE(en25q40_ranges);
			break;
		case EON_EN25Q80:
			*w25q_ranges = en25q80_ranges;
			*num_entries = ARRAY_SIZE(en25q80_ranges);
			break;
		case EON_EN25Q32:
			*w25q_ranges = en25q32_ranges;
			*num_entries = ARRAY_SIZE(en25q32_ranges);
			break;
		case EON_EN25Q64:
			*w25q_ranges = en25q64_ranges;
			*num_entries = ARRAY_SIZE(en25q64_ranges);
			break;
		case EON_EN25Q128:
			*w25q_ranges = en25q128_ranges;
			*num_entries = ARRAY_SIZE(en25q128_ranges);
			break;
		case EON_EN25S64:
			*w25q_ranges = en25s64_ranges;
			*num_entries = ARRAY_SIZE(en25s64_ranges);
			break;
		default:
			msg_cerr("%s():%d: EON flash chip mismatch (0x%04x)"
			         ", aborting\n", __func__, __LINE__,
				 flash->model_id);
			return -1;
		}
		break;
	case MACRONIX_ID:
		switch (flash->model_id) {
		case MACRONIX_MX25L1005:
			*w25q_ranges = mx25l1005_ranges;
			*num_entries = ARRAY_SIZE(mx25l1005_ranges);
			break;
		case MACRONIX_MX25L2005:
			*w25q_ranges = mx25l2005_ranges;
			*num_entries = ARRAY_SIZE(mx25l2005_ranges);
			break;
		case MACRONIX_MX25L4005:
			*w25q_ranges = mx25l4005_ranges;
			*num_entries = ARRAY_SIZE(mx25l4005_ranges);
			break;
		case MACRONIX_MX25L8005:
			*w25q_ranges = mx25l8005_ranges;
			*num_entries = ARRAY_SIZE(mx25l8005_ranges);
			break;
		case MACRONIX_MX25L1605:
			/* FIXME: MX25L1605 and MX25L1605D have different write
			 * protection capabilities, but share IDs */
			*w25q_ranges = mx25l1605d_ranges;
			*num_entries = ARRAY_SIZE(mx25l1605d_ranges);
			break;
		case MACRONIX_MX25L3205:
			*w25q_ranges = mx25l3205d_ranges;
			*num_entries = ARRAY_SIZE(mx25l3205d_ranges);
			break;
		case MACRONIX_MX25U3235E:
			*w25q_ranges = mx25u3235e_ranges;
			*num_entries = ARRAY_SIZE(mx25u3235e_ranges);
			break;
		case MACRONIX_MX25L6405:
			/* FIXME: MX25L64* chips have mixed capabilities and
			   share IDs */
			*w25q_ranges = mx25l6406e_ranges;
			*num_entries = ARRAY_SIZE(mx25l6406e_ranges);
			break;
		case MACRONIX_MX25U6435E:
			*w25q_ranges = mx25u6435e_ranges;
			*num_entries = ARRAY_SIZE(mx25u6435e_ranges);
			break;
		default:
			msg_cerr("%s():%d: MXIC flash chip mismatch (0x%04x)"
			         ", aborting\n", __func__, __LINE__,
			         flash->model_id);
			return -1;
		}
		break;
	case ST_ID:
		switch(flash->model_id) {
		case ST_N25Q064__1E:
		case ST_N25Q064__3E:
			*w25q_ranges = n25q064_ranges;
			*num_entries = ARRAY_SIZE(n25q064_ranges);
			break;
		default:
			msg_cerr("%s() %d: Micron flash chip mismatch"
				 " (0x%04x), aborting\n", __func__, __LINE__,
				 flash->model_id);
			return -1;
		}
		break;
	case GIGADEVICE_ID:
		switch(flash->model_id) {
		case GIGADEVICE_GD25LQ32:
			*w25q_ranges = w25q32_ranges;
			*num_entries = ARRAY_SIZE(w25q32_ranges);
			break;
		case GIGADEVICE_GD25Q64:
			*w25q_ranges = gd25q64_ranges;
			*num_entries = ARRAY_SIZE(gd25q64_ranges);
			break;
		/* TODO(shawnn): add support for other GD parts */
		default:
			msg_cerr("%s() %d: GigaDevice flash chip mismatch"
				 " (0x%04x), aborting\n", __func__, __LINE__,
				 flash->model_id);
			return -1;
		}
		break;
	case AMIC_ID_NOPREFIX:
		switch(flash->model_id) {
		case AMIC_A25L040:
			*w25q_ranges = a25l040_ranges;
			*num_entries = ARRAY_SIZE(a25l040_ranges);
			break;
		default:
			msg_cerr("%s() %d: AMIC flash chip mismatch"
				 " (0x%04x), aborting\n", __func__, __LINE__,
				 flash->model_id);
			return -1;
		}
		break;
	default:
		msg_cerr("%s: flash vendor (0x%x) not found, aborting\n",
		         __func__, flash->manufacture_id);
		return -1;
	}

	return 0;
}

int w25_range_to_status(const struct flashchip *flash,
                        unsigned int start, unsigned int len,
                        struct w25q_status *status)
{
	struct w25q_range *w25q_ranges;
	int i, range_found = 0;
	int num_entries;

	if (w25_range_table(flash, &w25q_ranges, &num_entries)) return -1;
	for (i = 0; i < num_entries; i++) {
		struct wp_range *r = &w25q_ranges[i].range;

		msg_cspew("comparing range 0x%x 0x%x / 0x%x 0x%x\n",
			  start, len, r->start, r->len);
		if ((start == r->start) && (len == r->len)) {
			status->bp0 = w25q_ranges[i].bp & 1;
			status->bp1 = w25q_ranges[i].bp >> 1;
			status->bp2 = w25q_ranges[i].bp >> 2;
			status->tb = w25q_ranges[i].tb;
			status->sec = w25q_ranges[i].sec;

			range_found = 1;
			break;
		}
	}

	if (!range_found) {
		msg_cerr("matching range not found\n");
		return -1;
	}
	return 0;
}

int w25_status_to_range(const struct flashchip *flash,
                        const struct w25q_status *status,
                        unsigned int *start, unsigned int *len)
{
	struct w25q_range *w25q_ranges;
	int i, status_found = 0;
	int num_entries;

	if (w25_range_table(flash, &w25q_ranges, &num_entries)) return -1;
	for (i = 0; i < num_entries; i++) {
		int bp;
		int table_bp, table_tb, table_sec;

		bp = status->bp0 | (status->bp1 << 1) | (status->bp2 << 2);
		msg_cspew("comparing  0x%x 0x%x / 0x%x 0x%x / 0x%x 0x%x\n",
		          bp, w25q_ranges[i].bp,
		          status->tb, w25q_ranges[i].tb,
		          status->sec, w25q_ranges[i].sec);
		table_bp = w25q_ranges[i].bp;
		table_tb = w25q_ranges[i].tb;
		table_sec = w25q_ranges[i].sec;
		if ((bp == table_bp || table_bp == X) &&
		    (status->tb == table_tb || table_tb == X) &&
		    (status->sec == table_sec || table_sec == X)) {
			*start = w25q_ranges[i].range.start;
			*len = w25q_ranges[i].range.len;

			status_found = 1;
			break;
		}
	}

	if (!status_found) {
		msg_cerr("matching status not found\n");
		return -1;
	}
	return 0;
}

/* Since most chips we use must be WREN-ed before WRSR,
 * we copy a write status function here before we have a good solution. */
static int spi_write_status_register_WREN(int status)
{
	int result;
	struct spi_command cmds[] = {
	{
	/* FIXME: WRSR requires either EWSR or WREN depending on chip type. */
		.writecnt       = JEDEC_WREN_OUTSIZE,
		.writearr       = (const unsigned char[]){ JEDEC_WREN },
		.readcnt        = 0,
		.readarr        = NULL,
	}, {
		.writecnt       = JEDEC_WRSR_OUTSIZE,
		.writearr       = (const unsigned char[]){ JEDEC_WRSR, (unsigned char) status },
		.readcnt        = 0,
		.readarr        = NULL,
	}, {
		.writecnt       = 0,
		.writearr       = NULL,
		.readcnt        = 0,
		.readarr        = NULL,
	}};

	result = spi_send_multicommand(cmds);
	if (result) {
	        msg_cerr("%s failed during command execution\n",
	                __func__);
	}

	/* WRSR performs a self-timed erase before the changes take effect. */
	programmer_delay(WRITE_STATUS_REGISTER_DELAY);

	return result;
}

/* Given a [start, len], this function calls w25_range_to_status() to convert
 * it to flash-chip-specific range bits, then sets into status register.
 */
static int w25_set_range(const struct flashchip *flash,
                         unsigned int start, unsigned int len)
{
	struct w25q_status status;
	int tmp = 0;
	int expected = 0;

	memset(&status, 0, sizeof(status));
	tmp = spi_read_status_register();
	memcpy(&status, &tmp, 1);
	msg_cdbg("%s: old status: 0x%02x\n", __func__, tmp);

	if (w25_range_to_status(flash, start, len, &status)) return -1;

	msg_cdbg("status.busy: %x\n", status.busy);
	msg_cdbg("status.wel: %x\n", status.wel);
	msg_cdbg("status.bp0: %x\n", status.bp0);
	msg_cdbg("status.bp1: %x\n", status.bp1);
	msg_cdbg("status.bp2: %x\n", status.bp2);
	msg_cdbg("status.tb: %x\n", status.tb);
	msg_cdbg("status.sec: %x\n", status.sec);
	msg_cdbg("status.srp0: %x\n", status.srp0);

	memcpy(&expected, &status, sizeof(status));
	spi_write_status_register_WREN(expected);

	tmp = spi_read_status_register();
	msg_cdbg("%s: new status: 0x%02x\n", __func__, tmp);
	if ((tmp & MASK_WP_AREA) == (expected & MASK_WP_AREA)) {
		return 0;
	} else {
		msg_cerr("expected=0x%02x, but actual=0x%02x.\n",
		          expected, tmp);
		return 1;
	}
}

/* Print out the current status register value with human-readable text. */
static int w25_wp_status(const struct flashchip *flash)
{
	struct w25q_status status;
	int tmp;
	unsigned int start, len;
	int ret = 0;

	memset(&status, 0, sizeof(status));
	tmp = spi_read_status_register();
	memcpy(&status, &tmp, 1);
	msg_cinfo("WP: status: 0x%02x\n", tmp);
	msg_cinfo("WP: status.srp0: %x\n", status.srp0);
	msg_cinfo("WP: write protect is %s.\n",
	          status.srp0 ? "enabled" : "disabled");

	msg_cinfo("WP: write protect range: ");
	if (w25_status_to_range(flash, &status, &start, &len)) {
		msg_cinfo("(cannot resolve the range)\n");
		ret = -1;
	} else {
		msg_cinfo("start=0x%08x, len=0x%08x\n", start, len);
	}

	return ret;
}

/* Set/clear the SRP0 bit in the status register. */
static int w25_set_srp0(const struct flashchip *flash, int enable)
{
	struct w25q_status status;
	int tmp = 0;
	int expected = 0;

	memset(&status, 0, sizeof(status));
	tmp = spi_read_status_register();
	/* FIXME: this is NOT endian-free copy. */
	memcpy(&status, &tmp, 1);
	msg_cdbg("%s: old status: 0x%02x\n", __func__, tmp);

	status.srp0 = enable ? 1 : 0;
	memcpy(&expected, &status, sizeof(status));
	spi_write_status_register_WREN(expected);

	tmp = spi_read_status_register();
	msg_cdbg("%s: new status: 0x%02x\n", __func__, tmp);
	if ((tmp & MASK_WP_AREA) != (expected & MASK_WP_AREA))
		return 1;

	return 0;
}

static int w25_enable_writeprotect(const struct flashchip *flash,
		enum wp_mode wp_mode)
{
	int ret;

	switch (wp_mode) {
	case WP_MODE_HARDWARE:
		ret = w25_set_srp0(flash, 1);
		break;
	default:
		msg_cerr("%s(): unsupported write-protect mode\n", __func__);
		return 1;
	}

	if (ret)
		msg_cerr("%s(): error=%d.\n", __func__, ret);
	return ret;
}

static int w25_disable_writeprotect(const struct flashchip *flash)
{
	int ret;

	ret = w25_set_srp0(flash, 0);
	if (ret)
		msg_cerr("%s(): error=%d.\n", __func__, ret);
	return ret;
}

static int w25_list_ranges(const struct flashchip *flash)
{
	struct w25q_range *w25q_ranges;
	int i, num_entries;

	if (w25_range_table(flash, &w25q_ranges, &num_entries)) return -1;
	for (i = 0; i < num_entries; i++) {
		msg_cinfo("start: 0x%06x, length: 0x%06x\n",
		          w25q_ranges[i].range.start,
		          w25q_ranges[i].range.len);
	}

	return 0;
}

/* FIXME: Move to spi25.c if it's a JEDEC standard opcode */
uint8_t w25q_read_status_register_2(void)
{
	static const unsigned char cmd[JEDEC_RDSR_OUTSIZE] = { 0x35 };
	unsigned char readarr[2];
	int ret;

	/* Read Status Register */
	ret = spi_send_command(sizeof(cmd), sizeof(readarr), cmd, readarr);
	if (ret) {
		/*
		 * FIXME: make this a benign failure for now in case we are
		 * unable to execute the opcode
		 */
		msg_cdbg("RDSR2 failed!\n");
		readarr[0] = 0x00;
	}

	return readarr[0];
}

static int w25q_wp_status(const struct flashchip *flash)
{
	struct w25q_status sr1;
	struct w25q_status_2 sr2;
	uint8_t tmp[2];
	unsigned int start, len;
	int ret = 0;

	memset(&sr1, 0, sizeof(sr1));
	tmp[0] = spi_read_status_register();
	memcpy(&sr1, &tmp[0], 1);

	memset(&sr2, 0, sizeof(sr2));
	tmp[1] = w25q_read_status_register_2();
	memcpy(&sr2, &tmp[1], 1);

	msg_cinfo("WP: status: 0x%02x%02x\n", tmp[1], tmp[0]);
	msg_cinfo("WP: status.srp0: %x\n", sr1.srp0);
	msg_cinfo("WP: status.srp1: %x\n", sr2.srp1);
	msg_cinfo("WP: write protect is %s.\n",
	          (sr1.srp0 || sr2.srp1) ? "enabled" : "disabled");

	msg_cinfo("WP: write protect range: ");
	if (w25_status_to_range(flash, &sr1, &start, &len)) {
		msg_cinfo("(cannot resolve the range)\n");
		ret = -1;
	} else {
		msg_cinfo("start=0x%08x, len=0x%08x\n", start, len);
	}

	return ret;
}

/*
 * W25Q adds an optional byte to the standard WRSR opcode. If /CS is
 * de-asserted after the first byte, then it acts like a JEDEC-standard
 * WRSR command. if /CS is asserted, then the next data byte is written
 * into status register 2.
 */
#define W25Q_WRSR_OUTSIZE	0x03
static int w25q_write_status_register_WREN(uint8_t s1, uint8_t s2)
{
	int result;
	struct spi_command cmds[] = {
	{
	/* FIXME: WRSR requires either EWSR or WREN depending on chip type. */
		.writecnt       = JEDEC_WREN_OUTSIZE,
		.writearr       = (const unsigned char[]){ JEDEC_WREN },
		.readcnt        = 0,
		.readarr        = NULL,
	}, {
		.writecnt       = W25Q_WRSR_OUTSIZE,
		.writearr       = (const unsigned char[]){ JEDEC_WRSR, s1, s2 },
		.readcnt        = 0,
		.readarr        = NULL,
	}, {
		.writecnt       = 0,
		.writearr       = NULL,
		.readcnt        = 0,
		.readarr        = NULL,
	}};

	result = spi_send_multicommand(cmds);
	if (result) {
	        msg_cerr("%s failed during command execution\n",
	                __func__);
	}

	/* WRSR performs a self-timed erase before the changes take effect. */
	programmer_delay(WRITE_STATUS_REGISTER_DELAY);

	return result;
}

/*
 * Set/clear the SRP1 bit in status register 2.
 * FIXME: make this more generic if other chips use the same SR2 layout
 */
static int w25q_set_srp1(const struct flashchip *flash, int enable)
{
	struct w25q_status sr1;
	struct w25q_status_2 sr2;
	uint8_t tmp, expected;

	tmp = spi_read_status_register();
	memcpy(&sr1, &tmp, 1);
	tmp = w25q_read_status_register_2();
	memcpy(&sr2, &tmp, 1);

	msg_cdbg("%s: old status 2: 0x%02x\n", __func__, tmp);

	sr2.srp1 = enable ? 1 : 0;

	memcpy(&expected, &sr2, 1);
	w25q_write_status_register_WREN(*((uint8_t *)&sr1), *((uint8_t *)&sr2));

	tmp = w25q_read_status_register_2();
	msg_cdbg("%s: new status 2: 0x%02x\n", __func__, tmp);
	if ((tmp & MASK_WP2_AREA) != (expected & MASK_WP2_AREA))
		return 1;

	return 0;
}

enum wp_mode get_wp_mode(const char *mode_str)
{
	enum wp_mode wp_mode = WP_MODE_UNKNOWN;

	if (!strcasecmp(mode_str, "hardware"))
		wp_mode = WP_MODE_HARDWARE;
	else if (!strcasecmp(mode_str, "power_cycle"))
		wp_mode = WP_MODE_POWER_CYCLE;
	else if (!strcasecmp(mode_str, "permanent"))
		wp_mode = WP_MODE_PERMANENT;

	return wp_mode;
}

static int w25q_disable_writeprotect(const struct flashchip *flash,
		enum wp_mode wp_mode)
{
	int ret = 1;
	struct w25q_status_2 sr2;
	uint8_t tmp;

	switch (wp_mode) {
	case WP_MODE_HARDWARE:
		ret = w25_set_srp0(flash, 0);
		break;
	case WP_MODE_POWER_CYCLE:
		tmp = w25q_read_status_register_2();
		memcpy(&sr2, &tmp, 1);
		if (sr2.srp1) {
			msg_cerr("%s(): must disconnect power to disable "
					"write-protection\n", __func__);
		} else {
			ret = 0;
		}
		break;
	case WP_MODE_PERMANENT:
		msg_cerr("%s(): cannot disable permanent write-protection\n",
				__func__);
		break;
	default:
		msg_cerr("%s(): invalid mode specified\n", __func__);
		break;
	}

	if (ret)
		msg_cerr("%s(): error=%d.\n", __func__, ret);
	return ret;
}

static int w25q_disable_writeprotect_default(const struct flashchip *flash)
{
	return w25q_disable_writeprotect(flash, WP_MODE_HARDWARE);
}

static int w25q_enable_writeprotect(const struct flashchip *flash,
		enum wp_mode wp_mode)
{
	int ret = 1;
	struct w25q_status sr1;
	struct w25q_status_2 sr2;
	uint8_t tmp;

	switch (wp_mode) {
	case WP_MODE_HARDWARE:
		if (w25q_disable_writeprotect(flash, WP_MODE_POWER_CYCLE)) {
			msg_cerr("%s(): cannot disable power cycle WP mode\n",
					__func__);
			break;
		}

		tmp = spi_read_status_register();
		memcpy(&sr1, &tmp, 1);
		if (sr1.srp0)
			ret = 0;
		else
			ret = w25_set_srp0(flash, 1);

		break;
	case WP_MODE_POWER_CYCLE:
		if (w25q_disable_writeprotect(flash, WP_MODE_HARDWARE)) {
			msg_cerr("%s(): cannot disable hardware WP mode\n",
					__func__);
			break;
		}

		tmp = w25q_read_status_register_2();
		memcpy(&sr2, &tmp, 1);
		if (sr2.srp1)
			ret = 0;
		else
			ret = w25q_set_srp1(flash, 1);

		break;
	case WP_MODE_PERMANENT:
		tmp = spi_read_status_register();
		memcpy(&sr1, &tmp, 1);
		if (sr1.srp0 == 0) {
			ret = w25_set_srp0(flash, 1);
			if (ret) {
				msg_perr("%s(): cannot enable SRP0 for "
						"permanent WP\n", __func__);
				break;
			}
		}

		tmp = w25q_read_status_register_2();
		memcpy(&sr2, &tmp, 1);
		if (sr2.srp1 == 0) {
			ret = w25q_set_srp1(flash, 1);
			if (ret) {
				msg_perr("%s(): cannot enable SRP1 for "
						"permanent WP\n", __func__);
				break;
			}
		}

		break;
	default:
		msg_perr("%s(): invalid mode %d\n", __func__, wp_mode);
		break;
	}

	if (ret)
		msg_cerr("%s(): error=%d.\n", __func__, ret);
	return ret;
}

/* W25P, W25X, and many flash chips from various vendors */
struct wp wp_w25 = {
	.list_ranges	= w25_list_ranges,
	.set_range	= w25_set_range,
	.enable		= w25_enable_writeprotect,
	.disable	= w25_disable_writeprotect,
	.wp_status	= w25_wp_status,

};

/* W25Q series has features such as a second status register and SFDP */
struct wp wp_w25q = {
	.list_ranges	= w25_list_ranges,
	.set_range	= w25_set_range,
	.enable		= w25q_enable_writeprotect,
	/*
	 * By default, disable hardware write-protection. We may change
	 * this later if we want to add fine-grained write-protect disable
	 * as a command-line option.
	 */
	.disable	= w25q_disable_writeprotect_default,
	.wp_status	= w25q_wp_status,
};
