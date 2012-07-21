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

#define REG_EC_HWVER    0xff00
#define REG_EC_FWVER    0xff01
#define REG_EC_EDIID    0xff24
#define REG_8051_CTRL   0xff14

#define HWVER 0xa2
#define EDIID 0x02
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

/* Configurable ec command/status */
static unsigned int port_ec_command = 0x6c;
static unsigned int port_ec_data    = 0x68;
static unsigned int port_ec_bios    = 0x66;

static uint8_t ec_reset_cmd         = 0x59;
static uint8_t ec_reset_data        = 0xf2;

static uint8_t ec_restart_cmd       = 0x59;
static uint8_t ec_restart_data      = 0xf9;

static const uint16_t ec_status_buf = 0xf554;
static const uint8_t ec_is_stopping = 0xa5;
static const uint8_t ec_is_running  = 0;

static const uint8_t mask_input_buffer_full    = 2;
static const uint8_t mask_output_buffer_full   = 1;

static unsigned int port_io_base = 0xfd60;
const int port_ene_bank   = 1;
const int port_ene_offset = 2;
const int port_ene_data   = 3;

static void ec_command(uint8_t cmd, uint8_t data)
{
	struct timeval begin, now;

	/* Spin wait for EC input buffer empty */
	gettimeofday(&begin, NULL);
	while (INB(port_ec_command) & mask_input_buffer_full) {
		gettimeofday(&now, NULL);
		if ((now.tv_sec - begin.tv_sec) >= EC_COMMAND_TIMEOUT) {
			msg_pdbg("%s: buf not empty\n", __func__);
			return;
		}
	}
	/* Write command */
	OUTB(cmd, port_ec_command);

	/* Spin wait for EC input buffer empty */
	gettimeofday(&begin, NULL);
	while (INB(port_ec_command) & mask_input_buffer_full) {
		gettimeofday(&now, NULL);
		if ((now.tv_sec - begin.tv_sec) >= EC_COMMAND_TIMEOUT) {
			msg_pdbg("%s: buf not empty\n", __func__);
			return;
		}
	}
	/* Write data */
	OUTB(data, port_ec_data);
}

static uint8_t ene_read(uint16_t addr)
{
	uint8_t bank;
	uint8_t offset;
	uint8_t data;

	bank   = addr >> 8;
	offset = addr & 0xff;

	OUTB(bank,   port_io_base + port_ene_bank);
	OUTB(offset, port_io_base + port_ene_offset);
	data = INB(port_io_base + port_ene_data);

	return data;
}

static void ene_write(uint16_t addr, uint8_t data)
{
	uint8_t bank;
	uint8_t offset;

	bank   = addr >> 8;
	offset = addr & 0xff;

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
		INB(port_io_base + port_ene_bank);
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

static int ene_spi_send_command(unsigned int writecnt,
				unsigned int readcnt,
				const unsigned char *writearr,
				unsigned char *readarr)
{
	int i;

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

static int ene_enter_flash_mode(void)
{
	uint8_t reg;

	struct timeval begin, now;
	gettimeofday(&begin, NULL);

	/* EC prepare reset */
	ec_command(ec_reset_cmd, ec_reset_data);

	/* Spin wait for EC ready */
	while (ene_read(ec_status_buf) != ec_is_running) {
		gettimeofday(&now, NULL);
		if ((now.tv_sec - begin.tv_sec) >= EC_COMMAND_TIMEOUT) {
			msg_pdbg("%s: ec reset busy\n", __func__);
			return -1;
		}
	}

	/* Wait 1 second */
	sleep(1);

	/* Reset 8051 */
	reg = ene_read(REG_8051_CTRL);
	reg |= CPU_RESET;
	ene_write(REG_8051_CTRL, reg);

	return 0;
}

static int ene_leave_flash_mode(void *data)
{
	uint8_t reg;
	struct timeval begin, now;

	reg = ene_read(REG_8051_CTRL);
	reg &= ~CPU_RESET;
	ene_write(REG_8051_CTRL, reg);

	gettimeofday(&begin, NULL);
	/* EC restart */
	while (ene_read(ec_status_buf) != ec_is_running) {
		gettimeofday(&now, NULL);
		if ((now.tv_sec - begin.tv_sec) >= EC_RESTART_TIMEOUT) {
			msg_pdbg("%s: ec restart busy\n", __func__);
			return 1;
		}
	}

	msg_pdbg("%s: send ec restart\n", __func__);
	ec_command(ec_restart_cmd, ec_restart_data);

	/* Trigger ec to bios interrupt */
	sleep(1);
	OUTB(0x80, port_ec_bios);

	return 0;
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
	uint8_t hwver, ediid;

	msg_pdbg("%s\n", __func__);
	hwver = ene_read(REG_EC_HWVER);
	ediid = ene_read(REG_EC_EDIID);

	if (hwver != HWVER || ediid != EDIID) {
		msg_pdbg("ENE EC not found (probe failed) : hwver %02x ediid %02x\n",
				hwver, ediid);
		return 0;
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

