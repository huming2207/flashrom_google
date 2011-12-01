/*
 * This file is part of the flashrom project.
 *
 * Copyright (C) 2010 NVIDIA Corporation
 * Copyright (C) 2011 Google Inc
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

#if defined(__arm__)
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "flash.h"
#include "programmer.h"
#include "tegra2_spi.h"

static void *gpio_base, *clkrst_base, *apbmisc_base, *spi_base;

#define SPI_TIMEOUT 50000  /* 100ms = 50000 * 2us */

/* On Seaboard: GPIO_PI3 = Port I = 8, bit = 3 */
#define UART_DISABLE_PORT 8
#define UART_DISABLE_BIT 3

/* Config port:bit as GPIO, not SFPIO (default) */
static void __set_config(unsigned port, unsigned bit, int type)
{
	u32 u;

	msg_pdbg("%s: port = %d, bit = %d, %s\n", __func__,
	         port, bit, type ? "GPIO" : "SFPIO");

	u = mmio_readl(GPIO_CNF(port));
	if (type)  /* GPIO */
		u |= 1 << bit;
	else
		u &= ~(1 << bit);
	rmmio_writel(u, GPIO_CNF(port));
}

/* Config GPIO port:bit as input or output (OE) */
static void __set_direction(unsigned port, unsigned bit, int output)
{
	u32 u;

	msg_pdbg("%s: port = %d, bit = %d, %s\n", __func__,
	         port, bit, output ? "OUT" : "IN");

	u = mmio_readl(GPIO_OE(port));
	if (output)
		u |= 1 << bit;
	else
		u &= ~(1 << bit);
	rmmio_writel(u, GPIO_OE(port));
}

/* set GPIO OUT port:bit as 0 or 1 */
static void __set_level(unsigned port, unsigned bit, int high)
{
	u32 u;

	msg_pdbg("%s: port = %d, bit %d == %d\n", __func__,
	         port, bit, high);

	u = mmio_readl(GPIO_OUT(port));
	if (high)
		u |= 1 << bit;
	else
		u &= ~(1 << bit);
	rmmio_writel(u, GPIO_OUT(port));
}

/* set GPIO port:bit as an output, with polarity 'value' */
static int tg2_gpio_direction_output(unsigned port, unsigned bit, int value)
{
	msg_pdbg("%s: port = %d, bit = %d, value = %d\n",
	         __func__, port, bit, value);

	/* Configure as a GPIO */
	__set_config(port, bit, 1);

	/* Configure GPIO output value. */
	__set_level(port, bit, value);

	/* Configure GPIO direction as output. */
	__set_direction(port, bit, 1);

	return 0;
}

static void spi_cs_activate(void)
{
	uint32_t *spi_cmd = (uint32_t *)spi_base;

	/*
	 * CS is negated on Tegra, so drive a 1 to get a 0
	 */
	mmio_writel(mmio_readl(spi_cmd) | SPI_CMD_CS_VAL, spi_cmd);
	msg_pspew("%s: CS driven %s\n", __func__,
	          (mmio_readl(spi_cmd) & SPI_CMD_CS_VAL) ? "LOW" : "HIGH");
}

static void spi_cs_deactivate(void)
{
	uint32_t *spi_cmd = (uint32_t *)spi_base;


	/*
	 * CS is negated on Tegra, so drive a 0 to get a 1
	*/
	mmio_writel(mmio_readl(spi_cmd) & ~SPI_CMD_CS_VAL, spi_cmd);
	msg_pspew("%s: CS driven %s\n", __func__,
	          (mmio_readl(spi_cmd) & SPI_CMD_CS_VAL) ? "LOW" : "HIGH");
}

/* Helper function to calculate the clock cycle in this round.
 * Also updates the byte count remaining to be used this round.
 *
 * For example, we want to write 6 bytes to SPI and then read 5 bytes back.
 *
 * +---+---+---+---+---+---+
 * | W | W | W | W | W | W |
 * +---+---+---+---+---+---+---+---+---+---+---+
 *                         | R | R | R | R | R |
 *                         +---+---+---+---+---+
 * |<-- round 0 -->|
 *                 |<-- round 1 -->|
 *                                 |<-- round 2 -->|
 *
 * So that the continuous calling this function would get:
 *
 * round| RET| writecnt  readcnt  bits  to_write  to_read
 * -----+----+---------------------------------------------
 * INIT |    |     6        5
 *    0 |  1 |     2        5      32       4        0
 *    1 |  1 |     0        3      32       2        2
 *    2 |  1 |     0        0      24       0        3
 *    3 |  0 |     -        -       -       -        -
 *
 */
int next4Bytes(uint32_t *writecnt, uint32_t *readcnt, int *num_bits,
               uint32_t *to_write, uint32_t *to_read) {
	assert(writecnt);
	assert(readcnt);
	assert(num_bits);
	assert(to_write);
	assert(to_read);

	*to_write = min(*writecnt, 4);
	*to_read = min(*readcnt, 4 - *to_write);

	*writecnt -= *to_write;
	*readcnt -= *to_read;

	*num_bits = (*to_write + *to_read) * 8;

	if (*num_bits)
		return 1;  /* need to be called again. */
	else
		return 0;  /* handled write and read requests. */
}

/*
 * Tegra2 FIFO design is ... interesting. For example, you want to Tx 2 bytes:
 *
 *                +---+---+
 *    writearr[]: | 0 | 1 |
 *                +---+---+
 *                    \   \
 *                     \   \
 *                      \   \
 *                       \   \
 *                        \   \
 *             31 +---+---+---+---+ 0
 *  tmp(32-bits): | X | X | 0 | 1 |
 *                +---+---+---+---+ LSB
 *
 * It is neither little or big endian. The first bit for SPI controller to
 * transfer is the bit 15 in FIFO, neither bit 31 or bit 0, because the transfer
 * length is 16 bits (2 bytes).
 *
 * Rx follows the similar rule. First bit comes at bit 0, and the whole FIFO
 * left-shifts 1 bit for every bit comes in. Hence, after reading 3 bytes,
 * the first coming bit will reside in bit 23.
 */
int tegra2_spi_send_command(unsigned int writecnt,
                            unsigned int readcnt,
                            const unsigned char *writearr,
                            unsigned char *readarr)
{
	int retval = 0;
	uint8_t *delayed_msg = NULL;  /* for UART is disabled. */
	uint32_t *spi_cmd = (uint32_t *)spi_base;
	uint32_t *spi_sts = (uint32_t *)(spi_base + 0x04);
	uint32_t *tx_fifo = (uint32_t *)(spi_base + 0x10);
	uint32_t *rx_fifo = (uint32_t *)(spi_base + 0x20);
	uint32_t status;
	uint32_t to_write, to_read;  /*  byte counts to fill FIFO. */
	uint32_t bits;  /* bit count to tell SPI controller. */

	mmio_writel(mmio_readl(spi_sts), spi_sts);
	mmio_writel(mmio_readl(spi_cmd) | SPI_CMD_TXEN | SPI_CMD_RXEN, spi_cmd);
	spi_cs_activate();

	while (next4Bytes(&writecnt, &readcnt, &bits, &to_write, &to_read)) {
		int i;
		uint32_t tmp;
		uint32_t tm;  /* timeout counter */

		/* prepare Tx FIFO */
		for (tmp = 0, i = 0; i < to_write; ++i) {
			tmp |= (*writearr++) << ((bits / 8 - 1 - i) * 8);
		}
		mmio_writel(tmp, tx_fifo);

		/* Kick the SCLK running: Shift out TX FIFO, and receive RX. */
		mmio_writel(mmio_readl(spi_cmd) & ~SPI_CMD_BIT_LENGTH_MASK,
		            spi_cmd);
		mmio_writel(mmio_readl(spi_cmd) | (bits - 1),
		            spi_cmd);
		mmio_writel(mmio_readl(spi_cmd) | SPI_CMD_GO, spi_cmd);

		/* Wait for controller completes the task. */
		for (tm = 0; tm < SPI_TIMEOUT; ++tm) {
			if (((status = mmio_readl(spi_sts)) &
			    (SPI_STAT_BSY | SPI_STAT_RDY)) == SPI_STAT_RDY)
				break;
			/* We setup clock to 6MHz, so that we shall come back
			 * after: 1 / (6MHz) * 8 bits = 1.333us
			 */
			programmer_delay(2);
		}
		mmio_writel(mmio_readl(spi_sts) | SPI_STAT_RDY, spi_sts);

		/* Since the UART is disabled here, we delay printing the
		 * message until spi_cs_deactivate() is called.
		 */
		if (tm >= SPI_TIMEOUT) {
			static uint8_t err[256];
			retval = -1;
			snprintf(err, sizeof(err),
			         "%s():%d BSY&RDY timeout, status = 0x%08x\n",
			         __func__, __LINE__, status);
			delayed_msg = err;
			break;
		}

		/* read RX FIFO */
		tmp = mmio_readl(rx_fifo);
		for (i = 0; i < to_read; ++i) {
			*readarr++ = (tmp >> ((to_read - 1 - i) * 8)) & 0xFF;
		}
	}

	mmio_writel(status = mmio_readl(spi_sts), spi_sts);

	spi_cs_deactivate();
	if (delayed_msg) {
		msg_perr("%s\n", delayed_msg);
	}

	return retval;
}

int tegra2_spi_read(struct flashchip *flash, uint8_t *buf, int start, int len)
{
	return spi_read_chunked(flash, buf, start, len,
	                        spi_programmer->max_data_read);
}

int tegra2_spi_write(struct flashchip *flash, uint8_t *buf, int start, int len)
{
	return spi_write_chunked(flash, buf, start, len,
	                         spi_programmer->max_data_write);
}

/* Unmap register spaces */
int tegra2_spi_shutdown(void *data)
{
	physunmap(gpio_base, 4096);
	physunmap(clkrst_base, 4096);
	physunmap(apbmisc_base, 4096);
	physunmap(spi_base - 0x380, 4096);
	return 0;
}

static const struct spi_programmer spi_programmer_tegra2 = {
	.type = SPI_CONTROLLER_TEGRA2,
	/* FIFO depth is 32, packets can be up to 32-bits in length. */
	.max_data_read = 128,
	.max_data_write = 128,
	.command = tegra2_spi_send_command,
	.multicommand = default_spi_send_multicommand,
	.read = tegra2_spi_read,
	.write_256 = tegra2_spi_write,
};

/* Map register spaces */
int tegra2_spi_init(void)
{
	u32 val;
	uint32_t *spi_cmd;
	uint32_t *spi_sts;

	buses_supported = BUS_SPI;
	register_spi_programmer(&spi_programmer_tegra2);

	gpio_base = physmap("GPIO", TEGRA2_GPIO_BASE, 4096);
	clkrst_base = physmap("CLK/RST", NV_ADDRESS_MAP_PPSB_CLK_RST_BASE,
	                      4096);
	apbmisc_base = physmap("APB MISC", NV_ADDRESS_MAP_APB_MISC_BASE, 4096);
	/* non-page offset */
	spi_base = physmap("SPI", TEGRA2_SPI_BASE - 0x380, 4096) + 0x380;

	register_shutdown(tegra2_spi_shutdown, NULL);

	flashbase = 0;  /* FIXME: to make sanity check happy. */

	/* Init variables */
	spi_cmd = (uint32_t *)spi_base;
	spi_sts = (uint32_t *)(spi_base + 0x04);

	/*
	 * SPI reset/clocks init - reset SPI, set clocks, release from reset
	 */

	/* SWE_SPI1_RST: Hold SPI controller 1 in reset */
	val = mmio_readl(clkrst_base + 0x08) | 0x800;
	rmmio_writel(val, (clkrst_base + 0x08));
	msg_pdbg("%s: ClkRst = %08x\n", __func__, val);

	/* CLK_ENB_SPI1: Enable clock to SPI 1 Controller */
	val = mmio_readl(clkrst_base + 0x14) | 0x800;
	rmmio_writel(val, (clkrst_base + 0x14));
	msg_pdbg("%s: ClkEnable = %08x\n", __func__, val);

	/* Change default SPI clock from 12MHz to 6MHz, same as BootROM */
	val = mmio_readl(clkrst_base + 0x114) | 0x2;
	rmmio_writel(val, (clkrst_base + 0x114));
	msg_pdbg("%s: ClkSrc = %08x\n", __func__, val);

	/* SWE_SPI1_RST: Clear SPI1 reset bit (take it out of reset),
	 * use regular mmio_writel (restore callback already registered) */
	val =  mmio_readl(clkrst_base + 0x08) & 0xFFFFF7FF;
	mmio_writel(val, (clkrst_base + 0x08));
	msg_pdbg("%s: ClkRst final = %08x\n", __func__, val);

	/* Clear stale status here */
	mmio_writel(SPI_STAT_RDY | SPI_STAT_RXF_FLUSH | SPI_STAT_TXF_FLUSH |
	            SPI_STAT_RXF_UNR | SPI_STAT_TXF_OVF, spi_sts);
	msg_pdbg("%s: STATUS = %08x\n", __func__, mmio_readl(spi_sts));

	/*
	 * Use sw-controlled CS, so we can clock in data after ReadID, etc.
	 */
	mmio_writel(mmio_readl(spi_cmd) | SPI_CMD_CS_SOFT, spi_cmd);
	msg_pdbg("%s: COMMAND = %08x\n", __func__, mmio_readl(spi_cmd));

	/*
	 * SPI pins on Tegra2 are muxed - change pinmux last due to UART issue
	 */
	val = mmio_readl(apbmisc_base + 0x88) | 0xC0000000;
	rmmio_writel(val, (apbmisc_base + 0x88));
	msg_pdbg("%s: PinMuxRegC = %08x\n", __func__, val);

	/* Set Z_LSPI to non-tristate mode */
	val = mmio_readl(apbmisc_base + 0x20) & 0xFFFFFFFE;
	rmmio_writel(val, (apbmisc_base + 0x20));
	msg_pdbg("%s: TriStateReg = %08x\n", __func__, val);

	/* delay 100ms so that all chars in buffer (1KB) can be flushed. */
	programmer_delay(100000);

	/*
	 * We need to dynamically change the pinmux, shared w/UART RXD/CTS!
	 */
	val = mmio_readl(apbmisc_base + 0x84) | 0x0000000C;  /* 3 = SFLASH */
	rmmio_writel(val, (apbmisc_base + 0x84));
	msg_pdbg("%s: PinMuxRegB = %08x\n", __func__, val);

	/* On Seaboard, MOSI/MISO are shared w/UART.
	 * Use GPIO I3 (UART_DISABLE) to tristate UART during SPI activity.
	 * Enable UART later (cs_deactivate) so we can use it for U-Boot comms.
	 */
	msg_pdbg("%s: DISABLING UART!\n", __func__);
	tg2_gpio_direction_output(UART_DISABLE_PORT, UART_DISABLE_BIT, 1);

	return 0;
}
#endif
