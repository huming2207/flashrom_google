/*
 * This file is part of the flashrom project.
 *
 * Copyright (C) 2009,2010 Carl-Daniel Hailfinger
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

#include <string.h>
#include <stdlib.h>
#include "flash.h"
#include "chipdrivers.h"
#include "programmer.h"
#include "flashchips.h"

/* Remove the #define below if you don't want SPI flash chip emulation. */
#define EMULATE_SPI_CHIP 1

#if EMULATE_SPI_CHIP
#define EMULATE_CHIP 1
#include "spi.h"
#endif

#if EMULATE_CHIP
#include <sys/types.h>
#include <sys/stat.h>

#if EMULATE_SPI_CHIP
/* The name of variable-size virtual chip. A 4MB flash example:
 *   flashrom -p dummy:emulate=VARIABLE_SIZE,size=4194304
 */
#define VARIABLE_SIZE_CHIP_NAME "VARIABLE_SIZE"
#endif
#endif

#if EMULATE_CHIP
static uint8_t *flashchip_contents = NULL;
enum emu_chip {
	EMULATE_NONE,
	EMULATE_ST_M25P10_RES,
	EMULATE_SST_SST25VF040_REMS,
	EMULATE_SST_SST25VF032B,
	EMULATE_VARIABLE_SIZE,
};
static enum emu_chip emu_chip = EMULATE_NONE;
static char *emu_persistent_image = NULL;
static int emu_chip_size = 0;
#if EMULATE_SPI_CHIP
static int emu_max_byteprogram_size = 0;
static int emu_max_aai_size = 0;
static int emu_jedec_se_size = 0;
static int emu_jedec_be_52_size = 0;
static int emu_jedec_be_d8_size = 0;
static int emu_jedec_ce_60_size = 0;
static int emu_jedec_ce_c7_size = 0;
#endif
#endif

static int spi_write_256_chunksize = 256;

int dummy_init(void)
{
	char *bustext = NULL;
	char *tmp = NULL;
#if EMULATE_CHIP
	struct stat image_stat;
#if EMULATE_SPI_CHIP
	int size = -1;  /* size for generic chip */
#endif
#endif

	msg_pspew("%s\n", __func__);

	bustext = extract_programmer_param("bus");
	msg_pdbg("Requested buses are: %s\n", bustext ? bustext : "default");
	if (!bustext)
		bustext = strdup("parallel+lpc+fwh+spi");
	/* Convert the parameters to lowercase. */
	tolower_string(bustext);

	buses_supported = CHIP_BUSTYPE_NONE;
	if (strstr(bustext, "parallel")) {
		buses_supported |= CHIP_BUSTYPE_PARALLEL;
		msg_pdbg("Enabling support for %s flash.\n", "parallel");
	}
	if (strstr(bustext, "lpc")) {
		buses_supported |= CHIP_BUSTYPE_LPC;
		msg_pdbg("Enabling support for %s flash.\n", "LPC");
	}
	if (strstr(bustext, "fwh")) {
		buses_supported |= CHIP_BUSTYPE_FWH;
		msg_pdbg("Enabling support for %s flash.\n", "FWH");
	}
	if (strstr(bustext, "spi")) {
		buses_supported |= CHIP_BUSTYPE_SPI;
		spi_controller = SPI_CONTROLLER_DUMMY;
		msg_pdbg("Enabling support for %s flash.\n", "SPI");
	}
	if (buses_supported == CHIP_BUSTYPE_NONE)
		msg_pdbg("Support for all flash bus types disabled.\n");
	free(bustext);

	tmp = extract_programmer_param("spi_write_256_chunksize");
	if (tmp) {
		spi_write_256_chunksize = atoi(tmp);
		free(tmp);
		if (spi_write_256_chunksize < 1) {
			msg_perr("invalid spi_write_256_chunksize\n");
			return 1;
		}
	}

#if EMULATE_CHIP
#if EMULATE_SPI_CHIP
	tmp = extract_programmer_param("size");
	if (tmp) {
		int multiplier = 1;
		if (strlen(tmp)) {
			int remove_last_char = 1;
			switch (tmp[strlen(tmp) - 1]) {
			case 'k': case 'K':
				multiplier = 1024;
				break;
			case 'm': case 'M':
				multiplier = 1024 * 1024;
				break;
			default:
				remove_last_char = 0;
				break;
			}
			if (remove_last_char) tmp[strlen(tmp) - 1] = '\0';
		}
		size = atoi(tmp) * multiplier;
	}
#endif

	tmp = extract_programmer_param("emulate");
	if (!tmp) {
		msg_pdbg("Not emulating any flash chip.\n");
		/* Nothing else to do. */
		return 0;
	}
#if EMULATE_SPI_CHIP
	if (!strcmp(tmp, "M25P10.RES")) {
		emu_chip = EMULATE_ST_M25P10_RES;
		emu_chip_size = 128 * 1024;
		emu_max_byteprogram_size = 128;
		emu_max_aai_size = 0;
		emu_jedec_se_size = 0;
		emu_jedec_be_52_size = 0;
		emu_jedec_be_d8_size = 32 * 1024;
		emu_jedec_ce_60_size = 0;
		emu_jedec_ce_c7_size = emu_chip_size;
		msg_pdbg("Emulating ST M25P10.RES SPI flash chip (RES, page "
			 "write)\n");
	}
	if (!strcmp(tmp, "SST25VF040.REMS")) {
		emu_chip = EMULATE_SST_SST25VF040_REMS;
		emu_chip_size = 512 * 1024;
		emu_max_byteprogram_size = 1;
		emu_max_aai_size = 0;
		emu_jedec_se_size = 4 * 1024;
		emu_jedec_be_52_size = 32 * 1024;
		emu_jedec_be_d8_size = 0;
		emu_jedec_ce_60_size = emu_chip_size;
		emu_jedec_ce_c7_size = 0;
		msg_pdbg("Emulating SST SST25VF040.REMS SPI flash chip (REMS, "
			 "byte write)\n");
	}
	if (!strcmp(tmp, "SST25VF032B")) {
		emu_chip = EMULATE_SST_SST25VF032B;
		emu_chip_size = 4 * 1024 * 1024;
		emu_max_byteprogram_size = 1;
		emu_max_aai_size = 2;
		emu_jedec_se_size = 4 * 1024;
		emu_jedec_be_52_size = 32 * 1024;
		emu_jedec_be_d8_size = 64 * 1024;
		emu_jedec_ce_60_size = emu_chip_size;
		emu_jedec_ce_c7_size = emu_chip_size;
		msg_pdbg("Emulating SST SST25VF032B SPI flash chip (RDID, AAI "
			 "write)\n");
	}
	if (!strncmp(tmp, VARIABLE_SIZE_CHIP_NAME,
	                  strlen(VARIABLE_SIZE_CHIP_NAME))) {
		if (size == -1) {
			msg_perr("%s: the size parameter is not given.\n",
			         __func__);
			free(tmp);
			return 1;
		}
		emu_chip = EMULATE_VARIABLE_SIZE;
		emu_chip_size = size;
		emu_max_byteprogram_size = 256;
		emu_max_aai_size = 0;
		emu_jedec_se_size = 4 * 1024;
		emu_jedec_be_52_size = 32 * 1024;
		emu_jedec_be_d8_size = 64 * 1024;
		emu_jedec_ce_60_size = emu_chip_size;
		emu_jedec_ce_c7_size = emu_chip_size;
		msg_pdbg("Emulating generic SPI flash chip (size=%d bytes)\n",
		         emu_chip_size);
	}
#endif
	if (emu_chip == EMULATE_NONE) {
		msg_perr("Invalid chip specified for emulation: %s\n", tmp);
		free(tmp);
		return 1;
	}
	free(tmp);
	flashchip_contents = malloc(emu_chip_size);
	if (!flashchip_contents) {
		msg_perr("Out of memory!\n");
		return 1;
	}
	msg_pdbg("Filling fake flash chip with 0xff, size %i\n", emu_chip_size);
	memset(flashchip_contents, 0xff, emu_chip_size);

	emu_persistent_image = extract_programmer_param("image");
	if (!emu_persistent_image) {
		/* Nothing else to do. */
		return 0;
	}
	if (!stat(emu_persistent_image, &image_stat)) {
		msg_pdbg("Found persistent image %s, size %li ",
			 emu_persistent_image, (long)image_stat.st_size);
		if (image_stat.st_size == emu_chip_size) {
			msg_pdbg("matches.\n");
			msg_pdbg("Reading %s\n", emu_persistent_image);
			read_buf_from_file(flashchip_contents, emu_chip_size,
					   emu_persistent_image);
		} else {
			msg_pdbg("doesn't match.\n");
		}
	}
#endif
	return 0;
}

int dummy_shutdown(void)
{
	msg_pspew("%s\n", __func__);
#if EMULATE_CHIP
	if (emu_chip != EMULATE_NONE) {
		if (emu_persistent_image) {
			msg_pdbg("Writing %s\n", emu_persistent_image);
			write_buf_to_file(flashchip_contents, emu_chip_size,
					  emu_persistent_image);
		}
		free(flashchip_contents);
	}
#endif
	return 0;
}

void *dummy_map(const char *descr, unsigned long phys_addr, size_t len)
{
	msg_pspew("%s: Mapping %s, 0x%lx bytes at 0x%08lx\n",
		  __func__, descr, (unsigned long)len, phys_addr);
	return (void *)phys_addr;
}

void dummy_unmap(void *virt_addr, size_t len)
{
	msg_pspew("%s: Unmapping 0x%lx bytes at %p\n",
		  __func__, (unsigned long)len, virt_addr);
}

void dummy_chip_writeb(uint8_t val, chipaddr addr)
{
	msg_pspew("%s: addr=0x%lx, val=0x%02x\n", __func__, addr, val);
}

void dummy_chip_writew(uint16_t val, chipaddr addr)
{
	msg_pspew("%s: addr=0x%lx, val=0x%04x\n", __func__, addr, val);
}

void dummy_chip_writel(uint32_t val, chipaddr addr)
{
	msg_pspew("%s: addr=0x%lx, val=0x%08x\n", __func__, addr, val);
}

void dummy_chip_writen(uint8_t *buf, chipaddr addr, size_t len)
{
	size_t i;
	msg_pspew("%s: addr=0x%lx, len=0x%08lx, writing data (hex):",
		  __func__, addr, (unsigned long)len);
	for (i = 0; i < len; i++) {
		if ((i % 16) == 0)
			msg_pspew("\n");
		msg_pspew("%02x ", buf[i]);
	}
}

uint8_t dummy_chip_readb(const chipaddr addr)
{
	msg_pspew("%s:  addr=0x%lx, returning 0xff\n", __func__, addr);
	return 0xff;
}

uint16_t dummy_chip_readw(const chipaddr addr)
{
	msg_pspew("%s:  addr=0x%lx, returning 0xffff\n", __func__, addr);
	return 0xffff;
}

uint32_t dummy_chip_readl(const chipaddr addr)
{
	msg_pspew("%s:  addr=0x%lx, returning 0xffffffff\n", __func__, addr);
	return 0xffffffff;
}

void dummy_chip_readn(uint8_t *buf, const chipaddr addr, size_t len)
{
	msg_pspew("%s:  addr=0x%lx, len=0x%lx, returning array of 0xff\n",
		  __func__, addr, (unsigned long)len);
	memset(buf, 0xff, len);
	return;
}

#if EMULATE_SPI_CHIP
static int emulate_spi_chip_response(unsigned int writecnt, unsigned int readcnt,
		      const unsigned char *writearr, unsigned char *readarr)
{
	int offs;
	static int aai_offs;
	static int aai_active = 0;

	if (writecnt == 0) {
		msg_perr("No command sent to the chip!\n");
		return 1;
	}
	/* TODO: Implement command blacklists here. */
	switch (writearr[0]) {
	case JEDEC_RES:
		if (emu_chip != EMULATE_ST_M25P10_RES)
			break;
		/* Respond with ST_M25P10_RES. */
		if (readcnt > 0)
			readarr[0] = 0x10;
		break;
	case JEDEC_REMS:
		if (emu_chip != EMULATE_SST_SST25VF040_REMS)
			break;
		/* Respond with SST_SST25VF040_REMS. */
		if (readcnt > 0)
			readarr[0] = 0xbf;
		if (readcnt > 1)
			readarr[1] = 0x44;
		break;
	case JEDEC_RDID:
		if (emu_chip == EMULATE_SST_SST25VF032B) {
			/* Respond with SST_SST25VF032B. */
			if (readcnt > 0)
				readarr[0] = 0xbf;
			if (readcnt > 1)
				readarr[1] = 0x25;
			if (readcnt > 2)
				readarr[2] = 0x4a;
		} else if (emu_chip == EMULATE_VARIABLE_SIZE) {
			const uint16_t man_id = VARIABLE_SIZE_MANUF_ID;
			const uint16_t dev_id = VARIABLE_SIZE_DEVICE_ID;
			if (readcnt > 0) readarr[0] = man_id >> 8;
			if (readcnt > 1) readarr[1] = man_id & 0xff;
			if (readcnt > 2) readarr[2] = dev_id >> 8;
			if (readcnt > 3) readarr[3] = dev_id & 0xff;
		}
		break;
	case JEDEC_RDSR:
		memset(readarr, 0, readcnt);
		if (aai_active)
			memset(readarr, 1 << 6, readcnt);
		break;
	case JEDEC_READ:
		offs = writearr[1] << 16 | writearr[2] << 8 | writearr[3];
		/* Truncate to emu_chip_size. */
		offs %= emu_chip_size;
		if (readcnt > 0)
			memcpy(readarr, flashchip_contents + offs, readcnt);
		break;
	case JEDEC_BYTE_PROGRAM:
		offs = writearr[1] << 16 | writearr[2] << 8 | writearr[3];
		/* Truncate to emu_chip_size. */
		offs %= emu_chip_size;
		if (writecnt < 5) {
			msg_perr("BYTE PROGRAM size too short!\n");
			return 1;
		}
		if (writecnt - 4 > emu_max_byteprogram_size) {
			msg_perr("Max BYTE PROGRAM size exceeded!\n");
			return 1;
		}
		memcpy(flashchip_contents + offs, writearr + 4, writecnt - 4);
		break;
	case JEDEC_AAI_WORD_PROGRAM:
		if (!emu_max_aai_size)
			break;
		if (!aai_active) {
			if (writecnt < JEDEC_AAI_WORD_PROGRAM_OUTSIZE) {
				msg_perr("Initial AAI WORD PROGRAM size too "
					 "short!\n");
				return 1;
			}
			if (writecnt > JEDEC_AAI_WORD_PROGRAM_OUTSIZE) {
				msg_perr("Initial AAI WORD PROGRAM size too "
					 "long!\n");
				return 1;
			}
			aai_active = 1;
			aai_offs = writearr[1] << 16 | writearr[2] << 8 |
				   writearr[3];
			/* Truncate to emu_chip_size. */
			aai_offs %= emu_chip_size;
			memcpy(flashchip_contents + aai_offs, writearr + 4, 2);
			aai_offs += 2;
		} else {
			if (writecnt < JEDEC_AAI_WORD_PROGRAM_CONT_OUTSIZE) {
				msg_perr("Continuation AAI WORD PROGRAM size "
					 "too short!\n");
				return 1;
			}
			if (writecnt > JEDEC_AAI_WORD_PROGRAM_CONT_OUTSIZE) {
				msg_perr("Continuation AAI WORD PROGRAM size "
					 "too long!\n");
				return 1;
			}
			memcpy(flashchip_contents + aai_offs, writearr + 1, 2);
			aai_offs += 2;
		}
		break;
	case JEDEC_WRDI:
		if (!emu_max_aai_size)
			break;
		aai_active = 0;
		break;
	case JEDEC_SE:
		if (!emu_jedec_se_size)
			break;
		if (writecnt != JEDEC_SE_OUTSIZE) {
			msg_perr("SECTOR ERASE 0x20 outsize invalid!\n");
			return 1;
		}
		if (readcnt != JEDEC_SE_INSIZE) {
			msg_perr("SECTOR ERASE 0x20 insize invalid!\n");
			return 1;
		}
		offs = writearr[1] << 16 | writearr[2] << 8 | writearr[3];
		if (offs & (emu_jedec_se_size - 1))
			msg_pdbg("Unaligned SECTOR ERASE 0x20\n");
		offs &= ~(emu_jedec_se_size - 1);
		memset(flashchip_contents + offs, 0xff, emu_jedec_se_size);
		break;
	case JEDEC_BE_52:
		if (!emu_jedec_be_52_size)
			break;
		if (writecnt != JEDEC_BE_52_OUTSIZE) {
			msg_perr("BLOCK ERASE 0x52 outsize invalid!\n");
			return 1;
		}
		if (readcnt != JEDEC_BE_52_INSIZE) {
			msg_perr("BLOCK ERASE 0x52 insize invalid!\n");
			return 1;
		}
		offs = writearr[1] << 16 | writearr[2] << 8 | writearr[3];
		if (offs & (emu_jedec_be_52_size - 1))
			msg_pdbg("Unaligned BLOCK ERASE 0x52\n");
		offs &= ~(emu_jedec_be_52_size - 1);
		memset(flashchip_contents + offs, 0xff, emu_jedec_be_52_size);
		break;
	case JEDEC_BE_D8:
		if (!emu_jedec_be_d8_size)
			break;
		if (writecnt != JEDEC_BE_D8_OUTSIZE) {
			msg_perr("BLOCK ERASE 0xd8 outsize invalid!\n");
			return 1;
		}
		if (readcnt != JEDEC_BE_D8_INSIZE) {
			msg_perr("BLOCK ERASE 0xd8 insize invalid!\n");
			return 1;
		}
		offs = writearr[1] << 16 | writearr[2] << 8 | writearr[3];
		if (offs & (emu_jedec_be_d8_size - 1))
			msg_pdbg("Unaligned BLOCK ERASE 0xd8\n");
		offs &= ~(emu_jedec_be_d8_size - 1);
		memset(flashchip_contents + offs, 0xff, emu_jedec_be_d8_size);
		break;
	case JEDEC_CE_60:
		if (!emu_jedec_ce_60_size)
			break;
		if (writecnt != JEDEC_CE_60_OUTSIZE) {
			msg_perr("CHIP ERASE 0x60 outsize invalid!\n");
			return 1;
		}
		if (readcnt != JEDEC_CE_60_INSIZE) {
			msg_perr("CHIP ERASE 0x60 insize invalid!\n");
			return 1;
		}
		offs = writearr[1] << 16 | writearr[2] << 8 | writearr[3];
		if (offs & (emu_jedec_ce_60_size - 1))
			msg_pdbg("Unaligned CHIP ERASE 0x60\n");
		offs &= ~(emu_jedec_ce_60_size - 1);
		/* emu_jedec_ce_60_size is emu_chip_size. */
		memset(flashchip_contents + offs, 0xff, emu_jedec_ce_60_size);
		break;
	case JEDEC_CE_C7:
		if (!emu_jedec_ce_c7_size)
			break;
		if (writecnt != JEDEC_CE_C7_OUTSIZE) {
			msg_perr("CHIP ERASE 0xc7 outsize invalid!\n");
			return 1;
		}
		if (readcnt != JEDEC_CE_C7_INSIZE) {
			msg_perr("CHIP ERASE 0xc7 insize invalid!\n");
			return 1;
		}
		offs = writearr[1] << 16 | writearr[2] << 8 | writearr[3];
		if (offs & (emu_jedec_ce_c7_size - 1))
			msg_pdbg("Unaligned CHIP ERASE 0xc7\n");
		offs &= ~(emu_jedec_ce_c7_size - 1);
		/* emu_jedec_ce_c7_size is emu_chip_size. */
		memset(flashchip_contents, 0xff, emu_jedec_ce_c7_size);
		break;
	default:
		/* No special response. */
		break;
	}
	return 0;
}
#endif

int dummy_spi_send_command(unsigned int writecnt, unsigned int readcnt,
		      const unsigned char *writearr, unsigned char *readarr)
{
	int i;

	msg_pspew("%s:", __func__);

	msg_pspew(" writing %u bytes:", writecnt);
	for (i = 0; i < writecnt; i++)
		msg_pspew(" 0x%02x", writearr[i]);

	/* Response for unknown commands and missing chip is 0xff. */
	memset(readarr, 0xff, readcnt);
#if EMULATE_SPI_CHIP
	switch (emu_chip) {
	case EMULATE_ST_M25P10_RES:
	case EMULATE_SST_SST25VF040_REMS:
	case EMULATE_SST_SST25VF032B:
	case EMULATE_VARIABLE_SIZE:
		if (emulate_spi_chip_response(writecnt, readcnt, writearr,
					      readarr)) {
			msg_perr("Invalid command sent to flash chip!\n");
			return 1;
		}
		break;
	default:
		break;
	}
#endif
	msg_pspew(" reading %u bytes:", readcnt);
	for (i = 0; i < readcnt; i++) {
		msg_pspew(" 0x%02x", readarr[i]);
	}
	msg_pspew("\n");
	return 0;
}

int dummy_spi_read(struct flashchip *flash, uint8_t *buf, int start, int len)
{
	/* Maximum read length is unlimited, use 64kB. */
	return spi_read_chunked(flash, buf, start, len, 64 * 1024);
}

int dummy_spi_write_256(struct flashchip *flash, uint8_t *buf, int start, int len)
{
	return spi_write_chunked(flash, buf, start, len,
				 spi_write_256_chunksize);
}

#if EMULATE_CHIP && EMULATE_SPI_CHIP
int probe_variable_size(struct flashchip *flash)
{
	int i;

	/* Skip the probing if we don't emulate this chip. */
	if (emu_chip != EMULATE_VARIABLE_SIZE)
		return 0;

	/*
	 * This will break if one day flashchip becomes read-only.
	 * Once that happens, we need to have special hacks in functions:
	 *
	 *     erase_and_write_flash() in flashrom.c
	 *     read_flash_to_file()
	 *     handle_romentries()
	 *     ...
	 *
	 * Search "total_size * 1024" in code.
	 */
	if (emu_chip_size % 1024)
		msg_perr("%s: emu_chip_size is not multipler of 1024.\n",
		         __func__);
	flash->total_size = emu_chip_size / 1024;
	msg_cdbg("%s: set flash->total_size to %dK bytes.\n", __func__,
	         flash->total_size);

	/* Update eraser count */
	for (i = 0; i < NUM_ERASEFUNCTIONS; i++) {
		struct block_eraser *eraser = &flash->block_erasers[i];
		if (eraser->block_erase == NULL)
			break;

		eraser->eraseblocks[0].count = emu_chip_size /
		    eraser->eraseblocks[0].size;
		msg_cdbg("%s: eraser.size=%d, .count=%d\n",
		         __func__, eraser->eraseblocks[0].size,
		         eraser->eraseblocks[0].count);
	}

	return 1;
}
#endif
