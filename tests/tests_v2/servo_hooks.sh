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

# Voltage gets exported from test_v2.sh and is mandatory for Servo.
if [ -z "$VOLTAGE" ]; then
	echo "Must specify voltage when using Servo."
	exit $EXIT_FAILURE
fi

# Users who have multiple Servos attached might need to override port.
if [ -z "$SERVO_PORT" ]; then
	SERVO_PORT="9999"
fi

# Servo's SPI1 channel targets the EC, SPI2 targets the host ROM.
if [ -z "$SERVO_SPI" ]; then
	SERVO_SPI="spi2"
fi

preflash_hook()
{
	local rc=0

	dut-control --port=${SERVO_PORT} ${SERVO_SPI}_buf_en:on ${SERVO_SPI}_buf_on_flex_en:on ${SERVO_SPI}_vref:pp${VOLTAGE}
	rc=$?
	sleep 1

	return $rc
}

postflash_hook()
{
	local rc=0

	dut-control --port=${SERVO_PORT} ${SERVO_SPI}_buf_en:off ${SERVO_SPI}_buf_on_flex_en:off
	rc=$?
	sleep 1

	return $rc
}

wp_sanity_check()
{
	local rc=0

	dut-control --port=${SERVO_PORT} fw_wp_en fw_wp_vref fw_wp >/dev/null
	rc=$?
	if [ $rc -ne 0 ]; then
		printf "dut-control failed. Check that servod is running.\n"
	fi

	return $rc
}

wp_enable_hook()
{
	local rc=0

	dut-control --port=${SERVO_PORT} fw_wp_en:on fw_wp_vref:pp${VOLTAGE}
	rc=$?
	sleep 1

	return $rc
}

wp_on_hook()
{
	local rc=0

	dut-control --port=${SERVO_PORT} fw_wp:on
	rc=$?
	sleep 1

	return $rc
}

wp_off_hook()
{
	local rc=0

	dut-control --port=${SERVO_PORT} fw_wp:off
	rc=$?
	sleep 1

	return $rc
}

wp_disable_hook()
{
	local rc=0

	dut-control --port=${SERVO_PORT} fw_wp_en:off fw_wp_vref:off
	rc=$?
	sleep 1

	return $rc
}
