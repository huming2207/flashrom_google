/*
 * This file is part of the flashrom project.
 *
 * Copyright (C) 2007, 2008, 2009 Carl-Daniel Hailfinger
 * Copyright (C) 2008 Ronald Hoogenboom <ronald@zonnet.nl>
 * Copyright (C) 2008 coresystems GmbH
 * Copyright (C) 2010 Google Inc.
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
 * Contains the ITE IT85* SPI specific routines
 *
 * FIXME: EC firmware updates on this chip can be interrupted due to factors
 * such as SMBus traffic. YOU MUST DISABLE any services, such as power
 * management daemons, which can interact with the EC during firmware update.
 */

#if defined(__i386__) || defined(__x86_64__)

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "flash.h"
#include "spi.h"
#include "programmer.h"

/* Supported ECs, ITE_LAST should always be LAST member */
enum ite_chip_id {
	ITE_IT85XX,
	ITE_IT8518,
	ITE_LAST
};

/* chip-specific parameters */
typedef struct {
	enum ite_chip_id   chip_id;
	uint8_t            port_data;
	uint8_t            port_cmd;
	uint8_t            copy_to_sram_cmd;
	uint8_t            exit_sram_cmd;
	uint32_t           exit_sram_delay;
} ite_chip;

/* table of supported chips + parameters, order by ite_chip_id index */
static ite_chip ite_chips[] = {
	{ ITE_IT85XX,
	  0x60,
	  0x64,
	  0xB4,
	  0xFE,
	  0,
	},

	{ ITE_IT8518,
	  0x62,
	  0x66,
	  0xDD,		/* default value, see note in it85xx_spi_send_command */
	  0xFF,
	  500000,
	}
};

/* pointer to table entry of identified chip */
static ite_chip *found_chip;

#define MAX_TIMEOUT 100000
#define MAX_TRY 5

/* Constants for I/O ports */
#define ITE_SUPERIO_PORT1	0x2e
#define ITE_SUPERIO_PORT2	0x4e

/* Constants for Logical Device registers */
#define LDNSEL			0x07

/* These are standard Super I/O 16-bit base address registers */
#define SHM_IO_BAR0		0x60  /* big-endian, this is high bits */
#define SHM_IO_BAR1		0x61

/* The 8042 keyboard controller uses an input buffer and an output buffer to
 * communicate with the host CPU. Both buffers are 1-byte depth. That means
 * IBF is set to 1 when the host CPU sends a command to the input buffer 
 * of the EC. IBF is cleared to 0 once the command is read by the EC.
 */
#define KB_IBF 			(1 << 1)  /* Input Buffer Full */
#define KB_OBF 			(1 << 0)  /* Output Buffer Full */

/* IT8502 supports two access modes:
 *   LPC_MEMORY: through the memory window in 0xFFFFFxxx (follow mode)
 *   LPC_IO: through I/O port (so called indirect memory)
 */
#undef LPC_MEMORY
#define LPC_IO

#ifdef LPC_IO
/* macro to fill in indirect-access registers. */
#define INDIRECT_A0(base, value) OUTB(value, (base) + 0)  /* little-endian */
#define INDIRECT_A1(base, value) OUTB(value, (base) + 1)
#define INDIRECT_A2(base, value) OUTB(value, (base) + 2)
#define INDIRECT_A3(base, value) OUTB(value, (base) + 3)
#define INDIRECT_READ(base) INB((base) + 4)
#define INDIRECT_WRITE(base, value) OUTB(value, (base) + 4)
#endif  /* LPC_IO */

#ifdef LPC_IO
unsigned int shm_io_base;
#endif
unsigned char *ce_high, *ce_low;
static int it85xx_scratch_rom_reenter = 0;

/* This function will poll the keyboard status register until either
 * an expected value shows up, or the timeout is reached.
 * timeout is in usec.
 *
 * Returns: 0 -- the expected value showed up.
 *          1 -- timeout.
 */
static int wait_for(const unsigned int mask, const unsigned int expected_value,
		    const int timeout, const char * error_message,
		    const char * function_name, const int lineno)
{
	int time_passed;

	for (time_passed = 0;; ++time_passed) {
		if ((INB(found_chip->port_cmd) & mask) == expected_value)
			return 0;
		if (time_passed >= timeout)
			break;
		programmer_delay(1);
	}
	if (error_message)
		msg_perr("%s():%d %s", function_name, lineno, error_message);
	return 1;
}

/* IT8502 employs a scratch RAM when flash is being updated. Call the following
 * two functions before/after flash erase/program. */
void it85xx_enter_scratch_rom(void)
{
	int ret, tries;

	if (it85xx_scratch_rom_reenter > 0)
		return;

	msg_pdbg("%s: entering scratch rom mode\n", __func__);

	for (tries = 0; tries < MAX_TRY; ++tries) {
		/* Wait until IBF (input buffer) is not full. */
		if (wait_for(KB_IBF, 0, MAX_TIMEOUT,
		             "* timeout at waiting for IBF==0.\n",
		             __func__, __LINE__))
			continue;

		/* Copy EC firmware to SRAM. */
		OUTB(found_chip->copy_to_sram_cmd, found_chip->port_cmd);

		/* Confirm EC has taken away the command. */
		if (wait_for(KB_IBF, 0, MAX_TIMEOUT,
		             "* timeout at taking command.\n",
		             __func__, __LINE__))
			continue;

		/* Waiting for OBF (output buffer) has data.
		 * Note sometimes the replied command might be stolen by kernel
		 * ISR so that it is okay as long as the command is 0xFA. */
		if (wait_for(KB_OBF, KB_OBF, MAX_TIMEOUT, NULL, NULL, 0))
			msg_pdbg("%s():%d * timeout at waiting for OBF.\n",
			         __func__, __LINE__);
		if ((ret = INB(found_chip->port_data)) == 0xFA) {
			break;
		} else {
			msg_perr("%s():%d * not run on SRAM ret=%d\n",
			         __func__, __LINE__, ret);
			continue;
		}
	}

	if (tries < MAX_TRY) {
		/* EC already runs on SRAM */
		it85xx_scratch_rom_reenter++;
		msg_pdbg("%s():%d * SUCCESS.\n", __func__, __LINE__);
	} else {
		msg_perr("%s():%d * Max try reached.\n", __func__, __LINE__);
	}
}

void it85xx_exit_scratch_rom(void)
{
	int tries;

	msg_pdbg("%s():%d was called ...\n", __func__, __LINE__);
	if (it85xx_scratch_rom_reenter <= 0)
		return;

	for (tries = 0; tries < MAX_TRY; ++tries) {
		/* Wait until IBF (input buffer) is not full. */
		if (wait_for(KB_IBF, 0, MAX_TIMEOUT,
		             "* timeout at waiting for IBF==0.\n",
		             __func__, __LINE__))
			continue;

		/* Exit SRAM. Run on flash. */
		OUTB(found_chip->exit_sram_cmd, found_chip->port_cmd);

		/* Confirm EC has taken away the command. */
		if (wait_for(KB_IBF, 0, MAX_TIMEOUT,
		             "* timeout at taking command.\n",
		             __func__, __LINE__)) {
			/* We cannot ensure if EC has exited update mode.
			 * If EC is in normal mode already, a further 0xFE
			 * command will reboot system. So, exit loop here. */
			tries = MAX_TRY;
			break;
		}

		break;
	}

	if (tries < MAX_TRY) {
		it85xx_scratch_rom_reenter = 0;
		msg_pdbg("%s():%d * SUCCESS.\n", __func__, __LINE__);
	} else {
		msg_perr("%s():%d * Max try reached.\n", __func__, __LINE__);
	}

	programmer_delay(found_chip->exit_sram_delay);
}

static int it85xx_shutdown(void *data)
{
	msg_pdbg("%s():%d\n", __func__, __LINE__);
	it85xx_exit_scratch_rom();

	return 0;	/* FIXME: Should probably return something meaningful */
}

static int it85xx_spi_common_init(struct superio s)
{
	chipaddr base;

	msg_pdbg("%s():%d superio.vendor=0x%02x\n", __func__, __LINE__,
	         s.vendor);

	if (register_shutdown(it85xx_shutdown, NULL))
		return 1;

#ifdef LPC_IO
	/* Get LPCPNP of SHM. That's big-endian. */
	sio_write(s.port, LDNSEL, 0x0F); /* Set LDN to SHM (0x0F) */
	shm_io_base = (sio_read(s.port, SHM_IO_BAR0) << 8) +
	              sio_read(s.port, SHM_IO_BAR1);
	msg_pdbg("%s():%d shm_io_base=0x%04x\n", __func__, __LINE__,
	         shm_io_base);

	/* These pointers are not used directly. They will be send to EC's
	 * register for indirect access. */
	base = 0xFFFFF000;
	ce_high = ((unsigned char *)base) + 0xE00;  /* 0xFFFFFE00 */
	ce_low = ((unsigned char *)base) + 0xD00;  /* 0xFFFFFD00 */

	/* pre-set indirect-access registers since in most of cases they are
	 * 0xFFFFxx00. */
	INDIRECT_A0(shm_io_base, base & 0xFF);
	INDIRECT_A2(shm_io_base, (base >> 16) & 0xFF);
	INDIRECT_A3(shm_io_base, (base >> 24));
#endif
#ifdef LPC_MEMORY
	/* FIXME: We should block accessing that region for anything else.
	 * Major TODO here, and it will be a lot of work.
	 */
	base = (chipaddr)physmap("it85 communication", 0xFFFFF000, 0x1000);
	msg_pdbg("%s():%d base=0x%08x\n", __func__, __LINE__,
	         (unsigned int)base);
	ce_high = (unsigned char *)(base + 0xE00);  /* 0xFFFFFE00 */
	ce_low = (unsigned char *)(base + 0xD00);  /* 0xFFFFFD00 */
#endif

	return 0;
}

/* According to ITE 8502 document, the procedure to follow mode is following:
 *   1. write 0x00 to LPC/FWH address 0xffff_fexxh (drive CE# high)
 *   2. write data to LPC/FWH address 0xffff_fdxxh (drive CE# low and MOSI
 *      with data)
 *   3. read date from LPC/FWH address 0xffff_fdxxh (drive CE# low and get
 *      data from MISO)
 */
static int it85xx_spi_send_command(const struct flashctx *flash, unsigned int writecnt, unsigned int readcnt,
			const unsigned char *writearr, unsigned char *readarr)
{
	int i;
	static int wdt_reset_flag_set = 0;

	if (found_chip->chip_id == ITE_IT8518) {
		/*
		 * 0xd8 - Sets WDT reset flag to reboot EC after exiting from
		 *        scratch mode. This flag will be be checked when
		 *        command 0x8c is received. Use this when changing
		 *        ROM content (erase / write commands).
		 * 0xdd - Same as d8, but without setting the WDT reset flag.
		 *        Use this for commands that do not change EC code.
		 *
		 * If opcode will cause ROM content to change and scratch
		 * mode has been previously entered, then we need to re-enter
		 * scratch mode using 0xd8.
		 *
		 * FIXME(dhendrix): This is specific to Stout. We should use
		 * better method to apply board hacks rather than using the
		 * EC's chip ID as the condition.
		 */
		switch (writearr[0]) {
		case JEDEC_BYTE_PROGRAM:
		case JEDEC_BE_52:
		case JEDEC_BE_D7:
		case JEDEC_BE_D8:
		case JEDEC_CE_60:
		case JEDEC_CE_C7:
		case JEDEC_SE:
			if (!wdt_reset_flag_set) {
				msg_pdbg("%s: changing copy_to_sram_cmd\n",
					__func__);
				found_chip->copy_to_sram_cmd = 0xd8;
				it85xx_exit_scratch_rom();
				wdt_reset_flag_set = 1;
			}
			break;
		default:
			break;
		}
	}

	it85xx_enter_scratch_rom();
	/* Exit scratch ROM ONLY when programmer shuts down. Otherwise, the
	 * temporary flash state may halt the EC.
	 */

#ifdef LPC_IO
	INDIRECT_A1(shm_io_base, (((unsigned long int)ce_high) >> 8) & 0xff);
	INDIRECT_WRITE(shm_io_base, 0xFF);  /* Write anything to this address.*/
	INDIRECT_A1(shm_io_base, (((unsigned long int)ce_low) >> 8) & 0xff);
#endif
#ifdef LPC_MEMORY
	mmio_writeb(0, ce_high);
#endif
	for (i = 0; i < writecnt; ++i) {
#ifdef LPC_IO
		INDIRECT_WRITE(shm_io_base, writearr[i]);
#endif
#ifdef LPC_MEMORY
		mmio_writeb(writearr[i], ce_low);
#endif
	}
	for (i = 0; i < readcnt; ++i) {
#ifdef LPC_IO
		readarr[i] = INDIRECT_READ(shm_io_base);
#endif
#ifdef LPC_MEMORY
		readarr[i] = mmio_readb(ce_low);
#endif
	}
#ifdef LPC_IO
	INDIRECT_A1(shm_io_base, (((unsigned long int)ce_high) >> 8) & 0xff);
	INDIRECT_WRITE(shm_io_base, 0xFF);  /* Write anything to this address.*/
#endif
#ifdef LPC_MEMORY
	mmio_writeb(0, ce_high);
#endif

	return 0;
}

static const struct spi_master spi_master_it8518 = {
	.type = SPI_CONTROLLER_IT85XX,
	.max_data_read = 256,
	.max_data_write = 256,
	.command = it85xx_spi_send_command,
	.multicommand = default_spi_send_multicommand,
	.read = default_spi_read,
	.write_256 = default_spi_write_256,
};

static const struct spi_master spi_master_it85xx = {
	.type = SPI_CONTROLLER_IT85XX,
	.max_data_read = 1,
	.max_data_write = 1,
	.command = it85xx_spi_send_command,
	.multicommand = default_spi_send_multicommand,
	.read = default_spi_read,
	.write_256 = default_spi_write_256,
};

/* it8518-specific i/o initialization */
void setup_it8518_io_base()
{
	OUTB(0x07, 0x2e); /* Set LDN to SHM */
	OUTB(0x0f, 0x2f);
	OUTB(0x60, 0x2e); /* Set IO space to 0x3F0 */
	OUTB(0x03, 0x2f);
	OUTB(0x61, 0x2e);
	OUTB(0xf0, 0x2f);
	OUTB(0x30, 0x2e); /* Enable this Logical Device */
	OUTB(0x01, 0x2f);
}

static int check_params(void)
{
	int ret = 0;
	char *p = NULL;

	p = extract_programmer_param("type");
	if (p && strcmp(p, "ec")) {
		msg_pdbg("it85xx only supports \"ec\" type devices\n");
		ret = 1;
	}

	free(p);
	return ret;
}

int it8518_spi_init(struct superio s)
{
	int ret;

	if (check_params())
		return 1;

	if (!(internal_buses_supported & BUS_FWH)) {
		msg_pdbg("%s():%d buses not support FWH\n", __func__, __LINE__);
		return 1;
	}

	found_chip = &ite_chips[ITE_IT8518];

#ifdef LPC_IO
        setup_it8518_io_base();
#endif

	ret = it85xx_spi_common_init(s);
	if (!ret) {
		msg_pdbg("%s: internal_buses_supported=0x%x\n", __func__,
		          internal_buses_supported);
		/* Check for FWH because IT85 listens to FWH cycles.
		 * FIXME: The big question is whether FWH cycles are necessary
		 * for communication even if LPC_IO is defined.
		 */
		if (internal_buses_supported & BUS_FWH)
			msg_pdbg("Registering IT85 SPI.\n");
		/* FIXME: Really leave FWH enabled? We can't use this region
		 * anymore since accessing it would mess up IT85 communication.
		 * If we decide to disable FWH for this region, we should print
		 * a debug message about it.
		 */
		/* Set this as SPI controller and add FWH | LPC to
		 * supported buses. */
		buses_supported |= BUS_LPC | BUS_FWH;
		register_spi_master(&spi_master_it8518);
	}
	return ret;
}

int it85xx_spi_init(struct superio s)
{
	int ret;

	if (alias && alias->type != ALIAS_EC)
		return 1;

	if (check_params())
		return 1;

	found_chip = &ite_chips[ITE_IT85XX];

	/*
	 * FIXME: This is necessary to ensure that access to the shared access
	 * window region is sent on the LPC bus. The old CLI syntax
	 * (-p internal:bus=lpc) would cause the chipset enable code to set the
	 * target bus appropriately before this function gets run, but the new
	 * syntax ("-p ec") does not cause that to happen.
	 */
	target_bus = BUS_LPC;
	msg_pdbg("%s: forcing target bus: 0x%08x\n", __func__, target_bus);
	chipset_flash_enable();

	ret = it85xx_spi_common_init(s);
	msg_pdbg("FWH: %s():%d ret=%d\n", __func__, __LINE__, ret);
	if (!ret) {
		msg_pdbg("%s: internal_buses_supported=0x%x\n", __func__,
		          internal_buses_supported);
		/* Check for FWH because IT85 listens to FWH cycles.
		 * FIXME: The big question is whether FWH cycles are necessary
		 * for communication even if LPC_IO is defined.
		 */
		if (internal_buses_supported & BUS_FWH)
			msg_pdbg("Registering IT85 SPI.\n");
		/* FIXME: Really leave FWH enabled? We can't use this region
		 * anymore since accessing it would mess up IT85 communication.
		 * If we decide to disable FWH for this region, we should print
		 * a debug message about it.
		 */
		/* Set this as SPI controller and add FWH | LPC to
		 * supported buses. */
		buses_supported |= BUS_LPC | BUS_FWH;
		register_spi_master(&spi_master_it85xx);
	}

	return ret;
}

#endif
