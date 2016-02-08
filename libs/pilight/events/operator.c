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
#include <sys/types.h>
#include <time.h>
#include <libgen.h>
#include <dirent.h>
#include <limits.h>

#include "../core/pilight.h"
#include "../core/common.h"
#include "../core/dso.h"
#include "../core/log.h"
#include "../config/settings.h"

#include "operator.h"
#include "operators/operator_header.h"

#ifndef _WIN32
void event_operator_remove(char *name) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	struct event_operators_t *currP, *prevP;

	prevP = NULL;

	for(currP = event_operators; currP != NULL; prevP = currP, currP = currP->next) {

		if(strcmp(currP->name, name) == 0) {
			if(prevP == NULL) {
				event_operators = currP->next;
			} else {
				prevP->next = currP->next;
			}

			logprintf(LOG_DEBUG, "removed operator %s", currP->name);
			FREE(currP->name);
			FREE(currP);

			break;
		}
	}
}
#endif

void event_operator_init(void) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	#include "operators/operator_init.h"

#ifndef _WIN32
	void *handle = NULL;
	void (*init)(void);
	void (*compatibility)(struct module_t *module);
	char path[PATH_MAX];
	struct module_t module;
	const char pilight_version[] = PILIGHT_VERSION, *stmp = NULL;
	char pilight_commit[3] = { 0 };
	char *operator_root = NULL;
	int check1 = 0, check2 = 0, valid = 1;

	struct dirent *file = NULL;
	DIR *d = NULL;
	struct stat s;

	if(settings_find_string("operators-root", &stmp) != 0) {
		/* If no operator root was set, use the default operator root */
		stmp = OPERATOR_ROOT;
	}
        size_t len = strlen(stmp);
        if (stmp[len - 1] == '/') {
                operator_root = STRDUP_OR_EXIT(stmp);
        } else {
                char b[len + 2];
                strcpy(b, stmp); strcat(b, "/");
                operator_root = STRDUP_OR_EXIT(b);
        }

	if((d = opendir(operator_root))) {
		while((file = readdir(d)) != NULL) {
			memset(path, '\0', PATH_MAX);
			sprintf(path, "%s%s", operator_root, file->d_name);
			if(stat(path, &s) == 0) {
				/* Check if file */
				if(S_ISREG(s.st_mode)) {
					if(strstr(file->d_name, ".so") != NULL) {
						valid = 1;

						if((handle = dso_load(path)) != NULL) {
							init = dso_function(handle, "init");
							compatibility = dso_function(handle, "compatibility");
							if(init != NULL && compatibility != NULL) {
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
										char tmp[strlen(module.name)+1];
										strcpy(tmp, module.name);
										event_operator_remove(tmp);
										init();
										logprintf(LOG_DEBUG, "loaded operator %s v%s", file->d_name, module.version);
									} else {
										if(module.reqcommit != NULL) {
											logprintf(LOG_ERR, "event operator %s requires at least pilight v%s (commit %s)", file->d_name, module.reqversion, module.reqcommit);
										} else {
											logprintf(LOG_ERR, "event operator %s requires at least pilight v%s", file->d_name, module.reqversion);
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
	FREE(operator_root);
#endif
}

void event_operator_register(struct event_operators_t **op, const char *name) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	CONFIG_ALLOC_NAMED_NODE(*op, name);
	(*op)->callback_string = NULL;
	(*op)->callback_number = NULL;
	CONFIG_PREPEND_NODE_TO_LIST(*op, event_operators);
}

int event_operator_gc(void) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	struct event_operators_t *tmp_operator = NULL;
	while(event_operators) {
		tmp_operator = event_operators;
		FREE(tmp_operator->name);
		event_operators = event_operators->next;
		FREE(tmp_operator);
	}
	FREE(event_operators);

	logprintf(LOG_DEBUG, "garbage collected event operator library");
	return 0;
}
