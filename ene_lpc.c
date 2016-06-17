/*
 * This file is part of the flashrom project.
 *
 * Copyright (C) 2012 Google Inc.
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
 */

#if defined(__i386__) || defined(__x86_64__)
#include <inttypes.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>

#include "chipdrivers.h"
#include "flash.h"
#include "programmer.h"
#include "spi.h"

/* Supported ENE ECs, ENE_LAST should always be LAST member */
enum ene_chip_id {
	ENE_KB932 = 0,
	ENE_KB94X,
	ENE_LAST
};

/* EC state */
enum ene_ec_state {
	EC_STATE_NORMAL,
	EC_STATE_IDLE,
	EC_STATE_RESET,
	EC_STATE_UNKNOWN
};

/* chip-specific parameters */
typedef struct {
	enum ene_chip_id chip_id;
	uint8_t          hwver;
	uint8_t          ediid;
	uint32_t         port_bios;
	uint32_t         port_ec_command;
	uint32_t         port_ec_data;
	uint8_t          ec_reset_cmd;
	uint8_t          ec_reset_data;
	uint8_t          ec_restart_cmd;
	uint8_t          ec_restart_data;
	uint8_t          ec_pause_cmd;
	uint8_t          ec_pause_data;
	uint16_t         ec_status_buf;
	uint8_t          ec_is_stopping;
	uint8_t          ec_is_running;
	uint8_t          ec_is_pausing;
	uint32_t         port_io_base;
} ene_chip;

/* table of supported chips + parameters */
static ene_chip ene_chips[] = {
	{ ENE_KB932,       /* chip_id */
	  0xa2, 0x02,      /* hwver + ediid */
	  0x66,            /* port_bios */
	  0x6c, 0x68,      /* port_ec_{command,data} */
	  0x59, 0xf2,      /* ec_reset_{cmd,data} */
	  0x59, 0xf9,      /* ec_restart_{cmd,data} */
	  0x59, 0xf1,      /* ec_pause_{cmd,data} */
	  0xf554,          /* ec_status_buf */
	  0xa5, 0x00,      /* ec_is_{stopping,running} masks */
	  0x33,            /* ec_is_pausing mask */
	  0xfd60 },        /* port_io_base */

	{ ENE_KB94X,       /* chip_id */
	  0xa3, 0x05,      /* hwver + ediid */
	  0x66,            /* port_bios */
	  0x66, 0x68,      /* port_ec_{command,data} */
	  0x7d, 0x10,      /* ec_reset_{cmd,data} */
	  0x7f, 0x10,      /* ec_restart_{cmd,data} */
	  0x7e, 0x10,      /* ec_pause_{cmd,data} */
	  0xf710,          /* ec_status_buf */
	  0x02, 0x00,      /* ec_is_{stopping,running} masks */
	  0x01,		   /* ec_is_pausing mask */
	  0x0380 },        /* port_io_base */
};

/* pointer to table entry of identified chip */
static ene_chip *found_chip;
/* current ec state */
static enum ene_ec_state ec_state = EC_STATE_NORMAL;

#define REG_EC_HWVER    0xff00
#define REG_EC_FWVER    0xff01
#define REG_EC_EDIID    0xff24
#define REG_8051_CTRL   0xff14
#define REG_EC_EXTCMD   0xff10

#define CPU_RESET 1

/* Hwardware registers */
#define REG_SPI_DATA    0xfeab
#define REG_SPI_COMMAND 0xfeac
#define REG_SPI_CONFIG  0xfead
#define CFG_CSn_FORCE_LOW        (1 << 4)
#define CFG_COMMAND_WRITE_ENABLE (1 << 3)
#define CFG_STATUS               (1 << 1)
#define CFG_ENABLE_BUSY_STATUS_CHECK (1 << 0)

/* Timeout */
#define EC_COMMAND_TIMEOUT 4
#define EC_RESTART_TIMEOUT 10
#define ENE_SPI_DELAY_CYCLE 4
#define EC_PAUSE_TIMEOUT 12
#define EC_RESET_TRIES 3

#define ENE_KB94X_PAUSE_WAKEUP_PORT  0x64

#define MASK_INPUT_BUFFER_FULL	2
#define MASK_OUTPUT_BUFFER_FULL	1

const int port_ene_bank   = 1;
const int port_ene_offset = 2;
const int port_ene_data   = 3;

static struct timeval pause_begin, pause_now;

static void ec_command(uint8_t cmd, uint8_t data)
{
	struct timeval begin, now;

	/* Spin wait for EC input buffer empty */
	gettimeofday(&begin, NULL);
	while (INB(found_chip->port_ec_command) & MASK_INPUT_BUFFER_FULL) {
		gettimeofday(&now, NULL);
		if ((now.tv_sec - begin.tv_sec) >= EC_COMMAND_TIMEOUT) {
			msg_pdbg("%s: buf not empty\n", __func__);
			return;
		}
	}

	/* Write command */
	OUTB(cmd, found_chip->port_ec_command);

	if (found_chip->chip_id == ENE_KB932) {
		/* Spin wait for EC input buffer empty */
		gettimeofday(&begin, NULL);
		while (INB(found_chip->port_ec_command) &
		       MASK_INPUT_BUFFER_FULL) {
			gettimeofday(&now, NULL);
			if ((now.tv_sec - begin.tv_sec) >=
			     EC_COMMAND_TIMEOUT) {
				msg_pdbg("%s: buf not empty\n", __func__);
				return;
			}
		}
		/* Write data */
		OUTB(data, found_chip->port_ec_data);
	}
}

static uint8_t ene_read(uint16_t addr)
{
	uint8_t  bank;
	uint8_t  offset;
	uint8_t  data;
	uint32_t port_io_base;

	bank   = addr >> 8;
	offset = addr & 0xff;
	port_io_base = found_chip->port_io_base;

	OUTB(bank,   port_io_base + port_ene_bank);
	OUTB(offset, port_io_base + port_ene_offset);
	data = INB(port_io_base + port_ene_data);

	return data;
}

static void ene_write(uint16_t addr, uint8_t data)
{
	uint8_t  bank;
	uint8_t  offset;
	uint32_t port_io_base;

	bank   = addr >> 8;
	offset = addr & 0xff;
	port_io_base = found_chip->port_io_base;

	OUTB(bank,   port_io_base + port_ene_bank);
	OUTB(offset, port_io_base + port_ene_offset);

	OUTB(data, port_io_base + port_ene_data);
}

/**
 * wait_cycles, wait for n LPC bus clock cycles
 *
 * @param       n: number of LPC cycles to wait
 * @return      void
 */
void wait_cycles(int n)
{
	while (n--)
		INB(found_chip->port_io_base + port_ene_bank);
}

static int is_spicmd_write(uint8_t cmd)
{
	switch (cmd) {
	case JEDEC_WREN:
		/* Chip Write Enable */
	case JEDEC_EWSR:
		/* Write Status Enable */
	case JEDEC_CE_60:
		/* Chip Erase 0x60 */
	case JEDEC_CE_C7:
		/* Chip Erase 0xc7 */
	case JEDEC_BE_52:
		/* Block Erase 0x52 */
	case JEDEC_BE_D8:
		/* Block Erase 0xd8 */
	case JEDEC_BE_D7:
		/* Block Erase 0xd7 */
	case JEDEC_SE:
		/* Sector Erase */
	case JEDEC_BYTE_PROGRAM:
		/* Write memory byte */
	case JEDEC_AAI_WORD_PROGRAM:
		/* Write AAI word */
		return 1;
	}
	return 0;
}

static void ene_spi_start(void)
{
	int cfg;

	cfg = ene_read(REG_SPI_CONFIG);
	cfg |= CFG_CSn_FORCE_LOW;
	cfg |= CFG_COMMAND_WRITE_ENABLE;
	ene_write(REG_SPI_CONFIG, cfg);

	wait_cycles(ENE_SPI_DELAY_CYCLE);
}

static void ene_spi_end(void)
{
	int cfg;

	cfg = ene_read(REG_SPI_CONFIG);
	cfg &= ~CFG_CSn_FORCE_LOW;
	cfg |= CFG_COMMAND_WRITE_ENABLE;
	ene_write(REG_SPI_CONFIG, cfg);

	wait_cycles(ENE_SPI_DELAY_CYCLE);
}

static int ene_spi_wait(void)
{
	struct timeval begin, now;

	gettimeofday(&begin, NULL);
	while(ene_read(REG_SPI_CONFIG) & CFG_STATUS) {
		gettimeofday(&now, NULL);
		if ((now.tv_sec - begin.tv_sec) >= EC_COMMAND_TIMEOUT) {
			msg_pdbg("%s: spi busy\n", __func__);
			return 1;
		}
	}
	return 0;
}

static int ene_pause_ec(void)
{
	struct timeval begin, now;

	if (!found_chip->ec_pause_cmd)
		return -1;

	/* EC prepare pause */
	ec_command(found_chip->ec_pause_cmd, found_chip->ec_pause_data);

	gettimeofday(&begin, NULL);
	/* Spin wait for EC ready */
	while (ene_read(found_chip->ec_status_buf) !=
	       found_chip->ec_is_pausing) {
		gettimeofday(&now, NULL);
		if ((now.tv_sec - begin.tv_sec) >=
		     EC_COMMAND_TIMEOUT) {
			msg_pdbg("%s: unable to pause ec\n", __func__);
			return -1;
		}
	}


	gettimeofday(&pause_begin, NULL);
	ec_state = EC_STATE_IDLE;
	return 0;
}

static int ene_resume_ec(void)
{
	struct timeval begin, now;


	if (found_chip->chip_id == ENE_KB94X)
		OUTB(0xff, ENE_KB94X_PAUSE_WAKEUP_PORT);
	else
		/* Trigger 8051 interrupt to resume */
		ene_write(REG_EC_EXTCMD, 0xff);

	gettimeofday(&begin, NULL);
	while (ene_read(found_chip->ec_status_buf) !=
	       found_chip->ec_is_running) {
		gettimeofday(&now, NULL);
		if ((now.tv_sec - begin.tv_sec) >=
		     EC_COMMAND_TIMEOUT) {
			msg_pdbg("%s: unable to resume ec\n", __func__);
			return -1;
		}
	}

	ec_state = EC_STATE_NORMAL;
	return 0;
}

static int ene_pause_timeout_check(void)
{
	gettimeofday(&pause_now, NULL);
	if ((pause_now.tv_sec - pause_begin.tv_sec) >=
			     EC_PAUSE_TIMEOUT) {
		if(ene_resume_ec() == 0)
			ene_pause_ec();

	}
	return 0;
}

static int ene_reset_ec(void)
{
	uint8_t reg;

	struct timeval begin, now;
	gettimeofday(&begin, NULL);

	/* EC prepare reset */
	ec_command(found_chip->ec_reset_cmd, found_chip->ec_reset_data);

	/* Spin wait for EC ready */
	while (ene_read(found_chip->ec_status_buf) !=
	       found_chip->ec_is_stopping) {
		gettimeofday(&now, NULL);
		if ((now.tv_sec - begin.tv_sec) >=
		     EC_COMMAND_TIMEOUT) {
			msg_pdbg("%s: unable to reset ec\n", __func__);
			return -1;
		}
	}

	/* Wait 1 second */
	sleep(1);

	/* Reset 8051 */
	reg = ene_read(REG_8051_CTRL);
	reg |= CPU_RESET;
	ene_write(REG_8051_CTRL, reg);

	ec_state = EC_STATE_RESET;
	return 0;
}

static int ene_enter_flash_mode(void)
{
	if (ene_pause_ec())
		return ene_reset_ec();
	return 0;
}

static int ene_spi_send_command(const struct flashctx *flash,
				unsigned int writecnt,
				unsigned int readcnt,
				const unsigned char *writearr,
				unsigned char *readarr)
{
	int i;
	int tries = EC_RESET_TRIES;

	if (ec_state == EC_STATE_IDLE && is_spicmd_write(writearr[0])) {
		do {
			/* Enter reset mode if we need to write/erase */
			if (ene_resume_ec())
				continue;

			if (!ene_reset_ec())
				break;
		} while (--tries > 0);

		if (!tries) {
			msg_perr("%s: EC failed reset, skipping write\n",
				__func__);
			ec_state = EC_STATE_IDLE;
			return 1;
		}
	}
	else if(found_chip->chip_id == ENE_KB94X && ec_state == EC_STATE_IDLE)
		ene_pause_timeout_check();

	ene_spi_start();

	for (i = 0; i < writecnt; i++) {
		ene_write(REG_SPI_COMMAND, writearr[i]);
		if (ene_spi_wait()) {
			msg_pdbg("%s: write count %d\n", __func__, i);
			return 1;
		}
	}

	for (i = 0; i < readcnt; i++) {
		/* Push data by clock the serial bus */
		ene_write(REG_SPI_COMMAND, 0);
		if (ene_spi_wait()) {
			msg_pdbg("%s: read count %d\n", __func__, i);
			return 1;
		}
		readarr[i] = ene_read(REG_SPI_DATA);
		if (ene_spi_wait()) {
			msg_pdbg("%s: read count %d\n", __func__, i);
			return 1;
		}
	}

	ene_spi_end();
	return 0;
}

static int ene_leave_flash_mode(void *data)
{
	int rv = 0;
	uint8_t reg;
	struct timeval begin, now;

	if (ec_state == EC_STATE_RESET) {
		reg = ene_read(REG_8051_CTRL);
		reg &= ~CPU_RESET;
		ene_write(REG_8051_CTRL, reg);

		gettimeofday(&begin, NULL);
		/* EC restart */
		while (ene_read(found_chip->ec_status_buf) !=
		       found_chip->ec_is_running) {
			gettimeofday(&now, NULL);
			if ((now.tv_sec - begin.tv_sec) >=
			     EC_RESTART_TIMEOUT) {
				msg_pdbg("%s: ec restart busy\n", __func__);
				rv = 1;
				goto exit;
			}
		}
		msg_pdbg("%s: send ec restart\n", __func__);
		ec_command(found_chip->ec_restart_cmd,
		           found_chip->ec_restart_data);

		ec_state = EC_STATE_NORMAL;
		rv = 0;
		goto exit;
	}

	rv = ene_resume_ec();

exit:
	/*
	 * Trigger ec interrupt after pause/reset by sending 0x80
	 * to bios command port.
	 */
	OUTB(0x80, found_chip->port_bios);
	return rv;
}

static const struct spi_programmer spi_programmer_ene = {
	.type = SPI_CONTROLLER_ENE,
	.max_data_read = 256,
	.max_data_write = 256,
	.command = ene_spi_send_command,
	.multicommand = default_spi_send_multicommand,
	.read = default_spi_read,
	.write_256 = default_spi_write_256,
};

int ene_probe_spi_flash(const char *name)
{
	uint8_t hwver, ediid, i;

	if (alias && alias->type != ALIAS_EC)
		return 1;

	msg_pdbg("%s\n", __func__);

	for (i = 0; i < ENE_LAST; ++i) {
		found_chip = &ene_chips[i];

		hwver = ene_read(REG_EC_HWVER);
		ediid = ene_read(REG_EC_EDIID);

		if(hwver == ene_chips[i].hwver &&
		   ediid == ene_chips[i].ediid) {
			break;
		}
	}

	if (i == ENE_LAST) {
		msg_pdbg("ENE EC not found (probe failed)\n");
		return 1;
	}

	/* TODO: probe the EC stop protocol
	 *
	 * Compal - ec_command(0x41, 0xa1) returns 43 4f 4d 50 41 4c 9c
	 */


	if (register_shutdown(ene_leave_flash_mode, NULL))
		return 1;

	ene_enter_flash_mode();

	buses_supported |= BUS_LPC;
	register_spi_programmer(&spi_programmer_ene);
	msg_pdbg("%s: successfully initialized ene\n", __func__);
	return 0;
}

#endif /* __i386__ || __x86_64__ */

