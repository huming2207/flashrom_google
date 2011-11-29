#!/bin/sh
#
# Copyright (C) 2010 Google Inc.
# Written by David Hendricks for Google Inc.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
#
#TODO: Compare size reported by JEDEC ID (opcode 9fh)

. "$(pwd)/common.sh"

logfile="${0}.log"

reported_size=$(./flashrom ${FLASHROM_PARAM} --get-size 2>/dev/null | tail -n 1)
actual_size=$(stat --printf="%s\n" ${BACKUP})
echo -n "$0: ${reported_size} ?= ${actual_size} ... " >> ${logfile}
if [ "$reported_size" != "$actual_size" ]; then
	echo "no." >> ${logfile}
	return ${EXIT_FAILURE}
else
	echo "yes." >> ${logfile}
fi

return ${EXIT_SUCCESS}
