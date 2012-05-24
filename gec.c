/*
 * This file is part of the flashrom project.
 *
 * Copyright (C) 2012 The Chromium OS Authors. All rights reserved.
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "flashchips.h"
#include "fmap.h"
#include "gec_lpc_commands.h"
#include "programmer.h"
#include "spi.h"
#include "writeprotect.h"


/* 1 if we detect a GEC on system */
static int detected = 0;

/* 1 if we want the flashrom to call erase_and_write_flash() again. */
static int need_2nd_pass = 0;

/* The range of each firmware copy from the image file to update.
 * But re-define the .flags as the valid flag to indicate the firmware is
 * new or not (if flags = 1).
 */
static struct fmap_area fwcopy[4];  // [0] is not used.

/* The names of enum lpc_current_image to match in FMAP area names. */
static const char *sections[4] = {
	"UNKNOWN SECTION",  // EC_LPC_IMAGE_UNKNOWN -- never matches
	"RO_SECTION",       // EC_LPC_IMAGE_RO
	"RW_SECTION_A",     // EC_LPC_IMAGE_RW_A
	"RW_SECTION_B",     // EC_LPC_IMAGE_RW_B
};

static int ec_timeout_usec = 1000000;

/* Waits for the EC to be unbusy. Returns 1 if busy, 0 if not busy. */
static int ec_busy(int timeout_usec)
{
	int i;
	for (i = 0; i < timeout_usec; i += 10) {
		usleep(10);  /* Delay first, in case we just sent a command */
		if (!(inb(EC_LPC_ADDR_USER_CMD) & EC_LPC_STATUS_BUSY_MASK))
			return 0;
	}
	return 1;  /* Timeout */
}


static enum lpc_status gec_get_result() {
	return inb(EC_LPC_ADDR_USER_DATA);
}


/* Sends a command to the EC.  Returns the command status code, or
 * -1 if other error. */
int ec_command(int command, const void *indata, int insize,
	       void *outdata, int outsize) {
	uint8_t *d;
	int i;

	if ((insize + outsize) > EC_LPC_PARAM_SIZE) {
		msg_pdbg2("Data size too big for buffer.\n");
		return -1;
	}

	if (ec_busy(ec_timeout_usec)) {
		msg_pdbg2("Timeout waiting for EC ready\n");
		return -1;
	}

	/* Write data, if any */
	/* TODO: optimized copy using outl() */
	for (i = 0, d = (uint8_t *)indata; i < insize; i++, d++) {
		msg_pdbg2("GEC: Port[0x%x] <-- 0x%x\n",
		          EC_LPC_ADDR_USER_PARAM + i, *d);
		outb(*d, EC_LPC_ADDR_USER_PARAM + i);
	}

	msg_pdbg2("GEC: Run EC Command: 0x%x ----\n", command);
	outb(command, EC_LPC_ADDR_USER_CMD);

	if (ec_busy(1000000)) {
		msg_pdbg2("Timeout waiting for EC response\n");
		return -1;
	}

	/* Check status */
	if ((i = gec_get_result()) != EC_LPC_RESULT_SUCCESS) {
		msg_pdbg2("EC returned error status %d\n", i);
		return i;
	}

	/* Read data, if any */
	for (i = 0, d = (uint8_t *)outdata; i < outsize; i++, d++) {
		*d = inb(EC_LPC_ADDR_USER_PARAM + i);
		msg_pdbg2("GEC: Port[0x%x] ---> 0x%x\n",
		          EC_LPC_ADDR_USER_PARAM + i, *d);
	}

	return 0;
}


#ifdef SUPPORT_CHECKSUM
static verify_checksum(uint8_t* expected,
                       unsigned int addr,
                       unsigned int count) {
	int rc;
	struct lpc_params_flash_checksum csp;
	struct lpc_response_flash_checksum csr;
	uint8_t cs;
	int j;

	csp.offset = addr;
	csp.size = count;

	rc = ec_command(EC_LPC_COMMAND_FLASH_CHECKSUM,
			&csp, sizeof(csp), &csr, sizeof(csr));
	if (rc) {
		msg_perr("GEC: verify_checksum() error.\n");
		return rc;
	}

	for (cs = 0, j = 0; j < count; ++j) {
		BYTE_IN(cs, expected[j]);
	}
	if (cs != csr.checksum) {
		msg_pdbg("GEC: checksum dismatch at 0x%02x "
		         "(ec: 0x%02x, local: 0x%02x). Retry.\n",
		         addr, csr.checksum, cs);
		msg_pdbg("GEC: ");
		for (j = 0; j < count; ++j) {
			msg_pdbg("%02x-", expected[j]);
			if ((j & 15) == 15) msg_pdbg("\nGEC: ");
		}
		programmer_delay(1000);
		return 1;
	}
	return 0;
}
#endif  /* SUPPORT_CHECKSUM */


/* Given the range not able to update, mark the corresponding
 * firmware as old.
 */
static void gec_invalidate_copy(unsigned int addr, unsigned int len)
{
	int i;

	for (i = EC_LPC_IMAGE_RO; i < ARRAY_SIZE(fwcopy); i++) {
		struct fmap_area *fw = &fwcopy[i];
		if ((addr >= fw->offset && (addr < fw->offset + fw->size)) ||
		    (fw->offset >= addr && (fw->offset < addr + len))) {
			msg_pdbg("Mark firmware [%s] as old.\n",
			         sections[i]);
			fw->flags = 0;  // mark as old
		}
	}
}


/* Asks EC to jump to a firmware copy. If target is EC_LPC_IMAGE_UNKNOWN,
 * then this functions picks a NEW firmware copy and jumps to it. Note that
 * RO is preferred, then A, finally B.
 *
 * Returns 0 for success.
 */
static int gec_jump_copy(enum lpc_current_image target) {
	struct lpc_params_reboot_ec p;
	int rc;

	memset(&p, 0, sizeof(p));
	p.target = target != EC_LPC_IMAGE_UNKNOWN ? target :
	           fwcopy[EC_LPC_IMAGE_RO].flags ? EC_LPC_IMAGE_RO :
	           fwcopy[EC_LPC_IMAGE_RW_A].flags ? EC_LPC_IMAGE_RW_A :
	           fwcopy[EC_LPC_IMAGE_RW_B].flags ? EC_LPC_IMAGE_RW_B :
	           EC_LPC_IMAGE_UNKNOWN;
	msg_pdbg("GEC is jumping to [%s]\n", sections[p.target]);
	if (p.target == EC_LPC_IMAGE_UNKNOWN) return 1;

	rc = ec_command(EC_LPC_COMMAND_REBOOT_EC,
	                &p, sizeof(p), NULL, 0);
	if (rc) {
		msg_perr("GEC cannot jump to [%s]\n", sections[p.target]);
	} else {
		msg_pdbg("GEC has jumped to [%s]\n", sections[p.target]);
	}

	/* Sleep 1 sec to wait the EC re-init. */
	usleep(1000000);

	return rc;
}


/* Given an image, this function parses FMAP and recognize the firmware
 * ranges.
 */
int gec_prepare(uint8_t *image, int size) {
	struct fmap *fmap;
	int i, j;

	if (!detected) return 0;

	// Parse the fmap in the image file and cache the firmware ranges.
	fmap = fmap_find_in_memory(image, size);
	if (!fmap) return 0;

	// Lookup RO/A/B sections in FMAP.
	for (i = 0; i < fmap->nareas; i++) {
		struct fmap_area *fa = &fmap->areas[i];
		for (j = EC_LPC_IMAGE_RO; j < ARRAY_SIZE(sections); j++) {
			if (!strcmp(sections[j], (const char *)fa->name)) {
				msg_pdbg("Found '%s' in image.\n", fa->name);
				memcpy(&fwcopy[j], fa, sizeof(*fa));
				fwcopy[j].flags = 1;  // mark as new
			}
		}
	}

	return gec_jump_copy(EC_LPC_IMAGE_RO);
}


/* Returns >0 if we need 2nd pass of erase_and_write_flash().
 *         <0 if we cannot jump to any firmware copy.
 *        ==0 if no more pass is needed.
 *
 * This function also jumps to new-updated firmware copy before return >0.
 */
int gec_need_2nd_pass(void) {
	if (!detected) return 0;

	if (need_2nd_pass) {
		if (gec_jump_copy(EC_LPC_IMAGE_UNKNOWN)) {
			return -1;
		}
	}

	return need_2nd_pass;
}


int gec_read(struct flashchip *flash, uint8_t *readarr,
             unsigned int blockaddr, unsigned int readcnt) {
	int i;
	int rc = 0;
	struct lpc_params_flash_read p;
	struct lpc_response_flash_read r;

	for (i = 0; i < readcnt; i += EC_LPC_FLASH_SIZE_MAX) {
		p.offset = blockaddr + i;
		p.size = min(readcnt - i, EC_LPC_FLASH_SIZE_MAX);
		rc = ec_command(EC_LPC_COMMAND_FLASH_READ,
		                &p, sizeof(p), &r, sizeof(r));
		if (rc) {
			msg_perr("GEC: Flash read error at offset 0x%x\n",
			         blockaddr + i);
			return rc;
		}

#ifdef SUPPORT_CHECKSUM
		if (verify_checksum(r.data, blockaddr + i,
			            min(readcnt - i, EC_LPC_FLASH_SIZE_MAX))) {
			msg_pdbg("GEC: re-read...\n");
			i -= EC_LPC_FLASH_SIZE_MAX;
			continue;
		}
#endif
		memcpy(readarr + i, r.data, p.size);
	}

	return rc;
}


static int gec_block_erase(struct flashchip *flash,
                           unsigned int blockaddr,
                           unsigned int len) {
	struct lpc_params_flash_erase erase;
	int rc;
#ifdef SUPPORT_CHECKSUM
	uint8_t *blank;
#endif

#ifdef SUPPORT_CHECKSUM
re_erase:
#endif
	erase.offset = blockaddr;
	erase.size = len;
	rc = ec_command(EC_LPC_COMMAND_FLASH_ERASE, &erase, sizeof(erase),
	                NULL, 0);
	if (rc == EC_LPC_RESULT_ACCESS_DENIED) {
		// this is active image.
		gec_invalidate_copy(blockaddr, len);
		need_2nd_pass = 1;
		return ACCESS_DENIED;
	}
	if (rc) {
		msg_perr("GEC: Flash erase error at address 0x%x, rc=%d\n",
		         blockaddr, rc);
		return rc;
	}

#ifdef SUPPORT_CHECKSUM
	blank = malloc(len);
	memset(blank, 0xff, len);
	if (verify_checksum(blank, blockaddr, len)) {
		msg_pdbg("GEC: Re-erase...\n");
		goto re_erase;
	}
#endif

	return rc;
}


int gec_write(struct flashchip *flash, uint8_t *buf, unsigned int addr,
                    unsigned int nbytes) {
	int i, rc = 0;
	unsigned int written = 0;
	struct lpc_params_flash_write p;

	for (i = 0; i < nbytes; i += written) {
		written = min(nbytes - i, EC_LPC_FLASH_SIZE_MAX);
		p.offset = addr + i;
		p.size = written;
		memcpy(p.data, &buf[i], written);
		rc = ec_command(EC_LPC_COMMAND_FLASH_WRITE, &p, sizeof(p),
		                NULL, 0);
		if (rc == EC_LPC_RESULT_ACCESS_DENIED) {
			// this is active image.
			gec_invalidate_copy(addr, nbytes);
			need_2nd_pass = 1;
			return ACCESS_DENIED;
		}

#ifdef SUPPORT_CHECKSUM
		if (verify_checksum(&buf[i], addr + i, written)) {
			msg_pdbg("GEC: re-write...\n");
			i -= written;
			continue;
		}
#endif

		if (rc) break;
	}

	return rc;
}


static int gec_list_ranges(const struct flashchip *flash) {
	msg_pinfo("You can specify any range:\n");
	msg_pinfo("  from: 0x%06x, to: 0x%06x\n", 0, flash->total_size * 1024);
	msg_pinfo("  unit: 0x%06x (%dKB)\n", 2048, 2048);
	return 0;
}


static int gec_set_range(const struct flashchip *flash,
                         unsigned int start, unsigned int len) {
	struct lpc_params_flash_wp_range p;
	int rc;

	p.offset = start;
	p.size = len;
	rc = ec_command(EC_LPC_COMMAND_FLASH_WP_SET_RANGE, &p, sizeof(p),
	                NULL, 0);
	if (rc) {
		msg_perr("GEC: wp_set_range error: rc=%d\n", rc);
		return rc;
	}

	return 0;
}


static int gec_enable_writeprotect(const struct flashchip *flash) {
	struct lpc_params_flash_wp_enable p;
	int rc;

	p.enable_wp = 1;
	rc = ec_command(EC_LPC_COMMAND_FLASH_WP_ENABLE, &p, sizeof(p),
	                NULL, 0);
	if (rc) {
		msg_perr("GEC: wp_enable_wp error: rc=%d\n", rc);
	}

	return rc;
}


static int gec_disable_writeprotect(const struct flashchip *flash) {
	struct lpc_params_flash_wp_enable p;
	int rc;

	p.enable_wp = 0;
	rc = ec_command(EC_LPC_COMMAND_FLASH_WP_ENABLE, &p, sizeof(p),
	                NULL, 0);
	if (rc) {
		msg_perr("GEC: wp_disable_wp error: rc=%d\n", rc);
	} else {
		msg_pinfo("Disabled WP. Reboot EC and de-assert #WP.\n");
	}

	return rc;
}


static int gec_wp_status(const struct flashchip *flash) {
	int rc;
	struct lpc_response_flash_wp_range range;
	struct lpc_response_flash_wp_enable en;
	uint8_t value;

	rc = ec_command(EC_LPC_COMMAND_FLASH_WP_GET_RANGE, NULL, 0,
	                &range, sizeof(range));
	if (rc) {
		msg_perr("GEC: wp_get_wp_range error: rc=%d\n", rc);
		return rc;
	}
	rc = ec_command(EC_LPC_COMMAND_FLASH_WP_GET_STATE, NULL, 0,
	                &en, sizeof(en));
	if (rc) {
		msg_perr("GEC: wp_get_wp_state error: rc=%d\n", rc);
		return rc;
	}

	/* TODO: Fix scripts which rely on SPI-specific terminology. */
	value = (en.enable_wp << 7);
	msg_pinfo("WP: status: 0x%02x\n", value);
	msg_pinfo("WP: status.srp0: %x\n", en.enable_wp);
	msg_pinfo("WP: write protect is %s.\n",
	          en.enable_wp ? "enabled" : "disabled");
	msg_pinfo("WP: write protect range: start=0x%08x, len=0x%08x\n",
	          range.offset, range.size);

	return 0;
}


static int gec_probe_size(struct flashchip *flash) {
	int rc;
	struct lpc_response_flash_info info;
	struct block_eraser *eraser;
	static struct wp wp = {
		.list_ranges    = gec_list_ranges,
		.set_range      = gec_set_range,
		.enable         = gec_enable_writeprotect,
		.disable        = gec_disable_writeprotect,
		.wp_status      = gec_wp_status,
	};

	rc = ec_command(EC_LPC_COMMAND_FLASH_INFO, NULL, 0,
	                &info, sizeof(info));
	if (rc) return 0;

	flash->total_size = info.flash_size / 1024;
	flash->page_size = min(info.write_block_size,
	                       info.erase_block_size);
	flash->tested = TEST_OK_PREW;
	eraser = &flash->block_erasers[0];
	eraser->eraseblocks[0].size = info.erase_block_size;
	eraser->eraseblocks[0].count = info.flash_size /
	                               eraser->eraseblocks[0].size;
	flash->wp = &wp;

	return 1;
};


static const struct opaque_programmer opaque_programmer_gec = {
	.max_data_read = EC_LPC_FLASH_SIZE_MAX,
	.max_data_write = EC_LPC_FLASH_SIZE_MAX,
	.probe = gec_probe_size,
	.read = gec_read,
	.write = gec_write,
	.erase = gec_block_erase,
};


/* Sends HELLO command to ACPI port and expects a value from Google EC.
 *
 * TODO: This is an intrusive command for non-Google ECs. Needs a more proper
 *       and more friendly way to detect.
 */
static int detect_ec(void) {
	struct lpc_params_hello request;
	struct lpc_response_hello response;
	int rc = 0;
	int old_timeout = ec_timeout_usec;

	if (target_bus != BUS_LPC) {
		msg_pdbg("%s():%d target_bus is not LPC.\n", __func__, __LINE__);
		return 1;
	}

	/* reduce timeout period temporarily in case EC is not present */
	ec_timeout_usec = 25000;

	/* Say hello to EC. */
	request.in_data = 0xf0e0d0c0;  /* Expect EC will add on 0x01020304. */
	rc = ec_command(EC_LPC_COMMAND_HELLO, &request, sizeof(request),
	                &response, sizeof(response));

	ec_timeout_usec = old_timeout;

	if (rc || response.out_data != 0xf1e2d3c4) {
		msg_pdbg("response.out_data is not 0xf1e2d3c4.\n"
		         "rc=%d, request=0x%x response=0x%x\n",
		         rc, request.in_data, response.out_data);
#ifdef SUPPORT_CHECKSUM
		/* In this mode, we can tolerate some bit errors. */
		{
			int diff = response.out_data ^ 0xf1e2d3c4;
			if (!(diff = (diff - 1) & diff)) return 0;// 1-bit error
			if (!(diff = (diff - 1) & diff)) return 0;// 2-bit error
			if (!(diff = (diff - 1) & diff)) return 0;// 3-bit error
			if (!(diff = (diff - 1) & diff)) return 0;// 4-bit error
		}
#endif
		return 1;
	}

	detected = 1;
	return 0;
}

/* Called by internal_init() */
int gec_probe_programmer(const char *name) {
	msg_pdbg("%s():%d ...\n", __func__, __LINE__);

	if (detect_ec()) return 1;

	register_opaque_programmer(&opaque_programmer_gec);
	if (buses_supported & BUS_SPI) {
		msg_pdbg("%s():%d remove BUS_SPI from buses_supported.\n",
		         __func__, __LINE__);
		buses_supported &= ~BUS_SPI;
	}
	buses_supported |= BUS_LPC;

	return 0;
}
