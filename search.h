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

#ifndef FLASHMAP_LIB_SEARCH_H__
#define FLASHMAP_LIB_SEARCH_H__

/* Our current state in the search process */
enum search_state_t {
	SEARCH_STATE_START,
	SEARCH_STATE_USE_HANDLER,	/* Call handler function */
	SEARCH_STATE_BINARY_SEARCH,	/* Fast binary search */
	SEARCH_STATE_FULL_SEARCH,	/* Slow incremental search */
	SEARCH_STATE_DONE,		/* Search completed */
};

/* Keeps track of the state of our search */
struct search_info {
	struct flashchip *flash;	/* Flash information */
	enum search_state_t state;	/* Current state */
	long int ceiling_size;		/* Lowest power of 2 >= flash size */
	long int stride;		/* Current binary search stride */
	off_t offset;			/* Next offset to return */
	uint8_t *image;			/* Cache of entire flash image */
	int min_size;			/* Minimum size of data to find */
	/*
	* Utility wrapper for using external programs to aid in our search.
	* @search: Pointer to search information
	* @offset: Wrapper will set this if successful
	*
	* @return 0 if successful, -1 to indicate failure or offset not
	* found by utility
	*/
	int (*handler)(struct search_info *search, off_t *offset);
};

/**
 * search_find_next() - Find the next offset to check in a search operation
 *
 * If search->image is not NULL, then it contains the full flash image and
 * the caller can use this instead of reading the data again.
 *
 * @search: Search information, set up by search_init()
 * @offsetp: Returns next offset to check
 * @return 0 if we have an offset, -1 if we have run out of places to look
 */
int search_find_next(struct search_info *search, off_t *offsetp);

/** search_init() - Get ready to start a search
 *
 * @search: Search information, set up by this function
 * @flash: Information about the flash chip
 * @min_size: Minimum size of region that we want to find
 */
void search_init(struct search_info *search, struct flashchip *flash,
		 int min_size);

/** search_free() - Free memory allocated by search
 *
 * @search: Search information, set up by search_init()
 */
void search_free(struct search_info *search);

#endif
