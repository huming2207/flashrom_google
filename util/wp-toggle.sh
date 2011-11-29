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

LOGFILE="${0}.log"
WP_DISABLED=0
WP_ENABLED=1
MAGIC="write protect is"

wp_toggle_fail()
{
	echo "$1" >> ${LOGFILE}
	echo "$0: failed" >> ${LOGFILE}
	exit ${EXIT_FAILURE}
}

wp_status()
{
	if [ "$(printf %s\\n "$1" | sed 's/.*enabled.*/enabled/')" = "enabled" ]; then
		return ${WP_ENABLED} 
	elif [ "$(printf %s\\n "$1" | sed 's/.*disabled.*/disabled/')" = "disabled" ]; then
		return ${WP_DISABLED}
	else
		wp_toggle_fail "unknown write protect status: \"$1\""
	fi
}

# Back-up old settings
tmp=$(./flashrom ${FLASHROM_PARAM} --wp-status 2>/dev/null | grep "$MAGIC")
wp_status "$tmp"
old_status=$?
echo "old write protect status: ${old_status}" >> ${LOGFILE}

# invert the old setting
if [ ${old_status} -eq ${WP_ENABLED} ]; then
	do_test_flashrom --wp-disable
	tmp=$(./flashrom ${FLASHROM_PARAM} --wp-status 2>/dev/null | grep "$MAGIC")
	wp_status "$tmp"
	if [ $? = ${WP_ENABLED} ]; then
		wp_toggle_fail "failed to disable write protection"
	fi
elif [ ${old_status} -eq ${WP_DISABLED} ]; then
	do_test_flashrom --wp-enable
	tmp=$(./flashrom ${FLASHROM_PARAM} --wp-status 2>/dev/null | grep "$MAGIC")
	wp_status "$tmp"
	if [ $? = ${WP_DISABLED} ]; then
		wp_toggle_fail "failed to enable write protection"
	fi
fi

# restore old setting
if [ ${old_status} -eq ${WP_ENABLED} ]; then
	do_test_flashrom --wp-enable
	tmp=$(./flashrom ${FLASHROM_PARAM} --wp-status 2>/dev/null | grep "$MAGIC")
	wp_status "$tmp"
	if [ $? != ${WP_ENABLED} ]; then
		wp_toggle_fail "failed to enable write protection"
	fi
elif [ ${old_status} -eq ${WP_DISABLED} ]; then
	do_test_flashrom --wp-disable
	wp_status "$tmp"
	if [ $? != ${WP_DISABLED} ]; then
		wp_toggle_fail "failed to disable write protection"
	fi
fi

echo "$0: passed" >> ${LOGFILE}
return ${EXIT_SUCCESS}
