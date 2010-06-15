#include <stdlib.h>
#include <string.h>

#include "flash.h"
#include "flashchips.h"
#include "chipdrivers.h"

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

static int w25_set_range(struct flashchip *flash,
                         unsigned int start, unsigned int len)
{
	struct w25q_status status;
	struct w25q_range *w25q_ranges;
	int i, num_entries = 0;
	int tmp = 0, range_found = 0;

	if (flash->manufacture_id != WINBOND_NEX_ID)
		return -1;
	switch(flash->model_id) {
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
//	case W_25Q64:
//		break;
	default:
		msg_cerr("%s: flash chip mismatch, aborting\n", __func__);
		return -1;
	}

	memset(&status, 0, sizeof(status));
	tmp = spi_read_status_register();
	memcpy(&status, &tmp, 1);
	msg_cdbg("%s: old status: 0x%02x\n", __func__, tmp);

	for (i = 0; i < num_entries; i++) {
		struct wp_range *r = &w25q_ranges[i].range;

		msg_cspew("comparing range 0x%x 0x%x / 0x%x 0x%x\n",
			  start, len, r->start, r->len);
		if ((start == r->start) && (len == r->len)) {
			status.bp0 = w25q_ranges[i].bp & 1;
			status.bp1 = w25q_ranges[i].bp >> 1;
			status.bp2 = w25q_ranges[i].bp >> 2;
			status.tb = w25q_ranges[i].tb;
			status.sec = w25q_ranges[i].sec;

			range_found = 1;
			break;
		}
	}

	if (!range_found) {
		msg_cerr("matching range not found\n");
		return -1;
	}

	msg_cdbg("status.busy: %x\n", status.busy);
	msg_cdbg("status.wel: %x\n", status.wel);
	msg_cdbg("status.bp0: %x\n", status.bp0);
	msg_cdbg("status.bp1: %x\n", status.bp1);
	msg_cdbg("status.bp2: %x\n", status.bp2);
	msg_cdbg("status.tb: %x\n", status.tb);
	msg_cdbg("status.sec: %x\n", status.sec);
	msg_cdbg("status.srp0: %x\n", status.srp0);

	memcpy(&tmp, &status, sizeof(status));
	spi_write_status_enable();
	spi_write_status_register(tmp);
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
	spi_write_status_enable();
	spi_write_status_register(tmp);
	msg_cdbg("%s: new status: 0x%02x\n",
		  __func__, spi_read_status_register());

	return 0;
}

struct wp wp_w25 = {
	.set_range	= w25_set_range,
	.enable		= w25_enable_writeprotect,
};
