/*
 * This file is part of the flashrom project.
 *
 * Copyright (C) 2000 Silicon Integrated System Corporation
 * Copyright (C) 2009,2010 Carl-Daniel Hailfinger
 * Copyright (C) 2011 Google Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#ifndef __LIBPAYLOAD__

#include <unistd.h>
#include <sys/time.h>
#include <time.h>
#include <stdlib.h>
#include <limits.h>
#include <errno.h>
#include "flash.h"

/* Are OS timers broken? */
int broken_timer = 0;

/* loops per microsecond */
static unsigned long micro = 1;

__attribute__ ((noinline)) void myusec_delay(int usecs)
{
	unsigned long i;
	for (i = 0; i < usecs * micro; i++) {
		/* Make sure the compiler doesn't optimize the loop away. */
		asm volatile ("" : : "rm" (i) );
	}
}

static unsigned long measure_os_delay_resolution(void)
{
	unsigned long timeusec;
	struct timeval start, end;
	unsigned long counter = 0;
	
	gettimeofday(&start, NULL);
	timeusec = 0;
	
	while (!timeusec && (++counter < 1000000000)) {
		gettimeofday(&end, NULL);
		timeusec = 1000000 * (end.tv_sec - start.tv_sec) +
			   (end.tv_usec - start.tv_usec);
		/* Protect against time going forward too much. */
		if ((end.tv_sec > start.tv_sec) &&
		    ((end.tv_sec - start.tv_sec) >= LONG_MAX / 1000000 - 1))
			timeusec = 0;
		/* Protect against time going backwards during leap seconds. */
		if ((end.tv_sec < start.tv_sec) || (timeusec > LONG_MAX))
			timeusec = 0;
	}
	return timeusec;
}

static unsigned long measure_delay(int usecs)
{
	unsigned long timeusec;
	struct timeval start, end;
	
	gettimeofday(&start, NULL);
	myusec_delay(usecs);
	gettimeofday(&end, NULL);
	timeusec = 1000000 * (end.tv_sec - start.tv_sec) +
		   (end.tv_usec - start.tv_usec);
	/* Protect against time going forward too much. */
	if ((end.tv_sec > start.tv_sec) &&
	    ((end.tv_sec - start.tv_sec) >= LONG_MAX / 1000000 - 1))
		timeusec = LONG_MAX;
	/* Protect against time going backwards during leap seconds. */
	if ((end.tv_sec < start.tv_sec) || (timeusec > LONG_MAX))
		timeusec = 1;

	return timeusec;
}

void myusec_calibrate_delay(void)
{
	unsigned long count = 1000;
	unsigned long timeusec, resolution;
	int i, tries = 0;

	if (!broken_timer)
		return;

	msg_pdbg("Calibrating delay loop... ");
	resolution = measure_os_delay_resolution();
	if (resolution) {
		msg_pdbg("OS timer resolution is %lu usecs, ", resolution);
	} else {
		msg_pinfo("OS timer resolution is unusable. ");
	}

recalibrate:
	count = 1000;
	while (1) {
		timeusec = measure_delay(count);
		if (timeusec > 1000000 / 4)
			break;
		if (count >= ULONG_MAX / 2) {
			msg_pinfo("timer loop overflow, reduced precision. ");
			break;
		}
		count *= 2;
	}
	tries ++;

	/* Avoid division by zero, but in that case the loop is shot anyway. */
	if (!timeusec)
		timeusec = 1;
	
	/* Compute rounded up number of loops per microsecond. */
	micro = (count * micro) / timeusec + 1;
	msg_pdbg("%luM loops per second, ", micro);

	/* Did we try to recalibrate less than 5 times? */
	if (tries < 5) {
		/* Recheck our timing to make sure we weren't just hitting
		 * a scheduler delay or something similar.
		 */
		for (i = 0; i < 4; i++) {
			if (resolution && (resolution < 10)) {
				timeusec = measure_delay(100);
			} else if (resolution && 
				   (resolution < ULONG_MAX / 200)) {
				timeusec = measure_delay(resolution * 10) *
					   100 / (resolution * 10);
			} else {
				/* This workaround should be active for broken
				 * OS and maybe libpayload. The criterion
				 * here is horrible or non-measurable OS timer
				 * resolution which will result in
				 * measure_delay(100)=0 whereas a longer delay
				 * (1000 ms) may be sufficient
				 * to get a nonzero time measurement.
				 */
				timeusec = measure_delay(1000000) / 10000;
			}
			if (timeusec < 90) {
				msg_pdbg("delay more than 10%% too short (got "
					 "%lu%% of expected delay), "
					 "recalculating... ", timeusec);
				goto recalibrate;
			}
		}
	} else {
		msg_perr("delay loop is unreliable, trying to continue ");
	}

	/* We're interested in the actual precision. */
	timeusec = measure_delay(10);
	msg_pdbg("10 myus = %ld us, ", timeusec);
	timeusec = measure_delay(100);
	msg_pdbg("100 myus = %ld us, ", timeusec);
	timeusec = measure_delay(1000);
	msg_pdbg("1000 myus = %ld us, ", timeusec);
	timeusec = measure_delay(10000);
	msg_pdbg("10000 myus = %ld us, ", timeusec);
	timeusec = measure_delay(resolution * 4);
	msg_pdbg("%ld myus = %ld us, ", resolution * 4, timeusec);

	msg_pdbg("OK.\n");
}

void internal_delay(int usecs)
{
	int ret, done_waiting = 0;
	unsigned long long nsecs;
	struct timespec req = { 0, 0 };

	if (broken_timer) {
		myusec_delay(usecs);
		return;
	}

	/* flashrom delays work with a microsecond granularity. However
	 * usleep has been obsoleted in POSIX.1-2001 and removed from
	 * POSIX.1-2008 with the suggestion to use nanosleep(2) instead.
	 */
	nsecs = 1000ULL * usecs;
	req.tv_sec = nsecs / 1000000000ULL;
	req.tv_nsec = nsecs % 1000000000ULL;

	while (!done_waiting) {
		struct timespec rem;
		ret = nanosleep(&req, &rem);
		if (ret && (errno == EINTR)) {
			req = rem;
			continue;
		}
		done_waiting = 1;
	}

	/* If nanosleep reports problems with copying information from user
	 * space we fall back to the "broken timer" code.
	 */
	if (ret && (errno == EFAULT)) {
		broken_timer = 1;
		/* Since we use delays quite early (i.e. during probing)
		 * we can recalibrate our delay loop interjacently without
		 * risking data integrity. This will only happen once.
		 */
		myusec_calibrate_delay();
		/* Now, for the sake of it, delay. */
		myusec_delay(usecs);
	}
}

#else 
#include <libpayload.h>

void myusec_calibrate_delay(void)
{
	get_cpu_speed();
}

void internal_delay(int usecs)
{
	udelay(usecs);
}
#endif
