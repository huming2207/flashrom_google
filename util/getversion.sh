#!/bin/sh
##
## This file is part of the flashrom project.
##
## Copyright (C) 2010 Chromium OS Authors
##
## This program is free software; you can redistribute it and/or modify
## it under the terms of the GNU General Public License as published by
## the Free Software Foundation; either version 2 of the License, or
## (at your option) any later version.
##
## This program is distributed in the hope that it will be useful,
## but WITHOUT ANY WARRANTY; without even the implied warranty of
## MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
## GNU General Public License for more details.
##
## You should have received a copy of the GNU General Public License
## along with this program; if not, write to the Free Software
## Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
##

# Rerieve subversion version mumber if available.
subv_version() {
    LC_ALL=C

    svnversion -cn . 2>/dev/null | \
	sed -e "s/.*://" -e "s/\([0-9]*\).*/\1/" | \
	grep "[0-9]" || LC_ALL=C svn info . 2>/dev/null | \
	awk '/^Revision:/ {print $$2 }' | \
	grep "[0-9]" || LC_ALL=C git svn info . 2>/dev/null | \
	awk '/^Revision:/ {print $$2 }' | \
	grep "[0-9]" || echo ''
}

#
# Generate git version string which has the following format:
#
#    <git server> : <commit hash> : <timestamp>
#
# In case the local tree is pristine, the timesatmp is derived from the top
# log message and is expressed in UTC.
#
# In case the local tree has modifications, the timestamp is the actual
# compilation time, this is indicated by adding a '+' to the end of the
# timestamp.
#
git_version() {
    local date_format="+%b %d %Y %H:%M:%S"
    local timestamp

    if [ ! -d .git ]; then
	echo ''
	return
    fi

    # are there local changes in the client?
    if  git status | \
	egrep '^# Change(s to be committed|d but not updated):$' > /dev/null
    then
        timestamp=$(date "${date_format} +")
    else
	# No local changes, get date of the last log record.
	local last_commit_date=$(git log | head -3 | grep '^Date:' | \
	    awk '{print $3" "$4" "$6" "$5" "$7}')
	timestamp=$(date --utc --date "${last_commit_date}" \
	    "${date_format} UTC")
    fi
    # Get remote server name (only the first line of `git remote' is
    # considered.
    local server=$(git remote -v |\
        awk '{
             if (!server) {
                server = $2;
                split(server, pieces, "//");
                print pieces[2] }
        }')

    # Get the commit hash.
    local commit=$(git log --oneline | head -1 | awk '{print $1}')

    # Generate the version string.
    echo "${server} : ${commit} : ${timestamp}"
}

v=$(subv_version)

if [ -z "${v}" ]; then
    v=$(git_version)
else
    # Make SVN version look like it did before git option was added.
    v="-r${v}"
fi
echo "${v}"
