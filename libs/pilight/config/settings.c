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

#ifndef _WIN32
	#include <regex.h>
#endif
#include <sys/stat.h>
#include <time.h>
#include <libgen.h>

#include "../core/pilight.h"
#include "../core/common.h"
#include "../core/json.h"
#include "../core/log.h"
#include "../../wiringx/wiringX.h"

#include "settings.h"

typedef struct settings_t {
	char *name;
	int type;
	union {
		char *string_;
		int number_;
	};
	struct settings_t *next;
} settings_t;

struct config_t *config_settings;

static struct settings_t *settings = NULL;

static void settings_add(struct settings_t *snode) {
	struct settings_t *tmp = settings;
	if(tmp) {
		while(tmp->next != NULL) {
			tmp = tmp->next;
		}
		tmp->next = snode;
		snode->next = NULL;
	} else {
		snode->next = settings;
		settings = snode;
	}
}

/* Add a string value to the settings struct */
static void settings_add_string(const char *name, char *value) {
	struct settings_t *snode = MALLOC_OR_EXIT(sizeof(struct settings_t));
	memset(snode, 0, sizeof(*snode));
	snode->name = STRDUP_OR_EXIT(name);
	snode->string_ = STRDUP_OR_EXIT(value);
	snode->type = JSON_STRING;

	settings_add(snode);
}

/* Add an int value to the settings struct */
static void settings_add_number(const char *name, int value) {
	struct settings_t *snode = MALLOC_OR_EXIT(sizeof(struct settings_t));
	memset(snode, 0, sizeof(*snode));
	snode->name = STRDUP_OR_EXIT(name);
	snode->number_ = value;
	snode->type = JSON_NUMBER;

	settings_add(snode);
}

/* Add a comment to the last added setting */
static void settings_add_comment(const char *comment) {
	struct settings_t *snode = MALLOC_OR_EXIT(sizeof(struct settings_t));
	memset(snode, 0, sizeof(*snode));
	snode->string_ = STRDUP_OR_EXIT(comment);
	snode->type = JSON_LINE_COMMENT;

	settings_add(snode);
}

/* Retrieve a numeric value from the settings struct */
int settings_find_number(const char *name, int *out) {
	struct settings_t *tmp_settings = settings;

	for (;tmp_settings; tmp_settings = tmp_settings->next) {
		if(tmp_settings->type == JSON_NUMBER && strcmp(tmp_settings->name, name) == 0) {
			*out = tmp_settings->number_;
			return EXIT_SUCCESS;
		}
	}

	return EXIT_FAILURE;
}

/* Retrieve a string value from the settings struct */
int settings_find_string(const char *name, const char **out) {
	struct settings_t *tmp_settings = settings;

	for (;tmp_settings; tmp_settings = tmp_settings->next) {
		if(tmp_settings->type == JSON_STRING && strcmp(tmp_settings->name, name) == 0) {
			*out = tmp_settings->string_;
			return EXIT_SUCCESS;
		}
	}

	return EXIT_FAILURE;
}

static int settings_parse(const JsonNode *root) {
	int have_error = 0;

#ifdef WEBSERVER
	int web_port = WEBSERVER_HTTP_PORT;
#ifdef WEBSERVER_HTTPS
	int web_ssl_port = WEBSERVER_HTTPS_PORT;
#endif
	int own_port = -1;

	char *webgui_root = STRDUP_OR_EXIT(WEBSERVER_ROOT);
#endif

#if !defined(__FreeBSD__) && !defined(_WIN32)
	regex_t regex;
	int reti;
#endif

	const JsonNode *jsettings = NULL;
	json_foreach_and_all(jsettings, root) {
		if(json_is_comment(jsettings)) {
			settings_add_comment(jsettings->string_);
			continue;
		}
		if(strcmp(jsettings->key, "port") == 0) {
			if(jsettings->tag != JSON_NUMBER || (int)jsettings->number_ < 1) {
				logprintf(LOG_ERR, "config setting \"%s\" must contain a number larger than 0", jsettings->key);
				have_error = 1;
				goto clear;
			}
#ifdef WEBSERVER
			if(strcmp(jsettings->key, "port") == 0) {
				own_port = (int)jsettings->number_;
			}
#endif
			settings_add_number(jsettings->key, (int)jsettings->number_);
		} else if(strcmp(jsettings->key, "firmware-gpio-reset") == 0
			|| strcmp(jsettings->key, "firmware-gpio-sck") == 0
			|| strcmp(jsettings->key, "firmware-gpio-mosi") == 0
			|| strcmp(jsettings->key, "firmware-gpio-miso") == 0) {
			if(wiringXSupported() == 0) {
				if(wiringXSetup() != 0) {
					have_error = 1;
					goto clear;
				}
			}
			if(jsettings->tag != JSON_NUMBER) {
				logprintf(LOG_ERR, "config setting \"%s\" must contain a number larger than 0", jsettings->key);
				have_error = 1;
				goto clear;
			} else if((wiringXSupported() == 0 && wiringXValidGPIO((int)jsettings->number_) != 0) || wiringXSupported() != 0) {
				logprintf(LOG_ERR, "config setting \"%s\" must contain a valid GPIO number", jsettings->key);
				have_error = 1;
				goto clear;
			} else {
				settings_add_number(jsettings->key, (int)jsettings->number_);
			}
		} else if(strcmp(jsettings->key, "standalone") == 0 ||
							strcmp(jsettings->key, "watchdog-enable") == 0 ||
							strcmp(jsettings->key, "stats-enable") == 0) {
			if(jsettings->tag != JSON_NUMBER) {
				logprintf(LOG_ERR, "config setting \"%s\" must be either 0 or 1", jsettings->key);
				have_error = 1;
				goto clear;
			} else if(jsettings->number_ < 0 || jsettings->number_ > 1) {
				logprintf(LOG_ERR, "config setting \"%s\" must be either 0 or 1", jsettings->key);
				have_error = 1;
				goto clear;
			} else {
				settings_add_number(jsettings->key, (int)jsettings->number_);
			}
		} else if(strcmp(jsettings->key, "log-level") == 0) {
			if(jsettings->tag != JSON_NUMBER) {
				logprintf(LOG_ERR, "config setting \"%s\" must contain a number from 0 till 6", jsettings->key);
				have_error = 1;
				goto clear;
			} else if((int)jsettings->number_ < 0 || (int)jsettings->number_ > 6) {
				logprintf(LOG_ERR, "config setting \"%s\" must contain a number from 0 till 6", jsettings->key);
				have_error = 1;
				goto clear;
			} else {
				settings_add_number(jsettings->key, (int)jsettings->number_);
			}
		} else if(strcmp(jsettings->key, "log-file") == 0
#ifndef _WIN32		
			|| strcmp(jsettings->key, "pid-file") == 0
#endif
			|| strcmp(jsettings->key, "pem-file") == 0) {
			if(jsettings->tag != JSON_STRING) {
				logprintf(LOG_ERR, "config setting \"%s\" must contain an existing file", jsettings->key);
				have_error = 1;
				goto clear;
			} else if(jsettings->string_ == NULL) {
				logprintf(LOG_ERR, "config setting \"%s\" must contain an existing file", jsettings->key);
				have_error = 1;
				goto clear;
			} else {
				if(path_exists(jsettings->string_) != EXIT_SUCCESS) {
					logprintf(LOG_ERR, "config setting \"%s\" must point to an existing folder", jsettings->key);
					have_error = 1;
					goto clear;
				} else {
					settings_add_string(jsettings->key, jsettings->string_);
				}
			}
		} else if(strcmp(jsettings->key, "whitelist") == 0) {
			if(jsettings->tag != JSON_STRING) {
				logprintf(LOG_ERR, "config setting \"%s\" must contain a valid ip address", jsettings->key);
				have_error = 1;
				goto clear;
			} else if(!jsettings->string_) {
				logprintf(LOG_ERR, "config setting \"%s\" must contain a valid ip addresses", jsettings->key);
				have_error = 1;
				goto clear;
			} else if(strlen(jsettings->string_) > 0) {
#if !defined(__FreeBSD__) && !defined(_WIN32)
				char validate[] = "^((\\*|[0-9]|[1-9][0-9]|1[0-9][0-9]|2([0-4][0-9]|5[0-5]))\\.(\\*|[0-9]|[1-9][0-9]|1[0-9][0-9]|2([0-4][0-9]|5[0-5]))\\.(\\*|[0-9]|[1-9][0-9]|1[0-9][0-9]|2([0-4][0-9]|5[0-5]))\\.(\\*|[0-9]|[1-9][0-9]|1[0-9][0-9]|2([0-4][0-9]|5[0-5]))(,[\\ ]|,|$))+$";
				reti = regcomp(&regex, validate, REG_EXTENDED);
				if(reti) {
					logprintf(LOG_ERR, "could not compile regex");
					have_error = 1;
					goto clear;
				}
				reti = regexec(&regex, jsettings->string_, 0, NULL, 0);
				if(reti == REG_NOMATCH || reti != 0) {
					logprintf(LOG_ERR, "config setting \"%s\" must contain valid ip addresses", jsettings->key);
					have_error = 1;
					regfree(&regex);
					goto clear;
				}
				regfree(&regex);
#endif
				int l = (int)strlen(jsettings->string_)-1;
				if(jsettings->string_[l] == ' ' || jsettings->string_[l] == ',') {
					logprintf(LOG_ERR, "config setting \"%s\" must contain valid ip addresses", jsettings->key);
					have_error = 1;
					goto clear;
				}
				settings_add_string(jsettings->key, jsettings->string_);
			}
#ifdef WEBSERVER

		} else if(strcmp(jsettings->key, "webserver-https-port") == 0) {
			if(jsettings->tag != JSON_NUMBER) {
				logprintf(LOG_ERR, "config setting \"%s\" must contain a number", jsettings->key);
				have_error = 1;
				goto clear;
			} else if(jsettings->number_ <= 0 || (double)(int)jsettings->number_ != jsettings->number_) {
				logprintf(LOG_ERR, "config setting \"%s\" must contain an integer number larger than 0", jsettings->key);
				have_error = 1;
				goto clear;
			} else {
#ifdef WEBSERVER_HTTPS
				web_ssl_port = (int)jsettings->number_;
				settings_add_number(jsettings->key, (int)jsettings->number_);
#else
				logprintf(LOG_WARNING, "config setting \"%s\" not supported. Not build with WEBSERVER_HTTPS defined.", jsettings->key);
#endif
			}
		} else if(strcmp(jsettings->key, "webserver-http-port") == 0) {
			if(jsettings->tag != JSON_NUMBER) {
				logprintf(LOG_ERR, "config setting \"%s\" must contain a number larget than 0", jsettings->key);
				have_error = 1;
				goto clear;
			} else if(jsettings->number_ < 0) {
				logprintf(LOG_ERR, "config setting \"%s\" must contain a number larger than 0", jsettings->key);
				have_error = 1;
				goto clear;
			} else {
				web_port = (int)jsettings->number_;
				settings_add_number(jsettings->key, (int)jsettings->number_);
			}
		} else if(strcmp(jsettings->key, "webserver-root") == 0) {
			if(jsettings->tag != JSON_STRING) {
				logprintf(LOG_ERR, "config setting \"%s\" must contain a valid path", jsettings->key);
				have_error = 1;
				goto clear;
			} else if(!jsettings->string_ || path_exists(jsettings->string_) != 0) {
				logprintf(LOG_ERR, "config setting \"%s\" must contain a valid path", jsettings->key);
				have_error = 1;
				goto clear;
			} else {
				webgui_root = REALLOC_OR_EXIT(webgui_root, strlen(jsettings->string_)+1);
				strcpy(webgui_root, jsettings->string_);
				settings_add_string(jsettings->key, jsettings->string_);
			}
		} else if(strcmp(jsettings->key, "webserver-enable") == 0) {
			if(jsettings->tag != JSON_NUMBER) {
				logprintf(LOG_ERR, "config setting \"%s\" must be either 0 or 1", jsettings->key);
				have_error = 1;
				goto clear;
			} else if(jsettings->number_ < 0 || jsettings->number_ > 1) {
				logprintf(LOG_ERR, "config setting \"%s\" must be either 0 or 1", jsettings->key);
				have_error = 1;
				goto clear;
			} else {
				settings_add_number(jsettings->key, (int)jsettings->number_);
			}
		} else if(strcmp(jsettings->key, "webserver-cache") == 0 ||
		          strcmp(jsettings->key, "webgui-websockets") == 0) {
			if(jsettings->tag != JSON_NUMBER) {
				logprintf(LOG_ERR, "config setting \"%s\" must be either 0 or 1", jsettings->key);
				have_error = 1;
				goto clear;
			} else if(jsettings->number_ < 0 || jsettings->number_ > 1) {
				logprintf(LOG_ERR, "config setting \"%s\" must be either 0 or 1", jsettings->key);
				have_error = 1;
				goto clear;
			} else {
				settings_add_number(jsettings->key, (int)jsettings->number_);
			}
		} else if(strcmp(jsettings->key, "webserver-authentication") == 0 && jsettings->tag == JSON_ARRAY) {
			const JsonNode *jtmp = json_first_child(jsettings);
			unsigned short i = 0;
			while(jtmp) {
				i++;
				if(jtmp->tag == JSON_STRING) {
					if(i == 1) {
						settings_add_string("webserver-authentication-username", jtmp->string_);
					} else if(i == 2) {
						settings_add_string("webserver-authentication-password", jtmp->string_);
					}
				} else {
					have_error = 1;
					break;
				}
				if(i > 2) {
					have_error = 1;
					break;
				}
				jtmp = jtmp->next;
			}
			if(i != 2 || have_error == 1) {
				logprintf(LOG_ERR, "config setting \"%s\" must be in the format of [ \"username\", \"password\" ]", jsettings->key);
				have_error = 1;
				goto clear;
			}
#endif // WEBSERVER
		} else if(strcmp(jsettings->key, "ntp-servers") == 0 && jsettings->tag == JSON_ARRAY) {
			const JsonNode *jtmp = json_first_child(jsettings);
			unsigned short i = 0;
			char name[25];
			while(jtmp) {
				if(jtmp->tag == JSON_STRING) {
					sprintf(name, "ntpserver%d", i);
					settings_add_string(name, jtmp->string_);
				} else {
					have_error = 1;
					break;
				}
				jtmp = jtmp->next;
				i++;
			}
			if(have_error == 1) {
				logprintf(LOG_ERR, "config setting \"%s\" must be in the format of [ \"0.eu.pool.ntp.org\", ... ]", jsettings->key);
				have_error = 1;
				goto clear;
			}
		} else if(strcmp(jsettings->key, "protocol-root") == 0 ||
							strcmp(jsettings->key, "hardware-root") == 0 ||
							strcmp(jsettings->key, "actions-root") == 0 ||
							strcmp(jsettings->key, "functions-root") == 0 ||
							strcmp(jsettings->key, "operators-root") == 0) {
			if(jsettings->tag != JSON_STRING) {
				logprintf(LOG_ERR, "config setting \"%s\" must contain a valid path", jsettings->key);
				have_error = 1;
				goto clear;
			} else if(!jsettings->string_ || path_exists(jsettings->string_) != 0) {
				logprintf(LOG_ERR, "config setting \"%s\" must contain a valid path", jsettings->key);
				have_error = 1;
				goto clear;
			} else {
				settings_add_string(jsettings->key, jsettings->string_);
			}
#ifdef EVENTS
		} else if(strcmp(jsettings->key, "smtp-sender") == 0) {
			if(jsettings->tag != JSON_STRING) {
				logprintf(LOG_ERR, "config setting \"%s\" must contain an e-mail address", jsettings->key);
				have_error = 1;
				goto clear;
			} else if(jsettings->string_ == NULL) {
				logprintf(LOG_ERR, "config setting \"%s\" must contain an e-mail address", jsettings->key);
				have_error = 1;
				goto clear;
			} else if(strlen(jsettings->string_) > 0) {
#if !defined(__FreeBSD__) && !defined(_WIN32)
				char validate[] = "^[a-zA-Z0-9_.]+@([a-zA-Z0-9]+\\.)+([a-zA-Z0-9]{2,3}){1,2}$";
				reti = regcomp(&regex, validate, REG_EXTENDED);
				if(reti) {
					logprintf(LOG_ERR, "could not compile regex for %s", jsettings->key);
					have_error = 1;
					goto clear;
				}
				reti = regexec(&regex, jsettings->string_, 0, NULL, 0);
				if(reti == REG_NOMATCH || reti != 0) {
					logprintf(LOG_ERR, "config setting \"%s\" must contain an e-mail address", jsettings->key);
					have_error = 1;
					regfree(&regex);
					goto clear;
				}
				regfree(&regex);
#endif
			settings_add_string(jsettings->key, jsettings->string_);
			}
		} else if(strcmp(jsettings->key, "smtp-user") == 0) {
			if(jsettings->tag != JSON_STRING) {
				logprintf(LOG_ERR, "config setting \"%s\" must contain a user id", jsettings->key);
				have_error = 1;
				goto clear;
			} else if(jsettings->string_ == NULL) {
				logprintf(LOG_ERR, "config setting \"%s\" must contain a user id", jsettings->key);
				have_error = 1;
				goto clear;
			} else {
				settings_add_string(jsettings->key, jsettings->string_);
			}
		} else if(strcmp(jsettings->key, "smtp-password") == 0) {
			if(jsettings->tag != JSON_STRING) {
				logprintf(LOG_ERR, "config setting \"%s\" must contain a password string", jsettings->key);
				have_error = 1;
				goto clear;
			} else if(jsettings->string_ == NULL) {
				logprintf(LOG_ERR, "config setting \"%s\" must contain a password string", jsettings->key);
				have_error = 1;
				goto clear;
			} else {
				settings_add_string(jsettings->key, jsettings->string_);
			}
		} else if(strcmp(jsettings->key, "smtp-host") == 0) {
			if(jsettings->tag != JSON_STRING) {
				logprintf(LOG_ERR, "config setting \"%s\" must contain an smtp host address", jsettings->key);
				have_error = 1;
				goto clear;
			} else if(jsettings->string_ == NULL) {
				logprintf(LOG_ERR, "config setting \"%s\" must contain an smtp host address", jsettings->key);
				have_error = 1;
				goto clear;
			} else if(strlen(jsettings->string_) > 0) {
#if !defined(__FreeBSD__) && !defined(_WIN32)
				char validate[] = "^([a-zA-Z0-9\\_\\-]){2,20}(\\.([a-zA-Z0-9\\_\\-]){2,20}){2,3}$";
				reti = regcomp(&regex, validate, REG_EXTENDED);
				if(reti) {
					logprintf(LOG_ERR, "could not compile regex for %s", jsettings->key);
					have_error = 1;
					goto clear;
				}
				reti = regexec(&regex, jsettings->string_, 0, NULL, 0);
				if(reti == REG_NOMATCH || reti != 0) {
					logprintf(LOG_ERR, "config setting \"%s\" must contain an smtp host address", jsettings->key);
					have_error = 1;
					regfree(&regex);
					goto clear;
				}
				regfree(&regex);
#endif
				settings_add_string(jsettings->key, jsettings->string_);
			}
		} else if(strcmp(jsettings->key, "smtp-port") == 0) {
			if(jsettings->tag != JSON_NUMBER) {
				logprintf(LOG_ERR, "config setting \"%s\" must be 25, 465 or 587", jsettings->key);
				have_error = 1;
				goto clear;
			} else if((int)jsettings->number_ != 25 && (int)jsettings->number_ != 465 && (int)jsettings->number_ != 587) {
				logprintf(LOG_ERR, "config setting \"%s\" must be 25, 465 or 587", jsettings->key);
				have_error = 1;
				goto clear;
			} else {
				settings_add_number(jsettings->key, (int)jsettings->number_);
			}
#endif //EVENTS
		} else if(strcmp(jsettings->key, "receive-while-sending") == 0) {
			if(jsettings->tag == JSON_NUMBER) {
				settings_add_number(jsettings->key, (int)jsettings->number_);
			} else {
				logprintf(LOG_ERR, "config setting \"%s\" must be a number (0 disables, 1 enables)", jsettings->key);
				have_error = 1;
				goto clear;
			}
		} else {
			logprintf(LOG_ERR, "config setting \"%s\" is invalid", jsettings->key);
			have_error = 1;
			goto clear;
		}
	}

#ifdef WEBSERVER
	if(web_port == own_port) {
		logprintf(LOG_ERR, "config setting \"port\" and \"webserver-http-port\" cannot be the same");
		have_error = 1;
		goto clear;
	}
#ifdef WEBSERVER_HTTPS
	if(web_ssl_port == own_port) {
		logprintf(LOG_ERR, "config setting \"port\" and \"webserver-https-port\" cannot be the same");
		have_error = 1;
		goto clear;
	}
#endif

#endif
clear:
#ifdef WEBSERVER
	FREE(webgui_root);
#endif
	return have_error;
}

static JsonNode *settings_sync(int level, const char *display) {
	struct JsonNode *root = json_mkobject();
	struct JsonNode *ntpservers = NULL;
	struct settings_t *tmp = NULL;
	char *username = NULL, *password = NULL;

	for (tmp = settings; tmp; tmp = tmp->next) {
		if(tmp->type == JSON_STRING && strncmp(tmp->name, "ntpserver", 9) == 0) {
			if(ntpservers == NULL) {
				ntpservers = json_mkarray();
			}
			json_append_element(ntpservers, json_mkstring(tmp->string_));
		} else {
			if(ntpservers != NULL && json_find_member(root, "ntp-servers") == NULL) {
				json_append_member(root, "ntp-servers", ntpservers);
			}
			if(tmp->type == JSON_STRING && strcmp(tmp->name, "webserver-authentication-username") == 0) {
				username = tmp->string_;
			} else if(tmp->type == JSON_STRING && strcmp(tmp->name, "webserver-authentication-password") == 0) {
				password = tmp->string_;
			} else if(tmp->type == JSON_NUMBER) {
				json_append_member(root, tmp->name, json_mknumber((double)tmp->number_, 0));
			} else if(tmp->type == JSON_STRING) {
				json_append_member(root, tmp->name, json_mkstring(tmp->string_));
			}
		}
		if(username != NULL && password != NULL && json_find_member(root, "webserver-authentication") == NULL) {
			struct JsonNode *jarray = json_mkarray();
			json_append_element(jarray, json_mkstring(username));
			json_append_element(jarray, json_mkstring(password));
			json_append_member(root, "webserver-authentication", jarray);
		}
	}
	if(json_find_member(root, "ntp-servers") == NULL && ntpservers != NULL) {
		json_append_member(root, "ntp-servers", ntpservers);
	}

	return root;
}

static int settings_gc(void) {
	struct settings_t *tmp;

	while(settings) {
		tmp = settings;
		FREE(tmp->name);
		if(tmp->type == JSON_STRING || tmp->type == JSON_LINE_COMMENT || tmp->type == JSON_BLOCK_COMMENT) {
			FREE(tmp->string_);
		}
		settings = settings->next;
		FREE(tmp);
	}

	logprintf(LOG_DEBUG, "garbage collected config settings library");
	return 1;
}

void settings_init(void) {
	/* Request settings json object in main configuration */
	config_register(&config_settings, "settings");
	config_settings->readorder = 0;
	config_settings->writeorder = 3;
	config_settings->parse=&settings_parse;
	config_settings->sync=&settings_sync;
	config_settings->gc=&settings_gc;
}
