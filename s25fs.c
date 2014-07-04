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
 * s25fs.c - Helper functions for Spansion S25FS SPI flash chips. This
 * currently assumes that we are operating the chip in legacy 24-bit addressing
 * mode and are working with uniform sectors.
 * TODO: Implement fancy 32-bit addressing and hybrid sector architecture
 * helpers.
 */

#include "chipdrivers.h"
#include "spi.h"

#define CMD_RDAR	0x65
#define CMD_WRAR	0x71
/* FIXME: These lengths assume we're operating in legacy mode */
#define CMD_RDAR_LEN	4
#define CMD_WRAR_LEN	5

#define CMD_RSTEN	0x66
#define CMD_RST		0x99

#define CR3NV_ADDR	0x000004
#define CR3NV_20H_NV	(1 << 3)

static int sf25s_read_cr(struct flashchip *flash, uint32_t addr)
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

static int sf25s_write_cr(struct flashchip *flash, uint32_t addr, uint8_t data)
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
	ret |= sf25s_write_cr(flash, CR3NV_ADDR, cfg);
	ret |= s25fs_software_reset(flash);
	return ret;
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
		cfg = sf25s_read_cr(flash, CR3NV_ADDR);
		if (!(cfg & CR3NV_20H_NV)) {
			sf25s_write_cr(flash, CR3NV_ADDR, cfg | CR3NV_20H_NV);
			s25fs_software_reset(flash);

			cfg = sf25s_read_cr(flash, CR3NV_ADDR);
			if (!(cfg & CR3NV_20H_NV)) {
				msg_cerr("%s: Unable to enable uniform "
					"block sizes.\n", __func__);
				return 1;
			}

			msg_cdbg("\n%s: CR3NV updated (0x%02x -> 0x%02x)\n",
					__func__, cfg,
					sf25s_read_cr(flash, CR3NV_ADDR));
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
