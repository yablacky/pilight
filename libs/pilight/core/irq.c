/*
	Copyright (C) 2013 - 2014 CurlyMo

	This file is part of pilight.

	pilight is free software: you can redistribute it and/or modify it under the
	terms of the GNU General Public License as published by the Free Software
	Foundation, either version 3 of the License, or (at your option) any later
	version.

	pilight is distributed in the hope that it will be useful, but WITHOUT ANY
	WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
	A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with pilight. If not, see	<http://www.gnu.org/licenses/>
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "irq.h"
#include "common.h"
#ifndef _WIN32
	#include "gc.h"
	#include "log.h"
	#include "mem.h"
	#include "../../wiringx/wiringX.h"
#endif

typedef struct timestamp_t {
	struct timeval first;
	struct timeval second;
} timestamp_t;

static timestamp_t timestamp;

/* Attaches an interrupt handler to a specific GPIO pin
   Whenever an rising, falling or changing interrupt occurs
   the function given as the last argument will be called.
 * @return int Microseconds since last interrupt. 0 if no interrupt within 1 second, -1 on error.
 */
int irq_read(int gpio) {
#ifndef _WIN32
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	int x = waitForInterrupt(gpio, 1000);
	if(x > 0) {
		timestamp.first = timestamp.second;
		gettimeofday(&timestamp.second, NULL);
		if (!can_timeval_diff(timestamp.first, timestamp.second))
			return 0;
		return (int) get_timeval_diff_usec(timestamp.first, timestamp.second);
	}
	return x;
#else
	return -1;
#endif
}
