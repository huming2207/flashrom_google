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

# Assume we can set enable/disable bits (that is, SPI chip is not hardware
# write-protected). Also, assume output is in the form:
# WP: write protect is enabled
# WP: write protect is disabled
#
# FIXME: Carl-Daniel pointed out that this script does little to validate that
# the settings on the chip are actually correct. It merely checks that Flashrom
# prints what we want it to print.

. "$(pwd)/common.sh"

logfile="${0}.log"
wp_disabled=0
wp_enabled=1
magic_string="write protect is"

wp_toggle_fail()
{
	echo "$1" >> ${logfile}
	echo "$0: failed" >> ${logfile}
	exit ${EXIT_FAILURE}
}

wp_status()
{
	if [ "$(printf %s\\n "$1" | sed 's/.*enabled.*/enabled/')" = "enabled" ]; then
		return ${wp_enabled}
	elif [ "$(printf %s\\n "$1" | sed 's/.*disabled.*/disabled/')" = "disabled" ]; then
		return ${wp_disabled}
	else
		wp_toggle_fail "unknown write protect status: \"$1\""
	fi
}

# Back-up old settings
tmp=$(./flashrom ${FLASHROM_PARAM} --wp-status 2>/dev/null | grep "$magic_string")
wp_status "$tmp"
old_status=$?
echo "old write protect status: ${old_status}" >> ${logfile}

# invert the old setting
if [ ${old_status} -eq ${wp_enabled} ]; then
	do_test_flashrom --wp-disable
	tmp=$(./flashrom ${FLASHROM_PARAM} --wp-status 2>/dev/null | grep "$magic_string")
	wp_status "$tmp"
	if [ $? -eq ${wp_enabled} ]; then
		wp_toggle_fail "failed to disable write protection"
	fi
elif [ ${old_status} -eq ${wp_disabled} ]; then
	do_test_flashrom --wp-enable
	tmp=$(./flashrom ${FLASHROM_PARAM} --wp-status 2>/dev/null | grep "$magic_string")
	wp_status "$tmp"
	if [ $? -eq ${wp_disabled} ]; then
		wp_toggle_fail "failed to enable write protection"
	fi
fi

# restore old setting
if [ ${old_status} -eq ${wp_enabled} ]; then
	do_test_flashrom --wp-enable
	tmp=$(./flashrom ${FLASHROM_PARAM} --wp-status 2>/dev/null | grep "$magic_string")
	wp_status "$tmp"
	if [ $? -ne ${wp_enabled} ]; then
		wp_toggle_fail "failed to enable write protection"
	fi
elif [ ${old_status} -eq ${wp_disabled} ]; then
	do_test_flashrom --wp-disable
	tmp=$(./flashrom ${FLASHROM_PARAM} --wp-status 2>/dev/null | grep "$magic_string")
	wp_status "$tmp"
	if [ $? -ne ${wp_disabled} ]; then
		wp_toggle_fail "failed to disable write protection"
	fi
fi

echo "$0: passed" >> ${logfile}
return ${EXIT_SUCCESS}
