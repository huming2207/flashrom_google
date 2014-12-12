/*
 * This file is part of the flashrom project.
 *
 * Copyright (C) 2014 Google Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * Neither the name of Google or the names of contributors or
 * licensors may be used to endorse or promote products derived from this
 * software without specific prior written permission.
 *
 * This software is provided "AS IS," without a warranty of any kind.
 * ALL EXPRESS OR IMPLIED CONDITIONS, REPRESENTATIONS AND WARRANTIES,
 * INCLUDING ANY IMPLIED WARRANTY OF MERCHANTABILITY, FITNESS FOR A
 * PARTICULAR PURPOSE OR NON-INFRINGEMENT, ARE HEREBY EXCLUDED.
 * GOOGLE INC AND ITS LICENSORS SHALL NOT BE LIABLE
 * FOR ANY DAMAGES SUFFERED BY LICENSEE AS A RESULT OF USING, MODIFYING
 * OR DISTRIBUTING THIS SOFTWARE OR ITS DERIVATIVES.  IN NO EVENT WILL
 * GOOGLE OR ITS LICENSORS BE LIABLE FOR ANY LOST REVENUE, PROFIT OR DATA,
 * OR FOR DIRECT, INDIRECT, SPECIAL, CONSEQUENTIAL, INCIDENTAL OR
 * PUNITIVE DAMAGES, HOWEVER CAUSED AND REGARDLESS OF THE THEORY OF
 * LIABILITY, ARISING OUT OF THE USE OF OR INABILITY TO USE THIS SOFTWARE,
 * EVEN IF GOOGLE HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGES.
 *
 * s25fs.c - Helper functions for Spansion S25FL and S25FS SPI flash chips.
 * Uses 24 bit addressing for the FS chips and 32 bit addressing for the FL
 * chips (which is required by the overlayed sector size devices).
 * TODO: Implement fancy hybrid sector architecture helpers.
 */

#include <string.h>

#include "chipdrivers.h"
#include "spi.h"

#define CMD_RDAR	0x65
#define CMD_WRAR	0x71
/* FIXME: These lengths assume we're operating in legacy mode */
#define CMD_RDAR_LEN	4
#define CMD_WRAR_LEN	5

#define CMD_RSTEN	0x66
#define CMD_RST		0x99

#define CR1NV_ADDR	0x000002
#define CR1NV_TBPROT_O	(1 << 5)
#define CR3NV_ADDR	0x000004
#define CR3NV_20H_NV	(1 << 3)

static int s25fs_read_cr(const struct flashchip *flash, uint32_t addr)
{
	int result;
	uint8_t cfg;
	/* By default, 8 dummy cycles are necessary for variable-latency
	   commands such as RDAR (see CR2NV[3:0]). */
	unsigned char read_cr_cmd[] = {
					CMD_RDAR,
					(addr >> 16) & 0xff,
					(addr >> 8) & 0xff,
					(addr & 0xff),
					0x00, 0x00, 0x00, 0x00,
					0x00, 0x00, 0x00, 0x00,
	};

	result = spi_send_command(sizeof(read_cr_cmd), 1, read_cr_cmd, &cfg);
	if (result) {
		msg_cerr("%s failed during command execution at address 0x%x\n",
			__func__, addr);
		return result;
	}

	return cfg;
}

static int s25fs_write_cr(struct flashchip *flash, uint32_t addr, uint8_t data)
{
	int result;
	struct spi_command cmds[] = {
	{
		.writecnt	= JEDEC_WREN_OUTSIZE,
		.writearr	= (const unsigned char[]){ JEDEC_WREN },
		.readcnt	= 0,
		.readarr	= NULL,
	}, {
		.writecnt	= CMD_WRAR_LEN,
		.writearr	= (const unsigned char[]){
					CMD_WRAR,
					(addr >> 16) & 0xff,
					(addr >> 8) & 0xff,
					(addr & 0xff),
					data
				},
		.readcnt	= 0,
		.readarr	= NULL,
	}, {
		.writecnt	= 0,
		.writearr	= NULL,
		.readcnt	= 0,
		.readarr	= NULL,
	}};

	result = spi_send_multicommand(cmds);
	if (result) {
		msg_cerr("%s failed during command execution at address 0x%x\n",
			__func__, addr);
		return result;
	}

	/* Poll WIP bit while command is in progress. The datasheet specifies
	   Tw is typically 145ms, but can be up to 750ms. */
	while (spi_read_status_register() & JEDEC_RDSR_BIT_WIP)
		programmer_delay(1000 * 10);

	return 0;
}

static int s25fs_software_reset(struct flashchip *flash)
{
	int result;
	struct spi_command cmds[] = {
	{
		.writecnt	= 1,
		.writearr	= (const unsigned char[]){ CMD_RSTEN },
		.readcnt	= 0,
		.readarr	= NULL,
	}, {
		.writecnt	= 1,
		.writearr	= (const unsigned char[]){ CMD_RST },
		.readcnt	= 0,
		.readarr	= NULL,
	}, {
		.writecnt	= 0,
		.writearr	= NULL,
		.readcnt	= 0,
		.readarr	= NULL,
	}};

	result = spi_send_multicommand(cmds);
	if (result) {
		msg_cerr("%s failed during command execution\n", __func__);
		return result;
	}

	/* Allow time for reset command to execute. The datasheet specifies
	 * Trph = 35us, double that to be safe. */
	programmer_delay(35 * 2);

	return 0;
}

static int s25fs_restore_cr3nv(struct flashchip *flash, uint8_t cfg)
{
	int ret = 0;

	msg_cdbg("Restoring CR3NV value to 0x%02x\n", cfg);
	ret |= s25fs_write_cr(flash, CR3NV_ADDR, cfg);
	ret |= s25fs_software_reset(flash);
	return ret;
}

/* returns state of top/bottom block protection, or <0 to indicate error */
int s25fs_tbprot_o(const struct flashchip *flash)
{
	int cr1nv = s25fs_read_cr(flash, CR1NV_ADDR);

	if (cr1nv < 0)
		return -1;

	/*
	 * 1 = BP starts at bottom (low address)
	 * 0 = BP start at top (high address)
	 */
	return cr1nv & CR1NV_TBPROT_O ? 1 : 0;
}

int s25fs_block_erase_d8(struct flashchip *flash,
		unsigned int addr, unsigned int blocklen)
{
	unsigned char cfg;
	int result;
	static int cr3nv_checked = 0;

	struct spi_command erase_cmds[] = {
	{
		.writecnt	= JEDEC_WREN_OUTSIZE,
		.writearr	= (const unsigned char[]){ JEDEC_WREN },
		.readcnt	= 0,
		.readarr	= NULL,
	}, {
		.writecnt	= JEDEC_BE_D8_OUTSIZE,
		.writearr	= (const unsigned char[]){
					JEDEC_BE_D8,
					(addr >> 16) & 0xff,
					(addr >> 8) & 0xff,
					(addr & 0xff)
				},
		.readcnt	= 0,
		.readarr	= NULL,
	}, {
		.writecnt	= 0,
		.writearr	= NULL,
		.readcnt	= 0,
		.readarr	= NULL,
	}};

	/* Check if hybrid sector architecture is in use and, if so,
	 * switch to uniform sectors. */
	if (!cr3nv_checked) {
		cfg = s25fs_read_cr(flash, CR3NV_ADDR);
		if (!(cfg & CR3NV_20H_NV)) {
			s25fs_write_cr(flash, CR3NV_ADDR, cfg | CR3NV_20H_NV);
			s25fs_software_reset(flash);

			cfg = s25fs_read_cr(flash, CR3NV_ADDR);
			if (!(cfg & CR3NV_20H_NV)) {
				msg_cerr("%s: Unable to enable uniform "
					"block sizes.\n", __func__);
				return 1;
			}

			msg_cdbg("\n%s: CR3NV updated (0x%02x -> 0x%02x)\n",
					__func__, cfg,
					s25fs_read_cr(flash, CR3NV_ADDR));
			/* Restore CR3V when flashrom exits */
			register_chip_restore(s25fs_restore_cr3nv, flash, cfg);
		}

		cr3nv_checked = 1;
	}

	result = spi_send_multicommand(erase_cmds);
	if (result) {
		msg_cerr("%s failed during command execution at address 0x%x\n",
			__func__, addr);
		return result;
	}

	/* Wait until the Write-In-Progress bit is cleared. */
	while (spi_read_status_register() & JEDEC_RDSR_BIT_WIP)
		programmer_delay(10 * 1000);
	/* FIXME: Check the status register for errors. */
	return 0;
}

int s25fl_block_erase(struct flashchip *flash,
		      unsigned int addr, unsigned int blocklen)
{
	unsigned char status;
	int result;
	static int cr3nv_checked = 0;

	struct spi_command erase_cmds[] = {
		{
			.writecnt	= JEDEC_WREN_OUTSIZE,
			.writearr	= (const unsigned char[]){
				JEDEC_WREN
			},
			.readcnt	= 0,
			.readarr	= NULL,
		}, {
			.writecnt	= JEDEC_BE_DC_OUTSIZE,
			.writearr	= (const unsigned char[]){
				JEDEC_BE_DC,
				(addr >> 24) & 0xff,
				(addr >> 16) & 0xff,
				(addr >> 8) & 0xff,
				(addr & 0xff)
			},
			.readcnt	= 0,
			.readarr	= NULL,
		}, {
			.writecnt	= 0,
			.readcnt	= 0,
		}
	};

	result = spi_send_multicommand(erase_cmds);
	if (result) {
		msg_cerr("%s failed during command execution at address 0x%x\n",
			__func__, addr);
		return result;
	}

	/* Wait until the Write-In-Progress bit is cleared. */
	status = spi_read_status_register();
	while (status & JEDEC_RDSR_BIT_WIP) {
		programmer_delay(1000);
		status = spi_read_status_register();
	}
	return (status & JEDEC_RDSR_BIT_ERASE_ERR) != 0;
}


int probe_spi_big_spansion(struct flashchip *flash)
{
	static const unsigned char cmd = JEDEC_RDID;
	int ret;
	unsigned char dev_id[6]; /* We care only about 6 first bytes */

	ret = spi_send_command(sizeof(cmd), sizeof(dev_id), &cmd, dev_id);

	if (!ret) {
		int i;

		for (i = 0; i < sizeof(dev_id); i++)
			msg_gdbg(" 0x%02x", dev_id[i]);
		msg_gdbg(".\n");

		if (dev_id[0] == flash->manufacture_id) {
			union {
				uint8_t array[4];
				uint32_t whole;
			} model_id;

	/*
	 * The structure of the RDID output is as follows:
	 *
	 *     offset   value              meaning
	 *       00h     01h      Manufacturer ID for Spansion
	 *       01h     20h           128 Mb capacity
	 *       01h     02h           256 Mb capacity
	 *       02h     18h           128 Mb capacity
	 *       02h     19h           256 Mb capacity
	 *       03h     4Dh       Full size of the RDID output (ignored)
	 *       04h     00h       FS: 256-kB physical sectors
	 *       04h     01h       FS: 64-kB physical sectors
	 *       04h     00h       FL: 256-kB physical sectors
	 *       04h     01h       FL: Mix of 64-kB and 4KB overlayed sectors
	 *       05h     80h       FL family
	 *       05h     81h       FS family
	 *
	 * Need to use bytes 1, 2, 4, and 5 to properly identify one of eight
	 * possible chips:
	 *
	 * 2 types * 2 possible sizes * 2 possible sector layouts
	 *
	 */
			memcpy(model_id.array, dev_id + 1, 2);
			memcpy(model_id.array + 2, dev_id + 4, 2);
			if (be_to_cpu32(model_id.whole) == flash->model_id)
				return 1;
		}
	}
	return 0;
}
