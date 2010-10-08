/*
 * This file is part of the flashrom project.
 *
 * Copyright (C) 2010 Google, Inc.
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
 * Neither the name of Nuvoton Technology Corporation. or the names of 
 * contributors or licensors may be used to endorse or promote products derived 
 * from this software without specific prior written permission.
 * 
 * This software is provided "AS IS," without a warranty of any kind. 
 * ALL EXPRESS OR IMPLIED CONDITIONS, REPRESENTATIONS AND WARRANTIES, 
 * INCLUDING ANY IMPLIED WARRANTY OF MERCHANTABILITY, FITNESS FOR A
 * PARTICULAR PURPOSE OR NON-INFRINGEMENT, ARE HEREBY EXCLUDED.
 * NUVOTON TECHNOLOGY CORPORATION. ("NUVOTON") AND ITS LICENSORS SHALL NOT BE LIABLE
 * FOR ANY DAMAGES SUFFERED BY LICENSEE AS A RESULT OF USING, MODIFYING
 * OR DISTRIBUTING THIS SOFTWARE OR ITS DERIVATIVES.  IN NO EVENT WILL
 * SUN OR ITS LICENSORS BE LIABLE FOR ANY LOST REVENUE, PROFIT OR DATA,
 * OR FOR DIRECT, INDIRECT, SPECIAL, CONSEQUENTIAL, INCIDENTAL OR
 * PUNITIVE DAMAGES, HOWEVER CAUSED AND REGARDLESS OF THE THEORY OF
 * LIABILITY, ARISING OUT OF THE USE OF OR INABILITY TO USE THIS SOFTWARE,
 * EVEN IF SUN HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGES.
 *
 * This is an UNOFFICIAL patch for the Nuvoton WPCE775x/NPCE781x. It was tested
 * for a specific hardware and firmware configuration and should be considered
 * unreliable. Please see the following URL for Nuvoton's authoritative,
 * officially supported flash update utility:
 * http://sourceforge.net/projects/nuvflashupdate/
 */

#include <assert.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "flash.h"
#include "chipdrivers.h"
#include "flashchips.h"
#include "programmer.h"
#include "spi.h"
#include "writeprotect.h"

/**
 *  Definition of WPCE775X WCB (Write Command Buffer), as known as Shared Access
 *  Window 2.
 *
 *  The document name is "WPCE775X Software User Guide Revision 1.2".
 *
 *  Assume the host is little endian.
 */
__attribute__((packed))
struct wpce775x_wcb {
	/* Byte 0: semaphore byte */
	unsigned char exe:1;  /* Bit0-RW- set by host. means wcb is ready to execute.
	                         should be cleared by host after RDY=1. */
	unsigned char resv0_41:4;
	unsigned char pcp:1;  /* Bit5-RO- set by EPCE775x. means preparation operations for
	                         flash update process is complete. */
	unsigned char err:1;  /* Bit6-RO- set by EPCE775x. means an error occurs. */
	unsigned char rdy:1;  /* Bit7-RO- set by EPCE775x. means operation is completed. */

	/* Byte 1-2: reserved */
	unsigned char byte1;
	unsigned char byte2;

	/* Byte 3: command code */
	unsigned char code;

	/* Byte 4-15: command field */
	unsigned char field[12];
};

/* The physical address of WCB -- Shared Access Window 2. */
static chipaddr wcb_physical_address;

/* The virtual address of WCB -- Shared Access Window 2. */
static volatile struct wpce775x_wcb *volatile wcb;

/* count of entering flash update mode */
static int in_flash_update_mode;

/* Indicate the flash chip attached to the WPCE7xxx chip.
 * This variable should be set in probe_wpce775x().
 * 0 means we haven't or cannot detect the chip type. */
struct flashchip *scan = 0;

/* SuperI/O related definitions and functions. */
/* Strapping options */
#define NUVOTON_SIO_PORT1	0x2e	/* No pull-down resistor */
#define NUVOTON_SIO_PORT2	0x164e	/* Pull-down resistor on BADDR0 */
/* Note: There's another funky state that we won't worry about right now */

/* SuperI/O Config */
#define NUVOTON_SIOCFG_LDN	0x07	/* LDN Bank Selector */
#define NUVOTON_SIOCFG_SID	0x20	/* SuperI/O ID */
#define NUVOTON_SIOCFG_SRID	0x27	/* SuperI/O Revision ID */
#define NUVOTON_LDN_SHM         0x0f    /* LDN of SHM module */

/* WPCE775x shared memory config registers (LDN 0x0f) */
#define WPCE775X_SHM_BASE_MSB	0x60
#define WPCE775X_SHM_BASE_LSB	0x61
#define WPCE775X_SHM_CFG	0xf0
#define WPCE775X_SHM_CFG_BIOS_FWH_EN	(1 << 3)
#define WPCE775X_SHM_CFG_FLASH_ACC_EN	(1 << 2)
#define WPCE775X_SHM_CFG_BIOS_EXT_EN	(1 << 1)
#define WPCE775X_SHM_CFG_BIOS_LPC_EN	(1 << 0)
#define WPCE775X_WIN_CFG		0xf1	/* window config */
#define WPCE775X_WIN_CFG_SHWIN_ACC	(1 << 6)

/* Shared access window 2 bar address registers */
#define WPCE775X_SHAW2BA_0	0xf8
#define WPCE775X_SHAW2BA_1	0xf9
#define WPCE775X_SHAW2BA_2	0xfa
#define WPCE775X_SHAW2BA_3	0xfb

/** probe for super i/o index
 *  @returns 0 to indicate success, <0 to indicate error
 */
static int nuvoton_get_sio_index(uint16_t *port)
{
	uint16_t ports[] = { NUVOTON_SIO_PORT2,
	                     NUVOTON_SIO_PORT1,
	};
	int i;
	static uint16_t port_internal, port_found = 0;

	if (port_found) {
		*port = port_internal;
		return 0;
	}

	get_io_perms();

	for (i = 0; i < ARRAY_SIZE(ports); i++) {
		uint8_t sid = sio_read(ports[i], NUVOTON_SIOCFG_SID);

		if (sid == 0xfc) {  /* Family ID */
			port_internal = ports[i];
			port_found = 1;
			break;
		}
	}

	if (!port_found) {
		msg_cdbg("\nfailed to obtain super i/o index");
		return -1;
	}

	msg_cdbg("\nsuper i/o index = 0x%04x\n", port_internal);
	*port = port_internal;
	return 0;
}

/** Call superio to get pre-configured WCB address.
 *  Read LDN 0x0f (SHM) idx:f8-fb (little-endian).
 */
static int get_shaw2ba(chipaddr *shaw2ba)
{
	uint16_t idx;
	uint8_t org_ldn;
	uint8_t win_cfg;
	uint8_t shm_cfg;

	if (nuvoton_get_sio_index(&idx) < 0)
		return -1;

	org_ldn = sio_read(idx, NUVOTON_SIOCFG_LDN);
	sio_write(idx, NUVOTON_SIOCFG_LDN, NUVOTON_LDN_SHM);

	/*
	 * To obtain shared access window 2 base address, we must OR the base
	 * address bytes, where SHAW2BA_0 is least significant and SHAW2BA_3
	 * most significant.
	 */
	*shaw2ba = sio_read(idx, WPCE775X_SHAW2BA_0) |
	           (sio_read(idx, WPCE775X_SHAW2BA_1) << 8) |
	           (sio_read(idx, WPCE775X_SHAW2BA_2) << 16) |
	           (sio_read(idx, WPCE775X_SHAW2BA_3) << 24);

	/*
	 * If SHWIN_ACC is cleared, then we're using LPC memory access
	 * and SHAW2BA_3-0 indicate bits 31-0. If SHWIN_ACC is set, then
	 * bits 7-4 of SHAW2BA_3 are ignored and bits 31-28 are indicated
	 * by the idsel nibble. (See table 25 "supported host address ranges"
	 * for more details)
	 */
	win_cfg = sio_read(idx, WPCE775X_WIN_CFG);
	if (win_cfg & WPCE775X_WIN_CFG_SHWIN_ACC) {
		uint8_t idsel;

		/* Make sure shared BIOS memory is enabled */
		shm_cfg = sio_read(idx, WPCE775X_SHM_CFG);
		if ((shm_cfg & WPCE775X_SHM_CFG_BIOS_FWH_EN))
			idsel = 0xf;
		else {
			msg_cdbg("Shared BIOS memory is diabled.\n");
			msg_cdbg("Please check SHM_CFG:BIOS_FWH_EN.\n");
	                goto error;
		}

		*shaw2ba &= 0x0fffffff;
		*shaw2ba |= idsel << 28;
	}
	
	sio_write(idx, NUVOTON_SIOCFG_LDN, org_ldn);
	return 0;
error:
	sio_write(idx, NUVOTON_SIOCFG_LDN, org_ldn);
	return -1;
}

/* Call superio to get pre-configured fwh_id.
 * Read LDN 0x0f (SHM) idx:f0.
 */
static int get_fwh_id(uint8_t *fwh_id)
{
	uint16_t idx;
	uint8_t org_ldn;

	if (nuvoton_get_sio_index(&idx) < 0)
		return -1;

	org_ldn = sio_read(idx, NUVOTON_SIOCFG_LDN);
	sio_write(idx, NUVOTON_SIOCFG_LDN, NUVOTON_LDN_SHM);
	*fwh_id = sio_read(idx, WPCE775X_SHM_CFG);
	sio_write(idx, NUVOTON_SIOCFG_LDN, org_ldn);

	return 0;
}

/** helper function to make sure the exe bit is 0 (no one is using EC).
 *  @return 1 for error; 0 for success.
 */
static int assert_ec_is_free(void)
{
	if (wcb->exe)
		msg_perr("ASSERT(wcb->exe==0), entering busy loop.\n");
	while(wcb->exe);
	return 0;
}

/** Trigger EXE bit, and block until operation completes.
 *  @return 1 for error; and 0 for success.
 */
static int blocked_exec(void)
{
	struct timeval begin, now;
	int timeout;  /* not zero if timeout occurs */
	int err;

	assert(wcb->rdy==0);

	/* raise EXE bit, and wait for operation complete or error occur. */
	wcb->exe = 1;

	timeout = 0;
	gettimeofday(&begin, NULL);
	while(wcb->rdy==0 && wcb->err==0) {
		gettimeofday(&now, NULL);
		/* According to Nuvoton's suggestion, few seconds is enough for
	         * longest flash operation, which is erase.
		 * Cutted from W25X16 datasheet, for max operation time
		 *   Byte program        tBP1  50us
		 *   Page program        tPP    3ms
		 *   Sector Erase (4KB)  tSE  200ms
		 *   Block Erase (64KB)  tBE    1s
		 *   Chip Erase          tCE   20s
	         * Since WPCE775x doesn't support chip erase,
		 * 3 secs is long enough for block erase.
		 */
		if ((now.tv_sec - begin.tv_sec) >= 4) {
			timeout += 1;
			break;
		}
	}

	/* keep ERR bit before clearing EXE bit. */
	err = wcb->err;

	/* Clear EXE bit, and wait for RDY back to 0. */
	wcb->exe = 0;
	gettimeofday(&begin, NULL);
	while(wcb->rdy) {
		gettimeofday(&now, NULL);
		/* 1 sec should be long enough for clearing rdy bit. */
		if (((now.tv_sec - begin.tv_sec)*1000*1000 +
	             (now.tv_usec - begin.tv_usec)) >= 1000*1000) {
			timeout += 1;
			break;
		}
	}

	if (err || timeout) {
		msg_cdbg("err=%d timeout=%d\n", err, timeout);
		return 1;
	}
	return 0;
}

/** Initialize the EC parameters.
 *  @return 1 for error; 0 for success.
 */
static int InitFlash(unsigned char srp)
{
	assert_ec_is_free();

	/* Byte 3: command code: Init Flash */
	wcb->code = 0x5A;

	/* Byte 4: opcode for Read Device Id */
	wcb->field[0] = JEDEC_RDID;

	/* Byte 5: opcode for Write Status Enable
	           JEDEC_EWSR defines 0x50, but W25Q16
	           accepts 0x06 to enable write status */
	wcb->field[1] = 0x06;

	/* Byte 6: opcode for Write Enable */
	wcb->field[2] = JEDEC_WREN;

	/* Byte 7: opcode for Read Status Register */
	wcb->field[3] = JEDEC_RDSR;

	/* Byte 8: opcode for Write Status Register */
	wcb->field[4] = JEDEC_WRSR;

	/* Byte 9: opcode for Flash Program */
	wcb->field[5] = JEDEC_BYTE_PROGRAM;

	/* Byte A: opcode for Sector or Block Erase. 0xD8: Block Erase (64KB), 0x20: Sector Erase (4KB) */
	/* TODO: dhendrix: We may need a more sophisticated routine to determine the proper values on a chip-by-chip basis in the future. */
	wcb->field[6] = JEDEC_SE;

	/* Byte B: status busy mask */
	wcb->field[7] = 0x01;

	/* Byte C: Status Register Value */
	wcb->field[8] = srp;  /* SRP (Status Register Protect), {TB, BP2, BP1, BP0} = {1, 0, 1, 1} */

	/* Byte D: Program Unit Size */
	wcb->field[9] = 0x01;

	/* Byte E: Page Size, 2^X bytes */
	wcb->field[10] = 0x08;

	/* Byte F: Read Device ID Type. 0x47 -- send 3 dummy addresses before read ID from flash */
	wcb->field[11] = 0x00;

	if (blocked_exec())
		return 1;
	return 0;
}

/** Read flash vendor/device IDs through EC.
 *  @param id0, id1, id2, id3 Pointers to store detected IDs. NULL will be ignored.
 *  @return 1 for error; 0 for success.
 */
static int ReadId(unsigned char* id0, unsigned char* id1,
	          unsigned char* id2, unsigned char* id3)
{
	assert_ec_is_free();

	wcb->code = 0xC0;       /* Byte 3: command code: Read ID */
	if (blocked_exec())
		return 1;

	msg_cdbg("id0: 0x%2x, id1: 0x%2x, id2: 0x%2x, id3: 0x%2x\n",
	         wcb->field[0], wcb->field[1], wcb->field[2], wcb->field[3]);
	if (id0) {
		*id0 = wcb->field[0];
	}
	if (id1) {
		*id1 = wcb->field[1];
	}
	if (id2) {
		*id2 = wcb->field[2];
	}
	if (id3) {
		*id3 = wcb->field[3];
	}

	return 0;
}

/** probe if WPCE775x is present.
 *  @return 0 for error; 1 for success
 */
int probe_wpce775x(struct flashchip *flash)
{
	unsigned char ids[4];
	unsigned long base;
	uint16_t sio_port;
	uint8_t srid;
	uint8_t fwh_id;
	uint32_t size;
	chipaddr original_memory;
	uint32_t original_size;
	int i;

	/* detect if wpce775x exists */
	if (nuvoton_get_sio_index(&sio_port) < 0) {
		msg_cdbg("No Nuvoton chip is found.\n");
		return 0;
	}
	srid = sio_read(sio_port, NUVOTON_SIOCFG_SRID);
	if ((srid & 0xE0) == 0xA0) {
		msg_pinfo("Found EC: WPCE775x (Vendor:0x%02x,ID:0x%02x,Rev:0x%02x) on sio_port:0x%x.\n",
		          sio_read(sio_port, NUVOTON_SIOCFG_SID),
		          srid >> 5, srid & 0x1f, sio_port);

	} else {
		msg_pinfo("Found EC: Nuvoton (Vendor:0x%02x,ID:0x%02x,Rev:0x%02x) on sio_port:0x%x.\n",
		          sio_read(sio_port, NUVOTON_SIOCFG_SID),
		          srid >> 5, srid & 0x1f, sio_port);
	}

	/* get the address of Shadow Window 2. */
	if (get_shaw2ba(&wcb_physical_address) < 0) {
		msg_cdbg("Cannot get the address of Shadow Window 2");
		return 0;
	}
	msg_cdbg("Get the address of WCB(SHA WIN2) at 0x%08x\n",
	         (uint32_t)wcb_physical_address);
	wcb = (struct wpce775x_wcb *)
	      programmer_map_flash_region("WPCE775X WCB",
	                                  wcb_physical_address,
	                                  getpagesize() /* min page size */);
	msg_cdbg("mapped wcb address: %p for physical addr: 0x%08lx\n", wcb, wcb_physical_address);
	if (!wcb) {
		msg_perr("FATAL! Cannot map memory area for wcb physical address.\n");
		return 0;
	}
	memset((void*)wcb, 0, sizeof(*wcb));

	if (get_fwh_id(&fwh_id) < 0) {
		msg_cdbg("Cannot get fwh_id value.\n");
		return 0;
	}
	msg_cdbg("get fwh_id: 0x%02x\n", fwh_id);

	/* TODO: set fwh_idsel of chipset.
	         Currently, we employ "-p internal:fwh_idsel=0x0000223e". */

	/* Initialize the parameters of EC SHM component */
	if (InitFlash(0x00))
		return 0;

	/* Query the flash vendor/device ID */
	if (ReadId(&ids[0], &ids[1], &ids[2], &ids[3]))
		return 0;

	/* In current design, flash->virtual_memory is mapped before calling flash->probe().
	 * So that we have to update flash->virtual_memory after we know real flash size.
	 * Unmap allocated memory before allocate new memory. */
	original_size = flash->total_size * 1024;  /* original flash size */
	original_memory = flash->virtual_memory;

	for (scan = &flashchips[0]; scan && scan->name; scan++) {
	        if (!(scan->bustype & CHIP_BUSTYPE_SPI)) {
	                msg_cdbg("WPCE775x: %s bustype supports no SPI: %s\n",
	                         scan->name, flashbuses_to_text(scan->bustype));
	                continue;
	        }
	        if ((scan->manufacture_id != GENERIC_MANUF_ID) &&
	            (scan->manufacture_id != ids[0])) {
	                msg_cdbg("WPCE775x: %s manufacture_id does not match: 0x%02x\n",
	                         scan->name, scan->manufacture_id);
	                continue;
	        }
	        if ((scan->model_id != GENERIC_DEVICE_ID) &&
	            (scan->model_id != ((ids[1]<<8)|ids[2]))) {
	                msg_cdbg("WPCE775x: %s model_id does not match: 0x%02x\n",
	                         scan->name, scan->model_id);
	                continue;
	        }

	        msg_cdbg("WPCE775x: found the flashchip %s.\n", scan->name);

	        /* Copy neccesary information */
	        flash->total_size = scan->total_size;
	        flash->page_size = scan->page_size;
	        memcpy(flash->block_erasers, scan->block_erasers,
	               sizeof(scan->block_erasers));
	        /* .block_erase is pointed to EC-specific way. */
	        for (i = 0; i < NUM_ERASEFUNCTIONS; ++i)
	                flash->block_erasers[i].block_erase = erase_wpce775x;
	        break;
	}
	if (!scan || !scan->name) {
	        msg_cdbg("WPCE775x: cannot recognize the flashchip.\n");
	        scan = 0;  /* since scan is a global variable, reset pointer
	                    * to indicate nothing was detected */
	        return 0;
	}

	/* Unmap allocated memory before allocate new memory. */
	programmer_unmap_flash_region((void*)original_memory, original_size);

	/* In current design, flash->virtual_memory is mapped before calling flash->probe().
	 * So that we have to update flash->virtual_memory after we know real flash size.
	 * Map new virtual address here. */
	size = flash->total_size * 1024;  /* new flash size */
	base = 0xffffffff - size + 1;
	flash->virtual_memory = (chipaddr)programmer_map_flash_region("flash chip", base, size);
	msg_cdbg("Remap memory to 0x%08lx from base: 0x%08lx size=0x%08lx\n",
	         flash->virtual_memory, base, (long unsigned int)size);

	return 1;
}

/** Tell EC to "enter flash update" mode. */
int EnterFlashUpdate(void)
{
	if (in_flash_update_mode) {
		/* already in update mode */
		in_flash_update_mode++;
		return 0;
	}

	wcb->code = 0x10;  /* Enter Flash Update */
	wcb->field[0] = 0x55;  /* required pattern by EC */
	wcb->field[1] = 0xAA;  /* required pattern by EC */
	wcb->field[2] = 0xCD;  /* required pattern by EC */
	wcb->field[3] = 0xBE;  /* required pattern by EC */
	if (blocked_exec()) {
		return 1;
	} else {
		in_flash_update_mode++;
		return 0;
	}
}

/** Tell EC to "exit flash update" mode.
 *  Without calling this function, the EC stays in busy-loop and will not
 *  response further request from host, which means system will halt.
 */
int ExitFlashUpdate(unsigned char exit_code)
{
	if (in_flash_update_mode <= 0) {
		msg_cdbg("Not in flash update mode yet.\n");
		return 1;
	}

	if (in_flash_update_mode >= 2) {
		in_flash_update_mode--;
		return 0;
	}

	wcb->code = exit_code;  /* Exit Flash Update */
	if (blocked_exec()) {
		return 1;
	} else {
		in_flash_update_mode--;
		return 0;
	}
}

/*
 * Note: The EC firmware this patch has been tested with uses the following
 * codes to indicate flash update status:
 * 0x20 is used for EC F/W no change, but BIOS changed (in Share mode)
 * 0x21 is used for EC F/W changed. Goto EC F/W, wait system reboot.
 * 0x22 is used for EC F/W changed, Goto EC Watchdog reset. */
int ExitFlashUpdateFirmwareNoChange(void) {
	return ExitFlashUpdate(0x20);
}

int ExitFlashUpdateFirmwareChanged(void) {
	return ExitFlashUpdate(0x21);
}

int erase_wpce775x(struct flashchip *flash, unsigned int blockaddr, unsigned int blocklen)
{
	assert_ec_is_free();

	/* Set Write Window on flash chip (optional).
	 * You may limit the window to partial flash for experimental. */
	wcb->code = 0xC5;  /* Set Write Window */
	wcb->field[0] = 0x00;  /* window base: little-endian */
	wcb->field[1] = 0x00;
	wcb->field[2] = 0x00;
	wcb->field[3] = 0x00;
	wcb->field[4] = 0x00;  /* window length: little-endian */
	wcb->field[5] = 0x00;
	wcb->field[6] = 0x20;
	wcb->field[7] = 0x00;
	if (blocked_exec())
		return 1;

	/* TODO: here assume block sizes are identical. The right way is to traverse
	 *       block_erasers[] and find out the corresponding block size. */
	unsigned int block_size = flash->block_erasers[0].eraseblocks[0].size;
	unsigned int current;

	msg_cdbg("Erasing ... 0x%08x 0x%08x\n", blockaddr, blocklen);

	if (EnterFlashUpdate()) return 1;

	for (current = 0;
	     current < blocklen;
	     current += block_size) {
		wcb->code = 0x80;  /* Sector/block erase */

		/* WARNING: assume the block address for EC is lalways little-endian. */
		unsigned int addr = blockaddr + current;
		wcb->field[0] = addr & 0xff;
		wcb->field[1] = (addr >> 8) & 0xff;
		wcb->field[2] = (addr >> 16) & 0xff;
		wcb->field[3] = (addr >> 24) & 0xff;
		if (blocked_exec())
			goto error_exit;
	}

	if (ExitFlashUpdateFirmwareChanged()) return 1;

	if (check_erased_range(flash, blockaddr, blocklen)) {
		msg_perr("ERASE FAILED!\n");
		return 1;
	}

	return 0;

error_exit:
	ExitFlashUpdateFirmwareChanged();
	return 1;
}

/** Callback function for do_romentries().
 *  @flash - point to flash info
 *  @buf - the address of buffer
 *  @addr - the start offset in buffer
 *  @len - length to write (start from @addr)
 */
int write_wpce775x_entry(struct flashchip *flash, uint8_t *buf,
	                 const chipaddr addr, size_t len)
{
	chipaddr current;
	unsigned int block_size = flash->block_erasers[0].eraseblocks[0].size;
        unsigned int bytes_until_next_block;
        unsigned int written = 0;

	msg_cdbg("Writing ... 0x%08lx 0x%08lx\n", (long unsigned int)addr,
	                                          (long unsigned int)len);

	if (EnterFlashUpdate()) return 1;
	for (current = addr;
	     current < addr + len;
	     current += written/* maximum program buffer */) {
		/* erase sector before write it. */
		if ((current & (block_size-1))==0) {
			if (erase_wpce775x(flash, current, block_size))
				goto error_exit;
		}

		/* wpce775x provides a 8-byte program buffer. */
		wcb->code = 0xA0;  /* Set Address */
		wcb->field[0] = current & 0xff;
		wcb->field[1] = (current >> 8) & 0xff;
		wcb->field[2] = (current >> 16) & 0xff;
		wcb->field[3] = (current >> 24) & 0xff;
		if (blocked_exec())
			goto error_exit;

                bytes_until_next_block = block_size - (current % block_size);
                if (bytes_until_next_block > 0 && bytes_until_next_block < 8)
                        written = bytes_until_next_block;
                else
                        written = 8;  /* maximum buffer size */
		wcb->code = 0xB0 | written;
		int i;
		for (i = 0; i < written; i++) {
			wcb->field[i] = buf[current + i];
		}
		if (blocked_exec())
			goto error_exit;
	}

	if (ExitFlashUpdateFirmwareChanged()) return 1;
	return 0;

error_exit:
	ExitFlashUpdateFirmwareChanged();
	return 1;
}

/** Write data to flash (layout supported).
 *  In some cases, EC and BIOS share a physical flash chip. So it is dangerous
 *  to erase whole flash chip when you only wanna update BIOS image.
 *  By calling do_romentries(), we won't erase/program those blocks/sectors
 *  not specified by -i parameter.
 */
int write_wpce775x(struct flashchip *flash, uint8_t * buf)
{
	return do_romentries(buf, flash, write_wpce775x_entry);
}

int set_range_wpce775x(struct flashchip *flash, unsigned int start, unsigned int len) {
	struct w25q_status status;

	if (w25_range_to_status(scan, start, len, &status)) return -1;

	/* Since WPCE775x doesn't support reading status register, we have to
	 * set SRP0 to 1 when writing status register. */
	status.srp0 = 1;

	msg_cdbg("Going to set: 0x%02x\n", *((unsigned char*)&status));
	msg_cdbg("status.busy: %x\n", status.busy);
	msg_cdbg("status.wel: %x\n", status.wel);
	msg_cdbg("status.bp0: %x\n", status.bp0);
	msg_cdbg("status.bp1: %x\n", status.bp1);
	msg_cdbg("status.bp2: %x\n", status.bp2);
	msg_cdbg("status.tb: %x\n", status.tb);
	msg_cdbg("status.sec: %x\n", status.sec);
	msg_cdbg("status.srp0: %x\n", status.srp0);

	/* InitFlash (with particular status value), and EnterFlashUpdate() then
	 * ExitFlashUpdate() immediately. Thus, the flash status register will
	 * be updated. */
	if (InitFlash(*(unsigned char*)&status))
		return -1;
	if (EnterFlashUpdate()) return 1;
		ExitFlashUpdateFirmwareNoChange();

	return 0;
}

static int enable_wpce775x(struct flashchip *flash)
{
	msg_cdbg("WPCE775x always sets SRP0 in set_range_wpce775x()\n");
	return 0;
}


struct wp wp_wpce775x = {
	.set_range      = set_range_wpce775x,
	.enable         = enable_wpce775x,
};
