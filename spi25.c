/*
 * This file is part of the flashrom project.
 *
 * Copyright (C) 2007, 2008, 2009, 2010 Carl-Daniel Hailfinger
 * Copyright (C) 2008 coresystems GmbH
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
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

/*
 * Contains the common SPI chip driver functions
 */

#include <stddef.h>
#include <string.h>
#include "flash.h"
#include "flashchips.h"
#include "chipdrivers.h"
#include "programmer.h"
#include "spi.h"

enum id_type {
	RDID,
	RDID4,
	REMS,
//	RES1,	/* TODO */
	RES2,
	NUM_ID_TYPES,
};

static struct {
	int is_cached;
	unsigned char bytes[4];		/* enough to hold largest ID type */
} id_cache[NUM_ID_TYPES];

void clear_spi_id_cache(void)
{
	memset(id_cache, 0, sizeof(id_cache));
	return;
}

static int spi_rdid(struct flashctx *flash, unsigned char *readarr, int bytes)
{
	static const unsigned char cmd[JEDEC_RDID_OUTSIZE] = { JEDEC_RDID };
	int ret;
	int i;

	ret = spi_send_command(flash, sizeof(cmd), bytes, cmd, readarr);
	if (ret)
		return ret;
	msg_cspew("RDID returned");
	for (i = 0; i < bytes; i++)
		msg_cspew(" 0x%02x", readarr[i]);
	msg_cspew(". ");
	return 0;
}

static int spi_rems(struct flashctx *flash, unsigned char *readarr)
{
	unsigned char cmd[JEDEC_REMS_OUTSIZE] = { JEDEC_REMS, 0, 0, 0 };
	uint32_t readaddr;
	int ret;

	ret = spi_send_command(flash, sizeof(cmd), JEDEC_REMS_INSIZE, cmd, readarr);
	if (ret == SPI_INVALID_ADDRESS) {
		/* Find the lowest even address allowed for reads. */
		readaddr = (spi_get_valid_read_addr(flash) + 1) & ~1;
		cmd[1] = (readaddr >> 16) & 0xff,
		cmd[2] = (readaddr >> 8) & 0xff,
		cmd[3] = (readaddr >> 0) & 0xff,
		ret = spi_send_command(flash, sizeof(cmd), JEDEC_REMS_INSIZE, cmd, readarr);
	}
	if (ret)
		return ret;
	msg_cspew("REMS returned 0x%02x 0x%02x. ", readarr[0], readarr[1]);
	return 0;
}

static int spi_res(struct flashctx *flash, unsigned char *readarr, int bytes)
{
	unsigned char cmd[JEDEC_RES_OUTSIZE] = { JEDEC_RES, 0, 0, 0 };
	uint32_t readaddr;
	int ret;
	int i;

	ret = spi_send_command(flash, sizeof(cmd), bytes, cmd, readarr);
	if (ret == SPI_INVALID_ADDRESS) {
		/* Find the lowest even address allowed for reads. */
		readaddr = (spi_get_valid_read_addr(flash) + 1) & ~1;
		cmd[1] = (readaddr >> 16) & 0xff,
		cmd[2] = (readaddr >> 8) & 0xff,
		cmd[3] = (readaddr >> 0) & 0xff,
		ret = spi_send_command(flash, sizeof(cmd), bytes, cmd, readarr);
	}
	if (ret)
		return ret;
	msg_cspew("RES returned");
	for (i = 0; i < bytes; i++)
		msg_cspew(" 0x%02x", readarr[i]);
	msg_cspew(". ");
	return 0;
}

int spi_write_enable(struct flashctx *flash)
{
	static const unsigned char cmd[JEDEC_WREN_OUTSIZE] = { JEDEC_WREN };
	int result;

	/* Send WREN (Write Enable) */
	result = spi_send_command(flash, sizeof(cmd), 0, cmd, NULL);

	if (result)
		msg_cerr("%s failed\n", __func__);

	return result;
}

int spi_write_disable(struct flashctx *flash)
{
	static const unsigned char cmd[JEDEC_WRDI_OUTSIZE] = { JEDEC_WRDI };

	/* Send WRDI (Write Disable) */
	return spi_send_command(flash, sizeof(cmd), 0, cmd, NULL);
}

static void rdid_get_ids(unsigned char *readarr,
		int bytes, uint32_t *id1, uint32_t *id2)
{
	if (!oddparity(readarr[0]))
		msg_cdbg("RDID byte 0 parity violation. ");

	/* Check if this is a continuation vendor ID.
	 * FIXME: Handle continuation device IDs.
	 */
	if (readarr[0] == 0x7f) {
		if (!oddparity(readarr[1]))
			msg_cdbg("RDID byte 1 parity violation. ");
		*id1 = (readarr[0] << 8) | readarr[1];
		*id2 = readarr[2];
		if (bytes > 3) {
			*id2 <<= 8;
			*id2 |= readarr[3];
		}
	} else {
		*id1 = readarr[0];
		*id2 = (readarr[1] << 8) | readarr[2];
	}
}

static int compare_id(struct flashctx *flash, uint32_t id1, uint32_t id2)
{
	msg_cdbg("id1 0x%02x, id2 0x%02x\n", id1, id2);

	if (id1 == flash->chip->manufacture_id && id2 == flash->chip->model_id) {
		/* Print the status register to tell the
		 * user about possible write protection.
		 */
		spi_prettyprint_status_register(flash);

		return 1;
	}

	/* Test if this is a pure vendor match. */
	if (id1 == flash->chip->manufacture_id &&
	    GENERIC_DEVICE_ID == flash->chip->model_id)
		return 1;

	/* Test if there is any vendor ID. */
	if (GENERIC_MANUF_ID == flash->chip->manufacture_id &&
	    id1 != 0xff)
		return 1;

	return 0;
}

int probe_spi_rdid(struct flashctx *flash)
{
	uint32_t id1, id2;

	if (!id_cache[RDID].is_cached) {
		if (spi_rdid(flash, id_cache[RDID].bytes, 3))
			return 0;
		id_cache[RDID].is_cached = 1;
	}

	rdid_get_ids(id_cache[RDID].bytes, 3, &id1, &id2);
	return compare_id(flash, id1, id2);
}

int probe_spi_rdid4(struct flashctx *flash)
{
	uint32_t id1, id2;

	/* Some SPI controllers do not support commands with writecnt=1 and
	 * readcnt=4.
	 */
	switch (spi_master->type) {
#if CONFIG_INTERNAL == 1
#if defined(__i386__) || defined(__x86_64__)
	case SPI_CONTROLLER_IT87XX:
	case SPI_CONTROLLER_WBSIO:
		msg_cinfo("4 byte RDID not supported on this SPI controller\n");
		break;
#endif
#endif
	default:
		break;
	}

	if (!id_cache[RDID4].is_cached) {
		if (spi_rdid(flash, id_cache[RDID4].bytes, 4))
			return 0;
		id_cache[RDID4].is_cached = 1;
	}

	rdid_get_ids(id_cache[RDID4].bytes, 4, &id1, &id2);
	return compare_id(flash, id1, id2);
}

int probe_spi_rems(struct flashctx *flash)
{
	uint32_t id1, id2;

	if (!id_cache[REMS].is_cached) {
		if (spi_rems(flash, id_cache[REMS].bytes))
			return 0;
		id_cache[REMS].is_cached = 1;
	}

	id1 = id_cache[REMS].bytes[0];
	id2 = id_cache[REMS].bytes[1];
	return compare_id(flash, id1, id2);
}

int probe_spi_res1(struct flashctx *flash)
{
	static const unsigned char allff[] = {0xff, 0xff, 0xff};
	static const unsigned char all00[] = {0x00, 0x00, 0x00};
	unsigned char readarr[3];
	uint32_t id2;

	/* We only want one-byte RES if RDID and REMS are unusable. */

	/* Check if RDID is usable and does not return 0xff 0xff 0xff or
	 * 0x00 0x00 0x00. In that case, RES is pointless.
	 */
	if (!spi_rdid(flash, readarr, 3) && memcmp(readarr, allff, 3) &&
	    memcmp(readarr, all00, 3)) {
		msg_cdbg("Ignoring RES in favour of RDID.\n");
		return 0;
	}
	/* Check if REMS is usable and does not return 0xff 0xff or
	 * 0x00 0x00. In that case, RES is pointless.
	 */
	if (!spi_rems(flash, readarr) && memcmp(readarr, allff, JEDEC_REMS_INSIZE) &&
	    memcmp(readarr, all00, JEDEC_REMS_INSIZE)) {
		msg_cdbg("Ignoring RES in favour of REMS.\n");
		return 0;
	}

	if (spi_res(flash, readarr, 1)) {
		return 0;
	}

	id2 = readarr[0];

	msg_cdbg("%s: id 0x%x\n", __func__, id2);

	if (id2 != flash->chip->model_id)
		return 0;

	/* Print the status register to tell the
	 * user about possible write protection.
	 */
	spi_prettyprint_status_register(flash);
	return 1;
}

int probe_spi_res2(struct flashctx *flash)
{
	uint32_t id1, id2;

	if (!id_cache[RES2].is_cached) {
		if (spi_res(flash, id_cache[RES2].bytes, 2))
			return 0;
		id_cache[RES2].is_cached = 1;
	}

	id1 = id_cache[RES2].bytes[0];
	id2 = id_cache[RES2].bytes[1];
	msg_cdbg("%s: id1 0x%x, id2 0x%x\n", __func__, id1, id2);

	if (id1 != flash->chip->manufacture_id || id2 != flash->chip->model_id)
		return 0;

	/* Print the status register to tell the
	 * user about possible write protection.
	 */
	spi_prettyprint_status_register(flash);
	return 1;
}

uint8_t spi_read_status_register(const struct flashctx *flash)
{
	static const unsigned char cmd[JEDEC_RDSR_OUTSIZE] = { JEDEC_RDSR };
	/* FIXME: No workarounds for driver/hardware bugs in generic code. */
	unsigned char readarr[2]; /* JEDEC_RDSR_INSIZE=1 but wbsio needs 2 */
	int ret;

	/* Read Status Register */
	if (flash->chip->read_status)
		ret = flash->chip->read_status(flash);
	else
		ret = spi_send_command(flash, sizeof(cmd), sizeof(readarr), cmd, readarr);
	if (ret)
		msg_cerr("RDSR failed!\n");

	return readarr[0];
}

/* Prettyprint the status register. Common definitions. */
void spi_prettyprint_status_register_welwip(uint8_t status)
{
	msg_cdbg("Chip status register: Write Enable Latch (WEL) is "
		     "%sset\n", (status & (1 << 1)) ? "" : "not ");
	msg_cdbg("Chip status register: Write In Progress (WIP/BUSY) is "
		     "%sset\n", (status & (1 << 0)) ? "" : "not ");
}

/* Prettyprint the status register. Common definitions. */
void spi_prettyprint_status_register_bp3210(uint8_t status, int bp)
{
	switch (bp) {
	/* Fall through. */
	case 3:
		msg_cdbg("Chip status register: Bit 5 / Block Protect 3 (BP3) "
			     "is %sset\n", (status & (1 << 5)) ? "" : "not ");
	case 2:
		msg_cdbg("Chip status register: Bit 4 / Block Protect 2 (BP2) "
			     "is %sset\n", (status & (1 << 4)) ? "" : "not ");
	case 1:
		msg_cdbg("Chip status register: Bit 3 / Block Protect 1 (BP1) "
			     "is %sset\n", (status & (1 << 3)) ? "" : "not ");
	case 0:
		msg_cdbg("Chip status register: Bit 2 / Block Protect 0 (BP0) "
			     "is %sset\n", (status & (1 << 2)) ? "" : "not ");
	}
}

/* Prettyprint the status register. Unnamed bits. */
void spi_prettyprint_status_register_bit(uint8_t status, int bit)
{
	msg_cdbg("Chip status register: Bit %i "
		 "is %sset\n", bit, (status & (1 << bit)) ? "" : "not ");
}

static void spi_prettyprint_status_register_common(uint8_t status)
{
	spi_prettyprint_status_register_bp3210(status, 3);
	spi_prettyprint_status_register_welwip(status);
}

/* Prettyprint the status register. Works for
 * ST M25P series
 * MX MX25L series
 */
void spi_prettyprint_status_register_st_m25p(uint8_t status)
{
	msg_cdbg("Chip status register: Status Register Write Disable "
		     "(SRWD) is %sset\n", (status & (1 << 7)) ? "" : "not ");
	msg_cdbg("Chip status register: Bit 6 is "
		     "%sset\n", (status & (1 << 6)) ? "" : "not ");
	spi_prettyprint_status_register_common(status);
}

void spi_prettyprint_status_register_sst25(uint8_t status)
{
	msg_cdbg("Chip status register: Block Protect Write Disable "
		     "(BPL) is %sset\n", (status & (1 << 7)) ? "" : "not ");
	msg_cdbg("Chip status register: Auto Address Increment Programming "
		     "(AAI) is %sset\n", (status & (1 << 6)) ? "" : "not ");
	spi_prettyprint_status_register_common(status);
}

/* Prettyprint the status register. Works for
 * SST 25VF016
 */
void spi_prettyprint_status_register_sst25vf016(uint8_t status)
{
	static const char *const bpt[] = {
		"none",
		"1F0000H-1FFFFFH",
		"1E0000H-1FFFFFH",
		"1C0000H-1FFFFFH",
		"180000H-1FFFFFH",
		"100000H-1FFFFFH",
		"all", "all"
	};
	spi_prettyprint_status_register_sst25(status);
	msg_cdbg("Resulting block protection : %s\n",
		     bpt[(status & 0x1c) >> 2]);
}

void spi_prettyprint_status_register_sst25vf040b(uint8_t status)
{
	static const char *const bpt[] = {
		"none",
		"0x70000-0x7ffff",
		"0x60000-0x7ffff",
		"0x40000-0x7ffff",
		"all blocks", "all blocks", "all blocks", "all blocks"
	};
	spi_prettyprint_status_register_sst25(status);
	msg_cdbg("Resulting block protection : %s\n",
		bpt[(status & 0x1c) >> 2]);
}

int spi_prettyprint_status_register(struct flashctx *flash)
{
	uint8_t status;

	status = spi_read_status_register(flash);
	msg_cdbg("Chip status register is %02x\n", status);
	switch (flash->chip->manufacture_id) {
	case ST_ID:
		if (((flash->chip->model_id & 0xff00) == 0x2000) ||
		    ((flash->chip->model_id & 0xff00) == 0x2500))
			spi_prettyprint_status_register_st_m25p(status);
		break;
	case MACRONIX_ID:
		if ((flash->chip->model_id & 0xff00) == 0x2000)
			spi_prettyprint_status_register_st_m25p(status);
		break;
	case SST_ID:
		switch (flash->chip->model_id) {
		case 0x2541:
			spi_prettyprint_status_register_sst25vf016(status);
			break;
		case 0x8d:
		case 0x258d:
			spi_prettyprint_status_register_sst25vf040b(status);
			break;
		default:
			spi_prettyprint_status_register_sst25(status);
			break;
		}
		break;
	}
	return 0;
}

static int spi_poll_wip(struct flashctx *const flash, const unsigned int poll_delay)
{
	/* FIXME: We can't tell if spi_read_status_register() failed. */
	/* FIXME: We don't time out. */
	while (spi_read_status_register(flash) & JEDEC_RDSR_BIT_WIP)
		programmer_delay(poll_delay);
	/* FIXME: Check the status register for errors. */
	return 0;
}

/**
 * Execute WREN plus another one byte `op`, optionally poll WIP afterwards.
 *
 * @param flash       the flash chip's context
 * @param op          the operation to execute
 * @param poll_delay  interval in us for polling WIP, don't poll if zero
 * @return 0 on success, non-zero otherwise
 */
static int spi_simple_write_cmd(struct flashctx *const flash, const uint8_t op, const unsigned int poll_delay)
{
	struct spi_command cmds[] = {
	{
		.writecnt = 1,
		.writearr = (const unsigned char[]){ JEDEC_WREN },
	}, {
		.writecnt = 1,
		.writearr = (const unsigned char[]){ op },
	},
		NULL_SPI_CMD,
	};

	const int result = spi_send_multicommand(flash, cmds);
	if (result)
		msg_cerr("%s failed during command execution\n", __func__);

	const int status = poll_delay ? spi_poll_wip(flash, poll_delay) : 0;

	return result ? result : status;
}

static int spi_write_extended_address_register(struct flashctx *const flash, const uint8_t regdata)
{
	struct spi_command cmds[] = {
	{
		.writecnt = 1,
		.writearr = (const unsigned char[]){ JEDEC_WREN },
	}, {
		.writecnt = 2,
		.writearr = (const unsigned char[]){ JEDEC_WRITE_EXT_ADDR_REG, regdata },
	},
		NULL_SPI_CMD,
	};

	const int result = spi_send_multicommand(flash, cmds);
	if (result)
		msg_cerr("%s failed during command execution\n", __func__);
	return result;
}

static int spi_set_extended_address(struct flashctx *const flash, const uint8_t addr_high)
{
	if (flash->address_high_byte != addr_high &&
	    spi_write_extended_address_register(flash, addr_high))
		return -1;
	flash->address_high_byte = addr_high;
	return 0;
}

static int spi_prepare_address(struct flashctx *const flash,
			       uint8_t cmd_buf[], const bool native_4ba, const unsigned int addr)
{
	if (native_4ba || flash->in_4ba_mode) {
		cmd_buf[1] = (addr >> 24) & 0xff;
		cmd_buf[2] = (addr >> 16) & 0xff;
		cmd_buf[3] = (addr >>  8) & 0xff;
		cmd_buf[4] = (addr >>  0) & 0xff;
		return 4;
	} else {
		if (flash->chip->feature_bits & FEATURE_4BA_EXT_ADDR) {
			if (spi_set_extended_address(flash, addr >> 24))
				return -1;
		} else {
			if (addr >> 24)
				return -1;
		}
		cmd_buf[1] = (addr >> 16) & 0xff;
		cmd_buf[2] = (addr >>  8) & 0xff;
		cmd_buf[3] = (addr >>  0) & 0xff;
		return 3;
	}
}

/**
 * Execute WREN plus another `op` that takes an address and
 * optional data, poll WIP afterwards.
 *
 * @param flash       the flash chip's context
 * @param op          the operation to execute
 * @param native_4ba  whether `op` always takes a 4-byte address
 * @param addr        the address parameter to `op`
 * @param out_bytes   bytes to send after the address,
 *                    may be NULL if and only if `out_bytes` is 0
 * @param out_bytes   number of bytes to send, 256 at most, may be zero
 * @param poll_delay  interval in us for polling WIP
 * @return 0 on success, non-zero otherwise
 */
static int spi_write_cmd(struct flashctx *const flash,
			 const uint8_t op, const bool native_4ba, const unsigned int addr,
			 const uint8_t *const out_bytes, const size_t out_len,
			 const unsigned int poll_delay)
{
	uint8_t cmd[1 + JEDEC_MAX_ADDR_LEN + 256];
	struct spi_command cmds[] = {
	{
		.writecnt = 1,
		.writearr = (const unsigned char[]){ JEDEC_WREN },
	}, {
		.writearr = cmd,
	},
		NULL_SPI_CMD,
	};

	cmd[0] = op;
	const int addr_len = spi_prepare_address(flash, cmd, native_4ba, addr);
	if (addr_len < 0)
		return 1;

	if (1 + addr_len + out_len > sizeof(cmd)) {
		msg_cerr("%s called for too long a write\n", __func__);
		return 1;
	}

	memcpy(cmd + 1 + addr_len, out_bytes, out_len);
	cmds[1].writecnt = 1 + addr_len + out_len;

	const int result = spi_send_multicommand(flash, cmds);
	if (result)
		msg_cerr("%s failed during command execution at address 0x%x\n", __func__, addr);

	const int status = spi_poll_wip(flash, poll_delay);

	return result ? result : status;
}

int spi_chip_erase_60(struct flashctx *flash)
{
	/* This usually takes 1-85s, so wait in 0.1s steps. */
	return spi_simple_write_cmd(flash, 0x60, 100 * 1000);
}

int spi_chip_erase_c7(struct flashctx *flash)
{
	/* This usually takes 1-85s, so wait in 0.1s steps. */
	return spi_simple_write_cmd(flash, 0xc7, 100 * 1000);
}

int spi_block_erase_52(struct flashctx *flash, unsigned int addr, unsigned int blocklen)
{
	/* This usually takes 100-4000ms, so wait in 20ms steps. */
	return spi_write_cmd(flash, 0x52, false, addr, NULL, 0, 20 * 1000);
}

/* Block size is usually
 * 32M (one die) for Micron
 */
int spi_block_erase_c4(struct flashctx *flash, unsigned int addr, unsigned int blocklen)
{
	/* This usually takes 240-480s, so wait in 100ms steps. */
	return spi_write_cmd(flash, 0xc4, false, addr, NULL, 0, 100 * 1000);
}

/* Block size is usually
 * 64k for Macronix
 * 32k for SST
 * 4-32k non-uniform for EON
 */
int spi_block_erase_d8(struct flashctx *flash, unsigned int addr, unsigned int blocklen)
{
	/* This usually takes 100-4000ms, so wait in 20ms steps. */
	return spi_write_cmd(flash, 0xd8, false, addr, NULL, 0, 20 * 1000);
}

/* Block size is usually
 * 4k for PMC
 */
int spi_block_erase_d7(struct flashctx *flash, unsigned int addr, unsigned int blocklen)
{
	/* This usually takes 100-4000ms, so wait in 20ms steps. */
	return spi_write_cmd(flash, 0xd7, false, addr, NULL, 0, 20 * 1000);
}

/* Page erase (usually 256B blocks) */
int spi_block_erase_db(struct flashctx *flash, unsigned int addr, unsigned int blocklen)
{
	/* This takes up to 20ms usually (on worn out devices
	   up to the 0.5s range), so wait in 1ms steps. */
	return spi_write_cmd(flash, 0xdb, false, addr, NULL, 0, 1 * 1000);
}

/* Sector size is usually 4k, though Macronix eliteflash has 64k */
int spi_block_erase_20(struct flashctx *flash, unsigned int addr, unsigned int blocklen)
{
	/* This usually takes 15-800ms, so wait in 2ms steps. */
	return spi_write_cmd(flash, 0x20, false, addr, NULL, 0, 2 * 1000);
}

int spi_block_erase_60(struct flashctx *flash, unsigned int addr, unsigned int blocklen)
{
	if ((addr != 0) || (blocklen != flash->chip->total_size * 1024)) {
		msg_cerr("%s called with incorrect arguments\n",
			__func__);
		return -1;
	}
	return spi_chip_erase_60(flash);
}

int spi_block_erase_c7(struct flashctx *flash, unsigned int addr, unsigned int blocklen)
{
	if ((addr != 0) || (blocklen != flash->chip->total_size * 1024)) {
		msg_cerr("%s called with incorrect arguments\n",
			__func__);
		return -1;
	}
	return spi_chip_erase_c7(flash);
}

/* Erase 4 KB of flash with 4-bytes address from ANY mode (3-bytes or 4-bytes) */
int spi_block_erase_21(struct flashctx *flash, unsigned int addr, unsigned int blocklen)
{
	/* This usually takes 15-800ms, so wait in 10ms steps. */
	return spi_write_cmd(flash, 0x21, true, addr, NULL, 0, 10 * 1000);
}

/* Erase 32 KB of flash with 4-bytes address from ANY mode (3-bytes or 4-bytes) */
int spi_block_erase_5c(struct flashctx *flash, unsigned int addr, unsigned int blocklen)
{
	/* This usually takes 100-4000ms, so wait in 100ms steps. */
	return spi_write_cmd(flash, 0x5c, true, addr, NULL, 0, 100 * 1000);
}

/* Erase 64 KB of flash with 4-bytes address from ANY mode (3-bytes or 4-bytes) */
int spi_block_erase_dc(struct flashctx *flash, unsigned int addr, unsigned int blocklen)
{
	/* This usually takes 100-4000ms, so wait in 100ms steps. */
	return spi_write_cmd(flash, 0xdc, true, addr, NULL, 0, 100 * 1000);
}

int spi_write_status_enable(struct flashctx *flash)
{
	static const unsigned char cmd[JEDEC_EWSR_OUTSIZE] = { JEDEC_EWSR };
	int result;

	/* Send EWSR (Enable Write Status Register). */
	result = spi_send_command(flash, sizeof(cmd), JEDEC_EWSR_INSIZE, cmd, NULL);

	if (result)
		msg_cerr("%s failed\n", __func__);

	return result;
}

/*
 * This is according the SST25VF016 datasheet, who knows it is more
 * generic that this...
 */
static int spi_write_status_register_ewsr(const struct flashctx *flash, int status)
{
	int result;
	int i = 0;
	struct spi_command cmds[] = {
	{
	/* WRSR requires either EWSR or WREN depending on chip type. */
		.writecnt	= JEDEC_EWSR_OUTSIZE,
		.writearr	= (const unsigned char[]){ JEDEC_EWSR },
		.readcnt	= 0,
		.readarr	= NULL,
	}, {
		.writecnt	= JEDEC_WRSR_OUTSIZE,
		.writearr	= (const unsigned char[]){ JEDEC_WRSR, (unsigned char) status },
		.readcnt	= 0,
		.readarr	= NULL,
	}, {
		.writecnt	= 0,
		.writearr	= NULL,
		.readcnt	= 0,
		.readarr	= NULL,
	}};

	result = spi_send_multicommand(flash, cmds);
	if (result) {
		msg_cerr("%s failed during command execution\n",
			__func__);
		/* No point in waiting for the command to complete if execution
		 * failed.
		 */
		return result;
	}
	/* WRSR performs a self-timed erase before the changes take effect.
	 * This may take 50-85 ms in most cases, and some chips apparently
	 * allow running RDSR only once. Therefore pick an initial delay of
	 * 100 ms, then wait in 10 ms steps until a total of 5 s have elapsed.
	 */
	programmer_delay(100 * 1000);
	while (spi_read_status_register(flash) & JEDEC_RDSR_BIT_WIP) {
		if (++i > 490) {
			msg_cerr("Error: WIP bit after WRSR never cleared\n");
			return TIMEOUT_ERROR;
		}
		programmer_delay(10 * 1000);
	}
	return 0;
}

int spi_write_status_register_wren(const struct flashctx *flash, int status)
{
	int result;
	int i = 0;
	struct spi_command cmds[] = {
	{
	/* WRSR requires either EWSR or WREN depending on chip type. */
		.writecnt	= JEDEC_WREN_OUTSIZE,
		.writearr	= (const unsigned char[]){ JEDEC_WREN },
		.readcnt	= 0,
		.readarr	= NULL,
	}, {
		.writecnt	= JEDEC_WRSR_OUTSIZE,
		.writearr	= (const unsigned char[]){ JEDEC_WRSR, (unsigned char) status },
		.readcnt	= 0,
		.readarr	= NULL,
	}, {
		.writecnt	= 0,
		.writearr	= NULL,
		.readcnt	= 0,
		.readarr	= NULL,
	}};

	result = spi_send_multicommand(flash, cmds);
	if (result) {
		msg_cerr("%s failed during command execution\n",
			__func__);
		/* No point in waiting for the command to complete if execution
		 * failed.
		 */
		return result;
	}
	/* WRSR performs a self-timed erase before the changes take effect.
	 * This may take 50-85 ms in most cases, and some chips apparently
	 * allow running RDSR only once. Therefore pick an initial delay of
	 * 100 ms, then wait in 10 ms steps until a total of 5 s have elapsed.
	 */
	programmer_delay(100 * 1000);
	while (spi_read_status_register(flash) & JEDEC_RDSR_BIT_WIP) {
		if (++i > 490) {
			msg_cerr("Error: WIP bit after WRSR never cleared\n");
			return TIMEOUT_ERROR;
		}
		programmer_delay(10 * 1000);
	}
	return 0;
}

int spi_write_status_register(const struct flashctx *flash, int status)
{
	int ret = 1;

	if (flash->chip->write_status) {
		ret = flash->chip->write_status(flash, status);
	} else {
		if (flash->chip->feature_bits & FEATURE_WRSR_WREN)
			ret = spi_write_status_register_wren(flash, status);
		if (ret && (flash->chip->feature_bits & FEATURE_WRSR_EWSR))
			ret = spi_write_status_register_ewsr(flash, status);
	}

	return ret;
}

static int spi_nbyte_program(struct flashctx *flash, unsigned int addr, const uint8_t *bytes, unsigned int len)
{
	const bool native_4ba = !!(flash->chip->feature_bits & FEATURE_4BA_WRITE);
	const uint8_t op = native_4ba ? JEDEC_BYTE_PROGRAM_4BA : JEDEC_BYTE_PROGRAM;
	return spi_write_cmd(flash, op, native_4ba, addr, bytes, len, 10);
}

int spi_restore_status(struct flashctx *flash, uint8_t status)
{
	msg_cdbg("restoring chip status (0x%02x)\n", status);
	return spi_write_status_register(flash, status);
}

/* A generic brute-force block protection disable works like this:
 * Write 0x00 to the status register. Check if any locks are still set (that
 * part is chip specific). Repeat once.
 */
int spi_disable_blockprotect(struct flashctx *flash)
{
	uint8_t status;
	int result;

	status = spi_read_status_register(flash);
	/* If block protection is disabled, stop here. */
	if ((status & 0x3c) == 0)
		return 0;

	/* restore status register content upon exit */
	register_chip_restore(spi_restore_status, flash, status);

	msg_cdbg("Some block protection in effect, disabling\n");
	result = spi_write_status_register(flash, status & ~0x3c);
	if (result) {
		msg_cerr("spi_write_status_register failed\n");
		return result;
	}
	status = spi_read_status_register(flash);
	if ((status & 0x3c) != 0) {
		msg_cerr("Block protection could not be disabled!\n");
		return 1;
	}
	return 0;
}

int spi_nbyte_read(struct flashctx *flash, unsigned int address, uint8_t *bytes, unsigned int len)
{
	const bool native_4ba = !!(flash->chip->feature_bits & FEATURE_4BA_READ);
	uint8_t cmd[1 + JEDEC_MAX_ADDR_LEN] = { JEDEC_READ, };

	const int addr_len = spi_prepare_address(flash, cmd, native_4ba, address);
	if (addr_len < 0)
		return 1;

	/* Send Read */
	return spi_send_command(flash, 1 + addr_len, len, cmd, bytes);
}

/*
 * Read a part of the flash chip.
 * FIXME: Use the chunk code from Michael Karcher instead.
 * Each naturally aligned area is read separately in chunks with a maximum size of chunksize.
 */
int spi_read_chunked(struct flashctx *flash, uint8_t *buf, unsigned int start, unsigned int len, unsigned int chunksize)
{
	int rc = 0, chunk_status = 0;
	unsigned int i, j, starthere, lenhere, toread;
	/* Limit for multi-die 4-byte-addressing chips. */
	unsigned int area_size = min(flash->chip->total_size * 1024, 16 * 1024 * 1024);

	/* Warning: This loop has a very unusual condition and body.
	 * The loop needs to go through each area with at least one affected
	 * byte. The lowest area number is (start / area_size) since that
	 * division rounds down. The highest area number we want is the area
	 * where the last byte of the range lives. That last byte has the
	 * address (start + len - 1), thus the highest area number is
	 * (start + len - 1) / area_size. Since we want to include that last
	 * area as well, the loop condition uses <=.
	 */
	for (i = start / area_size; i <= (start + len - 1) / area_size; i++) {
		/* Byte position of the first byte in the range in this area. */
		/* starthere is an offset to the base address of the chip. */
		starthere = max(start, i * area_size);
		/* Length of bytes in the range in this area. */
		lenhere = min(start + len, (i + 1) * area_size) - starthere;
		for (j = 0; j < lenhere; j += chunksize) {
			toread = min(chunksize, lenhere - j);
			rc = spi_nbyte_read(flash, starthere + j, buf + starthere - start + j, toread);
			
			if (chunk_status) {
				if (ignore_error(chunk_status)) {
					/* fill this chunk with 0xff bytes and
					   let caller know about the error */
					memset(buf + starthere - start + j, 0xff, toread);
					rc = chunk_status;
					chunk_status = 0;
					continue;
				} else {
					rc = chunk_status;
					break;
				}
			}
		}
		if (chunk_status)
			break;

		msg_ginfo("Reading: %.2f %%, 0x%08x to 0x%08x\n",
			(starthere / (double)(flash->chip->total_size * 1024)) * 100,
			starthere,
			starthere + lenhere - 1);
	}

	return rc;
}

/*
 * Read a part of the flash chip.
 * Ignore pages and read the data continuously, the only bound is the chunksize.
 */
int spi_read_unbound(struct flashctx *flash, uint8_t *buf, unsigned int start, unsigned int len, unsigned int chunksize)
{
	int rc = 0;
	unsigned int i;

	for (i = start; i < (start + len); i += chunksize) {

		/* Print current progress status */
		msg_ginfo("Reading: %.2f %%, 0x%08x to 0x%08x\n",
			(i / (double)(flash->chip->total_size * 1024)) * 100,
			i,
			i + chunksize - 1);

		int chunk_status = 0;
		unsigned int toread = min(chunksize, start + len - i);

		chunk_status = spi_nbyte_read(flash, i, buf + (i - start), toread);
		if (chunk_status) {
			if (ignore_error(chunk_status)) {
				/* fill this chunk with 0xff bytes and
				   let caller know about the error */
				memset(buf + (i - start), 0xff, toread);
				rc = chunk_status;
				continue;
			} else {
				rc = chunk_status;
				break;
			}
		}
	}

	return rc;
}

/*
 * Write a part of the flash chip.
 * FIXME: Use the chunk code from Michael Karcher instead.
 * Each page is written separately in chunks with a maximum size of chunksize.
 */
int spi_write_chunked(struct flashctx *flash, const uint8_t *buf, unsigned int start, unsigned int len, unsigned int chunksize)
{
	int rc = 0;
	unsigned int i, j, starthere, lenhere, towrite;
	/* FIXME: page_size is the wrong variable. We need max_writechunk_size
	 * in struct flashctx to do this properly. All chips using
	 * spi_chip_write_256 have page_size set to max_writechunk_size, so
	 * we're OK for now.
	 */
	unsigned int page_size = flash->chip->page_size;

	/* Warning: This loop has a very unusual condition and body.
	 * The loop needs to go through each page with at least one affected
	 * byte. The lowest page number is (start / page_size) since that
	 * division rounds down. The highest page number we want is the page
	 * where the last byte of the range lives. That last byte has the
	 * address (start + len - 1), thus the highest page number is
	 * (start + len - 1) / page_size. Since we want to include that last
	 * page as well, the loop condition uses <=.
	 */
	for (i = start / page_size; i <= (start + len - 1) / page_size; i++) {
		/* Byte position of the first byte in the range in this page. */
		/* starthere is an offset to the base address of the chip. */
		starthere = max(start, i * page_size);
		/* Length of bytes in the range in this page. */
		lenhere = min(start + len, (i + 1) * page_size) - starthere;

		for (j = 0; j < lenhere; j += chunksize) {
			towrite = min(chunksize, lenhere - j);

			rc = spi_nbyte_program(flash, starthere + j, buf + starthere - start + j, towrite);

			if (rc)
				return rc;
		}

		msg_ginfo("Writing: %.2f %%, 0x%08x to 0x%08x\n",
					(starthere / (double)(flash->chip->total_size * 1024)) * 100,
					starthere,
					starthere + lenhere - 1);
	}

	return 0;
}

/*
 * Program chip using byte programming. (SLOW!)
 * This is for chips which can only handle one byte writes
 * and for chips where memory mapped programming is impossible
 * (e.g. due to size constraints in IT87* for over 512 kB)
 */
/* real chunksize is 1, logical chunksize is 1 */
int spi_chip_write_1(struct flashctx *flash, const uint8_t *buf, unsigned int start, unsigned int len)
{
	unsigned int i;

	for (i = start; i < start + len; i++) {
		if (spi_nbyte_program(flash, i, buf + i - start, 1))
			return 1;
	}

	return 0;
}

int spi_aai_write(struct flashctx *flash, const uint8_t *buf, unsigned int start, unsigned int len)
{
	uint32_t pos = start;
	int result;
	unsigned char cmd[JEDEC_AAI_WORD_PROGRAM_CONT_OUTSIZE] = {
		JEDEC_AAI_WORD_PROGRAM,
	};
	struct spi_command cmds[] = {
	{
		.writecnt	= JEDEC_WREN_OUTSIZE,
		.writearr	= (const unsigned char[]){ JEDEC_WREN },
		.readcnt	= 0,
		.readarr	= NULL,
	}, {
		.writecnt	= JEDEC_AAI_WORD_PROGRAM_OUTSIZE,
		.writearr	= (const unsigned char[]){
					JEDEC_AAI_WORD_PROGRAM,
					(start >> 16) & 0xff,
					(start >> 8) & 0xff,
					(start & 0xff),
					buf[0],
					buf[1]
				},
		.readcnt	= 0,
		.readarr	= NULL,
	}, {
		.writecnt	= 0,
		.writearr	= NULL,
		.readcnt	= 0,
		.readarr	= NULL,
	}};

	switch (spi_master->type) {
#if CONFIG_INTERNAL == 1
#if defined(__i386__) || defined(__x86_64__)
	case SPI_CONTROLLER_IT87XX:
	case SPI_CONTROLLER_WBSIO:
		msg_perr("%s: impossible with this SPI controller,"
				" degrading to byte program\n", __func__);
		return spi_chip_write_1(flash, buf, start, len);
#endif
#endif
	default:
		break;
	}

	/* The even start address and even length requirements can be either
	 * honored outside this function, or we can call spi_byte_program
	 * for the first and/or last byte and use AAI for the rest.
	 * FIXME: Move this to generic code.
	 */
	/* The data sheet requires a start address with the low bit cleared. */
	if (start % 2) {
		msg_cerr("%s: start address not even! Please report a bug at "
			 "flashrom@flashrom.org\n", __func__);
		if (spi_chip_write_1(flash, buf, start, start % 2))
			return SPI_GENERIC_ERROR;
		pos += start % 2;
		cmds[1].writearr = (const unsigned char[]){
					JEDEC_AAI_WORD_PROGRAM,
					(pos >> 16) & 0xff,
					(pos >> 8) & 0xff,
					(pos & 0xff),
					buf[pos - start],
					buf[pos - start + 1]
				};
		/* Do not return an error for now. */
		//return SPI_GENERIC_ERROR;
	}
	/* The data sheet requires total AAI write length to be even. */
	if (len % 2) {
		msg_cerr("%s: total write length not even! Please report a "
			 "bug at flashrom@flashrom.org\n", __func__);
		/* Do not return an error for now. */
		//return SPI_GENERIC_ERROR;
	}


	result = spi_send_multicommand(flash, cmds);
	if (result) {
		msg_cerr("%s failed during start command execution\n",
			 __func__);
		/* FIXME: Should we send WRDI here as well to make sure the chip
		 * is not in AAI mode?
		 */
		return result;
	}
	while (spi_read_status_register(flash) & JEDEC_RDSR_BIT_WIP)
		programmer_delay(10);

	/* We already wrote 2 bytes in the multicommand step. */
	pos += 2;

	/* Are there at least two more bytes to write? */
	while (pos < start + len - 1) {
		cmd[1] = buf[pos++ - start];
		cmd[2] = buf[pos++ - start];
		spi_send_command(flash, JEDEC_AAI_WORD_PROGRAM_CONT_OUTSIZE, 0, cmd, NULL);
		while (spi_read_status_register(flash) & JEDEC_RDSR_BIT_WIP)
			programmer_delay(10);
	}

	/* Use WRDI to exit AAI mode. This needs to be done before issuing any
	 * other non-AAI command.
	 */
	spi_write_disable(flash);

	/* Write remaining byte (if any). */
	if (pos < start + len) {
		if (spi_chip_write_1(flash, buf + pos - start, pos, pos % 2))
			return SPI_GENERIC_ERROR;
		pos += pos % 2;
	}

	return 0;
}

/* Enter 4-bytes addressing mode (without sending WREN before) */
int spi_enter_4ba_b7(struct flashctx *flash)
{
	const unsigned char cmd = JEDEC_ENTER_4_BYTE_ADDR_MODE;

	const int ret = spi_send_command(flash, sizeof(cmd), 0, &cmd, NULL);
	if (!ret)
		flash->in_4ba_mode = true;
	return ret;
}

/* Enter 4-bytes addressing mode with sending WREN before */
int spi_enter_4ba_b7_we(struct flashctx *flash)
{
	const int ret = spi_simple_write_cmd(flash, JEDEC_ENTER_4_BYTE_ADDR_MODE, 0);
	if (!ret)
		flash->in_4ba_mode = true;
	return ret;
}

/* Exit 4-bytes addressing mode (without sending WREN before) */
int spi_exit_4ba_e9(struct flashctx *flash)
{
	const unsigned char cmd = JEDEC_EXIT_4_BYTE_ADDR_MODE;

	const int ret = spi_send_command(flash, sizeof(cmd), 0, &cmd, NULL);
 	if (!ret)
		flash->in_4ba_mode = false;
	return ret;
}

/* Exit 4-bytes addressing mode with sending WREN before */
int spi_exit_4ba_e9_we(struct flashctx *flash)
{
 	const int ret = spi_simple_write_cmd(flash, JEDEC_EXIT_4_BYTE_ADDR_MODE, 0);
 	if (!ret)
 		flash->in_4ba_mode = false;
 	return ret;
}

