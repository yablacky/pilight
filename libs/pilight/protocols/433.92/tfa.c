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
#include <string.h>
#include <math.h>

#include "../../core/pilight.h"
#include "../../core/common.h"
#include "../../core/dso.h"
#include "../../core/log.h"
#include "../protocol.h"
#include "../../core/binary.h"
#include "../../core/gc.h"
#include "tfa.h"

#define PULSE_MULTIPLIER	13
#define MIN_PULSE_LENGTH	220
#define MAX_PULSE_LENGTH	250
#define AVG_PULSE_LENGTH	235
#define RAW_LENGTH				86

typedef struct settings_t {
	double id;
	double channel;
	double temp;
	double humi;
	struct settings_t *next;
} settings_t;

static struct settings_t *settings = NULL;

static int validate(void) {
	if(tfa->rawlen == RAW_LENGTH) {
		if(tfa->raw[tfa->rawlen-1] >= (MIN_PULSE_LENGTH*PULSE_DIV) &&
		   tfa->raw[tfa->rawlen-1] <= (MAX_PULSE_LENGTH*PULSE_DIV)) {
			return 0;
		}
	}

	return -1;
}

static void parseCode(void) {
	int binary[RAW_LENGTH/2];
	int temp1 = 0, temp2 = 0, temp3 = 0;
	int humi1 = 0, humi2 = 0;
	int id = 0, battery = 0;
	int channel = 0;
	int i = 0, x = 0;
	double humi_offset = 0.0, temp_offset = 0.0;
	double temperature = 0.0, humidity = 0.0;

	for(x=1;x<tfa->rawlen-2;x+=2) {
		if(tfa->raw[x] > AVG_PULSE_LENGTH*PULSE_MULTIPLIER) {
			binary[i++] = 1;
		} else {
			binary[i++] = 0;
		}
	}

	id = binToDecRev(binary, 2, 9);
	channel = binToDecRev(binary, 12, 13) + 1;

	temp1 = binToDecRev(binary, 14, 17);
	temp2 = binToDecRev(binary, 18, 21);
	temp3 = binToDecRev(binary, 22, 25);
	                                                     /* Convert F to C */
	temperature = (int)((float)(((((temp3*256) + (temp2*16) + (temp1))*10) - 9000) - 3200) * ((float)5/(float)9));

	humi1 = binToDecRev(binary, 26, 29);
	humi2 = binToDecRev(binary, 30, 33);
	humidity = ((humi1)+(humi2*16));

	if(binToDecRev(binary, 34, 35) > 1) {
		battery = 0;
	} else {
		battery = 1;
	}

	struct settings_t *tmp = settings;
	while(tmp) {
		if(fabs(tmp->id-id) < EPSILON && fabs(tmp->channel-channel) < EPSILON) {
			humi_offset = tmp->humi;
			temp_offset = tmp->temp;
			break;
		}
		tmp = tmp->next;
	}

	temperature += temp_offset;
	humidity += humi_offset;

	tfa->message = json_mkobject();
	json_append_member(tfa->message, "id", json_mknumber(id, 0));
	json_append_member(tfa->message, "temperature", json_mknumber(temperature/100, 2));
	json_append_member(tfa->message, "humidity", json_mknumber(humidity, 2));
	json_append_member(tfa->message, "battery", json_mknumber(battery, 0));
	json_append_member(tfa->message, "channel", json_mknumber(channel, 0));
}

static int checkValues(const struct JsonNode *jvalues) {
	const struct JsonNode *jid = NULL;

	if((jid = json_find_member(jvalues, "id"))) {
		struct settings_t *snode = NULL;
		const struct JsonNode *jchild = NULL;
		const struct JsonNode *jchild1 = NULL;
		double channel = -1, id = -1;

		json_foreach(jchild, jid) {
			json_foreach(jchild1, jchild) {
				if(strcmp(jchild1->key, "channel") == 0) {
					channel = jchild1->number_;
				}
				if(strcmp(jchild1->key, "id") == 0) {
					id = jchild1->number_;
				}
			}
		}

		struct settings_t *tmp = settings;
		while(tmp) {
			if(fabs(tmp->id-id) < EPSILON && fabs(tmp->channel-channel) < EPSILON) {
				break;
			}
			tmp = tmp->next;
		}

		if(tmp == NULL) {
			CONFIG_ALLOC_UNNAMED_NODE(snode);
			snode->id = id;
			snode->channel = channel;

			json_find_number(jvalues, "temperature-offset", &snode->temp);
			json_find_number(jvalues, "humidity-offset", &snode->humi);

			CONFIG_PREPEND_NODE_TO_LIST(snode, settings);
		}
	}
	return 0;
}

static void gc(void) {
	struct settings_t *tmp = NULL;
	while(settings) {
		tmp = settings;
		settings = settings->next;
		FREE(tmp);
	}
}

#if !defined(MODULE) && !defined(_WIN32)
__attribute__((weak))
#endif
void tfaInit(void) {
	protocol_register(&tfa);
	protocol_set_id(tfa, "tfa");
	protocol_device_add(tfa, "tfa", "TFA weather stations");
	protocol_device_add(tfa, "conrad_weather", "Conrad Weather Stations");
	tfa->devtype = WEATHER;
	tfa->hwtype = RF433;
	tfa->maxgaplen = MAX_PULSE_LENGTH*PULSE_DIV;
	tfa->mingaplen = MIN_PULSE_LENGTH*PULSE_DIV;
	tfa->minrawlen = 86;
	tfa->maxrawlen = 86;

	options_add(&tfa->options, 't', "temperature", OPTION_HAS_VALUE, DEVICES_VALUE, JSON_NUMBER, NULL, "^[0-9]{1,3}$");
	options_add(&tfa->options, 'i', "id", OPTION_HAS_VALUE, DEVICES_ID, JSON_NUMBER, NULL, "[0-9]");
	options_add(&tfa->options, 'c', "channel", OPTION_HAS_VALUE, DEVICES_ID, JSON_NUMBER, NULL, "[0-9]");
	options_add(&tfa->options, 'h', "humidity", OPTION_HAS_VALUE, DEVICES_VALUE, JSON_NUMBER, NULL, "[0-9]");
	options_add(&tfa->options, 'b', "battery", OPTION_HAS_VALUE, DEVICES_VALUE, JSON_NUMBER, NULL, "^[01]$");

	// options_add(&tfa->options, 0, "decimals", OPTION_HAS_VALUE, DEVICES_SETTING, JSON_NUMBER, (void *)2, "[0-9]");
	options_add(&tfa->options, 0, "temperature-offset", OPTION_HAS_VALUE, DEVICES_SETTING, JSON_NUMBER, (void *)0, "[0-9]");
	options_add(&tfa->options, 0, "humidity-offset", OPTION_HAS_VALUE, DEVICES_SETTING, JSON_NUMBER, (void *)0, "[0-9]");
	options_add(&tfa->options, 0, "temperature-decimals", OPTION_HAS_VALUE, GUI_SETTING, JSON_NUMBER, (void *)2, "[0-9]");
	options_add(&tfa->options, 0, "humidity-decimals", OPTION_HAS_VALUE, GUI_SETTING, JSON_NUMBER, (void *)2, "[0-9]");
	options_add(&tfa->options, 0, "show-humidity", OPTION_HAS_VALUE, GUI_SETTING, JSON_NUMBER, (void *)1, "^[10]{1}$");
	options_add(&tfa->options, 0, "show-temperature", OPTION_HAS_VALUE, GUI_SETTING, JSON_NUMBER, (void *)1, "^[10]{1}$");
	options_add(&tfa->options, 0, "show-battery", OPTION_HAS_VALUE, GUI_SETTING, JSON_NUMBER, (void *)1, "^[10]{1}$");

	tfa->parseCode=&parseCode;
	tfa->checkValues=&checkValues;
	tfa->validate=&validate;
	tfa->gc=&gc;
}

#if defined(MODULE) && !defined(_WIN32)
void compatibility(struct module_t *module) {
	module->name = "tfa";
	module->version = "1.0";
	module->reqversion = "6.0";
	module->reqcommit = "84";
}

void init(void) {
	tfaInit();
}
#endif
