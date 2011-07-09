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

#ifndef __TEGRA2_SPI_H__
#define __TEGRA2_SPI_H__

// ***************************************************************************
// Hardware BARs

#define TEGRA2_GPIO_BASE			0x6000D000
#define TEGRA2_SPI_BASE				0x7000C380
#define NV_ADDRESS_MAP_PPSB_CLK_RST_BASE	0x60006000
#define NV_ADDRESS_MAP_APB_MISC_BASE		0x70000000

// ***************************************************************************
// Clock/reset controller
#define CLK_RST_ENB_H_0_OFFSET  0x14
#define CLK_RST_ENB_H_0_SPI1    (1 << 11)

// ***************************************************************************
// GPIO controller

#define GPIO_OFF(port)		(((port / 4) * 128) + ((port % 4) * 4))
#define GPIO_CNF(port)		(gpio_base + GPIO_OFF(port) + 0x00)
#define GPIO_OE(port)		(gpio_base + GPIO_OFF(port) + 0x10)
#define GPIO_OUT(port)		(gpio_base + GPIO_OFF(port) + 0x20)
#define GPIO_IN(port)		(gpio_base + GPIO_OFF(port) + 0x30)
#define GPIO_INT_STA(port)	(gpio_base + GPIO_OFF(port) + 0x40)
#define GPIO_INT_ENB(port)	(gpio_base + GPIO_OFF(port) + 0x50)
#define GPIO_INT_LVL(port)	(gpio_base + GPIO_OFF(port) + 0x60)
#define GPIO_INT_CLR(port)	(gpio_base + GPIO_OFF(port) + 0x70)

#define	SPI_CMD_GO		(1 << 30)
#define	SPI_CMD_ACTIVE_SCLK	(1 << 26)
#define	SPI_CMD_CK_SDA		(1 << 21)
#define	SPI_CMD_ACTIVE_SDA	(1 << 18)
#define	SPI_CMD_CS_POL		(1 << 16)
#define	SPI_CMD_TXEN		(1 << 15)
#define	SPI_CMD_RXEN		(1 << 14)
#define	SPI_CMD_CS_VAL		(1 << 13)
#define	SPI_CMD_CS_SOFT		(1 << 12)
#define	SPI_CMD_CS_DELAY	(1 << 9)
#define	SPI_CMD_CS3_EN		(1 << 8)
#define	SPI_CMD_CS2_EN		(1 << 7)
#define	SPI_CMD_CS1_EN		(1 << 6)
#define	SPI_CMD_CS0_EN		(1 << 5)
#define	SPI_CMD_BIT_LENGTH	(1 << 4)
#define	SPI_CMD_BIT_LENGTH_MASK	0x0000001F

#define	SPI_STAT_BSY		(1 << 31)
#define	SPI_STAT_RDY		(1 << 30)
#define	SPI_STAT_RXF_FLUSH	(1 << 29)
#define	SPI_STAT_TXF_FLUSH	(1 << 28)
#define	SPI_STAT_RXF_UNR	(1 << 27)
#define	SPI_STAT_TXF_OVF	(1 << 26)
#define	SPI_STAT_RXF_EMPTY	(1 << 25)
#define	SPI_STAT_RXF_FULL	(1 << 24)
#define	SPI_STAT_TXF_EMPTY	(1 << 23)
#define	SPI_STAT_TXF_FULL	(1 << 22)
#define	SPI_STAT_SEL_TXRX_N	(1 << 16)
#define	SPI_STAT_CUR_BLKCNT	(1 << 15)

#endif	/* __TEGRA2_SPI_H__ */
