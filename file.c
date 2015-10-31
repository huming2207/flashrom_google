/*
 * Copyright 2015, The Chromium OS Authors
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *    * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above
 *      copyright notice, this list of conditions and the following
 *      disclaimer in the documentation and/or other materials provided
 *      with the distribution.
 *    * Neither the name of Google Inc. nor the names of its
 *      contributors may be used to endorse or promote products derived
 *      from this software without specific prior written permission.
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
 * scanft() is derived from mosys source,s which were released under the
 * BSD license
 */

#include <dirent.h>
#include <errno.h>
#include <glob.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "flash.h"

#define FDT_ROOT	"/proc/device-tree"
#define FDT_ALIASES	"/proc/device-tree/aliases"

/* returns 1 if string if found, 0 if not, and <0 to indicate error */
static int find_string(const char *path, const char *str)
{
	int fd, ret = 0;
	struct stat s;
	off_t i;

	if (stat(path, &s) < 0) {
		msg_gerr("Cannot stat file \"%s\"\n", path);
		ret = -1;
		goto find_string_exit_0;
	}

	if (s.st_size < strlen(str))
		goto find_string_exit_0;

	fd = open(path, O_RDONLY, 0);
	if (fd < 0) {
		msg_gerr("Cannot open file \"%s\"\n", path);
		ret = -1;
		goto find_string_exit_0;
	}

	/* mmap() would be nice but it might not be implemented for some
	 * files in sysfs and procfs */
	for (i = 0; i <= s.st_size - strlen(str); i++) {
		int match_found = 1;
		off_t j;

		if (lseek(fd, i, SEEK_SET) == (off_t)-1) {
			msg_gerr("Failed to seek within \"%s\"\n", path);
			ret = -1;
			goto find_string_exit_1;
		}

		for (j = 0; j < strlen(str); j++) {
			char c;

			if (read(fd, &c, 1) != 1) {
				msg_gerr("Failed to read \"%s\"\n", path);
				ret = -1;
				goto find_string_exit_1;
			}

			if (str[j] != c) {
				match_found = 0;
				break;
			}
		}

		if (match_found) {
			ret = 1;
			break;
		}

	}

find_string_exit_1:
	close(fd);
find_string_exit_0:
	return ret;
}

 /*
 * scanft - scan filetree for file with option to search for content
 *
 * @root:	Where to begin search
 * @filename:	Name of file to search for
 * @str:	Optional NULL terminated string to search for
 * @symdepth:	Maximum depth of symlinks to follow. A negative value means
 * 		follow indefinitely. Zero means do not follow symlinks.
 *
 * The caller should be specific enough with root and symdepth arguments
 * to avoid finding duplicate information (especially in sysfs).
 *
 * returns allocated string with path of matching file if successful
 * returns NULL to indicate failure
 */
const char *scanft(const char *root, const char *filename,
		   const char *str, int symdepth)
{
	DIR *dp;
	struct dirent *d;
	struct stat s;
	const char *ret = NULL;

	if (lstat(root, &s) < 0) {
		msg_pdbg("%s: Error stat'ing %s: %s\n",
		        __func__, root, strerror(errno));
		return NULL;
	}

	if (S_ISLNK(s.st_mode)) {
		if (symdepth == 0)	/* Leaf has been reached */
			return NULL;
		else if (symdepth > 0)	/* Follow if not too deep in */
			symdepth--;
	} else if (!S_ISDIR(s.st_mode)) {
		return NULL;
	}

	if ((dp = opendir(root)) == NULL)
		return NULL;

	while (!ret && (d = readdir(dp))) {
		char newpath[PATH_MAX];

		/* Skip "." and ".." */
		if (!(strcmp(d->d_name, ".")) ||
		    !(strcmp(d->d_name, "..")))
			continue;

		snprintf(newpath, sizeof(newpath), "%s/%s", root, d->d_name);

		if (!strcmp(d->d_name, filename)) {
			if (!str || (find_string(newpath, str) == 1))
				ret = strdup(newpath);
		}

		if (!ret)
			ret = scanft(newpath, filename, str, symdepth);
	}

	closedir(dp);
	return ret;
}

/*
 * do_fdt_find_spi_nor_flash - Search FDT via procfs for SPI NOR flash
 *
 * @prefix:	Prefix of alias, for example "spi" will match spi*.
 * @compat:	String to look for in "compatible" node
 *
 * This function attempt to match FDT aliases with devices that have the given
 * compatible string.
 *
 * Example: If prefix is "spi" and compat is "jedec,spi-nor" then this function
 * will read the device descriptors in every alias beginning with "spi" and
 * search their respective devicetree nodes for "compatible" files containing
 * the string "jedec,spi-nor".
 *
 * returns 0 to indicate NOR flash has been found, <0 to indicate error
 */
static int do_fdt_find_spi_nor_flash(const char *prefix,
		const char *compat, unsigned int *bus, uint32_t *cs)
{
	DIR *dp;
	struct dirent *d;
	struct stat s;
	int found = 0;

	if ((dp = opendir(FDT_ALIASES)) == NULL)
		return -1;

	/*
	 * This loop will go thru the aliases sub-directory and kick-off a
	 * recursive search thru matching devicetree nodes.
	 */
	while (!found && (d = readdir(dp))) {
		char node[PATH_MAX];
		char pattern[PATH_MAX];
		char alias[64];
		int i, fd, len;
		glob_t pglob;

		/* allow partial match */
		if (strncmp(prefix, d->d_name, strlen(prefix)))
			continue;

		sprintf(node, "%s/%s", FDT_ALIASES, d->d_name);
		if (stat(node, &s) < 0) {
			msg_pdbg("%s: Error stat'ing %s: %s\n",
			        __func__, node, strerror(errno));
			continue;
		}

		if (!S_ISREG(s.st_mode))
			continue;

		fd = open(node, O_RDONLY);
		if (fd < 0) {
			msg_perr("Could not open %s\n", d->d_name);
			continue;
		}

		/* devicetree strings and files aren't always terminated */
		len = read(fd, alias, sizeof(alias) - 1);
		if (len < 0) {
			msg_perr("Could not read %s\n", d->d_name);
			close(fd);
			continue;
		}
		alias[len] = '\0';
		close(fd);

		/* We expect something in the form "/<type>@<address>", for
		   example "/spi@ff110000" */
		if (alias[0] != '/')
			continue;

		snprintf(node, sizeof(node), "%s%s", FDT_ROOT, alias);

		/*
		 * Descend into this node's directory. According to the DT
		 * specification, the SPI device node will be a subnode of
		 * the bus node. Thus, we need to look for:
		 * <path-to-spi-bus-node>/.../compatible
		 */
		sprintf(pattern, "%s/*/compatible", node);
		msg_pspew("Scanning glob pattern \"%s\"\n", pattern);
		i = glob(pattern, 0, NULL, &pglob);
		if (i == GLOB_NOSPACE)
			goto err_out;
		else if (i != 0)
			continue;

		/*
		 * For chip-select, look at the "reg" file located in
		 * the same sub-directory as the "compatible" file.
		 */
		for (i = 0; i < pglob.gl_pathc; i++) {
			char *reg_path;

			if (!find_string(pglob.gl_pathv[i], compat))
				continue;

			reg_path = strdup(pglob.gl_pathv[i]);
			if (!reg_path) {
				globfree(&pglob);
				goto err_out;
			}

			sprintf(strstr(reg_path, "compatible"), "reg");
			fd = open(reg_path, O_RDONLY);
			if (fd < 0) {
				msg_gerr("Cannot open \"%s\"\n", reg_path);
				free(reg_path);
				continue;
			}

			/* value is a 32-bit big-endian unsigned in FDT.  */
			if (read(fd, cs, 4) != 4) {
				msg_gerr("Cannot read \"%s\"\n", reg_path);
				free(reg_path);
				close(fd);
				continue;
			}

			*cs = ntohl(*cs);
			found = 1;
			free(reg_path);
			close(fd);
		}

		if (found) {
			/* Extract bus from the alias filename. */
			if (sscanf(d->d_name, "spi%u", bus) != 1) {
				msg_gerr("Unexpected format: \"d->d_name\"\n");
				found = 0;
				globfree(&pglob);
				goto err_out;
			}
		}

		globfree(&pglob);
	}

err_out:
	closedir(dp);
	return found ? 0 : -1;
}

/* Wrapper in case we want to use a list of compat strings or extend
 * this to search for other types of devices */
int fdt_find_spi_nor_flash(unsigned int *bus, unsigned int *cs)
{
	return do_fdt_find_spi_nor_flash("spi", "jedec,spi-nor", bus, cs);
}
