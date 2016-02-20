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

#ifndef _CONFIG_H_
#define _CONFIG_H_

#include "json.h"

#define CONFIG_INTERNAL	0
#define CONFIG_FORWARD	1
#define CONFIG_USER			2

typedef struct config_t {
	char *name;
	int (*parse)(JsonNode *);
	int readorder;
	int writeorder;
	JsonNode *(*sync)(int level, const char *media);
	int (*gc)(void);
	struct config_t *next;
} config_t;

int config_write(int level, const char *media);
int config_read(void);
int config_parse(struct JsonNode *root);
struct JsonNode *config_print(int level, const char *media);
int config_set_file(char *settfile);
void config_register(config_t **listener, const char *name);
int config_gc(void);
int config_set_file(char *settfile);
char *config_get_file(void);
void config_init(void);

/*
 *	These are macros so they can be used for allocation and traversion
 *	of various node types (as long they have a "name" and "next" member).
 *	CONFIG_APPEND_NODE_TO_LIST(node, head) and CONFIG_PREPEND_NODE_TO_LIS(node, head)
 *	are NULL pointer-safe (do nothing if node is NULL).
 */
#define CONFIG_ALLOC_UNNAMED_NODE(node) ((node) = memset(MALLOC_OR_EXIT(sizeof(*(node))), 0, sizeof(*(node))))

#define CONFIG_ALLOC_NAMED_NODE(node, the_name) do {		\
		CONFIG_ALLOC_UNNAMED_NODE(node);		\
		(node)->name = STRDUP_OR_EXIT(the_name);	\
	} while(0)

#define CONFIG_APPEND_NODE_TO_LIST(node, head) do {		\
		if ((node) == NULL) break;			\
		/* trick: use (node)->next as iterator */	\
		(node)->next = (head);				\
		if((node)->next != NULL) {			\
			while((node)->next->next != NULL)	\
				(node)->next = (node)->next->next;	\
			(node)->next->next = (node);		\
			(node)->next = NULL;			\
		} else {					\
			(head) = (node);			\
		}						\
	} while (0)

#define CONFIG_PREPEND_NODE_TO_LIST(node, head) do {		\
		if ((node) == NULL) break;			\
		(node)->next = (head);				\
		(head) = (node);				\
	} while (0)

#endif
