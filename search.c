/*
 * Copyright 2013, Google Inc.
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "flash.h"
#include "search.h"


/* Ceil a number to the minimum power of 2 value. For example,
 *   ceiling(2) = 2
 *   ceiling(5) = 8
 *   ceiling(254K) = 256K
 *
 * Return -1 if the input value is invalid..
 */
static long int ceiling(long int v) {
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

int search_find_next(struct search_info *search, off_t *offsetp)
{
	long int flash_size;
	int ret;

	flash_size = search->flash->total_size * 1024;
	switch (search->state) {
	case SEARCH_STATE_START:
		search->ceiling_size = ceiling(flash_size);
		search->state = SEARCH_STATE_USE_HANDLER;
		search->stride = search->ceiling_size / 2;
		search->offset = search->ceiling_size - search->stride;
		/* no break */
	case SEARCH_STATE_USE_HANDLER:
		search->state = SEARCH_STATE_BINARY_SEARCH;
		search->offset = search->ceiling_size - search->stride;
		if (search->handler) {
			ret = search->handler(search, offsetp);
			if (!ret && *offsetp < flash_size - search->min_size &&
					*offsetp >= 0)
				return 0;
		}
		/* no break */
	case SEARCH_STATE_BINARY_SEARCH:
		/*
		* For efficient operation, we start with the largest stride
		* possible and then decrease the stride on each iteration. We
		* will check for a remainder when modding the offset with the
		* previous stride. This makes it so that each offset is only
		* checked once.
		*
		* At some point, programmer transaction overhead becomes
		* greater than simply copying everything into RAM and
		* checking one byte at a time. At some arbitrary point, we'll
		* stop being clever and use brute force instead by copying
		* the while ROM into RAM and searching one byte at a time.
		*
		* In practice, the flash map is usually stored in a
		* write-protected section of flash which is often at the top
		* of ROM where the boot vector on x86 resides. Because of
		* this, we will search from top to bottom.
		*
		* We assume we can always return at least one offset here.
		*/
		*offsetp = search->offset;

		/*
		 * OK, now what offset should we return next? This loop skips
		 * any offsets that were already checked by larger strides.
		 */
		do {
			search->offset -= search->stride;
		} while (search->offset % (search->stride * 2) == 0);

		/* Move to next stride if necessary */
		if (search->offset < 0) {
			search->stride /= 2;
			search->offset = search->ceiling_size - search->stride;
			while (search->offset > flash_size - search->min_size)
				search->offset -= search->stride;
			if (search->stride < 16) {
				search->state = SEARCH_STATE_FULL_SEARCH;
				search->offset = flash_size - 1;
				search->image = malloc(flash_size);
				if (!search->image) {
					msg_gdbg("%s: failed to allocate %ld "
						 "bytes for search->image",
						 __func__, flash_size);
					return -1;
				}
				if (read_flash(search->flash, search->image,
							0, flash_size)) {
					msg_gdbg("[L%d] failed to read flash contents\n",
						__LINE__);
					return -1;
				}
				msg_gdbg("using brute force method to find fmap\n");
			}
		}
		return 0;
	case SEARCH_STATE_FULL_SEARCH:
		/*
		 * brute force
		 * We have read the entire ROM above, so the caller will be
		 * able to use search->image to access it.
		 *
		 * FIXME: This results in the entire ROM being read twice --
		 * once here and again in doit(). The performance penalty
		 * needs to be dealt with before going upstream.
		 */
		do {
			*offsetp = search->offset--;
		} while (*offsetp > flash_size - search->min_size);
		if (search->offset < 0)
			search->state = SEARCH_STATE_DONE;
		return 0;
	case SEARCH_STATE_DONE:
		break;
	}

	/* Give up, it is not there */
	return -1;
}

void search_init(struct search_info *search, struct flashctx *flash,
		 int min_size)
{
	memset(search, '\0', sizeof(*search));
	search->flash = flash;
	search->min_size = min_size;
}

void search_free(struct search_info *search)
{
	if (search->image)
		free(search->image);
}
