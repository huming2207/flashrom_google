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
#include "gec_ec_commands.h"
#include "programmer.h"
#include "spi.h"
#include "writeprotect.h"

/* 1 if we want the flashrom to call erase_and_write_flash() again. */
static int need_2nd_pass = 0;

/* 1 if we want the flashrom to try jumping to new firmware after update. */
static int try_latest_firmware = 0;

/* The range of each firmware copy from the image file to update.
 * But re-define the .flags as the valid flag to indicate the firmware is
 * new or not (if flags = 1).
 */
static struct fmap_area fwcopy[4];  // [0] is not used.

/* The names of enum lpc_current_image to match in FMAP area names. */
static const char *sections[3] = {
	"UNKNOWN SECTION",  // EC_IMAGE_UNKNOWN -- never matches
	"EC_RO",
	"EC_RW",
};


/* Given the range not able to update, mark the corresponding
 * firmware as old.
 */
static void gec_invalidate_copy(unsigned int addr, unsigned int len)
{
	int i;

	for (i = EC_IMAGE_RO; i < ARRAY_SIZE(fwcopy); i++) {
		struct fmap_area *fw = &fwcopy[i];
		if ((addr >= fw->offset && (addr < fw->offset + fw->size)) ||
		    (fw->offset >= addr && (fw->offset < addr + len))) {
			msg_pdbg("Mark firmware [%s] as old.\n",
			         sections[i]);
			fw->flags = 0;  // mark as old
		}
	}
}


/* Asks EC to jump to a firmware copy. If target is EC_IMAGE_UNKNOWN,
 * then this functions picks a NEW firmware copy and jumps to it. Note that
 * RO is preferred, then A, finally B.
 *
 * Returns 0 for success.
 */
static int gec_jump_copy(enum ec_current_image target) {
	struct ec_params_reboot_ec p;
	struct gec_priv *priv = (struct gec_priv *)opaque_programmer->data;
	int rc;

	memset(&p, 0, sizeof(p));
	p.cmd = target != EC_IMAGE_UNKNOWN ? target :
	        fwcopy[EC_IMAGE_RO].flags ? EC_IMAGE_RO :
	        fwcopy[EC_IMAGE_RW].flags ? EC_IMAGE_RW :
	        EC_IMAGE_UNKNOWN;
	msg_pdbg("GEC is jumping to [%s]\n", sections[p.cmd]);
	if (p.cmd == EC_IMAGE_UNKNOWN) return 1;

	rc = priv->ec_command(EC_CMD_REBOOT_EC, 0,
			      &p, sizeof(p), NULL, 0);
	if (rc < 0) {
		msg_perr("GEC cannot jump to [%s]:%d\n",
			 sections[p.cmd], rc);
	} else {
		msg_pdbg("GEC has jumped to [%s]\n", sections[p.cmd]);
		rc = EC_RES_SUCCESS;
	}

	/* Sleep 1 sec to wait the EC re-init. */
	usleep(1000000);

	return rc;
}


/* Given an image, this function parses FMAP and recognize the firmware
 * ranges.
 */
int gec_prepare(uint8_t *image, int size) {
	struct gec_priv *priv = (struct gec_priv *)opaque_programmer->data;
	struct fmap *fmap;
	int i, j;

	if (!(priv && priv->detected)) return 0;

	// Parse the fmap in the image file and cache the firmware ranges.
	fmap = fmap_find_in_memory(image, size);
	if (!fmap) return 0;

	// Lookup RO/A/B sections in FMAP.
	for (i = 0; i < fmap->nareas; i++) {
		struct fmap_area *fa = &fmap->areas[i];
		for (j = EC_IMAGE_RO; j < ARRAY_SIZE(sections); j++) {
			if (!strcmp(sections[j], (const char *)fa->name)) {
				msg_pdbg("Found '%s' in image.\n", fa->name);
				memcpy(&fwcopy[j], fa, sizeof(*fa));
				fwcopy[j].flags = 1;  // mark as new
			}
		}
	}

	/* Warning: before update, we jump the EC to RO copy. If you want to
	 *          change this behavior, please also check the gec_finish().
	 */
	return gec_jump_copy(EC_IMAGE_RO);
}


/* Returns >0 if we need 2nd pass of erase_and_write_flash().
 *         <0 if we cannot jump to any firmware copy.
 *        ==0 if no more pass is needed.
 *
 * This function also jumps to new-updated firmware copy before return >0.
 */
int gec_need_2nd_pass(void) {
	struct gec_priv *priv = (struct gec_priv *)opaque_programmer->data;

	if (!(priv && priv->detected)) return 0;

	if (need_2nd_pass) {
		if (gec_jump_copy(EC_IMAGE_UNKNOWN)) {
			return -1;
		}
	}

	return need_2nd_pass;
}


/* Returns 0 for success.
 *
 * Try latest firmware: B > A > RO
 *
 * This function assumes the EC jumps to RO at gec_prepare() so that
 * the fwcopy[RO].flags is old (0) and A/B are new. Please also refine
 * this code logic if you change the gec_prepare() behavior.
 */
int gec_finish(void) {
	struct gec_priv *priv = (struct gec_priv *)opaque_programmer->data;

	if (!(priv && priv->detected)) return 0;

	if (try_latest_firmware) {
		if (fwcopy[EC_IMAGE_RW].flags &&
		    gec_jump_copy(EC_IMAGE_RW) == 0) return 0;
		return gec_jump_copy(EC_IMAGE_RO);
	}

	return 0;
}


int gec_read(struct flashchip *flash, uint8_t *readarr,
             unsigned int blockaddr, unsigned int readcnt) {
	int rc = 0;
	struct ec_params_flash_read p;
	struct gec_priv *priv = (struct gec_priv *)opaque_programmer->data;
	int maxlen = opaque_programmer->max_data_read;
	uint8_t buf[maxlen];
	int offset = 0, count;

	while (offset < readcnt) {
		count = min(maxlen, readcnt - offset);
		p.offset = blockaddr + offset;
		p.size = count;
		rc = priv->ec_command(EC_CMD_FLASH_READ, 0,
				      &p, sizeof(p), buf, count);
		if (rc < 0) {
			msg_perr("GEC: Flash read error at offset 0x%x\n",
			         blockaddr + offset);
			return rc;
		} else {
			rc = EC_RES_SUCCESS;
		}

		memcpy(readarr + offset, buf, count);
		offset += count;
	}

	return rc;
}


int gec_block_erase(struct flashchip *flash,
                           unsigned int blockaddr,
                           unsigned int len) {
	struct ec_params_flash_erase erase;
	struct gec_priv *priv = (struct gec_priv *)opaque_programmer->data;
	int rc;

	erase.offset = blockaddr;
	erase.size = len;
	rc = priv->ec_command(EC_CMD_FLASH_ERASE, 0,
			      &erase, sizeof(erase), NULL, 0);
	if (rc == -EC_RES_ACCESS_DENIED) {
		// this is active image.
		gec_invalidate_copy(blockaddr, len);
		need_2nd_pass = 1;
		return ACCESS_DENIED;
	}
	if (rc < 0) {
		msg_perr("GEC: Flash erase error at address 0x%x, rc=%d\n",
		         blockaddr, rc);
		return rc;
	} else {
		rc = EC_RES_SUCCESS;
	}

	try_latest_firmware = 1;
	return rc;
}


int gec_write(struct flashchip *flash, uint8_t *buf, unsigned int addr,
                    unsigned int nbytes) {
	int i, rc = 0;
	unsigned int written = 0;
	struct ec_params_flash_write p;
	struct gec_priv *priv = (struct gec_priv *)opaque_programmer->data;
	int maxlen = opaque_programmer->max_data_write;

	for (i = 0; i < nbytes; i += written) {
		written = min(nbytes - i, maxlen);
		p.offset = addr + i;
		p.size = written;
		memcpy(p.data, &buf[i], written);
		rc = priv->ec_command(EC_CMD_FLASH_WRITE, 0,
				      &p, sizeof(p), NULL, 0);
		if (rc == -EC_RES_ACCESS_DENIED) {
			// this is active image.
			gec_invalidate_copy(addr, nbytes);
			need_2nd_pass = 1;
			return ACCESS_DENIED;
		}

		if (rc < 0) break;
		rc = EC_RES_SUCCESS;
	}

	try_latest_firmware = 1;
	return rc;
}


static int gec_list_ranges(const struct flashchip *flash) {
	msg_pinfo("You can specify any range:\n");
	msg_pinfo("  from: 0x%06x, to: 0x%06x\n", 0, flash->total_size * 1024);
	msg_pinfo("  unit: 0x%06x (%dKB)\n", 2048, 2);
	return 0;
}


static int gec_set_range(const struct flashchip *flash,
                         unsigned int start, unsigned int len) {

	/* TODO: update to latest ec_commands.h and reimplement. */
	msg_perr("GEC: set_range unimplemented\n");
	return -1;
}


static int gec_enable_writeprotect(const struct flashchip *flash) {
	/* TODO: update to latest ec_commands.h and reimplement. */
	msg_perr("GEC: enable_writeprotect unimplemented\n");
	return -1;
}


static int gec_disable_writeprotect(const struct flashchip *flash) {
	/* TODO: update to latest ec_commands.h and reimplement. */
	msg_perr("GEC: disable_writeprotect unimplemented\n");
	return -1;
}


static int gec_wp_status(const struct flashchip *flash) {
	/*
	 * TODO: update to latest ec_commands.h and reimplement.  For now,
	 * just claim chip is unprotected.
	 */

	/* TODO: Fix scripts which rely on SPI-specific terminology. */
	msg_pinfo("WP: status: 0x%02x\n", 0);
	msg_pinfo("WP: status.srp0: %x\n", 0);
	msg_pinfo("WP: write protect is %s.\n", "disabled");
	msg_pinfo("WP: write protect range: start=0x%08x, len=0x%08x\n", 0, 0);

	return 0;
}


int gec_probe_size(struct flashchip *flash) {
	int rc;
	struct ec_response_flash_info info;
	struct gec_priv *priv = (struct gec_priv *)opaque_programmer->data;
	struct block_eraser *eraser;
	static struct wp wp = {
		.list_ranges    = gec_list_ranges,
		.set_range      = gec_set_range,
		.enable         = gec_enable_writeprotect,
		.disable        = gec_disable_writeprotect,
		.wp_status      = gec_wp_status,
	};

	rc = priv->ec_command(EC_CMD_FLASH_INFO, 0,
			      NULL, 0, &info, sizeof(info));
	if (rc < 0) {
		msg_perr("%s(): FLASH_INFO returns %d.\n", __func__, rc);
		return 0;
	}

	flash->total_size = info.flash_size / 1024;
	flash->page_size = 64;
	flash->tested = TEST_OK_PREW;
	eraser = &flash->block_erasers[0];
	eraser->eraseblocks[0].size = info.erase_block_size;
	eraser->eraseblocks[0].count = info.flash_size /
	                               eraser->eraseblocks[0].size;
	flash->wp = &wp;

	return 1;
};
