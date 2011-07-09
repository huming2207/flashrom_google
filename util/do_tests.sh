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
# This is a simple test harness which contains setup and cleanup code common to 
# all tests. The syntax for this script is:
# do_tests.sh --options <test1> <test2> ... <testN>
#
# Arguments which are files are assumed to be tests which are run after common
# setup routines and before common cleanup routines.
#
# This script exports some global variables intended for use by individual unit
# tests. The important ones are:
# EXIT_SUCCESS & EXIT_FAILURE: Exit codes
# DEBUG: Indicates that extra verbosity should be used.
# BACKUP: The backup firmware image obtained during setup.

export EXIT_SUCCESS=0
export EXIT_FAILURE=1
export DEBUG=1

show_help()
{
	echo " \
	Usage: 
	do_test.sh [OPTION] test1.sh test2.sh ... testN.sh

	Environment variables:
	FLASHROM: Path to the Flashrom binary to test
	FLASHROM_PARAM: Parameters to pass in

	OPTIONS
	-h or --help
	    Display this message.

	Arguments with one or two leading dashes are processed as options.
	Arguments which are filenames are processed as test cases. Names of test
	cases may not contain spaces.
	"

	exit $EXIT_SUCCESS
}

msg_dbg() {
	if [ ${DEBUG} -eq 1 ]; then
		echo "$@"
	fi
}

# Parse command-line
OPTIONS=""
TESTS=""
for ARG in $@; do
	if echo "${ARG}" | grep "^-.*$" >/dev/null
	then
		OPTIONS=${OPTIONS}" "${ARG}
	elif [ -f ${ARG} ]; then
		TESTS=${TESTS}" "${ARG}
	fi
done

for ARG in ${OPTIONS}; do
	case ${ARG} in 
		-h|--help) 
			show_help;
			shift;; 
	esac; 
done

#
# Setup Routine
#

# The copy of flashrom to test. If unset, we'll assume the user wants to test
# a newly built flashrom binary in the parent directory (this script should
# reside in flashrom/util).
if [ -z "${FLASHROM}" ] ; then
	FLASHROM="./flashrom"
fi
echo "testing flashrom binary: ${FLASHROM}"

OLDDIR=$(pwd)

# test data location
TMPDIR=$(mktemp -d -t flashrom_test.XXXXXXXXXX)
if [ "$?" != "0" ] ; then
	echo "Could not create temporary directory"
	exit ${EXIT_FAILURE}
fi

which flashrom > /dev/null
#if [ "$?" != "0" ] ; then
if [ ${?} -ne 0 ] ; then
	echo "Please install a stable version of flashrom in your path."
	echo "This will be used to compare the test flashrom binary and "
	echo "restore your firmware image at the end of the test."
	exit ${EXIT_FAILURE}
fi

# Copy the test case files and flashrom to temporary directory.
cp "${FLASHROM}" "${TMPDIR}/"
for TEST in ${TESTS}; do
	cp ${TEST} "${TMPDIR}/"
done

cd "${TMPDIR}"
echo "Running test in ${TMPDIR}"

# Make a backup
echo "Reading firmware image"
BACKUP="backup.bin"
flashrom ${FLASHROM_PARAM} -r "$BACKUP" > /dev/null
if [ $? -ne 0 ]; then
	echo "Failed to create backup image"
	exit ${EXIT_FAILURE}
else
	echo "Original image saved as ${BACKUP}"
fi


# Attempt to write the backup image to ensure the system's flashrom is
# at least somewhat capable of restoring the image.
# FIXME: this requires a modification to flashrom to force blocks to be
# re-written, even if the content is the same.
#flashrom ${FLASHROM_PARAM} --force -r "$BACKUP" > /dev/null
#if [ $? -ne 0 ]; then
#      echo "The installed flashrom binary is not usable for testing"
#      exit ${EXIT_FAILURE}
#fi

export BACKUP=${BACKUP}

#
# Execute test cases
#
rc=${EXIT_SUCCESS}
msg_dbg "Test cases: ${TESTS}"
for TEST in ${TESTS}; do
	msg_dbg "Running test: \"${TEST}\""
	./${TEST}
	if [ ${?} -ne ${EXIT_SUCCESS} ]; then
		rc=${EXIT_FAILURE}
		break
	fi
done

#
# Cleanup Routine
#
if [ ${rc} -ne ${EXIT_SUCCESS} ] ; then
	echo "Result: FAILED"
else
	echo "Result: PASSED"
fi

echo "restoring original image using system's flashrom"
flashrom ${FLASHROM_PARAM} -w "$BACKUP"
echo "test files remain in ${TMPDIR}"
cd "${OLDDIR}"
exit ${rc}
