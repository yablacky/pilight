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
#include <sys/time.h>
#include <libgen.h>
#include <dirent.h>
#ifndef _WIN32
	#include <dlfcn.h>
#endif

#include "../core/pilight.h"
#include "../core/common.h"
#include "../core/config.h"
#include "../core/options.h"
#include "../core/dso.h"
#include "../core/log.h"
#include "../config/settings.h"

#include "function.h"
#include "functions/function_header.h"

#ifndef _WIN32
void event_function_remove(const char *name) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	struct event_functions_t *currP, *prevP;

	prevP = NULL;

	for(currP = event_functions; currP != NULL; prevP = currP, currP = currP->next) {

		if(strcmp(currP->name, name) == 0) {
			if(prevP == NULL) {
				event_functions = currP->next;
			} else {
				prevP->next = currP->next;
			}

			logprintf(LOG_DEBUG, "removed event function %s", currP->name);
			FREE(currP->name);
			FREE(currP);

			break;
		}
	}
}
#endif

void event_function_init(void) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	#include "functions/function_init.h"

#ifndef _WIN32
	void *handle = NULL;
	void (*init)(void);
	void (*compatibility)(struct module_t *module);
	char path[PATH_MAX];
	struct module_t module;
	const char pilight_version[] = PILIGHT_VERSION, *stmp = NULL;
	char pilight_commit[3] = { 0 };
	const char *functions_root = NULL;
	int check1 = 0, check2 = 0, valid = 1;

	struct dirent *file = NULL;
	DIR *d = NULL;
	struct stat s;

	if(settings_find_string("functions-root", &stmp) != 0) {
		/* If no function root was set, use the default function root */
		stmp = FUNCTION_ROOT;
	}
	size_t len = strlen(stmp);
	if(stmp[len-1] == '/') {
		functions_root = STRDUP_OR_EXIT(stmp);
	} else {
		char b[len + 2];
		strcpy(b, stmp); strcat(b, "/");
		functions_root = STRDUP_OR_EXIT(b);
	}

	if((d = opendir(functions_root))) {
		while((file = readdir(d)) != NULL) {
			memset(path, '\0', PATH_MAX);
			sprintf(path, "%s%s", functions_root, file->d_name);
			if(stat(path, &s) == 0) {
				/* Check if file */
				if(S_ISREG(s.st_mode)) {
					if(strstr(file->d_name, ".so") != NULL) {
						valid = 1;

						if((handle = dso_load(path)) != NULL) {
							init = dso_function(handle, "init");
							compatibility = dso_function(handle, "compatibility");
							if(init && compatibility) {
								compatibility(&module);
								if(module.name != NULL && module.version != NULL && module.reqversion != NULL) {

									if((check1 = vercmp(module.reqversion, pilight_version)) > 0) {
										valid = 0;
									}

									if(check1 == 0 && module.reqcommit != NULL) {
										sscanf(HASH, "v%*[0-9].%*[0-9]-%[0-9]-%*[0-9a-zA-Z\n\r]", pilight_commit);

										if(strlen(pilight_commit) > 0 && (check2 = vercmp(module.reqcommit, pilight_commit)) > 0) {
											valid = 0;
										}
									}
									if(valid == 1) {
										event_function_remove(module.name);
										init();
										logprintf(LOG_DEBUG, "loaded event function %s v%s", file->d_name, module.version);
									} else {
										if(module.reqcommit != NULL) {
											logprintf(LOG_ERR, "event function %s requires at least pilight v%s (commit %s)", file->d_name, module.reqversion, module.reqcommit);
										} else {
											logprintf(LOG_ERR, "event function %s requires at least pilight v%s", file->d_name, module.reqversion);
										}
									}
								} else {
									logprintf(LOG_ERR, "invalid module %s", file->d_name);
								}
							}
						}
					}
				}
			}
		}
		closedir(d);
	}
	FREE(functions_root);
#endif
}

void event_function_register(struct event_functions_t **act, const char *name) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	CONFIG_ALLOC_NAMED_NODE(*act, name);
	(*act)->run = NULL;
	CONFIG_PREPEND_NODE_TO_LIST(*act, event_functions);
}

int event_function_gc(void) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	struct event_functions_t *tmp_function = NULL;
	while(event_functions) {
		tmp_function = event_functions;
		FREE(tmp_function->name);
		event_functions = event_functions->next;
		FREE(tmp_function);
	}
	FREE(event_functions);

	logprintf(LOG_DEBUG, "garbage collected event function library");
	return 0;
}
