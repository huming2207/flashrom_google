#!/bin/sh

# This script tests the availability of semctl() and similar functions by
# checking that _POSIX_C_SOURCE indicates POSIX.1-2001 (or greaater) compliance.

if [ -z $CC ]; then
	CC=${CROSS_COMPILE}gcc
fi

echo "
#include <stdlib.h>
int main()\
{
#ifdef _POSIX_C_SOURCE
	if ((long)_POSIX_C_SOURCE >= 200112)
	        exit(EXIT_SUCCESS);
#else
#error
#endif
        exit(EXIT_FAILURE);
}
" > .test.c

${CC} -o .test .test.c
rc=$?
echo $rc
exit
