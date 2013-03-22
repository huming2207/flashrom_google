/* Copyright 2010, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *    * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *    * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Alternatively, this software may be distributed under the terms of the
 * GNU General Public License ("GPL") version 2 as published by the Free
 * Software Foundation.
 *
 * This is ported from the flashmap utility: http://flashmap.googlecode.com
 */

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "flash.h"
#include "fmap.h"


/* Ceil a number to the minimum power of 2 value. For example,
 *   ceiling(2) = 2
 *   ceiling(5) = 8
 *   ceiling(254K) = 256K
 *
 * Return -1 if the input value is invalid..
 */
long int ceiling(long int v) {
	int shiftwidth;

	if (v <= 0) return -1;

	/* it is power of 2. */
	if (!(v & (v - 1))) return v;

	/* pollute all bits below MSB to 1. */
	for (shiftwidth = (sizeof(v) * CHAR_BIT) / 2;
	     shiftwidth > 0;
	     shiftwidth /= 2) {
		v = v | (v >> shiftwidth);
	}

	return v + 1;
}


/* invoke crossystem and parse the returned string.
 * returns 0xFFFFFFFF if failed to get the value. */
#define CROSSYSTEM_FAIL (0xFFFFFFFF)
static uint32_t get_crossystem_fmap_base(struct flashchip *flash) {
	char cmd[] = "crossystem fmap_base";
	FILE *fp;
	int n;
	char buf[16];
	unsigned long fmap_base;
	unsigned long from_top;

	if (!(fp = popen(cmd, "r"))) {
		return CROSSYSTEM_FAIL;
	}
	n = fread(buf, 1, sizeof(buf) - 1, fp);
	fclose(fp);
	if (n < 0) {
		return CROSSYSTEM_FAIL;
	}
	buf[n] = '\0';
	if (strlen(buf) == 0) {
		return CROSSYSTEM_FAIL;
	}

	/* fmap_base is the absolute address in CPU address space.
	 * The top of BIOS address aligns to the last byte of address space,
	 * 0xFFFFFFFF. So we have to shift it to the address related to
	 * start of BIOS.
	 *
	 *  CPU address                  flash address
	 *      space                    p     space
	 *  0xFFFFFFFF   +-------+  ---  +-------+  0x400000
	 *               |       |   ^   |       | ^
	 *               |  4MB  |   |   |       | | from_top
	 *               |       |   v   |       | v
	 *  fmap_base--> | -fmap | ------|--fmap-|-- the offset we need.
	 *       ^       |       |       |       |
	 *       |       +-------+-------+-------+  0x000000
	 *       |       |       |
	 *       |       |       |
	 *       |       |       |
	 *       |       |       |
	 *  0x00000000   +-------+
	 *
	 */
	fmap_base = (unsigned long)strtoll(buf, (char **) NULL, 0);
	from_top = 0xFFFFFFFF - fmap_base + 1;
	if (from_top > flash->total_size * 1024) {
		/* Invalid fmap_base value for this chip, like EC's flash. */
		return CROSSYSTEM_FAIL;
	}
	return flash->total_size * 1024 - from_top;
}

int fmap_find(struct flashchip *flash, uint8_t **buf)
{
	long int offset = 0, ceiling_size;
	uint64_t sig, tmp64;
	struct fmap fmap;
	int fmap_size, fmap_found = 0, stride;

	memcpy(&sig, FMAP_SIGNATURE, strlen(FMAP_SIGNATURE));

	offset = get_crossystem_fmap_base(flash);
	if (CROSSYSTEM_FAIL != offset) {
		if (offset < 0 || offset >= flash->total_size * 1024 ||
		    read_flash(flash, (uint8_t *)&tmp64,
			    offset, sizeof(tmp64))) {
			msg_gdbg("[L%d] failed to read flash at "
			         "offset 0x%lx\n", __LINE__, offset);
			return -1;
		}
		if (!memcmp(&tmp64, &sig, sizeof(sig))) {
			fmap_found = 1;
		}
	}

	/*
	 * For efficient operation, we start with the largest stride possible
	 * and then decrease the stride on each iteration. We will check for a
	 * remainder when modding the offset with the previous stride. This
	 * makes it so that each offset is only checked once.
	 *
	 * At some point, programmer transaction overhead becomes greater than
	 * simply copying everything into RAM and checking one byte at a time.
	 * At some arbitrary point, we'll stop being clever and use brute
	 * force instead by copying the while ROM into RAM and searching one
	 * byte at a time.
	 *
	 * In practice, the flash map is usually stored in a write-protected
	 * section of flash which is often at the top of ROM where the boot
	 * vector on x86 resides. Because of this, we will search from top
	 * to bottom.
	 */
	ceiling_size = ceiling(flash->total_size * 1024);
	for (stride = ceiling_size / 2;
	     stride >= 16;
	     stride /= 2) {
		if (fmap_found)
			break;

		for (offset = ceiling_size - stride;
		     offset > 0;
		     offset -= stride) {
			int tmp;

			if (offset % (stride * 2) == 0 ||
			    offset >= flash->total_size * 1024)
				continue;
			if (read_flash(flash, (uint8_t *)&tmp64,
						offset, sizeof(tmp64))) {
				msg_gdbg("[L%d] failed to read flash at offset"
				         " 0x%lx\n", __LINE__, offset);
				return -1;
			}

			if (!memcmp(&tmp64, &sig, sizeof(sig))) {
				fmap_found = 1;
				break;
			}
		}
	}

	/* brute force */
	/* FIXME: This results in the entire ROM being read twice -- once here
	 * and again in doit(). The performance penalty needs to be dealt
	 * with before going upstream.
	 */
	if (!fmap_found) {
		int tmp;
		uint8_t *image = malloc(flash->total_size * 1024);

		msg_gdbg("using brute force method to find fmap\n");
		if (flash->read(flash, image, 0, flash->total_size * 1024)) {
			msg_gdbg("[L%d] failed to read flash\n", __LINE__);
			return -1;
		}
		for (offset = flash->total_size * 1024 - sizeof(sig);
		     offset > 0;
		     offset--) {
			if (!memcmp(&image[offset], &sig, sizeof(sig))) {
				fmap_found = 1;
				break;
			}
		}
		free(image);
	}

	if (!fmap_found)
		return 0;

	if (offset < 0) return -1;

	if (flash->read(flash, (uint8_t *)&fmap, offset, sizeof(fmap))) {
		msg_gdbg("[L%d] failed to read flash at offset 0x%lx\n",
		         __LINE__, offset);
		return -1;
	}

	fmap_size = sizeof(fmap) + (fmap.nareas * sizeof(struct fmap_area));
	*buf = malloc(fmap_size);

	if (flash->read(flash, *buf, offset, fmap_size)) {
		msg_gdbg("[L%d] failed to read %d bytes at offset 0x%lx\n",
		         __LINE__, fmap_size, offset);
		return -1;
	}
	return fmap_size;
}


/* Like fmap_find, but give a memory location to search FMAP. */
struct fmap *fmap_find_in_memory(uint8_t *image, int size)
{
	long int offset = 0;
	uint64_t sig;

	memcpy(&sig, FMAP_SIGNATURE, strlen(FMAP_SIGNATURE));

	for (offset = 0; offset < size; offset++) {
		if (!memcmp(&image[offset], &sig, sizeof(sig))) {
			return (struct fmap *)&image[offset];
		}
	}
	return NULL;
}
