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
 */

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <zlib.h>

#include "flash.h"
#include "fdtmap.h"
#include "flash.h"
#include "libfdt.h"
#include "layout.h"

#ifdef DEBUG
#define debug(a, b...) printf(a, ##b)
#else
#define debug(a, b...)
#endif

/* An entry in the flashmap - we only care about the offset and length */
struct fmap_entry {
	uint32_t offset;
	uint32_t length;
};

/**
 * Look up a property in a node and check that it has a minimum length.
 *
 * @blob: FDT blob
 * @node: node to examine
 * @prop_name: name of property to find
 * @min_len: minimum property length in bytes
 * @err: 0 if ok, or -FDT_ERR_NOTFOUND if the property is not
			found, or -FDT_ERR_BADLAYOUT if not enough data
 * @return pointer to cell, which is only valid if err == 0
 */
static const void *get_prop_check_min_len(const void *blob, int node,
		const char *prop_name, int min_len, int *err)
{
	const void *cell;
	int len;

	debug("%s: %s\n", __func__, prop_name);
	cell = fdt_getprop(blob, node, prop_name, &len);
	if (!cell)
		*err = -FDT_ERR_NOTFOUND;
	else if (len < min_len)
		*err = -FDT_ERR_BADLAYOUT;
	else
		*err = 0;
	return cell;
}

static int fdtdec_get_int_array(const void *blob, int node,
		const char *prop_name, uint32_t *array, int count)
{
	const uint32_t *cell;
	int i, err = 0;

	debug("%s: %s\n", __func__, prop_name);
	cell = get_prop_check_min_len(blob, node, prop_name,
				      sizeof(uint32_t) * count, &err);
	if (!err) {
		for (i = 0; i < count; i++)
			array[i] = fdt32_to_cpu(cell[i]);
	}
	return err;
}

/**
 * Read a flash entry from the fdt
 *
 * @blob: FDT blob
 * @node: Offset of node to read
 * @name: Name of node being read
 * @entry: Place to put offset and size of this node
 * @return 0 if ok, -ve on error
 */
static int read_entry(const void *blob, int node, const char *name,
		      struct fmap_entry *entry)
{
	uint32_t reg[2];
	int ret;

	ret = fdtdec_get_int_array(blob, node, "reg", reg, 2);
	if (ret) {
		debug("Node '%s' has bad/missing 'reg' property\n", name);
		return ret;
	}
	entry->offset = reg[0];
	entry->length = reg[1];

	return 0;
}

static int scan_flashmap(const void *blob, romlayout_t *rom_entries,
			  int max_entries)
{
	int romimages;
	int offset;
	int depth;
	int node;

	offset = fdt_node_offset_by_compatible(blob, -1,
			"chromeos,flashmap");
	if (offset < 0)
		return offset;

	for (depth = romimages = 0; offset > 0 && depth >= 0; offset = node) {
		struct fmap_entry entry;
		const char *name;
		romlayout_t *rl;
		char *s;
		int ret;

		node = fdt_next_node(blob, offset, &depth);
		if (node < 0)
			return node;
		if (depth != 1)
			continue;

		name = fdt_getprop(blob, node, "label", NULL);
		ret = read_entry(blob, node, name, &entry);
		if (ret)
			return ret;

		if (romimages >= max_entries) {
			msg_gerr("ROM image contains too many regions\n");
			return -1;
		}
		rl = &rom_entries[romimages];
		rl->start = entry.offset;

		/*
		 * Flashrom rom entries use absolute addresses. So for non-zero
		 * length entries, we need to subtract 1 from offset + size to
		 * determine the end address.
		 */
		rl->end = entry.offset + entry.length;
		if (entry.length)
			rl->end--;

		/* Use an upper case name for flashrom, with _ instead of - */
		strncpy(rl->name, name, sizeof(rl->name));
		rl->name[sizeof(rl->name) - 1] = '\0';
		for (s = rl->name; *s; s++)
			if (*s == '-')
				*s = '_';
			else if (*s == '@')
				break;
			else
				*s = toupper(*s);
		*s = '\0';

		*rl->file = '\0';
		rl->included = 0;

		msg_gdbg("added fdtmap region \"%s\" (file=\"%s\") as %sincluded,"
			 " offset: 0x%08x, end: 0x%08x\n",
			  rl->name,
			  rl->file,
			  rl->included ? "" : "not ",
			  rl->start,
			  rl->end);
		romimages++;
	}

	msg_gdbg("Found %d regions\n", romimages);

	return romimages;
}

int fdtmap_add_entries_from_buf(const void *blob, romlayout_t *rom_entries,
				int max_entries)
{
	int count;

	count = scan_flashmap(blob, rom_entries, max_entries);
	if (count < 0) {
		msg_gerr("Failed to read flashmap: '%s'\n", fdt_strerror(count));
		return -1;
	}

	return count;
}

int fdtmap_find(struct flashctx *flash, struct fdtmap_hdr *hdr, loff_t offset,
		uint8_t **buf)
{
	int fmap_size;
	uint32_t crc;

	if (memcmp(hdr->sig, FDTMAP_SIGNATURE, sizeof(hdr->sig)))
		return 0;

	msg_gdbg("%s: found possible fdtmap at offset %#lx\n",
		 __func__, (unsigned long)offset);

	fmap_size = hdr->size;
	*buf = malloc(fmap_size);
	msg_gdbg("%s: fdtmap size %#x\n", __func__, fmap_size);

	/* We may as well just read it here, to simplify the code */
	if (flash->chip->read(flash, *buf, offset + sizeof(*hdr), fmap_size)) {
		msg_gdbg("[L%d] failed to read %d bytes at offset %#lx\n",
			 __LINE__, fmap_size, (unsigned long)offset);
		return 0;
	}

	/* Sanity check, the FDT total size should equal fmap_size */
	if (fdt_totalsize(*buf) != fmap_size) {
		msg_gdbg("[L%d] FDT size %#x did not match header size %#x at %#lx\n",
			 __LINE__, fdt_totalsize(*buf),
			 fmap_size, (unsigned long)offset);
		return 0;
	}

	crc = crc32(0, Z_NULL, 0);
	crc = crc32(crc, *buf, fmap_size);
	/* Sanity check, the FDT total size should equal fmap_size */
	if (crc != hdr->crc32) {
		msg_gdbg("[L%d] CRC32 %#08x did not match expected %#08x at %#lx\n",
			 __LINE__, crc, hdr->crc32, (unsigned long)offset);
		return 0;
	}

	return 1;
}
