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
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#ifndef _WIN32
	#include <regex.h>
	#include <sys/ioctl.h>
	#include <dlfcn.h>
	#ifdef __mips__
		#define __USE_UNIX98
	#endif
	#include <pthread.h>
#endif
#include <sys/stat.h>
#include <time.h>
#include <libgen.h>
#include <dirent.h>

#include "../core/threads.h"
#include "../core/pilight.h"
#include "../core/common.h"
#include "../core/irq.h"
#include "../core/log.h"
#include "../core/json.h"
#include "../core/dso.h"
#include "settings.h"
#include "hardware.h"

#ifndef _WIN32
	#include "../../wiringx/wiringX.h"
#endif

static char *hwfile = NULL;
struct hardware_t *hardware;
struct conf_hardware_t *conf_hardware;

#include "../hardware/hardware_header.h"

#ifndef _WIN32
static void hardware_remove(const char *name) {
	struct hardware_t *currP, *prevP;

	prevP = NULL;

	for(currP = hardware; currP != NULL; prevP = currP, currP = currP->next) {

		if(strcmp(currP->id, name) == 0) {
			if(prevP == NULL) {
				hardware = currP->next;
			} else {
				prevP->next = currP->next;
			}

			logprintf(LOG_DEBUG, "removed config hardware module %s", currP->id);
			FREE(currP->id);
			options_delete(currP->options);
			FREE(currP->comment);
			FREE(currP);

			break;
		}
	}
}
#endif

void hardware_register(struct hardware_t **hw) {
	CONFIG_ALLOC_UNNAMED_NODE(*hw);
	(*hw)->options = NULL;
	(*hw)->comment = NULL;
	(*hw)->wait = 0;
	(*hw)->stop = 0;
	(*hw)->running = 0;
	(*hw)->minrawlen = 0;
	(*hw)->maxrawlen = 0;
	(*hw)->mingaplen = 0;
	(*hw)->maxgaplen = 0;

	(*hw)->init = NULL;
	(*hw)->deinit = NULL;
	(*hw)->receiveOOK = NULL;
	(*hw)->receivePulseTrain = NULL;
	(*hw)->receiveAPI = NULL;
	(*hw)->sendOOK = NULL;
	(*hw)->sendAPI = NULL;
	(*hw)->gc = NULL;
	(*hw)->settings = NULL;

	pthread_mutexattr_init(&(*hw)->attr);
	pthread_mutexattr_settype(&(*hw)->attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&(*hw)->lock, &(*hw)->attr);
	pthread_cond_init(&(*hw)->signal, NULL);

	CONFIG_PREPEND_NODE_TO_LIST(*hw, hardware);
}

void hardware_set_id(hardware_t *hw, const char *id) {
	hw->id = STRDUP_OR_EXIT(id);
}

static int hardware_gc(void) {
	struct hardware_t *htmp = hardware;
	struct conf_hardware_t *ctmp = NULL;

	while(hardware) {
		htmp = hardware;
		htmp->stop = 1;
		pthread_mutex_unlock(&htmp->lock);
		pthread_cond_signal(&htmp->signal);
		while(htmp->running == 1) {
			usleep(10);
		}
		thread_stop(htmp->id);
		if(htmp->deinit != NULL) {
			htmp->deinit();
		}
		if(htmp->gc != NULL) {
			htmp->gc();
		}
		FREE(htmp->id);
		options_delete(htmp->options);
		FREE(htmp->comment);
		hardware = hardware->next;
		FREE(htmp);
	}
	if(hardware != NULL) {
		FREE(hardware);
	}

	while(conf_hardware) {
		ctmp = conf_hardware;
		conf_hardware = conf_hardware->next;
		FREE(ctmp);
	}

	if(hwfile != NULL) {
		FREE(hwfile);
	}

	logprintf(LOG_DEBUG, "garbage collected config hardware library");
	return EXIT_SUCCESS;
}

static JsonNode *hardware_sync(int level, const char *display) {
	struct conf_hardware_t *tmp = conf_hardware;
	struct JsonNode *root = json_mkobject();
	while(tmp) {
		json_append_comment(root, tmp->hardware->comment);
		struct JsonNode *module = json_mkobject();
		struct options_t *options = tmp->hardware->options;
		while(options) {
			json_append_comment(module, options->comment);
			if(options->vartype == JSON_NUMBER) {
				json_append_member(module, options->name, json_mknumber(options->number_, 0));
			} else if(options->vartype == JSON_STRING) {
				json_append_member(module, options->name, json_mkstring(options->string_));
			}
			options = options->next;
		}
		json_append_member(root, tmp->hardware->id, module);
		tmp = tmp->next;
	}

	return root;
}

static int hardware_parse(const JsonNode *root) {
	struct conf_hardware_t *hnode = NULL;
	struct conf_hardware_t *tmp_confhw = NULL;
	struct options_t *hw_option = NULL;
	struct hardware_t *hw = NULL;

	const JsonNode *jmodule = NULL;
	const JsonNode *jvalues = NULL;

	int i = 0, have_error = 0;

	const char *hw_commentv[10]; size_t hw_commentn = 0;
	json_foreach_and_all(jmodule, root) {
		if(json_is_comment(jmodule)) {
			if(hw_commentn < countof(hw_commentv)) {
				hw_commentv[hw_commentn++] = jmodule->string_;
			}
			continue;
		}
		i++;
		/* A hardware module can only be a JSON object */
		if(jmodule->tag != JSON_OBJECT) {
			char *json = json_encode(jmodule);
			logprintf(LOG_ERR, "config hardware module #%d invalid format. Expected { ... } found %s", i, json);
			json_free(json);
			have_error = 1;
			goto clear;
		}
		/* Check if defined hardware module exists */
		for(hw = hardware; hw; hw = hw->next) {
			if(strcmp(hw->id, jmodule->key) == 0) {
				break;
			}
		
		}
		if(hw == NULL) {
			logprintf(LOG_ERR, "config hardware module #%d \"%s\" does not exist", i, jmodule->key);
			have_error = 1;
			goto clear;
		}
		if(hw->options == NULL) {
			logprintf(LOG_ERR, "config hardware module #%d \"%s\" exist but is no properly defined", i, jmodule->key);
			have_error = 1;
			goto clear;
		}

		/* Check for duplicate hardware modules */
		for(tmp_confhw = conf_hardware; tmp_confhw; tmp_confhw = tmp_confhw->next) {
			/* Only allow one module of the same name */
			if(strcmp(tmp_confhw->hardware->id, jmodule->key) == 0) {
				logprintf(LOG_ERR, "config hardware module #%d \"%s\", duplicate", i, jmodule->key);
				have_error = 1;
				goto clear;
			}
			/* And only allow one module covering the same frequency */
			if(tmp_confhw->hardware->hwtype == hw->hwtype) {
				logprintf(LOG_ERR, "config hardware module #%d \"%s\", duplicate freq.", i, jmodule->key);
				have_error = 1;
				goto clear;
			}
		}

		/* Check if all options required by the hardware module are present */
		for(hw_option = hw->options->next; hw_option; hw_option = hw_option->next) {
			if(hw_option->argtype == OPTION_HAS_VALUE) {
				jvalues = json_find_member(jmodule, hw_option->name);
			} else jvalues = NULL;	// ????

			if(jvalues == NULL) {
				logprintf(LOG_ERR, "config hardware module #%d \"%s\", setting \"%s\" missing", i, jmodule->key, hw_option->name);
				have_error = 1;
				goto clear;
			}
			/* Check if setting contains a valid value */
			if(hw_option->mask != NULL) {
				char btmp[64] = { 0 }; const char *stmp = btmp;

				if(jvalues->tag == JSON_NUMBER) {
					sprintf(btmp, "%d", (int)jvalues->number_);
				} else if(jvalues->tag == JSON_STRING) {
					stmp = jvalues->string_ ? jvalues->string_ : btmp;
				} else {
					logprintf(LOG_ERR, "config hardware module #%d \"%s\", setting \"%s\" is not number or string", i, jmodule->key, hw_option->name);
					have_error = 1;
					goto clear;
				}
#if !defined(__FreeBSD__) && !defined(_WIN32)
				regex_t regex;
				int reti;

				reti = regcomp(&regex, hw_option->mask, REG_EXTENDED);
				if(reti) {
					logprintf(LOG_ERR, "could not compile regex");
					exit(EXIT_FAILURE);
				}
				reti = regexec(&regex, stmp, 0, NULL, 0);
				if(reti == REG_NOMATCH || reti != 0) {
					logprintf(LOG_ERR, "config hardware module #%d \"%s\", setting \"%s\" invalid", i, jmodule->key, hw_option->name);
					have_error = 1;
					regfree(&regex);
					goto clear;
				}
				regfree(&regex);
#endif
			}
		}

		/* Check for any settings that are not valid for this hardware module */
		const char *opt_commentv[10]; size_t opt_commentn = 0;
		hw_option = NULL;
		json_foreach_and_all(jvalues, jmodule) {
			if(json_is_comment(jvalues)) {
				if(opt_commentn < countof(opt_commentv)) {
					opt_commentv[opt_commentn++] = jvalues->string_;
				}
				continue;
			}
			if(jvalues->tag == JSON_NUMBER || jvalues->tag == JSON_STRING) {
				for(hw_option = hw->options; hw_option; hw_option = hw_option->next) {
					if(strcmp(jvalues->key, hw_option->name) == 0 && jvalues->tag == hw_option->vartype) {
						break;
					}
				}
				if(hw_option == NULL) {
					logprintf(LOG_ERR, "config hardware module #%d \"%s\", setting \"%s\" invalid", i, jmodule->key, jvalues->key);
					have_error = 1;
					goto clear;
				}
				if(hw_option->vartype == JSON_NUMBER) {
					options_set_number(&hw_option, hw_option->id, jvalues->number_);
				} else if(hw_option->vartype == JSON_STRING) {
					options_set_string(&hw_option, hw_option->id, jvalues->string_);
				}
				if(opt_commentn > 0) {
					FREE(hw_option->comment);
					hw_option->comment = str_join("\n", opt_commentn, opt_commentv);
					opt_commentn = 0; 
				}
			}
			opt_commentn = 0; 
		}
		if(opt_commentn > 0) {
			// no place where to assign that trailing comment ...
			logprintf(LOG_WARNING, "config hardware module #%d \"%s\": comments behind last option dismissed", i, jmodule->key );
		}

		if(hw->settings != NULL) {
			/* Sync all settings with the hardware module */
			json_foreach(jvalues, jmodule) {
				if(hw->settings(jvalues) == EXIT_FAILURE) {
					logprintf(LOG_ERR, "config hardware module #%d \"%s\", setting \"%s\" invalid", i, jmodule->key, jvalues->key);
					have_error = 1;
					goto clear;
				}
			}
		}

		if(hw_commentn > 0) {
			FREE(hw->comment);
			hw->comment = str_join("\n", hw_commentn, hw_commentv);
			hw_commentn = 0;
		}

		hnode = MALLOC_OR_EXIT(sizeof(*hnode));
		hnode->hardware = hw;
		hnode->next = conf_hardware;
		conf_hardware = hnode;
		hw_commentn = 0;
	}

	if(hw_commentn > 0) {
		// no place where to assign that trailing comment ...
		logprintf(LOG_WARNING, "config hardware: comments behind last module definition dismissed");
	}

clear:
	return have_error;
}

void hardware_init(void) {
	/* Request hardware json object in main configuration */
	config_register(&config_hardware, "hardware");
	config_hardware->readorder = 4;
	config_hardware->writeorder = 4;
	config_hardware->parse=&hardware_parse;
	config_hardware->sync=&hardware_sync;
	config_hardware->gc=&hardware_gc;

	#include "../hardware/hardware_init.h"

#ifndef _WIN32
	void *handle = NULL;
	void (*init)(void);
	void (*compatibility)(struct module_t *module);
	struct module_t module;
	const char pilight_version[] = PILIGHT_VERSION;
	char *hardware_root = NULL;
	const char *tmpstr = NULL;
	int check1 = 0, check2 = 0, valid = 1;

	struct dirent *file = NULL;
	DIR *d = NULL;
	struct stat s;

	if(settings_find_string("hardware-root", &tmpstr) != 0) {
		/* If no hardware root was set, use the default hardware root */
		tmpstr =  HARDWARE_ROOT;
	}
	size_t len = strlen(tmpstr);
	hardware_root = MALLOC_OR_EXIT(len + 2);	// 1 for trailing '/' we may add.
	strcpy(hardware_root, tmpstr);
	if(hardware_root[len-1] != '/') {
		strcat(hardware_root, "/");
	}

	if((d = opendir(hardware_root))) {
		while((file = readdir(d)) != NULL) {
			char path[strlen(hardware_root) + strlen(file->d_name) + 1];
			strcpy(path, hardware_root); strcat(path, file->d_name);
			if(stat(path, &s) != 0) {
				continue;
			}
			/* Check if file */
			if(!S_ISREG(s.st_mode)) {
				continue;
			}
			if(strstr(file->d_name, ".so") == NULL) {
				continue;
			}
			if((handle = dso_load(path)) == NULL) {
				continue;
			}
			valid = 1;
			init = dso_function(handle, "init");
			compatibility = dso_function(handle, "compatibility");
			if(init == NULL || compatibility == NULL ) {
				continue;
			}
			compatibility(&module);
			if(module.name != NULL && module.version != NULL && module.reqversion != NULL) {
				if((check1 = vercmp(module.reqversion, pilight_version)) > 0) {
					valid = 0;
				}
				if(check1 == 0 && module.reqcommit != NULL) {
					char pilight_commit[64] = { 0 };
					sscanf(HASH, "v%*[0-9].%*[0-9]-%[0-9]-%*[0-9a-zA-Z\n\r]", pilight_commit);

					if(strlen(pilight_commit) > 0 && (check2 = vercmp(module.reqcommit, pilight_commit)) > 0) {
						valid = 0;
					}
				}

				if(valid == 1) {
					hardware_remove(module.name);
					init();
					logprintf(LOG_DEBUG, "loaded config hardware module %s v%s", file->d_name, module.version);
				} else {
					if(module.reqcommit != NULL) {
						logprintf(LOG_ERR, "config hardware module %s requires at least pilight v%s (commit %s)", file->d_name, module.reqversion, module.reqcommit);
					} else {
						logprintf(LOG_ERR, "config hardware module %s requires at least pilight v%s", file->d_name, module.reqversion);
					}
				}
			} else {
				logprintf(LOG_ERR, "invalid module %s", file->d_name);
			}
		}
		closedir(d);
	}
	FREE(hardware_root);
#endif
}
