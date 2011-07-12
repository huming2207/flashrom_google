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

#include <stdint.h>

#if defined(__i386__) || defined(__x86_64__)
#ifndef __ICH_DESCRIPTORS_H__
#define __ICH_DESCRIPTORS_H__ 1

/* should probably be in ichspi.h */
#define msg_pdbg2 msg_pspew

#define ICH9_REG_FDOC		0xB0	/* 32 Bits Flash Descriptor Observability Control */
					/* 0-1: reserved */
#define FDOC_FDSI_OFF		2	/* 2-11: Flash Descriptor Section Index */
#define FDOC_FDSI		(0x3f << FDOC_FDSI_OFF)
#define FDOC_FDSS_OFF		12	/* 12-14: Flash Descriptor Section Select */
#define FDOC_FDSS		(0x3 << FDOC_FDSS_OFF)
					/* 15-31: reserved */

#define ICH9_REG_FDOD		0xB4	/* 32 Bits Flash Descriptor Observability Data */

/* Field locations and semantics for LVSCC, UVSCC and related words in the flash
 * descriptor are equal therefore they all share the same macros below. */
#define VSCC_BES_OFF		0	/* 0-1: Block/Sector Erase Size */
#define VSCC_BES			(0x3 << VSCC_BES_OFF)
#define VSCC_WG_OFF		2	/* 2: Write Granularity */
#define VSCC_WG				(0x1 << VSCC_WG_OFF)
#define VSCC_WSR_OFF		3	/* 3: Write Status Required */
#define VSCC_WSR			(0x1 << VSCC_WSR_OFF)
#define VSCC_WEWS_OFF		4	/* 4: Write Enable on Write Status */
#define VSCC_WEWS			(0x1 << VSCC_WEWS_OFF)
					/* 5-7: reserved */
#define VSCC_EO_OFF		8	/* 8-15: Erase Opcode */
#define VSCC_EO				(0xff << VSCC_EO_OFF)
					/* 16-22: reserved */
#define VSCC_VCL_OFF		23	/* 23: Vendor Component Lock */
#define VSCC_VCL			(0x1 << VSCC_VCL_OFF)
					/* 24-31: reserved */

#define pprint_reg(reg, bit, val, sep) msg_pdbg("%s=%d" sep, #bit, (val & reg##_##bit)>>reg##_##bit##_OFF)
#define pprint_reg_hex(reg, bit, val, sep) msg_pdbg("%s=0x%x" sep, #bit, (val & reg##_##bit)>>reg##_##bit##_OFF)
void prettyprint_ich9_reg_vscc(uint32_t reg_val);

enum chipset {
	CHIPSET_UNKNOWN,
	CHIPSET_ICH7 = 7,
	CHIPSET_ICH8,
	CHIPSET_ICH9,
	CHIPSET_ICH10,
	CHIPSET_SERIES_5_IBEX_PEAK,
	CHIPSET_SERIES_6_COUGAR_POINT,
	CHIPSET_SERIES_7_PANTHER_POINT
};

struct flash_descriptor_addresses {
	uint32_t (*FCBA)(void);
	uint32_t (*FRBA)(void);
	uint32_t (*FMBA)(void);
	uint32_t (*FMSBA)(void);
	uint32_t (*FLREG_limit)(uint32_t flreg);
	uint32_t (*FLREG_base)(uint32_t flreg);
	uint32_t (*FISBA)(void);
#ifdef ICH_DESCRIPTORS_FROM_MMAP_DUMP
	uint32_t (*VTBA)(void);
#endif // ICH_DESCRIPTORS_FROM_MMAP_DUMP
};
	
struct flash_descriptor {
	uint32_t FLVALSIG;	/* 0x00 */
	union {			/* 0x04 */
		uint32_t FLMAP0;
		struct {
			uint8_t FCBA	:8;
			uint8_t NC	:2;
			unsigned	:6;
			uint8_t FRBA	:8;
			uint8_t NR	:3;
			unsigned	:5;
		};
	};
	union {			/* 0x08 */
		uint32_t FLMAP1;
		struct {
			uint8_t FMBA	:8;
			uint8_t NM	:3;
			unsigned	:5;
			union {
				uint8_t FISBA	:8;
				uint8_t FPSBA	:8;
			};
			uint8_t ISL	:8;
		};
	};
	union {			/* 0x0c */
		uint32_t FLMAP2;
		struct {
			uint8_t  FMSBA	:8;
			uint8_t  MSL	:8;
			unsigned	:16;
		};
	};
};

struct flash_component {
	union {			/* 0x00 */
		uint32_t FLCOMP;
		struct {
			uint8_t  comp1_density	:3;
			uint8_t  comp2_density	:3;
			unsigned		:11;
			uint8_t  freq_read	:3;
			uint8_t  fastread	:1;
			uint8_t  freq_fastread	:3;
			uint8_t  freq_write	:3;
			uint8_t  freq_read_id	:3;
			unsigned		:2;
		};
	};
	union {			/* 0x04 */
		uint32_t FLILL;
		struct {
			uint8_t invalid_instr0;
			uint8_t invalid_instr1;
			uint8_t invalid_instr2;
			uint8_t invalid_instr3;
		};
	};
	union {			/* 0x08 */
		uint32_t FLPB;
		struct {
			uint16_t FPBA	:13;
			unsigned	:19;
		};
	};
};

struct flash_region {
	
	union {
		uint32_t FLREG0; /* Flash Descriptor */
		struct {
			uint16_t reg0_base	:13;
			unsigned		:3;
			uint16_t reg0_limit	:13;
			unsigned		:3;
		};
	};
	union {
		uint32_t FLREG1; /* BIOS */
		struct {
			uint16_t reg1_base	:13;
			unsigned		:3;
			uint16_t reg1_limit	:13;
			unsigned		:3;
		};
	};
	union {
		uint32_t FLREG2; /* ME */
		struct {
			uint16_t reg2_base	:13;
			unsigned		:3;
			uint16_t reg2_limit	:13;
			unsigned		:3;
		};
	};
	union {
		uint32_t FLREG3; /* GbE */
		struct {
			uint16_t reg3_base	:13;
			unsigned		:3;
			uint16_t reg3_limit	:13;
			unsigned		:3;
		};
	};
} frba;

struct flash_master {
	union {
		uint32_t FLMSTR1;
		struct {
			uint16_t BIOS_req_ID		:16;
			uint8_t  BIOS_descr_read	:1;
			uint8_t  BIOS_BIOS_read		:1;
			uint8_t  BIOS_ME_read		:1;
			uint8_t  BIOS_GbE_read		:1;
			uint8_t  BIOS_plat_read		:1;
			unsigned			:3;
			uint8_t  BIOS_descr_write	:1;
			uint8_t  BIOS_BIOS_write	:1;
			uint8_t  BIOS_ME_write		:1;
			uint8_t  BIOS_GbE_write		:1;
			uint8_t  BIOS_plat_write	:1;
			unsigned			:3;
		};
	};
	union {
		uint32_t FLMSTR2;
		struct {
			uint16_t ME_req_ID		:16;
			uint8_t  ME_descr_read	:1;
			uint8_t  ME_BIOS_read		:1;
			uint8_t  ME_ME_read		:1;
			uint8_t  ME_GbE_read		:1;
			uint8_t  ME_plat_read		:1;
			unsigned			:3;
			uint8_t  ME_descr_write		:1;
			uint8_t  ME_BIOS_write		:1;
			uint8_t  ME_ME_write		:1;
			uint8_t  ME_GbE_write		:1;
			uint8_t  ME_plat_write		:1;
			unsigned			:3;
		};
	};
	union {
		uint32_t FLMSTR3;
		struct {
			uint16_t GbE_req_ID		:16;
			uint8_t  GbE_descr_read		:1;
			uint8_t  GbE_BIOS_read		:1;
			uint8_t  GbE_ME_read		:1;
			uint8_t  GbE_GbE_read		:1;
			uint8_t  GbE_plat_read		:1;
			unsigned			:3;
			uint8_t  GbE_descr_write	:1;
			uint8_t  GbE_BIOS_write		:1;
			uint8_t  GbE_ME_write		:1;
			uint8_t  GbE_GbE_write		:1;
			uint8_t  GbE_plat_write		:1;
			unsigned			:3;
		};
	};
};

#ifdef ICH_DESCRIPTORS_FROM_MMAP_DUMP
struct flash_strap {
	union {
		struct {
			union {
				uint32_t STRP0;
				struct {
					uint8_t  ME_DISABLE		:1;
					unsigned			:6;
					uint8_t  TCOMODE		:1;
					uint8_t  ASD			:7;
					uint8_t  BMCMODE		:1;
					unsigned			:3;
					uint8_t  GLAN_PCIE_SEL		:1;
					uint8_t  GPIO12_SEL		:2;
					uint8_t  SPICS1_LANPHYPC_SEL	:1;
					uint8_t  MESM2SEL		:1;
					unsigned			:1;
					uint8_t  ASD2			:7;
				};
			};
			union {
				uint32_t STRP1;
				struct {
					uint8_t  ME_disable_B		:1;
					unsigned			:31;
				};
			};
		}ich8;
		union {
			uint32_t STRPs[15];
			struct {
				union {
					uint32_t STRP0;
					struct {
						unsigned			:1;
						uint8_t  cs_ss2			:1;
						unsigned			:5;
						uint8_t  SMB_EN			:1;
						uint8_t  SML0_EN		:1;
						uint8_t  SML1_EN		:1;
						uint8_t  SML1FRQ		:2;
						uint8_t  SMB0FRQ		:2;
						uint8_t  SML0FRQ		:2;
						unsigned			:4;
						uint8_t  LANPHYPC_GP12_SEL	:1;
						uint8_t  cs_ss1			:1;
						unsigned			:2;
						uint8_t  DMI_REQID_DIS		:1;
						unsigned			:4;
						uint8_t  BBBS			:2;
						unsigned			:1;
					};
				};
				union {
					uint32_t STRP1;
					struct {
					};
				};
				union {
					uint32_t STRP2;
					struct {
					};
				};
				union {
					uint32_t STRP3;
					struct {
					};
				};
				union {
					uint32_t STRP4;
					struct {
					};
				};
				union {
					uint32_t STRP5;
					struct {
					};
				};
				union {
					uint32_t STRP6;
					struct {
					};
				};
				union {
					uint32_t STRP7;
					struct {
					};
				};
				union {
					uint32_t STRP8;
					struct {
					};
				};
				union {
					uint32_t STRP9;
					struct {
					};
				};
				union {
					uint32_t STRP10;
					struct {
					};
				};
				union {
					uint32_t STRP11;
					struct {
					};
				};
				union {
					uint32_t STRP12;
					struct {
					};
				};
				union {
					uint32_t STRP13;
					struct {
					};
				};
				union {
					uint32_t STRP14;
					struct {
					};
				};
				union {
					uint32_t STRP15;
					struct {
					};
				};
			};
		}ibex;
	};
};

struct flash_upper_map {
	union {
		uint32_t FLUMAP1;
		struct {
			uint8_t  VTBA	:8;
			uint8_t  VTL	:8;
			unsigned	:16;
		};
	};
	struct {
		union {
			uint32_t JID;
			struct {
				uint8_t vid	:8;
				uint8_t cid0	:8;
				uint8_t cid1	:8;
				unsigned	:8;
			};
		};
		union {
			uint32_t VSCC;
			struct {
				uint8_t  ubes	:2;
				uint8_t  uwg	:1;
				uint8_t  uwsr	:1;
				uint8_t  uwews	:1;
				unsigned	:3;
				uint8_t  ueo	:8;
				uint8_t  lbes	:2;
				uint8_t  lwg	:1;
				uint8_t  lwsr	:1;
				uint8_t  lwews	:1;
				unsigned	:3;
				uint16_t leo	:16;
			};
		};
	}vscc_table[128];
};
#endif // ICH_DESCRIPTORS_FROM_MMAP_DUMP

void prettyprint_ich_descriptors(enum chipset);

void prettyprint_ich_descriptor_map(void);
void prettyprint_ich_descriptor_component(void);
void prettyprint_ich_descriptor_region(void);
void prettyprint_ich_descriptor_master(void);

int getFCBA_component_density(uint8_t comp);

#ifdef ICH_DESCRIPTORS_FROM_MMAP_DUMP

void prettyprint_ich_descriptor_upper_map(void);
void prettyprint_ich_descriptor_straps(enum chipset cs);
int read_ich_descriptors_from_dump(uint32_t *dump, enum chipset cs);

#else // ICH_DESCRIPTORS_FROM_MMAP_DUMP

void read_ich_descriptors_from_fdo(void *spibar);

#endif // ICH_DESCRIPTORS_FROM_MMAP_DUMP

#endif // __ICH_DESCRIPTORS_H__
#endif // defined(__i386__) || defined(__x86_64__)
