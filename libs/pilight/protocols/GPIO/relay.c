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
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <math.h>

#include "../../core/pilight.h"
#include "../../core/common.h"
#include "../../core/dso.h"
#include "../../core/log.h"
#include "../../core/gc.h"
#include "../protocol.h"
#include "relay.h"

#include "../../../wiringx/wiringX.h"

static char *state = NULL;

static void createMessage(int gpio, int state) {
	relay->message = json_mkobject();
	json_append_member(relay->message, "gpio", json_mknumber(gpio, 0));
	if(state == 1)
		json_append_member(relay->message, "state", json_mkstring("on"));
	else
		json_append_member(relay->message, "state", json_mkstring("off"));
}

static int createCode(const JsonNode *code) {
	int gpio = -1;
	int state = -1;
	double itmp = -1;
	const char *def = NULL;
	int have_error = 0;

	relay->rawlen = 0;
	if(json_find_string(code, "default-state", &def) != 0) {
		def =  "off";
	}

	if(json_find_number(code, "gpio", &itmp) == 0)
		gpio = (int)round(itmp);
	if(json_find_number(code, "off", &itmp) == 0)
		state=0;
	else if(json_find_number(code, "on", &itmp) == 0)
		state=1;

	if(gpio == -1 || state == -1) {
		logprintf(LOG_ERR, "relay: insufficient number of arguments");
		have_error = 1;
		goto clear;
	} else if(wiringXSupported() == 0) {
		if(wiringXSetup() < 0) {
			logprintf(LOG_ERR, "unable to setup wiringX") ;
			return EXIT_FAILURE;
		} else {
			if(wiringXValidGPIO(gpio) != 0) {
				logprintf(LOG_ERR, "relay: invalid gpio range");
				have_error = 1;
				goto clear;
			} else {
				if(strstr(progname, "daemon") != NULL) {
					pinMode(gpio, OUTPUT);
					if(strcmp(def, "off") == 0) {
						if(state == 1) {
							digitalWrite(gpio, LOW);
						} else if(state == 0) {
							digitalWrite(gpio, HIGH);
						}
					} else {
						if(state == 0) {
							digitalWrite(gpio, LOW);
						} else if(state == 1) {
							digitalWrite(gpio, HIGH);
						}
					}
				} else {
					wiringXGC();
				}
			createMessage(gpio, state);
			goto clear;
			}
		}
	}

clear:
	if(have_error) {
		return EXIT_FAILURE;
	} else {
		return EXIT_SUCCESS;
	}
}

static void printHelp(void) {
	printf("\t -t --on\t\t\tturn the relay on\n");
	printf("\t -f --off\t\t\tturn the relay off\n");
	printf("\t -g --gpio=gpio\t\t\tthe gpio the relay is connected to\n");
}

static int checkValues(const JsonNode *code) {
	const char *def = NULL;
	const struct JsonNode *jid = NULL;
	const struct JsonNode *jchild = NULL;
	double itmp = -1;

	if(json_find_string(code, "default-state", &def) != 0) {
		def = "off";
	}
	if(strcmp(def, "on") != 0 && strcmp(def, "off") != 0) {
		return 1;
	}

	/* Get current relay state and validate GPIO number */
	if((jid = json_find_member(code, "id")) != NULL) {
		if((jchild = json_find_element(jid, 0)) != NULL) {
			if(json_find_number(jchild, "gpio", &itmp) == 0) {
				if(wiringXSupported() == 0) {
					int gpio = (int)itmp;
					int state = -1;
					if(wiringXSetup() < 0) {
						logprintf(LOG_ERR, "unable to setup wiringX") ;
						return -1;
					} else if(wiringXValidGPIO(gpio) != 0) {
						logprintf(LOG_ERR, "relay: invalid gpio range");
						return -1;
					} else {
						pinMode(gpio, INPUT);
						state = digitalRead(gpio);
						if(strcmp(def, "on") == 0) {
							state ^= 1;
						}

						relay->message = json_mkobject();
						JsonNode *code = json_mkobject();
						json_append_member(code, "gpio", json_mknumber(gpio, 0));
						if(state == 1) {
							json_append_member(code, "state", json_mkstring("on"));
						} else {
							json_append_member(code, "state", json_mkstring("off"));
						}

						json_append_member(relay->message, "message", code);
						json_append_member(relay->message, "origin", json_mkstring("sender"));
						json_append_member(relay->message, "protocol", json_mkstring(relay->id));
						if(pilight.broadcast != NULL) {
							pilight.broadcast(relay->id, relay->message, PROTOCOL);
						}
						json_delete(relay->message);
						relay->message = NULL;
					}
				}
			}
		}
	}

	return 0;
}

static void gc(void) {
	FREE(state);
}

#if !defined(MODULE) && !defined(_WIN32)
__attribute__((weak))
#endif
void relayInit(void) {

	protocol_register(&relay);
	protocol_set_id(relay, "relay");
	protocol_device_add(relay, "relay", "GPIO Connected Relays");
	relay->devtype = RELAY;
	relay->hwtype = HWRELAY;
	relay->multipleId = 0;

	options_add(&relay->options, 't', "on", OPTION_NO_VALUE, DEVICES_STATE, JSON_STRING, NULL, NULL);
	options_add(&relay->options, 'f', "off", OPTION_NO_VALUE, DEVICES_STATE, JSON_STRING, NULL, NULL);
	options_add(&relay->options, 'g', "gpio", OPTION_HAS_VALUE, DEVICES_ID, JSON_NUMBER, NULL, "[0-9]");

	state = STRDUP_OR_EXIT("off");
	options_add(&relay->options, 0, "default-state", OPTION_HAS_VALUE, DEVICES_SETTING, JSON_STRING, (void *)state, NULL);
	options_add(&relay->options, 0, "readonly", OPTION_HAS_VALUE, GUI_SETTING, JSON_NUMBER, (void *)0, "^[10]{1}$");
	options_add(&relay->options, 0, "confirm", OPTION_HAS_VALUE, GUI_SETTING, JSON_NUMBER, (void *)0, "^[10]{1}$");

	relay->checkValues=&checkValues;
	relay->createCode=&createCode;
	relay->printHelp=&printHelp;
	relay->gc=&gc;
}

#if defined(MODULE) && !defined(_WIN32)
void compatibility(struct module_t *module) {
	module->name = "relay";
	module->version = "3.3";
	module->reqversion = "6.0";
	module->reqcommit = "84";
}

void init(void) {
	relayInit();
}
#endif
