/*
 * Copyright 2012, The Chromium OS Authors
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
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "flash.h"

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

