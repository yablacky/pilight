/*
  Copyright (C) 2013 - 2014 CurlyMo (curlymoo1@gmail.com)
								2011 Joseph A. Adams (joeyadams3.14159@gmail.com)
  All rights reserved.

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
  THE SOFTWARE.
*/

#ifndef CCAN_JSON_H
#define CCAN_JSON_H

#include <stdbool.h>
#include <stddef.h>

#define JSON_TAG_UNDEFINED	(0)
#define JSON_NULL		(0x1)
#define JSON_BOOL		(0x2)
#define JSON_STRING		(0x4)
#define JSON_NUMBER		(0x8)
#define JSON_ARRAY		(0x16)
#define JSON_OBJECT		(0x23)
#define JSON_LINE_COMMENT	(0x32)
#define JSON_BLOCK_COMMENT	(0x33)
#define JSON_TAG_HIGH_VALUE	(0x33)

#define JsonTag			int

typedef struct JsonNode JsonNode;

struct JsonNode
{
	/* only if parent is an object or array (NULL otherwise) */
	JsonNode *parent;
	JsonNode *prev, *next;

	/* only if parent is an object (NULL otherwise) */
	char *key; /* Must be valid UTF-8. */

	JsonTag tag;
	union {
		/* JSON_BOOL */
		bool bool_;

		/* JSON_STRING */
		/* JSON_LINE_COMMENT */
		/* JSON_BLOCK_COMMENT */
		char *string_; /* Must be valid UTF-8. */

		/* JSON_NUMBER */
		double number_;

		/* JSON_ARRAY */
		/* JSON_OBJECT */
		struct {
			JsonNode *head, *tail;
		} children;
	};
	int decimals_;
};

/*** Encoding, decoding, and validation ***/

#define JSON_WANT_EMBEDDED_COMMENTS ((JsonNode**) -1)	// special comment value for json_decode_ex()

JsonNode   *json_decode          (const char *json);
JsonNode   *json_decode_ex       (const char *json, const char **problem,
					JsonNode **comments); // delivers problem and comments.
int         json_get_line_number (const char *json, const char *problem_from_json_decode_ex,
					int *position_in_line);

char       *json_encode          (const JsonNode *node);
char       *json_encode_string   (const char *str);
char       *json_stringify       (const JsonNode *node, const char *space);
char       *json_stringify_ex    (const JsonNode *node, const char *space,
					const JsonNode *comments_from_json_decode_ex);

bool        json_validate        (const char *json, const char **problem);

/*** Lookup and traversal ***/

#define json_is_container(node) ((node)->tag == JSON_ARRAY || (node)->tag == JSON_OBJECT)
#define json_is_comment(node)	((node)->tag == JSON_LINE_COMMENT || (node)->tag == JSON_BLOCK_COMMENT)

const JsonNode	*json_find_element   (const JsonNode *array_or_object, int index);
const JsonNode	*json_find_member    (const JsonNode *object_or_array, const char *key);
const JsonNode	*json_first_child    (const JsonNode *node);

bool json_get_number(const JsonNode *object_or_array, const char *name, double *out);
bool json_get_string(const JsonNode *object_or_array, const char *name, const char **out);

#define json_foreach_and_all(i, object_or_array)        \
        for ((i) = json_first_child(object_or_array);   \
                 (i) != NULL;                           \
                 (i) = (i) ? (i)->next                  \
                   : json_first_child(object_or_array))

#define json_foreach(i, object_or_array) json_foreach_and_all(i, object_or_array) \
                                             if (json_is_comment(i)) continue; else
                                         

/*** Construction and manipulation ***/

JsonNode *json_mknull(void);
JsonNode *json_mkbool(bool b);
JsonNode *json_mkstring(const char *s);
JsonNode *json_mknumber(double n, int decimals);
JsonNode *json_mkarray(void);
JsonNode *json_mkobject(void);

JsonNode *json_append_element(JsonNode *array, JsonNode *element);	// returns the added element
JsonNode *json_prepend_element(JsonNode *array, JsonNode *element);	// returns the added element
JsonNode *json_append_member(JsonNode *object, const char *key, JsonNode *value);	// returns the added element
JsonNode *json_prepend_member(JsonNode *object, const char *key, JsonNode *value);	// returns the added element
JsonNode *json_append_comment(JsonNode *target, const char *comment);	// returns the added element

bool json_convert_type_force(const JsonNode *node, JsonTag new_type);
bool json_convert_type      (      JsonNode *node, JsonTag new_type);

      JsonNode *json_remove_from_parent      (      JsonNode *node);	// returns node->prev.
const JsonNode *json_remove_from_parent_force(const JsonNode *node);	// returns node->prev.

      JsonNode *json_delete      (      JsonNode *node);	// returns node->prev. Allows to delete in json_foreach().
const JsonNode *json_delete_force(const JsonNode *node);	// returns node->prev. Allows to delete in json_foreach().

JsonNode *json_strip_comments(JsonNode *root, JsonNode *collector_object); // returns new root.

void json_free(void *a);

/*** Debugging ***/

/*
 * Look for structure and encoding problems in a JsonNode or its descendents.
 *
 * If a problem is detected, return false, writing a description of the problem
 * to errmsg (unless errmsg is NULL).
 */
bool json_check(const JsonNode *node, char errmsg[256]);

#endif
