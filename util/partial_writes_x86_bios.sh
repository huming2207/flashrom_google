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
# This script attempts to test Flashrom partial write capability by writing
# patterns of 0xff and 0x00 bytes to the lowest 128KB of flash. 128KB is chosen
# since 64KB is usually the largest possible block size, so we will try to
# cover at least two blocks with this test.
#
# TODO: We need to make sure to force re-writes when desired since flashrom
# may skip regions which do not need to be re-written.
#

LOGFILE="${0}.log"
ZERO_4K="00_4k.bin"
FF_4K="ff_4k.bin"
FF_4K_TEXT="ff_4k.txt"

TESTFILE="test.bin"

partial_writes_fail()
{
	echo "$1" >> ${LOGFILE}
	echo "$0: failed" >> ${LOGFILE}
	exit ${EXIT_FAILURE}
}

# FIXME: this is a chromium-os -ism. most distros don't strip out "diff"...
which diff > /dev/null
if [ "$?" != "0" ] ; then
	partial_writes_fail "diff is required to use this script"
fi

which uuencode > /dev/null
if [ "$?" != "0" ] ; then
	partial_writes_fail "uuencode is required to use this script"
fi

# Make 4k worth of 0xff bytes
echo "begin 640 $FF_4K" > "$FF_4K_TEXT"
i=0
while [ $i -le 90 ] ; do
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

#
# Actual tests are performed below.
#
NUM_REGIONS=16

# Make a layout - 4K regions on 4K boundaries. This will test basic
# functionality of erasing and writing specific blocks.
offset=0
for i in `seq 0 $((${NUM_REGIONS} - 1))` ; do
	offset_00=$((${i} * 8192))
	offset_ff=$((${i} * 8192 + 4096))
	echo "\
`printf 0x%x $((${start} + ${offset_00}))`:`printf 0x%x $((${start} + ${offset_00} + 0xfff))` 00_${i}
`printf 0x%x $((${start} + ${offset_ff}))`:`printf 0x%x $((${start} + ${offset_ff} + 0xfff))` ff_${i}
	" >> layout_bios_4k_aligned.txt
done

cp "${BACKUP}" "$TESTFILE"
i=0
while [ $i -lt $NUM_REGIONS ] ; do
	tmpstr="aligned region ${i} test: "
	offset=$((${i} * 8192))
	dd if=${ZERO_4K} of=${TESTFILE} bs=1 conv=notrunc seek=${offset} 2> /dev/null
	dd if=${FF_4K} of=${TESTFILE} bs=1 conv=notrunc seek=$((${offset} + 4096)) 2> /dev/null

	./flashrom ${FLASHROM_PARAM} -l layout_bios_4k_aligned.txt -i 00_${i} -i ff_${i} -w "$TESTFILE" 2> /dev/null
	if [ "$?" != "0" ] ; then
		partial_writes_fail "${tmpstr}failed to flash region"
	fi

	# download the entire ROM image and use diff to compare to ensure
	# flashrom logic does not violate user-specified regions
	flashrom ${FLASHROM_PARAM} -r difftest.bin 2> /dev/null
	diff -q difftest.bin "$TESTFILE"
	if [ "$?" != "0" ] ; then
		partial_writes_fail "${tmpstr}failed diff test"
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
# Note: The last chunk of 0xff bytes is only 2K as to avoid overrunning a 128KB
# test image.
#
for i in `seq 0 $((${NUM_REGIONS} - 1))` ; do
	offset_00=$((${i} * 8192 + 2048))
	offset_ff=$((${i} * 8192 + 4096 + 2048))
	echo "\
`printf 0x%06x $((${start} + ${offset_00}))`:`printf 0x%06x $((${start} + ${offset_00} + 0xfff))` 00_${i}
`printf 0x%06x $((${start} + ${offset_ff}))`:`printf 0x%06x $((${start} + ${offset_ff} + 0xfff))` ff_${i}
	" >> layout_bios_unaligned.txt
done

# reset the test file and ROM to the original state
flashrom ${FLASHROM_PARAM} -w "${BACKUP}" > /dev/null
cp "$BACKUP" "$TESTFILE"

i=0
while [ $i -lt $NUM_REGIONS ] ; do
	tmpstr="aligned region ${i} test: "
	offset=$(($((${i} * 8192)) + 2048))
	# Protect against too long write
	writelen=4096
	if [ $((${offset} + 4096 + 4096)) -ge 131072 ]; then
		writelen=$((131072 - $((${offset} + 4096))))
		if [ ${writelen} -lt 0 ]; then
			writelen=0
		fi
	fi
	dd if=${ZERO_4K} of=${TESTFILE} bs=1 conv=notrunc seek=${offset} 2> /dev/null
	dd if=${FF_4K} of=${TESTFILE} bs=1 conv=notrunc seek=$((${offset} + 4096)) count=writelen 2> /dev/null

	./flashrom ${FLASHROM_PARAM} -l layout_bios_unaligned.txt -i 00_${i} -i ff_${i} -w "$TESTFILE" 2> /dev/null
	if [ "$?" != "0" ] ; then
		partial_writes_fail "${tmpstr} failed to flash region"
	fi

	# download the entire ROM image and use diff to compare to ensure
	# flashrom logic does not violate user-specified regions
	flashrom ${FLASHROM_PARAM} -r difftest.bin 2> /dev/null
	diff -q difftest.bin "$TESTFILE"
	if [ "$?" != "0" ] ; then
		partial_writes_fail "${tmpstr} failed diff test"
	fi
	rm -f difftest.bin

	i=$((${i} + 1))
	echo "${tmpstr}passed" >> ${LOGFILE}
done

return "$EXIT_SUCCESS"