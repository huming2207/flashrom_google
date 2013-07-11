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
#include "gec.h"
#include "gec_lock.h"
#include "gec_ec_commands.h"
#include "programmer.h"
#include "spi.h"
#include "writeprotect.h"

/* FIXME: used for wp hacks */
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
struct wp_data {
	int enable;
	unsigned int start;
	unsigned int len;
};
#define WP_STATE_HACK_FILENAME "/mnt/stateful_partition/flashrom_wp_state"

/* If software sync is enabled, then we don't try the latest firmware copy
 * after updating.
 */
#define SOFTWARE_SYNC_ENABLED

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

/* EC_FLASH_REGION_WP_RO is the highest numbered region so it also indicates
 * the number of regions */
static struct ec_response_flash_region_info regions[EC_FLASH_REGION_WP_RO + 1];

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


static int gec_get_current_image(struct gec_priv *priv)
{
	struct ec_response_get_version resp;
	int rc;

	rc = priv->ec_command(EC_CMD_GET_VERSION, 0, NULL, 0, &resp,
			      sizeof(resp));
	if (rc < 0) {
		msg_perr("GEC cannot get the running copy: rc=%d\n", rc);
		return rc;
	}
	if (resp.current_image == EC_IMAGE_UNKNOWN) {
		msg_perr("GEC gets unknown running copy\n");
		return -1;
	}

	return resp.current_image;
}


static int gec_get_region_info(struct gec_priv *priv,
			       enum ec_flash_region region,
			       struct ec_response_flash_region_info *info)
{
	struct ec_params_flash_region_info req;
	struct ec_response_flash_region_info resp;
	int rc;

	req.region = region;
	rc = priv->ec_command(EC_CMD_FLASH_REGION_INFO,
			      EC_VER_FLASH_REGION_INFO, &req, sizeof(req),
			      &resp, sizeof(resp));
	if (rc < 0) {
		msg_perr("Cannot get the WP_RO region info: %d\n", rc);
		return rc;
	}

	info->offset = resp.offset;
	info->size = resp.size;
	return 0;
}


/* Asks EC to jump to a firmware copy. If target is EC_IMAGE_UNKNOWN,
 * then this functions picks a NEW firmware copy and jumps to it. Note that
 * RO is preferred, then A, finally B.
 *
 * Returns 0 for success.
 */
static int gec_jump_copy(enum ec_current_image target) {
	struct ec_response_get_version c;
	struct ec_params_reboot_ec p;
	struct gec_priv *priv = (struct gec_priv *)opaque_programmer->data;
	int rc;

	/* Since the EC may return EC_RES_SUCCESS twice if the EC doesn't
	 * jump to different firmware copy. The second EC_RES_SUCCESS would
	 * set the OBF=1 and the next command cannot be executed.
	 * Thus, we call EC to jump only if the target is different.
	 */
	rc = gec_get_current_image(priv);
	if (rc < 0)
		return 1;
	if (rc == target)
		return 0;

	memset(&p, 0, sizeof(p));

	/* Translate target --> EC reboot command parameter */
	switch (target) {
	case EC_IMAGE_RO:
		p.cmd = EC_REBOOT_JUMP_RO;
		break;
	case EC_IMAGE_RW:
		p.cmd = EC_REBOOT_JUMP_RW;
		break;
	default:
		/*
		 * If target is unspecified, set EC reboot command to use
		 * a new image. Also set "target" so that it may be used
		 * to update the priv->current_image if jump is successful.
		 */
		if (fwcopy[EC_IMAGE_RO].flags) {
			p.cmd = EC_REBOOT_JUMP_RO;
			target = EC_IMAGE_RO;
		} else if (fwcopy[EC_IMAGE_RW].flags) {
			p.cmd = EC_REBOOT_JUMP_RW;
			target = EC_IMAGE_RW;
		} else {
			p.cmd = EC_IMAGE_UNKNOWN;
		}
		break;
	}

	msg_pdbg("GEC is jumping to [%s]\n", sections[p.cmd]);
	if (p.cmd == EC_IMAGE_UNKNOWN) return 1;

	if (c.current_image == p.cmd) {
		msg_pdbg("GEC is already in [%s]\n", sections[p.cmd]);
		priv->current_image = target;
		return 0;
	}

	rc = priv->ec_command(EC_CMD_REBOOT_EC, 0,
			      &p, sizeof(p), NULL, 0);
	if (rc < 0) {
		msg_perr("GEC cannot jump to [%s]:%d\n",
			 sections[p.cmd], rc);
	} else {
		msg_pdbg("GEC has jumped to [%s]\n", sections[p.cmd]);
		rc = EC_RES_SUCCESS;
		priv->current_image = target;
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


/*
 * returns 0 to indicate area does not overlap current EC image
 * returns 1 to indicate area overlaps current EC image or error
 */
static int in_current_image(struct gec_priv *priv,
		unsigned int addr, unsigned int len)
{
	int ret;
	enum ec_current_image image;
	uint32_t region_offset;
	uint32_t region_size;

	image = priv->current_image;
	region_offset = priv->region[image].offset;
	region_size = priv->region[image].size;

	if ((addr + len - 1 < region_offset) ||
		(addr > region_offset + region_size - 1)) {
		return 0;
	}
	return 1;
}


int gec_block_erase(struct flashchip *flash,
                           unsigned int blockaddr,
                           unsigned int len) {
	struct ec_params_flash_erase erase;
	struct gec_priv *priv = (struct gec_priv *)opaque_programmer->data;
	int rc;

	if (in_current_image(priv, blockaddr, len)) {
		gec_invalidate_copy(blockaddr, len);
		need_2nd_pass = 1;
		return ACCESS_DENIED;
	}

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

#ifndef SOFTWARE_SYNC_ENABLED
	try_latest_firmware = 1;
#endif
	return rc;
}


int gec_write(struct flashchip *flash, uint8_t *buf, unsigned int addr,
                    unsigned int nbytes) {
	int i, rc = 0;
	unsigned int written = 0;
	struct ec_params_flash_write p;
	struct gec_priv *priv = (struct gec_priv *)opaque_programmer->data;
	int maxlen = opaque_programmer->max_data_write;
	uint8_t *packet;

	packet = malloc(sizeof(p) + maxlen);
	if (!packet)
		return -1;

	for (i = 0; i < nbytes; i += written) {
		written = min(nbytes - i, maxlen);
		p.offset = addr + i;
		p.size = written;

		if (in_current_image(priv, p.offset, p.size)) {
			gec_invalidate_copy(addr, nbytes);
			need_2nd_pass = 1;
			return ACCESS_DENIED;
		}

		memcpy(packet, &p, sizeof(p));
		memcpy(packet + sizeof(p), &buf[i], written);
		rc = priv->ec_command(EC_CMD_FLASH_WRITE, 0,
				      packet, sizeof(p) + p.size, NULL, 0);

		if (rc == -EC_RES_ACCESS_DENIED) {
			// this is active image.
			gec_invalidate_copy(addr, nbytes);
			need_2nd_pass = 1;
			return ACCESS_DENIED;
		}

		if (rc < 0) break;
		rc = EC_RES_SUCCESS;
	}

#ifndef SOFTWARE_SYNC_ENABLED
	try_latest_firmware = 1;
#endif
	free(packet);
	return rc;
}


static int gec_list_ranges(const struct flashchip *flash) {
	struct gec_priv *priv = (struct gec_priv *)opaque_programmer->data;
	struct ec_response_flash_region_info info;
	int rc;

	rc = gec_get_region_info(priv, EC_FLASH_REGION_WP_RO, &info);
	if (rc < 0) {
		msg_perr("Cannot get the WP_RO region info: %d\n", rc);
		return 1;
	}

	msg_pinfo("Supported write protect range:\n");
	msg_pinfo("  disable: start=0x%06x len=0x%06x\n", 0, 0);
	msg_pinfo("  enable:  start=0x%06x len=0x%06x\n", info.offset,
		  info.size);

	return 0;
}


/*
 * Helper function for flash protection.
 *
 *  On EC API v1, the EC write protection has been simplified to one-bit:
 *  EC_FLASH_PROTECT_RO_AT_BOOT, which means the state is either enabled
 *  or disabled. However, this is different from the SPI-style write protect
 *  behavior. Thus, we re-define the flashrom command (SPI-style) so that
 *  either SRP or range is non-zero, the EC_FLASH_PROTECT_RO_AT_BOOT is set.
 *
 *    SRP     Range      | PROTECT_RO_AT_BOOT
 *     0        0        |         0
 *     0     non-zero    |         1
 *     1        0        |         1
 *     1     non-zero    |         1
 *
 *
 *  Besides, to make the protection take effect as soon as possible, we
 *  try to set EC_FLASH_PROTECT_RO_NOW at the same time. However, not
 *  every EC supports RO_NOW, thus we then try to protect the entire chip.
 */
static int set_wp(int enable) {
	struct gec_priv *priv = (struct gec_priv *)opaque_programmer->data;
	struct ec_params_flash_protect p;
	struct ec_response_flash_protect r;
	const int ro_at_boot_flag = EC_FLASH_PROTECT_RO_AT_BOOT;
	const int ro_now_flag = EC_FLASH_PROTECT_RO_NOW;
	int need_an_ec_cold_reset = 0;
	int rc;

	/* Try to set RO_AT_BOOT and RO_NOW first */
	memset(&p, 0, sizeof(p));
	p.mask = (ro_at_boot_flag | ro_now_flag);
	p.flags = enable ? (ro_at_boot_flag | ro_now_flag) : 0;
	rc = priv->ec_command(EC_CMD_FLASH_PROTECT, EC_VER_FLASH_PROTECT,
			      &p, sizeof(p), &r, sizeof(r));
	if (rc < 0) {
		msg_perr("FAILED: Cannot set the RO_AT_BOOT and RO_NOW: %d\n",
			 rc);
		return 1;
	}

	/* Read back */
	memset(&p, 0, sizeof(p));
	rc = priv->ec_command(EC_CMD_FLASH_PROTECT, EC_VER_FLASH_PROTECT,
			      &p, sizeof(p), &r, sizeof(r));
	if (rc < 0) {
		msg_perr("FAILED: Cannot get RO_AT_BOOT and RO_NOW: %d\n",
			 rc);
		return 1;
	}

	if (!enable) {
		/* The disable case is easier to check. */
		if (r.flags & ro_at_boot_flag) {
			msg_perr("FAILED: RO_AT_BOOT is not clear.\n");
			return 1;
		} else if (r.flags & ro_now_flag) {
			msg_perr("FAILED: RO_NOW is asserted unexpectedly.\n");
			need_an_ec_cold_reset = 1;
			goto exit;
		}

		msg_pdbg("INFO: RO_AT_BOOT is clear.\n");
		return 0;
	}

	/* Check if RO_AT_BOOT is set. If not, fail in anyway. */
	if (r.flags & ro_at_boot_flag) {
		msg_pdbg("INFO: RO_AT_BOOT has been set.\n");
	} else {
		msg_perr("FAILED: RO_AT_BOOT is not set.\n");
		return 1;
	}

	/* Then, we check if the protection has been activated. */
	if (r.flags & ro_now_flag) {
		/* Good, RO_NOW is set. */
		msg_pdbg("INFO: RO_NOW is set. WP is active now.\n");
	} else if (r.writable_flags & EC_FLASH_PROTECT_ALL_NOW) {
		struct ec_params_reboot_ec reboot;

		msg_pdbg("WARN: RO_NOW is not set. Trying ALL_NOW.\n");

		memset(&p, 0, sizeof(p));
		p.mask = EC_FLASH_PROTECT_ALL_NOW;
		p.flags = EC_FLASH_PROTECT_ALL_NOW;
		rc = priv->ec_command(EC_CMD_FLASH_PROTECT,
				      EC_VER_FLASH_PROTECT,
				      &p, sizeof(p), &r, sizeof(r));
		if (rc < 0) {
			msg_perr("FAILED: Cannot set ALL_NOW: %d\n", rc);
			return 1;
		}

		/* Read back */
		memset(&p, 0, sizeof(p));
		rc = priv->ec_command(EC_CMD_FLASH_PROTECT,
				      EC_VER_FLASH_PROTECT,
				      &p, sizeof(p), &r, sizeof(r));
		if (rc < 0) {
			msg_perr("FAILED:Cannot get ALL_NOW: %d\n", rc);
			return 1;
		}

		if (!(r.flags & EC_FLASH_PROTECT_ALL_NOW)) {
			msg_perr("FAILED: ALL_NOW is not set.\n");
			need_an_ec_cold_reset = 1;
			goto exit;
		}

		msg_pdbg("INFO: ALL_NOW has been set. WP is active now.\n");

		/*
		 * Our goal is to protect the RO ASAP. The entire protection
		 * is just a workaround for platform not supporting RO_NOW.
		 * It has side-effect that the RW is also protected and leads
		 * the RW update failed. So, we arrange an EC code reset to
		 * unlock RW ASAP.
		 */
		memset(&reboot, 0, sizeof(reboot));
		reboot.cmd = EC_REBOOT_COLD;
		reboot.flags = EC_REBOOT_FLAG_ON_AP_SHUTDOWN;
		rc = priv->ec_command(EC_CMD_REBOOT_EC, 0,
				      &reboot, sizeof(reboot), NULL, 0);
		if (rc < 0) {
			msg_perr("WARN: Cannot arrange a cold reset at next "
				 "shutdown to unlock entire protect.\n");
			msg_perr("      But you can do it manually.\n");
		} else {
			msg_pdbg("INFO: A cold reset is arranged at next "
				 "shutdown.\n");
		}

	} else {
		msg_perr("FAILED: RO_NOW is not set.\n");
		msg_perr("FAILED: The PROTECT_RO_AT_BOOT is set, but cannot "
			 "make write protection active now.\n");
		need_an_ec_cold_reset = 1;
	}

exit:
	if (need_an_ec_cold_reset) {
		msg_perr("FAILED: You may need a reboot to take effect of "
			 "PROTECT_RO_AT_BOOT.\n");
		return 1;
	}

	return 0;
}

static int gec_set_range(const struct flashchip *flash,
                         unsigned int start, unsigned int len) {
	struct gec_priv *priv = (struct gec_priv *)opaque_programmer->data;
	struct ec_response_flash_region_info info;
	int rc;

	/* Check if the given range is supported */
	rc = gec_get_region_info(priv, EC_FLASH_REGION_WP_RO, &info);
	if (rc < 0) {
		msg_perr("FAILED: Cannot get the WP_RO region info: %d\n", rc);
		return 1;
	}
	if ((!start && !len) ||  /* list supported ranges */
	    ((start == info.offset) && (len == info.size))) {
		/* pass */
	} else {
		msg_perr("FAILED: Unsupported write protection range "
			 "(0x%06x,0x%06x)\n\n", start, len);
		msg_perr("Currently supported range:\n");
		msg_perr("  disable: (0x%06x,0x%06x)\n", 0, 0);
		msg_perr("  enable:  (0x%06x,0x%06x)\n", info.offset,
			 info.size);
		return 1;
	}

	return set_wp(!!len);
}


static int gec_enable_writeprotect(const struct flashchip *flash,
		enum wp_mode wp_mode) {
	int ret;

	switch (wp_mode) {
	case WP_MODE_HARDWARE:
		ret = set_wp(1);
		break;
	default:
		msg_perr("%s():%d Unsupported write-protection mode\n",
				__func__, __LINE__);
		ret = 1;
		break;
	}

	return ret;
}


static int gec_disable_writeprotect(const struct flashchip *flash) {
	return set_wp(0);
}


static int gec_wp_status(const struct flashchip *flash) {
	struct gec_priv *priv = (struct gec_priv *)opaque_programmer->data;
	struct ec_params_flash_protect p;
	struct ec_response_flash_protect r;
	int start, len;  /* wp range */
	int enabled;
	int rc;

	memset(&p, 0, sizeof(p));
	rc = priv->ec_command(EC_CMD_FLASH_PROTECT, EC_VER_FLASH_PROTECT,
			      &p, sizeof(p), &r, sizeof(r));
	if (rc < 0) {
		msg_perr("FAILED: Cannot get the write protection status: %d\n",
			 rc);
		return 1;
	} else if (rc < sizeof(r)) {
		msg_perr("FAILED: Too little data returned (expected:%zd, "
			 "actual:%d)\n", sizeof(r), rc);
		return 1;
	}

	start = len = 0;
	if (r.flags & EC_FLASH_PROTECT_RO_AT_BOOT) {
		struct ec_response_flash_region_info info;

		msg_pdbg("%s(): EC_FLASH_PROTECT_RO_AT_BOOT is set.\n",
			 __func__);
		rc = gec_get_region_info(priv, EC_FLASH_REGION_WP_RO, &info);
		if (rc < 0) {
			msg_perr("FAILED: Cannot get the WP_RO region info: "
				 "%d\n", rc);
			return 1;
		}
		start = info.offset;
		len = info.size;
	} else {
		msg_pdbg("%s(): EC_FLASH_PROTECT_RO_AT_BOOT is clear.\n",
			 __func__);
	}

	/*
	 * If neither RO_NOW or ALL_NOW is set, it means write protect is
	 * NOT active now.
	 */
	if (!(r.flags & (EC_FLASH_PROTECT_RO_NOW | EC_FLASH_PROTECT_ALL_NOW)))
		start = len = 0;

	/* Remove the SPI-style messages. */
	enabled = r.flags & EC_FLASH_PROTECT_RO_AT_BOOT ? 1 : 0;
	msg_pinfo("WP: status: 0x%02x\n", enabled ? 0x80 : 0x00);
	msg_pinfo("WP: status.srp0: %x\n", enabled);
	msg_pinfo("WP: write protect is %s.\n",
			enabled ? "enabled" : "disabled");
	msg_pinfo("WP: write protect range: start=0x%08x, len=0x%08x\n",
	          start, len);

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
	rc = gec_get_current_image(priv);
	if (rc < 0) {
		msg_perr("%s(): Failed to probe (no current image): %d\n",
			 __func__, rc);
		return 0;
	}
	priv->current_image = rc;
	priv->region = &regions[0];

	flash->total_size = info.flash_size / 1024;
	flash->page_size = 64;
	flash->tested = TEST_OK_PREW;
	eraser = &flash->block_erasers[0];
	eraser->eraseblocks[0].size = info.erase_block_size;
	eraser->eraseblocks[0].count = info.flash_size /
	                               eraser->eraseblocks[0].size;
	flash->wp = &wp;

	/* FIXME: EC_IMAGE_* is ordered differently from EC_FLASH_REGION_*,
	 * so we need to be careful about using these enums as array indices */
	rc = gec_get_region_info(priv, EC_FLASH_REGION_RO,
				 &priv->region[EC_IMAGE_RO]);
	if (rc) {
		msg_perr("%s(): Failed to probe (cannot find RO region): %d\n",
			 __func__, rc);
		return 0;
	}

	rc = gec_get_region_info(priv, EC_FLASH_REGION_RW,
				 &priv->region[EC_IMAGE_RW]);
	if (rc) {
		msg_perr("%s(): Failed to probe (cannot find RW region): %d\n",
			 __func__, rc);
		return 0;
	}

	return 1;
};
