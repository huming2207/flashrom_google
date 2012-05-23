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

/*returns 1 if contents matches, 0 if not, and <0 to indicate error */
static int file_contents_match(const char *path, const char *str)
{
	FILE *fp;
	char *content;
	int len = strlen(str);
	int ret = -1;

	content = malloc(len);
	if (!content)
		return -1;

	if ((fp = fopen(path, "r")) == NULL) {
		msg_pdbg("Error opening %s: %s\n", path, strerror(errno));
		goto file_contents_match_done;
	}
	if (fread(content, 1, len, fp) < 1) {
		msg_pdbg("Error reading %s: %s\n", path, strerror(ferror(fp)));
		goto file_contents_match_done;
	}

	if (!strncmp(str, content, len))
		ret = 1;
	else
		ret = 0;

file_contents_match_done:
	fclose(fp);
	free(content);
	return ret;
}

 /*
 * scanft - scan filetree for file with option to parse some content
 *
 * @root:	Where to begin search
 * @filename:	Name of file to search for
 * @str:	Optional NULL terminated string to check at the beginning
 * 		of the file
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
			if (!str || file_contents_match(newpath, str))
				ret = strdup(newpath);
		}

		if (!ret)
			ret = scanft(newpath, filename, str, symdepth);
	}

	closedir(dp);
	return ret;
}

