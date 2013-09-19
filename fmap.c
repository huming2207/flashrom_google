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
#include "search.h"

int fmap_find(struct flashchip *flash, struct fmap *fmap, loff_t offset,
	      uint8_t **buf)
{
	int fmap_size;

	if (memcmp(&fmap->signature, FMAP_SIGNATURE, sizeof(fmap->signature)))
		return 0;

	fmap_size = sizeof(*fmap) + fmap->nareas * sizeof(struct fmap_area);
	*buf = malloc(fmap_size);

	if (read_flash(flash, *buf, offset, fmap_size)) {
		msg_gdbg("[L%d] failed to read %d bytes at offset 0x%lx\n",
			 __LINE__, fmap_size, (unsigned long)offset);
		return -1;
	}

	return 1;
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
