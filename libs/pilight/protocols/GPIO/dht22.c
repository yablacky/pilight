/*
	Copyright (C) 2013 CurlyMo

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
#include <stdint.h>
#include <dirent.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <sys/stat.h>
#ifdef _WIN32
	#ifdef __mips__
		#define __USE_UNIX98
	#endif
#endif
#include <pthread.h>

#include "../../core/pilight.h"
#include "../../core/common.h"
#include "../../core/dso.h"
#include "../../core/log.h"
#include "../../core/threads.h"
#include "../../core/binary.h"
#include "../../core/gc.h"
#include "../../core/json.h"
#include "../protocol.h"
#include "dht22.h"

#define MAXTIMINGS 100

#if !defined(__FreeBSD__) && !defined(_WIN32)
#include "../../../wiringx/wiringX.h"

static unsigned short loop = 1;
static unsigned short threads = 0;

static pthread_mutex_t lock;
static pthread_mutexattr_t attr;

static uint8_t sizecvt(const int read_value) {
	/* digitalRead() and friends from wiringx are defined as returning a value
	   < 256. However, they are returned as int() types. This is a safety function */
	if(read_value > 255 || read_value < 0) {
		logprintf(LOG_NOTICE, "invalid data from wiringX library");
		return -1;
	}

	return (uint8_t)read_value;
}

static void *thread(void *param) {
	struct protocol_threads_t *node = (struct protocol_threads_t *)param;
	const struct JsonNode *json = (struct JsonNode *)node->param;
	const struct JsonNode *jid = NULL;
	const struct JsonNode *jchild = NULL;
	int *id = 0;
	int nrid = 0, y = 0, interval = 10, nrloops = 0, x = 0;
	double temp_offset = 0.0, humi_offset = 0.0, itmp = 0.0;

	threads++;

	if((jid = json_find_member(json, "id"))) {
		jchild = json_first_child(jid);
		while(jchild) {
			if(json_find_number(jchild, "gpio", &itmp) == 0) {
				id = REALLOC(id, (sizeof(int)*(size_t)(nrid+1)));
				id[nrid] = (int)round(itmp);
				nrid++;
			}
			jchild = jchild->next;
		}
	}

	if(json_find_number(json, "poll-interval", &itmp) == 0)
		interval = (int)round(itmp);
	json_find_number(json, "temperature-offset", &temp_offset);
	json_find_number(json, "humidity-offset", &humi_offset);

	while(loop) {
		if(protocol_thread_wait(node, interval, &nrloops) == ETIMEDOUT) {
			pthread_mutex_lock(&lock);
			for(y=0;y<nrid;y++) {
				int tries = 5;
				unsigned short got_correct_date = 0;
				while(tries && !got_correct_date && loop) {

					uint8_t laststate = HIGH;
					uint8_t counter = 0;
					uint8_t j = 0, i = 0;

					int dht22_dat[5] = {0,0,0,0,0};

					// pull pin down for 18 milliseconds
					pinMode(id[y], OUTPUT);
					digitalWrite(id[y], HIGH);
					usleep(500000);  // 500 ms
					// then pull it up for 40 microseconds
					digitalWrite(id[y], LOW);
					usleep(20000);
					// prepare to read the pin
					pinMode(id[y], INPUT);

					// detect change and read data
					for(i=0; (i<MAXTIMINGS && loop); i++) {
						counter = 0;
						delayMicroseconds(10);

						while((x = sizecvt(digitalRead(id[y]))) == laststate && x != -1 && loop) {
							counter++;
							delayMicroseconds(1);
							if(counter == 255) {
								break;
							}
						}
						laststate = sizecvt(digitalRead(id[y]));

						if(counter == 255) {
							break;
						}

						// ignore first 3 transitions
						if((i >= 4) && (i%2 == 0)) {
							// shove each bit into the storage bytes
							dht22_dat[(int)((double)j/8)] <<= 1;
							if(counter > 16)
								dht22_dat[(int)((double)j/8)] |= 1;
							j++;
						}
					}

					// check we read 40 bits (8bit x 5 ) + verify checksum in the last byte
					// print it out if data is good
					if((j >= 40) && (dht22_dat[4] == ((dht22_dat[0] + dht22_dat[1] + dht22_dat[2] + dht22_dat[3]) & 0xFF))) {
						got_correct_date = 1;

						double h = dht22_dat[0] * 256 + dht22_dat[1];
						double t = (dht22_dat[2] & 0x7F)* 256 + dht22_dat[3];
						t += temp_offset;
						h += humi_offset;

						if((dht22_dat[2] & 0x80) != 0)
							t *= -1;

						dht22->message = json_mkobject();
						JsonNode *code = json_mkobject();
						json_append_member(code, "gpio", json_mknumber(id[y], 0));
						json_append_member(code, "temperature", json_mknumber(t/10, 1));
						json_append_member(code, "humidity", json_mknumber(h/10, 1));

						json_append_member(dht22->message, "message", code);
						json_append_member(dht22->message, "origin", json_mkstring("receiver"));
						json_append_member(dht22->message, "protocol", json_mkstring(dht22->id));

						if(pilight.broadcast != NULL) {
							pilight.broadcast(dht22->id, dht22->message, PROTOCOL);
						}
						json_delete(dht22->message);
						dht22->message = NULL;
					} else {
						logprintf(LOG_DEBUG, "dht22 data checksum was wrong");
						tries--;
						protocol_thread_wait(node, 1, &nrloops);
					}
				}
			}
			pthread_mutex_unlock(&lock);
		}
	}
	pthread_mutex_unlock(&lock);

	FREE(id);
	threads--;
	return (void *)NULL;
}

static struct threadqueue_t *initDev(const JsonNode *jdevice) {
	if(wiringXSupported() == 0 && wiringXSetup() == 0) {
		loop = 1;
		char *output = json_stringify(jdevice, NULL);
		JsonNode *json = json_decode(output);
		json_free(output);

		struct protocol_threads_t *node = protocol_thread_init(dht22, json);
		return threads_register("dht22", &thread, (void *)node, 0);
	} else {
		return NULL;
	}
}

static void threadGC(void) {
	loop = 0;
	protocol_thread_stop(dht22);
	while(threads > 0) {
		usleep(10);
	}
	protocol_thread_free(dht22);
}

static int checkValues(const JsonNode *code) {
	const struct JsonNode *jid = NULL;
	const struct JsonNode *jchild = NULL;
	double itmp = -1;

	/* Validate GPIO number */
	if((jid = json_find_member(code, "id")) != NULL) {
		if((jchild = json_find_element(jid, 0)) != NULL) {
			if(json_find_number(jchild, "gpio", &itmp) == 0) {
				if(wiringXSupported() == 0) {
					int gpio = (int)itmp;
					if(wiringXSetup() < 0) {
						logprintf(LOG_ERR, "unable to setup wiringX") ;
						return -1;
					} else if(wiringXValidGPIO(gpio) != 0) {
						logprintf(LOG_ERR, "relay: invalid gpio range");
						return -1;
					}
				}
			}
		}
	}

	return 0;
}
#endif

#if !defined(MODULE) && !defined(_WIN32)
__attribute__((weak))
#endif
void dht22Init(void) {
#if !defined(__FreeBSD__) && !defined(_WIN32)
	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&lock, &attr);
#endif

	protocol_register(&dht22);
	protocol_set_id(dht22, "dht22");
	protocol_device_add(dht22, "dht22", "1-wire Temperature and Humidity Sensor");
	protocol_device_add(dht22, "am2302", "1-wire Temperature and Humidity Sensor");
	dht22->devtype = WEATHER;
	dht22->hwtype = SENSOR;

	options_add(&dht22->options, 't', "temperature", OPTION_HAS_VALUE, DEVICES_VALUE, JSON_NUMBER, NULL, "^[0-9]{1,3}$");
	options_add(&dht22->options, 'h', "humidity", OPTION_HAS_VALUE, DEVICES_VALUE, JSON_NUMBER, NULL, "^[0-9]{1,3}$");
	options_add(&dht22->options, 'g', "gpio", OPTION_HAS_VALUE, DEVICES_ID, JSON_NUMBER, NULL, NULL);

	// options_add(&dht22->options, 0, "decimals", OPTION_HAS_VALUE, DEVICES_SETTING, JSON_NUMBER, (void *)1, "[0-9]");
	options_add(&dht22->options, 0, "temperature-offset", OPTION_HAS_VALUE, DEVICES_SETTING, JSON_NUMBER, (void *)0, "[0-9]");
	options_add(&dht22->options, 0, "humidity-offset", OPTION_HAS_VALUE, DEVICES_SETTING, JSON_NUMBER, (void *)0, "[0-9]");
	options_add(&dht22->options, 0, "temperature-decimals", OPTION_HAS_VALUE, GUI_SETTING, JSON_NUMBER, (void *)1, "[0-9]");
	options_add(&dht22->options, 0, "humidity-decimals", OPTION_HAS_VALUE, GUI_SETTING, JSON_NUMBER, (void *)1, "[0-9]");
	options_add(&dht22->options, 0, "show-temperature", OPTION_HAS_VALUE, GUI_SETTING, JSON_NUMBER, (void *)1, "^[10]{1}$");
	options_add(&dht22->options, 0, "show-humidity", OPTION_HAS_VALUE, GUI_SETTING, JSON_NUMBER, (void *)1, "^[10]{1}$");
	options_add(&dht22->options, 0, "poll-interval", OPTION_HAS_VALUE, DEVICES_SETTING, JSON_NUMBER, (void *)10, "[0-9]");

#if !defined(__FreeBSD__) && !defined(_WIN32)
	dht22->initDev=&initDev;
	dht22->threadGC=&threadGC;
	dht22->checkValues=&checkValues;
#endif
}

#if defined(MODULE) && !defined(_WIN32)
void compatibility(struct module_t *module) {
	module->name = "dht22";
	module->version = "2.4";
	module->reqversion = "6.0";
	module->reqcommit = "84";
}

void init(void) {
	dht22Init();
}
#endif
