#!/bin/sh
#
# Copyright (C) 2016 Google Inc.
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

# $1: Chip voltage (in millivolts)
wp_enable_hook()
{
	local rc=0

	dut-control fw_wp_en:on fw_wp_vref:pp${1}
	rc=$?

	return $rc
}

wp_on_hook()
{
	local rc=0

	dut-control fw_wp:on
	rc=$?

	return $rc
}

wp_off_hook()
{
	local rc=0

	dut-control fw_wp:off
	rc=$?

	return $rc
}

wp_disable_hook()
{
	local rc=0

	dut-control fw_wp_en:off fw_wp_vref:off
	rc=$?

	return $rc
}
