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
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <libgen.h>

#include "pilight.h"
#include "common.h"
#include "json.h"
#include "config.h"
#include "log.h"
#include "../config/devices.h"
#include "../config/settings.h"
#include "../config/registry.h"
#ifdef EVENTS
	#include "../config/rules.h"
#endif
#include "../config/hardware.h"
#include "../config/gui.h"

static struct config_t *config;

static void sort_list(int r) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	struct config_t *a = NULL;
	struct config_t *b = NULL;
	struct config_t *c = NULL;
	struct config_t *e = NULL;
	struct config_t *tmp = NULL;

	while(config && e != config->next) {
		c = a = config;
		b = a->next;
		while(a != e) {
			if((r == 0 && a->writeorder > b->writeorder) ||
			   (r == 1 && a->readorder > b->readorder)) {
				if(a == config) {
					tmp = b->next;
					b->next = a;
					a->next = tmp;
					config = b;
					c = b;
				} else {
					tmp = b->next;
					b->next = a;
					a->next = tmp;
					c->next = b;
					c = b;
				}
			} else {
				c = a;
				a = a->next;
			}
			b = a->next;
			if(b == e)
				e = a;
		}
	}
}

/* The location of the config file */
static char *configfile = NULL;

int config_gc(void) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	struct config_t *listeners;
	while(config) {
		listeners = config;
		listeners->gc();
		FREE(config->name);
		config = config->next;
		FREE(listeners);
	}
	if(config != NULL) {
		FREE(config);
	}
	if(configfile != NULL) {
		FREE(configfile);
	}
	logprintf(LOG_DEBUG, "garbage collected config library");
	return 1;
}

int config_parse(JsonNode *root) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	struct JsonNode *jconfig = NULL;
	unsigned short error = 0;

	sort_list(1);
	struct config_t *listeners = config;
	while(listeners) {
		if((jconfig = json_find_member(root, listeners->name))) {
			if(listeners->parse) {
				if(listeners->parse(jconfig) == EXIT_FAILURE) {
					error = 1;
					break;
				}
			}
		}
		listeners = listeners->next;
	}

	if(error == 1) {
		config_gc();
		return EXIT_FAILURE;
	} else {
		return EXIT_SUCCESS;
	}
}

JsonNode *config_print(int level, const char *media) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	struct JsonNode *root = json_mkobject();

	sort_list(0);
	struct config_t *listeners = config;
	while(listeners) {
		if(listeners->sync) {
			struct JsonNode *child = listeners->sync(level, media);
			if(child != NULL) {
				json_append_member(root, listeners->name, child);
			}
		}
		listeners = listeners->next;
	}

	return root;
}

int config_write(int level, const char *media) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	struct JsonNode *root = json_mkobject();
	FILE *fp;

	sort_list(0);
	struct config_t *listeners = config;
	while(listeners) {
		if(listeners->sync) {
			struct JsonNode *child = listeners->sync(level, media);
			if(child != NULL) {
				json_append_member(root, listeners->name, child);
			}
		}
		listeners = listeners->next;
	}

	/* Overwrite config file with proper format */
	if((fp = fopen(configfile, "w+")) == NULL) {
		logprintf(LOG_ERR, "cannot write config file: %s", configfile);
		json_delete(root);
		return EXIT_FAILURE;
	}
	fseek(fp, 0L, SEEK_SET);
	char *content = NULL;
	if((content = json_stringify(root, "\t")) != NULL) {
		fwrite(content, sizeof(char), strlen(content), fp);
		json_free(content);
	}
	fclose(fp);
	json_delete(root);
	return EXIT_SUCCESS;
}

int config_read(void) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	char *content = NULL;
	const char *problem = NULL;
	struct JsonNode *root = NULL;

	/* Read JSON config file */
	if(file_get_contents(configfile, &content) == 0) {
		/* Validate JSON and turn into JSON object */
		if(json_validate(content, &problem) == false) {
			size_t lino = 0;
			if (problem == NULL)
				problem = "(sorry no problem information)";
			else {
				char * const p = content + (problem - content);
				size_t len = strlen(p);
				if (len > 64)
					p[65] = 0;
				char ch = *p; *p = 0; // make temporary null terminated
				char *s;
				for (s = content; s; s ? s++ : s) {
					lino++;
					s = strchr(s, '\n');
				}
				*p = ch;	// undo null termination
			}
			logprintf(LOG_ERR, "config is not in a valid json format."
					" Problem in line %d at %s ...", lino, problem);
			FREE(content);
			return EXIT_FAILURE;
		}
		root = json_decode(content);

		if (1) {
			char fn[strlen(configfile) + sizeof(".bak")];
			strcpy(fn, configfile); strcat(fn, ".bak");
			FILE *fp = fopen(fn, "w");
			if(fp) {
				char *json = json_stringify(root, "\t");
				fputs(json ? json : "/* something went worng... */\n", fp);
				fclose(fp);
			}
		}

		if(config_parse(root) != EXIT_SUCCESS) {
			FREE(content);
			json_delete(root);
			return EXIT_FAILURE;
		}
		json_delete(root);
		config_write(1, "all");
		FREE(content);
	}
	return EXIT_SUCCESS;
}

void config_register(config_t **listener, const char *name) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	CONFIG_ALLOC_NAMED_NODE(*listener, name);
	(*listener)->parse = NULL;
	(*listener)->sync = NULL;
	(*listener)->gc = NULL;
	CONFIG_PREPEND_NODE_TO_LIST(*listener, config);
}

int config_set_file(char *settfile) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	if(access(settfile, R_OK | W_OK) != -1) {
		configfile = REALLOC_OR_EXIT(configfile, strlen(settfile)+1);
		strcpy(configfile, settfile);
	} else {
		logprintf(LOG_ERR, "the config file %s does not exists", settfile);
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}

char *config_get_file(void) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	return configfile;
}

void config_init() {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	hardware_init();
	settings_init();
	devices_init();
	gui_init();
#ifdef EVENTS
	rules_init();
#endif
	registry_init();
}
