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

#include <stdlib.h>
#include <string.h>

#include "flash.h"
#include "fmap.h"

extern int fmap_find(struct flashchip *flash, uint8_t **buf)
{
	unsigned long int offset = 0;
	uint64_t sig, tmp64;
	int i, fmap_found = 0;
	/* Note: keep the strides sorted in largest to smallest order for
	   efficient operation below. Strides should all be powers of 2. */
	unsigned int strides[] = { 64 * 1024, 4 * 1024, 64 };
	struct fmap fmap;
	int fmap_size;

	memcpy(&sig, FMAP_SIGNATURE, strlen(FMAP_SIGNATURE));

	/*
	 * Find FMAP signature within image. The alignment requirements
	 * increased over time due to speed concerns, so we'll use an array to
	 * represent the strides we wish to attempt to use.
	 *
	 * For efficient operation, we start with the largest stride possible
	 * and then decrease the stride on each iteration. We will check for a
	 * remainder when modding the offset with the previous stride. This
	 * makes it so that each offset is only checked once.
	 */
	for (i = 0; i < ARRAY_SIZE(strides); i++) {
		for (offset = flash->total_size * 1024 - strides[i];
		     offset > 0;
		     offset -= strides[i]) {
			if (i > 0) {
				if (offset % strides[i-1] == 0)
					continue;
			}

			if (flash->read(flash, (uint8_t *)&tmp64,
			                offset, sizeof(tmp64))) {
				msg_gdbg("failed to read flash at "
				         "offset 0x%lx\n", offset);
				return -1;
			}

			if (!memcmp(&tmp64, &sig, sizeof(sig))) {
				fmap_found = 1;
				break;
			}
		}
		if (fmap_found)
			break;
	}

	if (!fmap_found)
		return 0;

	if (flash->read(flash, (uint8_t *)&fmap, offset, sizeof(fmap))) {
		msg_gdbg("failed to read flash at offset 0x%lx\n", offset);
		return -1;
	}

	fmap_size = sizeof(fmap) + (fmap.nareas * sizeof(struct fmap_area));
	*buf = malloc(fmap_size);

	if (flash->read(flash, *buf, offset, fmap_size)) {
		msg_gdbg("failed to read %d bytes at offset 0x%lx\n",
		         fmap_size, offset);
		return -1;
	}
	return fmap_size;
}
