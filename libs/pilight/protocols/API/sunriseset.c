/*
	Copyright (C) 2014 CurlyMo

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
#include <dirent.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#ifndef __USE_XOPEN
	#define __USE_XOPEN
#endif
#include <time.h>
#include <math.h>

#include "../../core/threads.h"
#include "../../core/pilight.h"
#include "../../core/common.h"
#include "../../core/ntp.h"
#include "../../core/dso.h"
#include "../../core/datetime.h" // Full path because we also have a datetime protocol
#include "../../core/log.h"
#include "../protocol.h"
#include "../../core/json.h"
#include "../../core/gc.h"
#include "sunriseset.h"

#define PI 3.1415926
#define PIX 57.29578049044297 // 180 / PI
#define ZENITH 90.83333333333333

static unsigned short loop = 1;
static unsigned short threads = 0;

static double calculate(int year, int month, int day, double lon, double lat, int rising, int tz) {
	int N = (int)((floor(275 * month / 9)) - ((floor((month + 9) / 12)) *
			((1 + floor((year - 4 * floor(year / 4) + 2) / 3)))) + (int)day - 30);

	double lngHour = lon / 15.0;
	double T = 0;

	if(rising) {
		T = N + ((6 - lngHour) / 24);
	} else {
		T = N + ((18 - lngHour) / 24);
	}

	double M = (0.9856 * T) - 3.289;
	double M1 = M * PI / 180;
	double L = fmod((M + (1.916 * sin(M1)) + (0.020 * sin(2 * M1)) + 282.634), 360.0);
	double L1 = L * PI / 180;
	double L2 = lat * PI / 180;
	double SD = 0.39782 * sin(L1);
	double CH = (cos(ZENITH * PI / 180)-(SD * sin(L2))) / (cos((PIX * asin((SD))) * PI / 180) * cos(L2));

	if(CH > 1) {
		return -1;
	} else if(CH < -1) {
		return -1;
	}

	double RA = fmod((PIX * atan((0.91764 * tan(L1)))), 360);
	double MQ = (RA + (((floor(L / 90)) * 90) - ((floor(RA / 90)) * 90))) / 15;
	double A;
	double B;
	if(rising == 0) {
		A = 0.06571;
		B = 6.595;
	} else {
		A = 0.06571;
		B = 6.618;
	}

	double t = ((rising ? 360 - PIX * acos(CH) : PIX * acos(CH)) / 15) + MQ - (A * T) - B;
	double UT = fmod((t - lngHour) + 24.0, 24.0);
	double min = (round(60*fmod(UT, 1))/100);

	if(min >= 0.60) {
		min -= 0.60;
	}

	double hour = UT-min;

	return ((round(hour)+min)+tz)*100;
}

static void *thread(void *param) {
	struct protocol_threads_t *thread = (struct protocol_threads_t *)param;
	struct JsonNode *json = (struct JsonNode *)thread->param;
	const struct JsonNode *jid = NULL;
	const struct JsonNode *jchild = NULL;
	const struct JsonNode *jchild1 = NULL;
	double longitude = 0, latitude = 0;
	char UTC[] = "UTC", *tz = NULL;

	time_t timenow = 0;
	struct tm tm;
	int nrloops = 0, risetime = 0, settime = 0, hournow = 0, newdst = 0;
	int firstrun = 0, target_offset = 0, x = 0, dst = 0, dstchange = 0;
	int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;

	threads++;

	if((jid = json_find_member(json, "id"))) {
		jchild = json_first_child(jid);
		while(jchild) {
			jchild1 = json_first_child(jchild);
			while(jchild1) {
				if(strcmp(jchild1->key, "longitude") == 0) {
					longitude = jchild1->number_;
				}
				if(strcmp(jchild1->key, "latitude") == 0) {
					latitude = jchild1->number_;
				}
				jchild1 = jchild1->next;
			}
			jchild = jchild->next;
		}
	}

	if((tz = coord2tz(longitude, latitude)) == NULL) {
		logprintf(LOG_DEBUG, "could not determine timezone");
		tz = UTC;
	} else {
		logprintf(LOG_DEBUG, "%.6f:%.6f seems to be in timezone: %s", longitude, latitude, tz);
	}

	timenow = time(NULL);
	timenow -= getntpdiff();
	dst = isdst(timenow, tz);
	if(isntpsynced() == 0) {
		x = 1;
	}

	/* Check how many hours we differ from UTC? */
	target_offset = tzoffset(UTC, tz);

	while(loop) {
		protocol_thread_wait(thread, 1, &nrloops);
		timenow = time(NULL);
		timenow -= getntpdiff();

			/* Get UTC time */
#ifdef _WIN32
		struct tm *tm1;
		if((tm1 = gmtime(&timenow)) != NULL) {
			memcpy(&tm, tm1, sizeof(struct tm));
#else
		if(gmtime_r(&timenow, &tm) != NULL) {
#endif
			year = tm.tm_year+1900;
			month = tm.tm_mon+1;
			day = tm.tm_mday;
			/* Add our hour difference to the UTC time */
			tm.tm_hour += target_offset;
			/* Add possible daylist savings time hour */
			tm.tm_hour += dst;
			hour = tm.tm_hour;
			minute = tm.tm_min;
			second = tm.tm_sec;

			datefix(&year, &month, &day, &hour, &minute, &second);

			if((minute == 0 && second == 1) || (isntpsynced() == 0 && x == 0)) {
				x = 1;
				if((newdst = isdst(timenow, tz)) != dst) {
					dstchange = 1;
				} else {
					dstchange = 0;
				}
				dst = newdst;
			}

			hournow = (hour*100)+minute;
			if(((hournow == 0 || hournow == risetime || hournow == settime) && second == 0)
				 || (settime == 0 && risetime == 0) || dstchange == 1) {

				if(settime == 0 && risetime == 0) {
					firstrun = 1;
				}

				sunriseset->message = json_mkobject();
				JsonNode *code = json_mkobject();
				risetime = (int)calculate(year, month, day, longitude, latitude, 1, target_offset);
				settime = (int)calculate(year, month, day, longitude, latitude, 0, target_offset);

				if(dst == 1) {
					risetime += 100;
					settime += 100;
					if(risetime > 2400) {
						risetime -= 2400;
					}
					if(settime > 2400) {
						settime -= 2400;
					}
				}

				json_append_member(code, "longitude", json_mknumber(longitude, 6));
				json_append_member(code, "latitude", json_mknumber(latitude, 6));

				/* Only communicate the sun state change when they actually occur,
					 and only communicate the new times when the day changes */
				if(hournow != 0 || firstrun == 1) {
					if(hournow >= risetime && hournow < settime) {
						json_append_member(code, "sun", json_mkstring("rise"));
					} else {
						json_append_member(code, "sun", json_mkstring("set"));
					}
				}
				if(hournow == 0 || firstrun == 1) {
					json_append_member(code, "sunrise", json_mknumber(((double)risetime/100), 2));
					json_append_member(code, "sunset", json_mknumber(((double)settime/100), 2));
				}

				json_append_member(sunriseset->message, "message", code);
				json_append_member(sunriseset->message, "origin", json_mkstring("receiver"));
				json_append_member(sunriseset->message, "protocol", json_mkstring(sunriseset->id));

				if(pilight.broadcast != NULL) {
					pilight.broadcast(sunriseset->id, sunriseset->message, PROTOCOL);
				}
				json_delete(sunriseset->message);
				sunriseset->message = NULL;
				firstrun = 0;
			}
		}
	}

	threads--;
	return (void *)NULL;
}

static struct threadqueue_t *initDev(const JsonNode *jdevice) {
	loop = 1;
	char *output = json_stringify(jdevice, NULL);
	JsonNode *json = json_decode(output);
	json_free(output);

	struct protocol_threads_t *node = protocol_thread_init(sunriseset, json);
	return threads_register("sunriseset", &thread, (void *)node, 0);
}

static void threadGC(void) {
	loop = 0;
	protocol_thread_stop(sunriseset);
	while(threads > 0) {
		usleep(10);
	}
	protocol_thread_free(sunriseset);
}

static int checkValues(const JsonNode *code) {
	const char *sun = NULL;

	if(json_find_string(code, "sun", &sun) == 0) {
		if(strcmp(sun, "rise") != 0 && strcmp(sun, "set") != 0) {
			return 1;
		}
	}
	return 0;
}

#if !defined(MODULE) && !defined(_WIN32)
__attribute__((weak))
#endif
void sunRiseSetInit(void) {

	protocol_register(&sunriseset);
	protocol_set_id(sunriseset, "sunriseset");
	protocol_device_add(sunriseset, "sunriseset", "Sunrise / Sunset Calculator");
	sunriseset->devtype = WEATHER;
	sunriseset->hwtype = API;
	sunriseset->multipleId = 0;
#if PILIGHT_V >= 6
	sunriseset->masterOnly = 1;
#endif

	options_add(&sunriseset->options, 'o', "longitude", OPTION_HAS_VALUE, DEVICES_ID, JSON_NUMBER, NULL, NULL);
	options_add(&sunriseset->options, 'a', "latitude", OPTION_HAS_VALUE, DEVICES_ID, JSON_NUMBER, NULL, NULL);
	options_add(&sunriseset->options, 'u', "sunrise", OPTION_HAS_VALUE, DEVICES_VALUE, JSON_NUMBER, NULL, "^[0-9]{3,4}$");
	options_add(&sunriseset->options, 'd', "sunset", OPTION_HAS_VALUE, DEVICES_VALUE, JSON_NUMBER, NULL, "^[0-9]{3,4}$");
	options_add(&sunriseset->options, 's', "sun", OPTION_HAS_VALUE, DEVICES_VALUE, JSON_STRING, NULL, NULL);

	// options_add(&sunriseset->options, 0, "decimals", OPTION_HAS_VALUE, DEVICES_SETTING, JSON_NUMBER, (void *)2, "[0-9]");
	options_add(&sunriseset->options, 0, "sunriseset-decimals", OPTION_HAS_VALUE, GUI_SETTING, JSON_NUMBER, (void *)2, "[0-9]");
	options_add(&sunriseset->options, 0, "show-sunriseset", OPTION_HAS_VALUE, GUI_SETTING, JSON_NUMBER, (void *)1, "^[10]{1}$");

	sunriseset->initDev=&initDev;
	sunriseset->threadGC=&threadGC;
	sunriseset->checkValues=&checkValues;
}

#if defined(MODULE) && !defined(_WIN32)
void compatibility(struct module_t *module) {
	module->name = "sunriseset";
	module->version = "2.6";
	module->reqversion = "6.0";
	module->reqcommit = "115";
}

void init(void) {
	sunRiseSetInit();
}
#endif
