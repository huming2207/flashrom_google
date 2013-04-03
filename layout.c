/*
 * This file is part of the flashrom project.
 *
 * Copyright (C) 2005-2008 coresystems GmbH
 * (Written by Stefan Reinauer <stepan@coresystems.de> for coresystems GmbH)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>

#include "flash.h"
#include "fdtmap.h"
#include "fmap.h"
#include "libfdt.h"
#include "layout.h"
#include "programmer.h"
#include "search.h"

#if CONFIG_INTERNAL == 1
char *mainboard_vendor = NULL;
char *mainboard_part = NULL;
#endif
static int romimages = 0;

#define MAX_ROMLAYOUT	64

/*
 * include_args lists arguments specified at the command line with -i. They
 * must be processed at some point so that desired regions are marked as
 * "included" in the master rom_entries list.
 */
static char *include_args[MAX_ROMLAYOUT];
static int num_include_args = 0;  /* the number of valid entries. */
static romlayout_t rom_entries[MAX_ROMLAYOUT];

#if CONFIG_INTERNAL == 1 /* FIXME: Move the whole block to cbtable.c? */
static char *def_name = "DEFAULT";


/* Return TRUE if user specifies any -i argument. */
int specified_partition() {
	return num_include_args != 0;
}

int show_id(uint8_t *bios, int size, int force)
{
	unsigned int *walk;
	unsigned int mb_part_offset, mb_vendor_offset;
	char *mb_part, *mb_vendor;

	mainboard_vendor = def_name;
	mainboard_part = def_name;

	walk = (unsigned int *)(bios + size - 0x10);
	walk--;

	if ((*walk) == 0 || ((*walk) & 0x3ff) != 0) {
		/* We might have an NVIDIA chipset BIOS which stores the ID
		 * information at a different location.
		 */
		walk = (unsigned int *)(bios + size - 0x80);
		walk--;
	}

	/*
	 * Check if coreboot last image size is 0 or not a multiple of 1k or
	 * bigger than the chip or if the pointers to vendor ID or mainboard ID
	 * are outside the image of if the start of ID strings are nonsensical
	 * (nonprintable and not \0).
	 */
	mb_part_offset = *(walk - 1);
	mb_vendor_offset = *(walk - 2);
	if ((*walk) == 0 || ((*walk) & 0x3ff) != 0 || (*walk) > size ||
	    mb_part_offset > size || mb_vendor_offset > size) {
		msg_pdbg("Flash image seems to be a legacy BIOS. "
		         "Disabling coreboot-related checks.\n");
		return 0;
	}

	mb_part = (char *)(bios + size - mb_part_offset);
	mb_vendor = (char *)(bios + size - mb_vendor_offset);
	if (!isprint((unsigned char)*mb_part) ||
	    !isprint((unsigned char)*mb_vendor)) {
		msg_pdbg("Flash image seems to have garbage in the ID location."
		       " Disabling checks.\n");
		return 0;
	}

	msg_pdbg("coreboot last image size "
		     "(not ROM size) is %d bytes.\n", *walk);

	mainboard_part = strdup(mb_part);
	mainboard_vendor = strdup(mb_vendor);
	msg_pdbg("Manufacturer: %s\n", mainboard_vendor);
	msg_pdbg("Mainboard ID: %s\n", mainboard_part);

	/*
	 * If lb_vendor is not set, the coreboot table was
	 * not found. Nor was -m VENDOR:PART specified.
	 */
	if (!lb_vendor || !lb_part) {
		msg_pdbg("Note: If the following flash access fails, "
		       "try -m <vendor>:<mainboard>.\n");
		return 0;
	}

	/* These comparisons are case insensitive to make things
	 * a little less user^Werror prone. 
	 */
	if (!strcasecmp(mainboard_vendor, lb_vendor) &&
	    !strcasecmp(mainboard_part, lb_part)) {
		msg_pdbg("This firmware image matches this mainboard.\n");
	} else {
		if (force_boardmismatch) {
			msg_pinfo("WARNING: This firmware image does not "
			       "seem to fit to this machine - forcing it.\n");
		} else {
			msg_pinfo("ERROR: Your firmware image (%s:%s) does not "
			       "appear to\n       be correct for the detected "
			       "mainboard (%s:%s)\n\nOverride with -p internal:"
			       "boardmismatch=force if you are absolutely sure "
			       "that\nyou are using a correct "
			       "image for this mainboard or override\nthe detected "
			       "values with --mainboard <vendor>:<mainboard>.\n\n",
			       mainboard_vendor, mainboard_part, lb_vendor,
			       lb_part);
			exit(1);
		}
	}

	return 0;
}
#endif

#ifndef __LIBPAYLOAD__
int read_romlayout(char *name)
{
	FILE *romlayout;
	char tempstr[256];
	int i;

	romlayout = fopen(name, "r");

	if (!romlayout) {
		msg_gerr("ERROR: Could not open ROM layout (%s).\n",
			name);
		return -1;
	}

	while (!feof(romlayout)) {
		char *tstr1, *tstr2;

		if (romimages >= MAX_ROMLAYOUT) {
			msg_gerr("Maximum number of ROM images (%i) in layout "
				 "file reached before end of layout file.\n",
				 MAX_ROMLAYOUT);
			msg_gerr("Ignoring the rest of the layout file.\n");
			break;
		}
		if (2 != fscanf(romlayout, "%s %s\n", tempstr, rom_entries[romimages].name))
			continue;
#if 0
		// fscanf does not like arbitrary comments like that :( later
		if (tempstr[0] == '#') {
			continue;
		}
#endif
		tstr1 = strtok(tempstr, ":");
		tstr2 = strtok(NULL, ":");
		if (!tstr1 || !tstr2) {
			msg_gerr("Error parsing layout file.\n");
			fclose(romlayout);
			return 1;
		}
		rom_entries[romimages].start = strtol(tstr1, (char **)NULL, 16);
		rom_entries[romimages].end = strtol(tstr2, (char **)NULL, 16);
		rom_entries[romimages].included = 0;
		strcpy(rom_entries[romimages].file, "");
		romimages++;
	}

	for (i = 0; i < romimages; i++) {
		msg_gdbg("romlayout %08x - %08x named %s\n",
			     rom_entries[i].start,
			     rom_entries[i].end, rom_entries[i].name);
	}

	fclose(romlayout);

	return 0;
}
#endif

/*
 * Invoke crossystem and parse the returned string to produce an offset
 * @search: Search information
 * @offset: Place to put offset
 * @return 0 if offset found, -1 if not
 */
static int get_crossystem_fmap_base(struct search_info *search, off_t *offset)
{
	char cmd[] = "crossystem fmap_base";
	FILE *fp;
	int n;
	char buf[16];
	unsigned long fmap_base;
	unsigned long from_top;

	if (!(fp = popen(cmd, "r")))
		return -1;
	n = fread(buf, 1, sizeof(buf) - 1, fp);
	fclose(fp);
	if (n < 0)
		return -1;
	buf[n] = '\0';
	if (strlen(buf) == 0)
		return -1;

	/*
	 * There are 2 kinds of fmap_base returned from crossystem.
	 *
	 *  1. Shadow ROM/BIOS area (x86), such as 0xFFxxxxxx.
	 *  2. Offset to start of flash, such as 0x00xxxxxx.
	 *
	 * The shadow ROM is a cached copy of the BIOS ROM which resides below
	 * 4GB host/CPU memory address space on x86. The top of BIOS address
	 * aligns to the last byte of address space, 0xFFFFFFFF. So to obtain
	 * the ROM offset when shadow ROM is used, we subtract the fmap_base
	 * from 4G minus 1.
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
	 * We'll use bit 31 to determine if the shadow BIOS area is being used.
	 * This is sort of a hack, but allows us to perform sanity checking for
	 * older x86-based Chrome OS platforms.
	 */

	fmap_base = (unsigned long)strtoll(buf, (char **) NULL, 0);
	msg_gdbg("%s: fmap_base: %#lx, ROM size: 0x%x\n",
		__func__, fmap_base, search->flash->total_size * 1024);

	if (fmap_base & (1 << 31)) {
		from_top = 0xFFFFFFFF - fmap_base + 1;
		msg_gdbg("%s: fmap is located in shadow ROM, from_top: %#lx\n",
				__func__, from_top);
		if (from_top > search->flash->total_size * 1024)
			return -1;
		*offset = (search->flash->total_size * 1024) - from_top;
	} else {
		msg_gdbg("%s: fmap is located in physical ROM\n", __func__);
		if (fmap_base > search->flash->total_size * 1024)
			return -1;
		*offset = fmap_base;
	}

	msg_gdbg("%s: ROM offset: %#lx\n", __func__, *offset);
	return 0;
}

static int add_fmap_entries_from_buf(struct flashchip *flash,
				     const uint8_t *buf)
{
	struct fmap *fmap;
	int i;

	fmap = (struct fmap *)(buf);

	for (i = 0; i < fmap->nareas; i++) {
		if (romimages >= MAX_ROMLAYOUT) {
			msg_gerr("ROM image contains too many regions\n");
			return -1;
		}
		rom_entries[romimages].start = fmap->areas[i].offset;

		/*
		 * Flashrom rom entries use absolute addresses. So for non-zero
		 * length entries, we need to subtract 1 from offset + size to
		 * determine the end address.
		 */
		rom_entries[romimages].end = fmap->areas[i].offset +
		                             fmap->areas[i].size;
		if (fmap->areas[i].size)
			rom_entries[romimages].end--;

		memset(rom_entries[romimages].name, 0,
		       sizeof(rom_entries[romimages].name));
		memcpy(rom_entries[romimages].name, fmap->areas[i].name,
		       min(sizeof(rom_entries[romimages].name),
		           sizeof(fmap->areas[i].name)));

		rom_entries[romimages].included = 0;
		strcpy(rom_entries[romimages].file, "");

		msg_gdbg("added fmap region \"%s\" (file=\"%s\") as %sincluded,"
			 " offset: 0x%08x, size: 0x%08x\n",
			  rom_entries[romimages].name,
			  rom_entries[romimages].file,
			  rom_entries[romimages].included ? "" : "not ",
			  rom_entries[romimages].start,
			  rom_entries[romimages].end);
		romimages++;
	}

	return romimages;
}

enum found_t {
	FOUND_NONE,
	FOUND_FMAP,
	FOUND_FDTMAP,
};

/* returns the number of entries added, or <0 to indicate error */
int add_fmap_entries(struct flashchip *flash)
{
	enum found_t found = FOUND_NONE;
	int ret = -1;
	struct search_info search;
	union {
		struct fdtmap_hdr fdtmap;
		struct fmap fmap;
	} hdr;
	uint8_t *buf = NULL;
	off_t offset;

	romimages = 0;
	search_init(&search, flash, sizeof(hdr));
	search.handler = get_crossystem_fmap_base;
	while (found == FOUND_NONE && !search_find_next(&search, &offset)) {
		if (search.image)
			memcpy(&hdr, search.image + offset, sizeof(hdr));
		else if (flash->read(flash, (uint8_t *)&hdr, offset,
				sizeof(hdr))) {
			msg_gdbg("[L%d] failed to read flash at offset %#lx\n",
				__LINE__, offset);
			return -1;
		}
		ret = fmap_find(flash, &hdr.fmap, offset, &buf);
		if (ret == 1) {
			found = FOUND_FMAP;
		}
#ifdef CONFIG_FDTMAP
		if (ret == 0) {
			ret = fdtmap_find(flash, &hdr.fdtmap, offset, &buf);
			if (ret == 1)
				found = FOUND_FDTMAP;
		}
#endif
		if (ret < 0)
			return ret;
	}

	switch (found) {
#ifdef CONFIG_FDTMAP
	case FOUND_FDTMAP:
		/* It looks valid, so use it */
		romimages = fdtmap_add_entries_from_buf(buf, rom_entries,
							  MAX_ROMLAYOUT);
		break;
#endif
	case FOUND_FMAP:
		romimages = add_fmap_entries_from_buf(flash, buf);
	default:
		msg_gdbg("%s: no fmap present\n", __func__);
	}
	if (buf)
		free(buf);
	search_free(&search);

	return romimages;
}

int get_num_include_args(void) {
  return num_include_args;
}

/* register an include argument (-i) for later processing */
int register_include_arg(char *name)
{
	if (num_include_args >= MAX_ROMLAYOUT) {
		msg_gerr("too many regions included\n");
		return -1;
	}

	include_args[num_include_args] = name;
	num_include_args++;
	return num_include_args;
}

int find_romentry(char *name)
{
	int i;
	char *file = NULL;
	char *has_colon;

	if (!romimages)
		return -1;

	/* -i <image>[:<file>] */
	has_colon = strchr(name, ':');
	if (strtok(name, ":")) {
		file = strtok(NULL, "");
		if (has_colon && file == NULL) {
			msg_gerr("Missing filename parameter in %s\n", name);
			return -1;
		}
	}
	msg_gdbg("Looking for \"%s\" (file=\"%s\")... ",
	         name, file ? file : "<not specified>");

	for (i = 0; i < romimages; i++) {
		if (!strcmp(rom_entries[i].name, name)) {
			rom_entries[i].included = 1;
			snprintf(rom_entries[i].file,
			         sizeof(rom_entries[i].file),
			         "%s", file ? file : "");
			msg_gdbg("found.\n");
			return i;
		}
	}
	msg_gdbg("not found.\n");	// Not found. Error.

	return -1;
}

/*
 * process_include_args - process -i arguments
 *
 * returns 0 to indicate success, <0 to indicate failure
 */
int process_include_args() {
	int i;

	for (i = 0; i < num_include_args; i++) {
		if (include_args[i]) {
			/* User has specified the area name, but no layout file
			 * is loaded, and no fmap is stored in BIOS.
			 * Return error. */
			if (!romimages) {
				msg_gerr("No layout info is available.\n");
				return -1;
			}

			if (find_romentry(include_args[i]) < 0) {
				msg_gerr("Invalid entry specified: %s\n",
				         include_args[i]);
				return -1;
			}
		} else {
			break;
		}
	}

	return 0;
}

int find_next_included_romentry(unsigned int start)
{
	int i;
	unsigned int best_start = UINT_MAX;
	int best_entry = -1;

	/* First come, first serve for overlapping regions. */
	for (i = 0; i < romimages; i++) {
		if (!rom_entries[i].included)
			continue;
		/* Already past the current entry? */
		if (start > rom_entries[i].end)
			continue;
		/* Inside the current entry? */
		if (start >= rom_entries[i].start)
			return i;
		/* Entry begins after start. */
		if (best_start > rom_entries[i].start) {
			best_start = rom_entries[i].start;
			best_entry = i;
		}
	}
	return best_entry;
}

static int read_content_from_file(int entry, uint8_t *newcontents) {
	char *file;
	FILE *fp;
	int len;

	/* If file name is specified for this partition, read file
	 * content to overwrite. */
	file = rom_entries[entry].file;
	len = rom_entries[entry].end - rom_entries[entry].start + 1;
	if (file[0]) {
		int numbytes;
		if ((fp = fopen(file, "rb")) == NULL) {
			perror(file);
			return -1;
		}
		numbytes = fread(newcontents + rom_entries[entry].start,
		                 1, len, fp);
		fclose(fp);
		if (numbytes == -1) {
			perror(file);
			return -1;
		}
	}
	return 0;
}

int handle_romentries(struct flashchip *flash, uint8_t *oldcontents, uint8_t *newcontents)
{
	unsigned int start = 0;
	int entry;
	unsigned int size = flash->total_size * 1024;

	/* If no regions were specified for inclusion, assume
	 * that the user wants to write the complete new image.
	 */
	if (num_include_args == 0)
		return 0;

	/* Non-included romentries are ignored.
	 * The union of all included romentries is used from the new image.
	 */
	while (start < size) {

		entry = find_next_included_romentry(start);
		/* No more romentries for remaining region? */
		if (entry < 0) {
			memcpy(newcontents + start, oldcontents + start,
			       size - start);
			break;
		}

		/* For non-included region, copy from old content. */
		if (rom_entries[entry].start > start)
			memcpy(newcontents + start, oldcontents + start,
			       rom_entries[entry].start - start);
		/* For included region, copy from file if specified. */
		if (read_content_from_file(entry, newcontents) < 0) return -1;

		/* Skip to location after current romentry. */
		start = rom_entries[entry].end + 1;
		/* Catch overflow. */
		if (!start)
			break;
	}
			
	return 0;
}
static int write_content_to_file(int entry, uint8_t *buf) {
	char *file;
	FILE *fp;
	int len = rom_entries[entry].end - rom_entries[entry].start + 1;

	file = rom_entries[entry].file;
	if (file[0]) {  /* save to file if name is specified. */
		int numbytes;
		if ((fp = fopen(file, "wb")) == NULL) {
			perror(file);
			return -1;
		}
		numbytes = fwrite(buf + rom_entries[entry].start, 1, len, fp);
		fclose(fp);
		if (numbytes != len) {
			perror(file);
			return -1;
		}
	}
	return 0;
}

/*  Reads flash content specified with -i argument into *buf. */
int handle_partial_read(
    struct flashchip *flash,
    uint8_t *buf,
    int (*read) (struct flashchip *flash, uint8_t *buf,
                 unsigned int start, unsigned int len),
    int write_to_file) {

	unsigned int start = 0;
	int entry;
	unsigned int size = flash->total_size * 1024;
	int count = 0;

	/* If no regions were specified for inclusion, assume
	 * that the user wants to read the complete image.
	 */
	if (num_include_args == 0)
		return 0;

	/* Walk through the table and write content to file for those included
	 * partition. */
	while (start < size) {
		int len;

		entry = find_next_included_romentry(start);
		/* No more romentries for remaining region? */
		if (entry < 0) {
			break;
		}
		++count;

		/* read content from flash. */
		len = rom_entries[entry].end - rom_entries[entry].start + 1;
		if (read(flash, buf + rom_entries[entry].start,
		         rom_entries[entry].start, len)) {
			msg_perr("flash partial read failed.");
			return -1;
		}
		/* If file is specified, write this partition to file. */
		if (write_to_file) {
			if (write_content_to_file(entry, buf) < 0) return -1;
		}

		/* Skip to location after current romentry. */
		start = rom_entries[entry].end + 1;
		/* Catch overflow. */
		if (!start)
			break;
	}

	return count;
}

/* Instead of verifying the whole chip, this functions only verifies those
 * content in specified partitions (-i).
 */
int handle_partial_verify(
    struct flashchip *flash,
    uint8_t *buf,
    int (*verify) (struct flashchip *flash, uint8_t *buf,
                   unsigned int start, unsigned int len, const char *message)) {
	unsigned int start = 0;
	int entry;
	unsigned int size = flash->total_size * 1024;

	/* If no regions were specified for inclusion, assume
	 * that the user wants to read the complete image.
	 */
	if (num_include_args == 0)
		return 0;

	/* Walk through the table and write content to file for those included
	 * partition. */
	while (start < size) {
		int len;

		entry = find_next_included_romentry(start);
		/* No more romentries for remaining region? */
		if (entry < 0) {
			break;
		}

		/* read content from flash. */
		len = rom_entries[entry].end - rom_entries[entry].start + 1;
		if (verify(flash, buf + rom_entries[entry].start,
		           rom_entries[entry].start, len, NULL)) {
			msg_perr("flash partial verify failed.");
			return -1;
		}

		/* Skip to location after current romentry. */
		start = rom_entries[entry].end + 1;
		/* Catch overflow. */
		if (!start)
			break;
	}

	return 0;
}
