/*
 * This file is part of the flashrom project.
 *
 * Copyright (c) 2010  Matthias Wenzel <bios at mazzoo dot de>
 * Copyright (c) 2011  Stefan Tauner
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

#include "ich_descriptors.h"

#if defined(__i386__) || defined(__x86_64__)

#ifdef ICH_DESCRIPTORS_FROM_MMAP_DUMP

#define DESCRIPTOR_MODE_MAGIC 0x0ff0a55a
#define msg_pdbg printf
#define msg_perr printf
#include <stdio.h>

struct flash_strap fisba;
struct flash_upper_map flumap;

#else

#include "flash.h" /* for msg_* */

#endif // ICH_DESCRIPTORS_FROM_MMAP_DUMP

struct flash_descriptor fdbar = { 0 };
struct flash_component fcba = { {0} };
struct flash_region frba = { {0} };
struct flash_master fmba = { {0} };

uint32_t getFCBA(void)
{
	return (fdbar.FLMAP0 <<  4) & 0x00000ff0;
}

uint32_t getFRBA(void)
{
	return (fdbar.FLMAP0 >> 12) & 0x00000ff0;
}

uint32_t getFMBA(void)
{
	return (fdbar.FLMAP1 <<  4) & 0x00000ff0;
}

uint32_t getFMSBA(void)
{
	return (fdbar.FLMAP2 <<  4) & 0x00000ff0;
}

uint32_t getFLREG_limit(uint32_t flreg)
{
	return (flreg >>  4) & 0x01fff000;
}

uint32_t getFLREG_base(uint32_t flreg)
{
	return (flreg << 12) & 0x01fff000;
}

uint32_t getFISBA(void)
{
	return (fdbar.FLMAP1 >> 12) & 0x00000ff0;
}

/** Returns the integer representation of the component density with index
comp in bytes or 0 if a correct size can not be determined. */
int getFCBA_component_density(uint8_t comp)
{
	uint8_t size_enc;
	const int dec_mem[6] = {
		      512 * 1024,
		 1 * 1024 * 1024,
		 2 * 1024 * 1024,
		 4 * 1024 * 1024,
		 8 * 1024 * 1024,
		16 * 1024 * 1024,
	};

	switch(comp) {
	case 0:
		size_enc = fcba.comp1_density;
		break;
	case 1:
		if (fdbar.NC == 0)
			return 0;
		size_enc = fcba.comp2_density;
		break;
	default:
		msg_perr("Only component index 0 or 1 are supported yet.\n");
		return 0;
	}
	if (size_enc > 5) {
		msg_perr("Density of component with index %d illegal or "
			 "unsupported. Encoded density is 0x%x.\n", comp,
			 size_enc);
		return 0;
	}
	return dec_mem[size_enc];
}

#ifdef ICH_DESCRIPTORS_FROM_MMAP_DUMP
uint32_t getVTBA(void)
{	/* The bits in FLUMAP1 describe bits 4-11 out of 24 in total;
	 * others are 0. */
	return (flumap.FLUMAP1 << 4) & 0x0ff0;
}
#endif

const struct flash_descriptor_addresses desc_addr = {
	.FCBA = getFCBA,
	.FRBA = getFRBA,
	.FMBA = getFMBA,
	.FMSBA = getFMSBA,
	.FISBA = getFISBA,
	.FLREG_limit = getFLREG_limit,
	.FLREG_base = getFLREG_base,
#ifdef ICH_DESCRIPTORS_FROM_MMAP_DUMP
	.VTBA = getVTBA,
#endif
};

void prettyprint_ich_descriptors(enum chipset cs)
{
	prettyprint_ich_descriptor_map();
	prettyprint_ich_descriptor_component();
	prettyprint_ich_descriptor_region();
	prettyprint_ich_descriptor_master();
#ifdef ICH_DESCRIPTORS_FROM_MMAP_DUMP
	if (cs >= CHIPSET_ICH8) {
		prettyprint_ich_descriptor_upper_map();
		prettyprint_ich_descriptor_straps(cs);
	}
#endif // ICH_DESCRIPTORS_FROM_MMAP_DUMP
	msg_pdbg("\n");
}

void prettyprint_ich_descriptor_map(void)
{
	msg_pdbg("=== FDBAR ===\n");
	msg_pdbg("FLVALSIG 0x%8.8x\n", fdbar.FLVALSIG);
	msg_pdbg("FLMAP0   0x%8.8x\n", fdbar.FLMAP0  );
	msg_pdbg("FLMAP1   0x%8.8x\n", fdbar.FLMAP1  );
	msg_pdbg("FLMAP2   0x%8.8x\n", fdbar.FLMAP2  );
	msg_pdbg("\n");
	msg_pdbg("--- FDBAR details ---\n");
	msg_pdbg("0x%2.2x        NR    Number of Regions\n", fdbar.NR   );
	msg_pdbg("0x%8.8x  FRBA  Flash Region Base Address\n", desc_addr.FRBA() );
	msg_pdbg("0x%2.2x        NC    Number of Components\n", fdbar.NC   );
	msg_pdbg("0x%8.8x  FCBA  Flash Component Base Address\n", desc_addr.FCBA() );
	msg_pdbg("\n");
	msg_pdbg("0x%2.2x        ISL   ICH Strap Length\n", fdbar.ISL  );
	msg_pdbg("0x%8.8x  FISBA Flash ICH Strap Base Address\n", desc_addr.FISBA());
	msg_pdbg("0x%2.2x        NM    Number of Masters\n", fdbar.NM   );
	msg_pdbg("0x%8.8x  FMBA  Flash Master Base Address\n", desc_addr.FMBA() );
	msg_pdbg("\n");
	msg_pdbg("0x%2.2x        MSL   MCH Strap Length\n", fdbar.MSL  );
	msg_pdbg("0x%8.8x  FMSBA Flash MCH Strap Base Address\n", desc_addr.FMSBA());
}

void prettyprint_ich_descriptor_component(void)
{
	const char * const str_freq[8] = {
		"20 MHz",		/* 000 */
		"33 MHz",		/* 001 */
		"reserved/illegal",	/* 010 */
		"reserved/illegal",	/* 011 */
		"50 MHz",		/* 100 */
		"reserved/illegal",	/* 101 */
		"reserved/illegal",	/* 110 */
		"reserved/illegal"	/* 111 */
	};
	const char * const str_mem[8] = {
		"512kB",
		"1 MB",
		"2 MB",
		"4 MB",
		"8 MB",
		"16 MB",
		"undocumented/illegal",
		"reserved/illegal"
	};

	msg_pdbg("\n");
	msg_pdbg("=== FCBA ===\n");
	msg_pdbg("FLCOMP   0x%8.8x\n", fcba.FLCOMP);
	msg_pdbg("FLILL    0x%8.8x\n", fcba.FLILL );
	msg_pdbg("\n");
	msg_pdbg("--- FCBA details ---\n");
	msg_pdbg("0x%2.2x    freq_read_id   %s\n",
		fcba.freq_read_id , str_freq[fcba.freq_read_id ]);
	msg_pdbg("0x%2.2x    freq_write     %s\n",
		fcba.freq_write   , str_freq[fcba.freq_write   ]);
	msg_pdbg("0x%2.2x    freq_fastread  %s\n",
		fcba.freq_fastread, str_freq[fcba.freq_fastread]);
	msg_pdbg("0x%2.2x    fastread       %ssupported\n",
		fcba.fastread, fcba.fastread ? "" : "not ");
	msg_pdbg("0x%2.2x    freq_read      %s\n",
		fcba.freq_read, str_freq[fcba.freq_read    ]);
	msg_pdbg("0x%2.2x    comp 1 density %s\n",
		fcba.comp1_density, str_mem[fcba.comp1_density]);
	if (fdbar.NC)
		msg_pdbg("0x%2.2x    comp 2 density %s\n",
			fcba.comp2_density, str_mem[fcba.comp2_density]);
	else
		msg_pdbg("0x%2.2x    comp 2 is not used (FLMAP0.NC=0)\n",
			fcba.comp2_density);
	msg_pdbg("\n");
	msg_pdbg("0x%2.2x    invalid instr 0\n", fcba.invalid_instr0);
	msg_pdbg("0x%2.2x    invalid instr 1\n", fcba.invalid_instr1);
	msg_pdbg("0x%2.2x    invalid instr 2\n", fcba.invalid_instr2);
	msg_pdbg("0x%2.2x    invalid instr 3\n", fcba.invalid_instr3);
}

void prettyprint_ich_descriptor_region(void)
{
	msg_pdbg("\n");
	msg_pdbg("=== FRBA ===\n");
	msg_pdbg("FLREG0   0x%8.8x\n", frba.FLREG0);
	msg_pdbg("FLREG1   0x%8.8x\n", frba.FLREG1);
	msg_pdbg("FLREG2   0x%8.8x\n", frba.FLREG2);
	msg_pdbg("FLREG3   0x%8.8x\n", frba.FLREG3);
	msg_pdbg("\n");
	msg_pdbg("--- FRBA details ---\n");
	msg_pdbg("0x%8.8x  region 0 limit (descr)\n", desc_addr.FLREG_limit(frba.FLREG0));
	msg_pdbg("0x%8.8x  region 0 base  (descr)\n", desc_addr.FLREG_base(frba.FLREG0));
	msg_pdbg("0x%8.8x  region 1 limit ( BIOS)\n", desc_addr.FLREG_limit(frba.FLREG1));
	msg_pdbg("0x%8.8x  region 1 base  ( BIOS)\n", desc_addr.FLREG_base(frba.FLREG1));
	msg_pdbg("0x%8.8x  region 2 limit ( ME  )\n", desc_addr.FLREG_limit(frba.FLREG2));
	msg_pdbg("0x%8.8x  region 2 base  ( ME  )\n", desc_addr.FLREG_base(frba.FLREG2));
	msg_pdbg("0x%8.8x  region 3 limit ( GbE )\n", desc_addr.FLREG_limit(frba.FLREG3));
	msg_pdbg("0x%8.8x  region 3 base  ( GbE )\n", desc_addr.FLREG_base(frba.FLREG3));
}

void prettyprint_ich_descriptor_master(void)
{
	msg_pdbg("\n");
	msg_pdbg("=== FMBA ===\n");
	msg_pdbg("FLMSTR1  0x%8.8x\n", fmba.FLMSTR1);
	msg_pdbg("FLMSTR2  0x%8.8x\n", fmba.FLMSTR2);
	msg_pdbg("FLMSTR3  0x%8.8x\n", fmba.FLMSTR3);

	msg_pdbg("\n");
	msg_pdbg("--- FMBA details ---\n");
	msg_pdbg("BIOS can %s write GbE\n",   fmba.BIOS_GbE_write   ? "   " : "NOT");
	msg_pdbg("BIOS can %s write ME\n",    fmba.BIOS_ME_write    ? "   " : "NOT");
	msg_pdbg("BIOS can %s write BIOS\n",  fmba.BIOS_BIOS_write  ? "   " : "NOT");
	msg_pdbg("BIOS can %s write descr\n", fmba.BIOS_descr_write ? "   " : "NOT");
	msg_pdbg("BIOS can %s read  GbE\n",   fmba.BIOS_GbE_read    ? "   " : "NOT");
	msg_pdbg("BIOS can %s read  ME\n",    fmba.BIOS_ME_read     ? "   " : "NOT");
	msg_pdbg("BIOS can %s read  BIOS\n",  fmba.BIOS_BIOS_read   ? "   " : "NOT");
	msg_pdbg("BIOS can %s read  descr\n", fmba.BIOS_descr_read  ? "   " : "NOT");
	msg_pdbg("ME   can %s write GbE\n",   fmba.ME_GbE_write     ? "   " : "NOT");
	msg_pdbg("ME   can %s write ME\n",    fmba.ME_ME_write      ? "   " : "NOT");
	msg_pdbg("ME   can %s write BIOS\n",  fmba.ME_BIOS_write    ? "   " : "NOT");
	msg_pdbg("ME   can %s write descr\n", fmba.ME_descr_write   ? "   " : "NOT");
	msg_pdbg("ME   can %s read  GbE\n",   fmba.ME_GbE_read      ? "   " : "NOT");
	msg_pdbg("ME   can %s read  ME\n",    fmba.ME_ME_read       ? "   " : "NOT");
	msg_pdbg("ME   can %s read  BIOS\n",  fmba.ME_BIOS_read     ? "   " : "NOT");
	msg_pdbg("ME   can %s read  descr\n", fmba.ME_descr_read    ? "   " : "NOT");
	msg_pdbg("GbE  can %s write GbE\n",   fmba.GbE_GbE_write    ? "   " : "NOT");
	msg_pdbg("GbE  can %s write ME\n",    fmba.GbE_ME_write     ? "   " : "NOT");
	msg_pdbg("GbE  can %s write BIOS\n",  fmba.GbE_BIOS_write   ? "   " : "NOT");
	msg_pdbg("GbE  can %s write descr\n", fmba.GbE_descr_write  ? "   " : "NOT");
	msg_pdbg("GbE  can %s read  GbE\n",   fmba.GbE_GbE_read     ? "   " : "NOT");
	msg_pdbg("GbE  can %s read  ME\n",    fmba.GbE_ME_read      ? "   " : "NOT");
	msg_pdbg("GbE  can %s read  BIOS\n",  fmba.GbE_BIOS_read    ? "   " : "NOT");
	msg_pdbg("GbE  can %s read  descr\n", fmba.GbE_descr_read   ? "   " : "NOT");
}

void prettyprint_ich9_reg_vscc(uint32_t reg_val)
{
	pprint_reg_hex(VSCC, BES, reg_val, ", ");
	pprint_reg(VSCC, WG, reg_val, ", ");
	pprint_reg(VSCC, WSR, reg_val, ", ");
	pprint_reg(VSCC, WEWS, reg_val, ", ");
	pprint_reg_hex(VSCC, EO, reg_val, ", ");
	pprint_reg(VSCC, VCL, reg_val, "\n");
}


#ifdef ICH_DESCRIPTORS_FROM_MMAP_DUMP
void prettyprint_ich_descriptor_straps_ich8(void)
{
	const char * const str_GPIO12[4] = {
		"GPIO12",
		"LAN PHY Power Control Function (Native Output)",
		"GLAN_DOCK# (Native Input)",
		"invalid configuration",
	};

	msg_pdbg("\n");
	msg_pdbg("=== FISBA ===\n");
	msg_pdbg("STRP0    0x%8.8x\n", fisba.ich8.STRP0);

	msg_pdbg("\n");
	msg_pdbg("--- FISBA details ---\n");
	msg_pdbg("ME SMBus addr2 0x%2.2x\n", fisba.ich8.ASD2);
	msg_pdbg("ME SMBus addr1 0x%2.2x\n", fisba.ich8.ASD);
	msg_pdbg("ME SMBus Controller is connected to %s\n", fisba.ich8.MESM2SEL ? "SMLink pins" : "SMBus pins");
	msg_pdbg("SPI CS1 is used for %s\n", fisba.ich8.SPICS1_LANPHYPC_SEL ? "LAN PHY Power Control Function" : "SPI Chip Select");
	msg_pdbg("GPIO12_SEL is used as %s\n", str_GPIO12[fisba.ich8.GPIO12_SEL]);
	msg_pdbg("PCIe Port 6 is used for %s\n", fisba.ich8.GLAN_PCIE_SEL ? "integrated GLAN" : "PCI Express");
	msg_pdbg("Intel AMT SMBus Controller 1 is connected to %s\n",   fisba.ich8.BMCMODE ? "SMLink" : "SMBus");
	msg_pdbg("TCO slave is on %s. Intel AMT SMBus Controller 1 is %sabled\n",
		fisba.ich8.TCOMODE ? "SMBus" : "SMLink", fisba.ich8.TCOMODE ? "en" : "dis");
	msg_pdbg("ME A is %sabled\n", fisba.ich8.ME_DISABLE ? "dis" : "en");

	msg_pdbg("\n");
	msg_pdbg("=== FMSBA ===\n");
	msg_pdbg("STRP1    0x%8.8x\n", fisba.ich8.STRP1);

	msg_pdbg("\n");
	msg_pdbg("--- FMSBA details ---\n");
	msg_pdbg("ME B is %sabled\n", fisba.ich8.ME_disable_B ? "dis" : "en");
}

void prettyprint_ich_descriptor_straps_ibex(void)
{
	int i;
	msg_pdbg("\n");
	msg_pdbg("=== FPSBA ===\n");
	for(i = 0; i <= 15; i++)
		msg_pdbg("STRP%-2d = 0x%8.8x\n", i, fisba.ibex.STRPs[i]);
}

void prettyprint_ich_descriptor_straps(enum chipset cs)
{
	switch (cs) {
	case CHIPSET_ICH8:
		prettyprint_ich_descriptor_straps_ich8();
		break;
	case CHIPSET_SERIES_5_IBEX_PEAK:
		prettyprint_ich_descriptor_straps_ibex();
		break;
	case CHIPSET_UNKNOWN:
		break;
	default:
		msg_pdbg("\n");
		msg_pdbg("The meaning of the descriptor straps are unknown yet.\n");
		break;
	}
}

void prettyprint_rdid(uint32_t reg_val)
{
	uint8_t mid = reg_val & 0xFF;
	uint16_t did = ((reg_val >> 16) & 0xFF) | (reg_val & 0xFF00);
	msg_pdbg("Manufacturer ID 0x%02x, Device ID 0x%04x\n", mid, did);
}

void prettyprint_ich_descriptor_upper_map(void)
{
	int i;
	msg_pdbg("\n");
	msg_pdbg("=== FLUMAP ===\n");
	msg_pdbg("FLUMAP1  0x%8.8x\n", flumap.FLUMAP1);

	msg_pdbg("\n");
	msg_pdbg("--- FLUMAP details ---\n");
	msg_pdbg("VTL  (length)       = %d\n", flumap.VTL);
	msg_pdbg("VTBA (base address) = 0x%6.6x\n", desc_addr.VTBA());
	msg_pdbg("\n");

	for (i=0; i < flumap.VTL/2; i++)
	{
		uint32_t jid = flumap.vscc_table[i].JID;
		uint32_t vscc = flumap.vscc_table[i].VSCC;
		msg_pdbg("  JID%d  = 0x%8.8x\n", i, jid);
		msg_pdbg("  VSCC%d = 0x%8.8x\n", i, vscc);
		msg_pdbg("    "); /* indention */
		prettyprint_rdid(jid);
		msg_pdbg("    "); /* indention */
		prettyprint_ich9_reg_vscc(vscc);
	}
}

int read_ich_descriptors_from_dump(uint32_t *dump, enum chipset cs)
{
	int i;
	uint8_t pch_bug_offset = 0;
	if (dump[0] != DESCRIPTOR_MODE_MAGIC) {
		if (dump[4] == DESCRIPTOR_MODE_MAGIC)
			pch_bug_offset = 4;
		else
			return -1;
	}

	/* map */
	fdbar.FLVALSIG	= dump[0 + pch_bug_offset];
	fdbar.FLMAP0	= dump[1 + pch_bug_offset];
	fdbar.FLMAP1	= dump[2 + pch_bug_offset];
	fdbar.FLMAP2	= dump[3 + pch_bug_offset];

	/* component */
	fcba.FLCOMP	= dump[(desc_addr.FCBA() >> 2) + 0];
	fcba.FLILL	= dump[(desc_addr.FCBA() >> 2) + 1];
	fcba.FLPB	= dump[(desc_addr.FCBA() >> 2) + 2];

	/* region */
	frba.FLREG0 = dump[(desc_addr.FRBA() >> 2) + 0];
	frba.FLREG1 = dump[(desc_addr.FRBA() >> 2) + 1];
	frba.FLREG2 = dump[(desc_addr.FRBA() >> 2) + 2];
	frba.FLREG3 = dump[(desc_addr.FRBA() >> 2) + 3];

	/* master */
	fmba.FLMSTR1 = dump[(desc_addr.FMBA() >> 2) + 0];
	fmba.FLMSTR2 = dump[(desc_addr.FMBA() >> 2) + 1];
	fmba.FLMSTR3 = dump[(desc_addr.FMBA() >> 2) + 2];

	/* upper map */
	flumap.FLUMAP1 = dump[(0x0efc >> 2) + 0];

	for (i=0; i < flumap.VTL; i++)
	{
		flumap.vscc_table[i].JID  = dump[(desc_addr.VTBA() >> 2) + i * 2 + 0];
		flumap.vscc_table[i].VSCC = dump[(desc_addr.VTBA() >> 2) + i * 2 + 1];
	}
	/* straps */
	/* FIXME: detect chipset correctly */
	switch (cs) {
		case CHIPSET_ICH8:
			fisba.ich8.STRP0 = dump[(desc_addr.FISBA() >> 2) + 0];
			fisba.ich8.STRP1 = dump[(desc_addr.FMSBA() >> 2) + 0];
			break;
		case CHIPSET_SERIES_5_IBEX_PEAK:
			for(i = 0; i <= 15; i++)
				fisba.ibex.STRPs[i] = dump[(desc_addr.FISBA() >> 2) + i];
			break;
		default:
			break;
	}

	return 0;
}
#else // ICH_DESCRIPTORS_FROM_MMAP_DUMP

static uint32_t read_descriptor_reg(uint8_t section, uint16_t offset, void *spibar)
{
	uint32_t control = 0;
	control |= (section << FDOC_FDSS_OFF) & FDOC_FDSS;
	control |= (offset << FDOC_FDSI_OFF) & FDOC_FDSI;
	*(volatile uint32_t *) (spibar + ICH9_REG_FDOC) = control;
	return *(volatile uint32_t *)(spibar + ICH9_REG_FDOD);
}

void read_ich_descriptors_from_fdo(void *spibar)
{
	msg_pdbg("Reading flash descriptors "
		 "mapped by the chipset via FDOC/FDOD...");
	/* descriptor map section */
	fdbar.FLVALSIG	= read_descriptor_reg(0, 0, spibar);
	fdbar.FLMAP0	= read_descriptor_reg(0, 1, spibar);
	fdbar.FLMAP1	= read_descriptor_reg(0, 2, spibar);
	fdbar.FLMAP2	= read_descriptor_reg(0, 3, spibar);

	/* component section */
	fcba.FLCOMP	= read_descriptor_reg(1, 0, spibar);
	fcba.FLILL	= read_descriptor_reg(1, 1, spibar);
	fcba.FLPB	= read_descriptor_reg(1, 2, spibar);

	/* region section */
	frba.FLREG0 = read_descriptor_reg(2, 0, spibar);
	frba.FLREG1 = read_descriptor_reg(2, 1, spibar);
	frba.FLREG2 = read_descriptor_reg(2, 2, spibar);
	frba.FLREG3 = read_descriptor_reg(2, 3, spibar);

	/* master section */
	fmba.FLMSTR1 = read_descriptor_reg(3, 0, spibar);
	fmba.FLMSTR2 = read_descriptor_reg(3, 1, spibar);
	fmba.FLMSTR3 = read_descriptor_reg(3, 2, spibar);

	/* accessing strap section and upper map is impossible via FDOC/D(?) */
	msg_pdbg(" done\n");
}

#endif // ICH_DESCRIPTORS_FROM_MMAP_DUMP
#endif // defined(__i386__) || defined(__x86_64__)
