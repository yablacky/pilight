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
	struct timeval check;
} timestamp_t;

static timestamp_t timestamp[64];
static int last_state[64];
static size_t ok_state[64] =  { 0 };

static int irq_read_ex(int gpio);
static int use_read_ex = 0;
int irq_read_ex_enable(int enable)
{
	int old = use_read_ex;
	use_read_ex = enable;
	return old;
}

/* Attaches an interrupt handler to a specific GPIO pin
   Whenever an rising, falling or changing interrupt occurs
   the function given as the last argument will be called.
 * @return int Microseconds since last interrupt. 0 if no interrupt within 1 second, -1 on error.
 */
int irq_read(int gpio) {
	if (use_read_ex) {
		return irq_read_ex(gpio);
	}
#ifndef _WIN32
	logprintf(LOG_STACK, "%s(%d)", __FUNCTION__, gpio);

	if (gpio < 0 || gpio >= countof(timestamp))
		return -1;
	int x = waitForInterrupt(gpio, 1000);
	if(x > 0) {
		timestamp_t *ts = timestamp + gpio;

		ts->first = ts->second;
		gettimeofday(&ts->second, NULL);

		if (!can_timeval_diff(ts->first, ts->second)) {
			return 0;
		}
		return (int) get_timeval_diff_usec(ts->first, ts->second);

	}
	return x;
#else
	return -1;
#endif
}

/*
 * Like irq_read() with following extensions:
 * (1) checks the value of the signal that causes the interrupt.
 *     Under normal circumstances the signal should alternate between high and low on each call.
 *     If a signal value is not as expected, a log-message is generated with detail information
 *     about the mismatch.
 * (2) returns value of that signal in bit (1<<30) of the return value.
 * @param int gpio Pin number to check for interrupt and signal value.
 * @return int Microseconds since last interrupt with signal state in bit (1<<30). 0 if no interrupt within 1 second, -1 on error.
 */
static int irq_read_ex(int gpio) {
#ifndef _WIN32
	logprintf(LOG_STACK, "%s(%d)", __FUNCTION__, gpio);

	if (gpio < 0 || gpio >= countof(timestamp))
		return -1;
	timestamp_t *ts = timestamp + gpio;

	gettimeofday(&ts->check, NULL);
	int x = waitForInterrupt(gpio, 1000);
	if(x > 0) {
		int now_state = (digitalRead(gpio) & 1) | 2;

		ts->first = ts->second;
		gettimeofday(&ts->second, NULL);

		if (!can_timeval_diff(ts->first, ts->second)) {
			last_state[gpio] = now_state;
			return 0;
		}
		x = (int) get_timeval_diff_usec(ts->first, ts->second);

		if (last_state[gpio] != now_state) {
			last_state[gpio] = now_state;
			++ok_state[gpio];
		} else {
			int expected = last_state[gpio] ^ 1;
			int y = (int) get_timeval_diff_usec(ts->check, ts->second);

			if (x-y < 50) {	// allow digitalRead() being wrong within 50 us.
				now_state = expected;
				last_state[gpio] = now_state;
				++ok_state[gpio];
			}
			else {
				logprintf(LOG_ERR, "%s(%d): DigR:"
					" after %3u OK: exp^%c, got^%c, blind=%d us, wait=%d us, dura=%d us",
					__FUNCTION__, gpio, ok_state[gpio],
					(expected&1)?'H':'L', (now_state&1)?'H':'L',
					x-y, y, x);
				ok_state[gpio] = 0;
			}
		}
		if (now_state & 1)
			x |= (1 << 30);
	}
	return x;
#else
	return -1;
#endif
}
