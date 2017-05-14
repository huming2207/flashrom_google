#!/bin/sh
#
# Copyright (C) 2012 Google Inc.
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

export EXIT_SUCCESS=0
export EXIT_FAILURE=1

system_flashrom=$(which flashrom)
if [ -z "$system_flashrom" ]; then
	echo "Flashrom is not installed on this sytem"
	exit $EXIT_FAILURE
fi

# do_test_flashrom: Wrapper that will run the flashrom binary which is
# to be tested using parameters passed in both via the command-line and
# from the caller. Output will be suppressed, and return code of the
# flashrom command will be returned to the caller.
do_test_flashrom() {
	./flashrom ${FLASHROM_PARAM} "$@" >/dev/null 2>&1
	return $?
}

# same as do_test_flashrom, but will use the system's installed flashrom
# binary instead
system_flashrom() {
	$system_flashrom ${FLASHROM_PARAM} "$@" >/dev/null 2>&1
	return $?
}
