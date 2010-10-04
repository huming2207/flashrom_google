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
	X	= 0	/* don't care */
};

struct w25q_range {
	enum bit_state sec;		/* if 1, bp[2:0] describe sectors */
	enum bit_state tb;		/* top/bottom select */
	unsigned short int bp : 3;	/* block protect bitfield */
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

int wp_get_status(const struct flashchip *flash,
                  unsigned int start, unsigned int len,
                  struct w25q_status *status)
{
	struct w25q_range *w25q_ranges;
	int i, num_entries = 0;
	int range_found = 0;

	switch (flash->manufacture_id) {
	case WINBOND_NEX_ID:
		switch(flash->model_id) {
		case W_25X10:
			w25q_ranges = w25x10_ranges;
			num_entries = ARRAY_SIZE(w25x10_ranges);
			break;
		case W_25X20:
			w25q_ranges = w25x20_ranges;
			num_entries = ARRAY_SIZE(w25x20_ranges);
			break;
		case W_25X40:
			w25q_ranges = w25x40_ranges;
			num_entries = ARRAY_SIZE(w25x40_ranges);
			break;
		case W_25X80:
			w25q_ranges = w25x80_ranges;
			num_entries = ARRAY_SIZE(w25x80_ranges);
			break;
		case W_25Q80:
			w25q_ranges = w25q80_ranges;
			num_entries = ARRAY_SIZE(w25q80_ranges);
			break;
		case W_25Q16:
			w25q_ranges = w25q16_ranges;
			num_entries = ARRAY_SIZE(w25q16_ranges);
			break;
		case W_25Q32:
			w25q_ranges = w25q32_ranges;
			num_entries = ARRAY_SIZE(w25q32_ranges);
			break;
		case W_25Q64:
			w25q_ranges = w25q64_ranges;
			num_entries = ARRAY_SIZE(w25q64_ranges);
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
		case EN_25F40:
			w25q_ranges = en25f40_ranges;
			num_entries = ARRAY_SIZE(en25f40_ranges);
			break;
		default:
			msg_cerr("%s():%d: EON flash chip mismatch (0x%04x)"
			         ", aborting\n", __func__, __LINE__,
				 flash->model_id);
			return -1;
		}
		break;
	case MX_ID:
		switch (flash->model_id) {
		case MX_25L3205:
			w25q_ranges = mx25l3205d_ranges;
			num_entries = ARRAY_SIZE(mx25l3205d_ranges);
			break;
		default:
			msg_cerr("%s():%d: MXIC flash chip mismatch (0x%04x)"
			         ", aborting\n", __func__, __LINE__,
			         flash->model_id);
			return -1;
		}
		break;
	default:
		msg_cerr("%s: flash vendor (0x%x) not found, aborting\n",
		         __func__, flash->manufacture_id);
		return -1;
	}

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
	return result;
}

static int w25_set_range(struct flashchip *flash,
                         unsigned int start, unsigned int len)
{
	struct w25q_status status;
	int tmp;

	memset(&status, 0, sizeof(status));
	tmp = spi_read_status_register();
	memcpy(&status, &tmp, 1);
	msg_cdbg("%s: old status: 0x%02x\n", __func__, tmp);

	if (wp_get_status(flash, start, len, &status)) return -1;

	msg_cdbg("status.busy: %x\n", status.busy);
	msg_cdbg("status.wel: %x\n", status.wel);
	msg_cdbg("status.bp0: %x\n", status.bp0);
	msg_cdbg("status.bp1: %x\n", status.bp1);
	msg_cdbg("status.bp2: %x\n", status.bp2);
	msg_cdbg("status.tb: %x\n", status.tb);
	msg_cdbg("status.sec: %x\n", status.sec);
	msg_cdbg("status.srp0: %x\n", status.srp0);

	memcpy(&tmp, &status, sizeof(status));
	spi_write_status_register_WREN(tmp);
	msg_cdbg("%s: new status: 0x%02x\n",
		  __func__, spi_read_status_register());

	return 0;
}

static int w25_enable_writeprotect(struct flashchip *flash)
{
	struct w25q_status status;
	int tmp = 0;

	memset(&status, 0, sizeof(status));
	tmp = spi_read_status_register();
	memcpy(&status, &tmp, 1);
	msg_cdbg("%s: old status: 0x%02x\n", __func__, tmp);

	status.srp0 = 1;
	memcpy(&tmp, &status, sizeof(status));
	spi_write_status_register_WREN(tmp);
	msg_cdbg("%s: new status: 0x%02x\n",
		  __func__, spi_read_status_register());

	return 0;
}

struct wp wp_w25 = {
	.set_range	= w25_set_range,
	.enable		= w25_enable_writeprotect,
};
