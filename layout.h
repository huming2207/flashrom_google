/*
 * This file is part of the flashrom project.
 *
 * Copyright (C) 2013 Google Inc.
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

#ifndef FLASHMAP_LIB_LAYOUT_H__
#define FLASHMAP_LIB_LAYOUT_H__

typedef struct romlayout {
	unsigned int start;
	unsigned int end;
	unsigned int included;
	char name[256];
	char file[256];  /* file[0]=='\0' means not specified. */
} romlayout_t;

/**
 * Extract regions to current directory
 *
 * @flash: Information about flash chip to access
 * @return 0 if OK, non-zero on error
 */
int extract_regions(struct flashctx *flash);

int specified_partition();
int read_romlayout(char *name);
int find_romentry(char *name);
int fill_romentry(romlayout_t *entry, int n);
int handle_romentries(struct flashctx *flash, uint8_t *oldcontents, uint8_t *newcontents);
int add_fmap_entries(struct flashctx *flash);
int get_num_include_args(void);
int register_include_arg(char *name);
int process_include_args(void);
int num_include_files(void);
int included_regions_overlap(void);
int handle_romentries(struct flashctx *flash, uint8_t *oldcontents, uint8_t *newcontents);
int handle_partial_read(
    struct flashctx *flash,
    uint8_t *buf,
    int (*read) (struct flashctx *flash, uint8_t *buf,
                 unsigned int start, unsigned int len),
    int write_to_file);
    /* RETURN: the number of partitions that have beenpartial read.
    *         ==0 means no partition is specified.
    *         < 0 means writing file error. */
int handle_partial_verify(
    struct flashctx *flash,
    uint8_t *buf,
    int (*verify) (struct flashctx *flash, uint8_t *buf, unsigned int start,
                   unsigned int len, const char* message));
    /* RETURN: ==0 means all identical.
               !=0 means buf and flash are different. */

#endif
