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


/* The location of the config file */
static char *config_filename = NULL;

/* The root configuration hook */
static struct config_t *config_root = NULL;

/* The comments found and stipped of from the config file */
static JsonNode *config_root_comments = NULL;

static JsonNode *config_last_known_good = NULL;
static JsonNode *config_last_comments = NULL;

static void sort_list(int r) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	struct config_t *a = NULL;
	struct config_t *b = NULL;
	struct config_t *c = NULL;
	struct config_t *e = NULL;
	struct config_t *tmp = NULL;

	while(config_root && e != config_root->next) {
		c = a = config_root;
		b = a->next;
		while(a != e) {
			if((r == 0 && a->writeorder > b->writeorder) ||
			   (r == 1 && a->readorder > b->readorder)) {
				if(a == config_root) {
					tmp = b->next;
					b->next = a;
					a->next = tmp;
					config_root = b;
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

int config_gc(void) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	struct config_t *listeners;
	while(config_root) {
		listeners = config_root;
		listeners->gc();
		FREE(config_root->name);
		config_root = config_root->next;
		FREE(listeners);
	}

	FREE(config_filename);
	config_filename = NULL;

	json_delete(config_root_comments);
	config_root_comments = NULL;

	json_delete(config_last_known_good);
	config_last_known_good = NULL;

	json_delete(config_last_comments);
	config_last_comments = NULL;

	logprintf(LOG_DEBUG, "garbage collected config library");
	return 1;
}

int config_parse(const JsonNode *root) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	const struct JsonNode *jconfig = NULL;
	unsigned short error = 0;

	sort_list(1);
	struct config_t *listeners = config_root;
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
	struct config_t *listeners = config_root;
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
	FILE *fp = NULL;

	sort_list(0);
	struct config_t *listeners = config_root;
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
	char *content = json_stringify_ex(root, "\t", config_root_comments);
	if(content == NULL) {
		logprintf(LOG_ERR, "cannot create proper config file lines from current configuration."
					" Config file '%s' not written.", config_filename);
		json_delete(root);
		return EXIT_FAILURE;
	}
	int ret = EXIT_FAILURE;
	if((fp = fopen(config_filename, "w")) == NULL) {
		logprintf(LOG_ERR, "cannot open config file '%s' for writing (%s)", config_filename, strerror(errno));
	} else {
		if (fwrite(content, sizeof(char), strlen(content), fp) < 0) {
			logprintf(LOG_ERR, "cannot write config file '%s' (%s)", config_filename, strerror(errno));
		} else {
			ret = EXIT_SUCCESS;
		}
		fclose(fp);
	}
	json_free(content);
	json_delete(root);
	return ret;
}

int config_read(void) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	char *content = NULL;
	const char *problem = NULL;
	struct JsonNode *root = NULL, *comments = NULL;

	/* Read JSON config file (file_get_contents() has 'wrong' opposite return value) */
	if(file_get_contents(config_filename, &content) == -1) {
		return EXIT_FAILURE;
	}

	/* Validate JSON and turn into JSON object */
	root = json_decode_ex(content, &problem, JSON_WANT_EMBEDDED_COMMENTS);  //&comments);
	if(root == NULL) {
		int line_number = 0, line_pos = 0;
		if (problem == NULL)
			problem = "(sorry, no problem information)";
		else {
			line_number = json_get_line_number(content, problem, &line_pos);
			// limit output length of problem code:
			static const char ellipsis[] = " ...";
			static const int max_len = 64;
			char * const p = content + (problem - content);
			size_t len = strlen(p);
			if (len > max_len + sizeof(ellipsis))
				strcpy(p + max_len, ellipsis);
		}
		logprintf(LOG_ERR, "config is not in a valid json format."
			" Problem in line %d at offset %d: %s", line_number, line_pos, problem);
		FREE(content);
		return EXIT_FAILURE;
	}
// LUTZ
	if (1) {
		static const char _bak[] = ".bak";
		char fn[strlen(config_filename) + sizeof(_bak)];
		strcpy(fn, config_filename); strcat(fn, _bak);
		FILE *fp = fopen(fn, "w");
		if(fp) {
			fputs(	"\n/*****************************************"
				"\n * The json code with comments embedded:"
				"\n *****************************************/"
				"\n", fp);
			char *json = json_stringify(root, "\t");
			fputs(json ? json : "/* failed to generate json code ... */", fp);
			if (json && *json && json[strlen(json)-1] != '\n')
				fputc('\n', fp);
			json_free(json);

			root = json_strip_comments(root, comments = json_mkobject());

			fputs(	"\n/*****************************************"
				"\n * The pure json code with comments stripped off:"
				"\n *****************************************/"
				"\n", fp);
			json = json_stringify(root, "\t");
			fputs(json ? json : "/* failed to generate json code ... */", fp);
			if (json && *json && json[strlen(json)-1] != '\n')
				fputc('\n', fp);
			json_free(json);

			fputs(	"\n/*****************************************"
				"\n * The stipped off comments:"
				"\n *****************************************/"
				"\n", fp);
			json = json_stringify(comments, "\t");
			fputs(json ? json : "/* failed to generate json code ... */", fp);
			if (json && *json && json[strlen(json)-1] != '\n')
				fputc('\n', fp);

			fputs(	"\n/*****************************************"
				"\n * The pure code with comments merged in again:"
				"\n *****************************************/"
				"\n", fp);
			json = json_stringify_ex(root, "\t", comments);
			fputs(json ? json : "/* failed to generate json code ... */", fp);
			if (json && *json && json[strlen(json)-1] != '\n')
				fputc('\n', fp);
			fclose(fp);
		}
		else
			root = json_strip_comments(root, comments = json_mkobject());
	}
	else
// END LUTZ
		root = json_strip_comments(root, comments = json_mkobject());

// TODO: implement a "last known good" and restore to this in case a new config does not parse well.
	if(config_parse(root) != EXIT_SUCCESS) {
		FREE(content);
		json_delete(comments);
		json_delete(root);
		return EXIT_FAILURE;
	}
	json_delete(root);
	json_delete(config_root_comments);
	config_root_comments = comments;

	config_write(1, "all");

	FREE(content);
	return EXIT_SUCCESS;
}

void config_register(config_t **listener, const char *name) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	CONFIG_ALLOC_NAMED_NODE(*listener, name);
	(*listener)->parse = NULL;
	(*listener)->sync = NULL;
	(*listener)->gc = NULL;
	CONFIG_PREPEND_NODE_TO_LIST(*listener, config_root);
}

int config_set_file(const char *settfile) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	if(access(settfile, R_OK | W_OK) != -1) {
		config_filename = REALLOC_OR_EXIT(config_filename, strlen(settfile)+1);
		strcpy(config_filename, settfile);
	} else {
		logprintf(LOG_ERR, "the config file %s does not exist or is not writable", settfile);
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}

const char *config_get_file(void) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	return config_filename;
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
