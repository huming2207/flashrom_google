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
# ec.sh: This will attempt to stress partial write capabilities for EC firmware
# ROMs. There are two major parts to the test:
# 1. Write an alternative firmware image to the "fw" region specified in a
#    layout file.
# 2. Stress partial writes using a known pattern in an unused region of the
#    EC firmware ROM with 128KB of space.
#
# REQUIRED ENVIRONMENT VARIABLES
# ------------------------------
# Due to the way the test framework copies tests to a temporary location, the
# environment variables must specify absolute paths to files they reference.
#
# $ALT_EC_IMAGE -- Required environment variable
# This script requires an alternative EC firmware. Most ECs actively read code
# and data from the firmware ROM during run-time. An example of an EC firmware
# update process for supported ECs:
# 1. Enter update mode -- Copy relevant code and data to internal RAM
# 2. Enable programming interface
# 3. When host is done programming ROM, issue "exit update mode"
# 4. Re-load code/data from ROM.
#
# Step 4 can cause failure if we attempt to test using patterns. Instead, we
# must provide the EC with usable code. For blackbox testing, this essentially
# means replacing the current firmware image with a different image which still
# works.
#
# $LAYOUT_FILE -- Another required environment variable
# A layout file is required for two reasons:
# 1. We need a way of knowing which regions are safe to leave in a clobbered
#    state after exiting flash update mode. They will be labeled "unused".
# 2. We need a way of knowing where the EC firmware resides since some ECs
#    boot from bottom of ROM and others from top of ROM. This will be the
#    region labeled "fw"
#
# The following example is for a 1MB ROM with 128KB occupied by EC firmware.
# The EC in this example loads firmware from the lowest address.
# 0x000000 0x01ffff fw
# 0x020000 0x0fffff unused

. "$(pwd)/common.sh"

LOGFILE="${0}.log"
ZERO_4K="00_4k.bin"
FF_4K="ff_4k.bin"
FF_4K_TEXT="ff_4k.txt"
NUM_REGIONS=16

TESTFILE="test.bin"

partial_writes_ec_fail()
{
	echo "$1" >> ${LOGFILE}
	echo "$0: failed" >> ${LOGFILE}
	exit ${EXIT_FAILURE}
}

which uuencode > /dev/null
if [ "$?" != "0" ] ; then
	partial_writes_ec_fail "uuencode is required to use this script"
fi

# FIXME: This is a chromium os -ism. Most distros don't strip out "diff".
which diff > /dev/null
if [ "$?" != "0" ] ; then
	partial_writes_ec_fail "diff is required to use this script"
fi

# FIXME: extra chromium os paranoia
which printf > /dev/null
if [ "$?" != "0" ] ; then
	partial_writes_ec_fail "printf is required to use this script"
fi

echo "User-provided \$ALT_EC_IMAGE: ${ALT_EC_IMAGE}" >> ${LOGFILE}
if [ -z "$ALT_EC_IMAGE" ] || [ ! -e "$ALT_EC_IMAGE" ]; then
	partial_writes_ec_fail "Please provide absolute path to alternate EC firmware image using the ALT_EC_IMAGE environment variable."
fi

echo "User-provided \$LAYOUT_FILE: ${LAYOUT_FILE}" >> ${LOGFILE}
if [ -z "$LAYOUT_FILE" ] || [ ! -e "$LAYOUT_FILE" ]; then
	partial_writes_ec_fail "Please provide absolute path to layout file using the LAYOUT_FILE environment variable"
fi

#
# Part 1: Write an alternate firmware image to the "fw" region in EC flash
#

do_test_flashrom -l ${LAYOUT_FILE} -i fw -w "${ALT_EC_IMAGE}"
if [ $? -ne 0 ]; then
	partial_writes_ec_fail "Failed to write alternate EC firmware" >> ${LOGFILE}
else
	echo "Wrote alternate EC firmware image successfully" >> ${LOGFILE}
fi

# Restore original firmware region
do_test_flashrom -l ${LAYOUT_FILE} -i fw -w "${BACKUP}"
if [ $? -ne 0 ]; then
	partial_writes_ec_fail "Failed to restore original EC firmware" >> ${LOGFILE}
else
	echo "Restored original firmware image successfully" >> ${LOGFILE}
fi

#
# Part 2: Write a pattern to an "unused" region in EC flash
#

ranges=$(awk '{ if ( $2 == "unused" ) print $1 }' "${LAYOUT_FILE}")
range_found=0
for range in $ranges; do
	start=$(echo $range | awk -F":" '{ print $1 }')
	end=$(echo $range | awk -F":" '{ print $2 }')
	len=$((${end} - ${start}))

	echo "Testing if range is usable: ${start}:${end}, len=${len}" >> ${LOGFILE}
	if [ ${len} -lt $(($((${NUM_REGIONS} - 1)) * 4096)) ]; then
		continue
	else
		range_found=1
		break
	fi
done

if [ $range_found -ne 1 ]; then
	partial_writes_ec_fail "No suitable unused range found"
else
	echo "Found usable range: ${start}:${end}, len=${len}"
fi

# Make 4k worth of 0xff bytes
echo "begin 640 $FF_4K" > "$FF_4K_TEXT"
i=0
while [ $i -le 90 ]; do
	echo "M____________________________________________________________" >> "$FF_4K_TEXT"
	i=$((${i} + 1))
done
echo "!_P``" >> "$FF_4K_TEXT"
echo "\`" >> "$FF_4K_TEXT"
echo "end" >> "$FF_4K_TEXT"
uudecode -o "$FF_4K" "$FF_4K_TEXT"
rm -f "$FF_4K_TEXT"

# Make 4k worth of 0x00 bytes
dd if=/dev/zero of="$ZERO_4K" bs=1 count=4096 2> /dev/null
echo "ffh pattern written in ${FF_4K}"
echo "00h pattern written in ${ZERO_4K}"

# Actual tests are performed below.
#

# Make a layout - 4K regions on 4K boundaries. This will test basic
# functionality of erasing and writing specific blocks.
for i in `seq 0 $((${NUM_REGIONS} - 1))` ; do
	offset_00=$((${i} * 8192))
	offset_ff=$((${i} * 8192 + 4096))
	echo "\
`printf 0x%x $((${start} + ${offset_00}))`:`printf 0x%x $((${start} + ${offset_00} + 0xfff))` 00_${i}
`printf 0x%x $((${start} + ${offset_ff}))`:`printf 0x%x $((${start} + ${offset_ff} + 0xfff))` ff_${i}
	" >> layout_ec_4k_aligned.txt
done

cp "${BACKUP}" "$TESTFILE"
i=0
while [ $i -lt $NUM_REGIONS ] ; do
	tmpstr="aligned region ${i} test: "
	offset=$((${start} + $((${i} * 8192))))
	dd if=${ZERO_4K} of=${TESTFILE} bs=1 conv=notrunc seek=${offset} 2> /dev/null
	dd if=${FF_4K} of=${TESTFILE} bs=1 conv=notrunc seek=$((${offset} + 4096)) 2> /dev/null

	do_test_flashrom -l layout_ec_4k_aligned.txt -i 00_${i} -i ff_${i} -w "$TESTFILE"
	if [ $? -ne 0 ] ; then
		partial_writes_ec_fail "${tmpstr}failed to flash"
	fi

	# download the entire ROM image and use diff to compare to ensure
	# flashrom logic does not violate user-specified regions
	flashrom ${FLASHROM_PARAM} -r difftest.bin 2> /dev/null
	diff -q difftest.bin "$TESTFILE"
	if [ "$?" != "0" ] ; then
		partial_writes_ec_fail "${tmpstr}failed diff test"
	fi
	rm -f difftest.bin

	i=$((${i} + 1))
	echo "${tmpstr}passed" >> ${LOGFILE}
done

# Make a layout - 4K regions on 4.5K boundaries. This will help find problems
# with logic that only operates on part of a block. For example, if a user
# wishes to re-write a fraction of a block, then:
# 1. The whole block must be erased.
# 2. The old content must be restored at unspecified offsets.
# 3. The new content must be written at specified offsets.
#
# Note: The last chunk of 0xff bytes is too long, so special logic was added to
# below to avoid overrunning a 128KB test region.
#
for i in `seq 0 $((${NUM_REGIONS} - 1))` ; do
	offset_00=$((${i} * 8192 + 2048))
	offset_ff=$((${i} * 8192 + 4096 + 2048))
	echo "\
`printf 0x%06x $((${start} + ${offset_00}))`:`printf 0x%06x $((${start} + ${offset_00} + 0xfff))` 00_${i}
`printf 0x%06x $((${start} + ${offset_ff}))`:`printf 0x%06x $((${start} + ${offset_ff} + 0xfff))` ff_${i}
	" >> layout_ec_unaligned.txt
done

# reset the test file and ROM to the original state
flashrom ${FLASHROM_PARAM} -w "${BACKUP}" > /dev/null
cp "$BACKUP" "$TESTFILE"

i=0
while [ $i -lt $NUM_REGIONS ] ; do
	tmpstr="unaligned region ${i} test: "
	offset=$(($((${i} * 8192)) + 2048))

	# Protect against too long write
	writelen=4096
	if [ $((${offset} + 4096 + 4096)) -ge 131072 ]; then
		writelen=$((131072 - $((${offset} + 4096))))
		if [ ${writelen} -lt 0 ]; then
			writelen=0
		fi
	fi

	dd if=${ZERO_4K} of=${TESTFILE} bs=1 conv=notrunc seek=$((${start} + ${offset})) 2> /dev/null
	dd if=${FF_4K} of=${TESTFILE} bs=1 conv=notrunc seek=$((${start} + ${offset} + 4096)) count=writelen 2> /dev/null

	do_test_flashrom -l layout_ec_unaligned.txt -i 00_${i} -i ff_${i} -w "$TESTFILE"
	if [ $? -ne 0 ] ; then
		partial_writes_ec_fail "${tmpstr}failed to flash region"
	fi

	# download the entire ROM image and use diff to compare to ensure
	# flashrom logic does not violate user-specified regions
	flashrom ${FLASHROM_PARAM} -r difftest.bin 2> /dev/null
	diff -q difftest.bin "$TESTFILE"
	if [ "$?" != "0" ] ; then
		partial_writes_ec_fail "${tmpstr}failed diff test"
	fi
	rm -f difftest.bin

	i=$((${i} + 1))
	echo "${tmpstr}passed" >> ${LOGFILE}
done

return "$EXIT_SUCCESS"
