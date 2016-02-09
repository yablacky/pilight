/*
  Copyright (C) 2011 Joseph A. Adams (joeyadams3.14159@gmail.com)
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
#if 0
  Features added 2016 by Lutz Schwarz (schwarz.ware@gmx.de):

  (1)	The JSON parser now accepts dangling commas so [1,] is allowed
	and is the same as [1] and { "foo": "bar", } is ok accordingly.
	Dangling commas are simply ignored, e.g. they will not generated
	when stringifying the parsed result.

  (2)	The JSON parser now accepts comments at some usual (not all)
	positions in the json code. Comments may be written as block
	comments /* ... */ or C++ style comments which are from //
	up to end of line. Nested block comments are not allowed.

	In particular, comments are allowed just before array elements
	and before object members or as only or last comment in an
	container, like this:
	[ // An array starts here.
		// The next is an empty object:
		{ },

		/*
		** This is a multi line comment and it "belongs"
		** to the next item in the array:
		*/
		"this is the commented item",

		// And now an object with commented members:
		{
			// this comment "belongs" to "name"
			"name": "foo",
			"age": 88, // surprise: comment belongs to "income"!
			/* The income is
			   confidential, of course. */
			"income": 0
		},
		// To keep this the last comment in the array, add
		// more elements just in front of this comment.
	]

	Examples for positions that are not allowed:
	{
		"foo" /* bad-place */ : /* also bad */ "bar",

		[ "baz" /* not a good place */ , "boo" ],
		[ "bar" /* place ok if last */ ]
	},

	The JSON parser can deliver the resulting json-node-tree
	with embedded comments which are then simply added to the
	enclosing container (like cockoo-s egs) just as regular
	members.
	Code that traverses the json-node-tree must care for them.
	There is a json_is_comment(node) to do so. json_foreach() does
	not deliver comment enries. But json_foreach_and_all() does.

	The json comments may also stripped off from the json-node-tree
	all at once, can be stored separately (it is a json object as
	well) and can be merged in again when stringifying.  Thereby
	all matching comments (by structure depth, name and/or index)
	are merged in, all others are silently ignored.

	Use json_decode_ex() and json_stringify_ex() if you like to
	deal with comments. For downward compatibility the previous
	function json_decode() and json_stringify() behave as usual
	(and do not deliver nor merge-in comments).

	The comment style (/* */ or //....) is remembered when parsed
	and will be used on stringifying if possible. Should the
	comment text contain characters or character sequences that
	would render the generated comment un-parsable, a suitable
	style is used. So it is safe to programmatically modify or
	insert comments of arbitrary comment text.

  (3)	The JSON parser now returns the position of the error in
	case the json code could not be parsed. For convenience
	there is a json_get_line_number() to find the line number.
	
#endif

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "mem.h"

#ifdef MALLOC_OR_EXIT

/*
 * Assume these macros are also defined:
	REALLOC_OR_EXIT(p, len)
	STRDUP_OR_EXIT(str)
	FREE(p)
 */

#else

#define out_of_memory() do {                    \
		fprintf(stderr, "Out of memory.\n");    \
		exit(EXIT_FAILURE);                     \
	} while (0)

static void *json_malloc(unsigned int *len)
{
	void *b = malloc(a, len);
	if (b == NULL)
		out_of_memory();
	return b;
}

static void *json_realloc(void *a, unsigned int *len)
{
	void *b = realloc(a, len);
	if (b == NULL)
		out_of_memory();
	return b;
}

/* Sadly, strdup is not portable. */
static char *json_strdup(const char *str)
{
	return strcpy(json_malloc(strlen(str)+1), str);
}

#define MALLOC_OR_EXIT(len)     json_malloc(len)
#define REALLOC_OR_EXIT(p, len) json_realloc(p, len)
#define STRDUP_OR_EXIT(str)     json_strdup(str)
#define FREE(p) free(p)

#endif

/* String buffer */

typedef struct
{
	char *cur;
	char *end;
	char *start;
	const char *space;
} SB;

static void sb_init(SB *sb)
{
	sb->start = (char*) MALLOC_OR_EXIT(17);
	memset(sb->start, 0, 17);
	sb->cur = sb->start;
	sb->end = sb->start + 16;
	sb->space = NULL;
}

/* sb and need may be evaluated multiple times. */
#define sb_need(sb, need) do {                  \
		if ((sb)->end - (sb)->cur < (need))     \
			sb_grow(sb, need);                  \
	} while (0)

static void sb_grow(SB *sb, int need)
{
	size_t length = sb->cur - sb->start;
	size_t alloc = sb->end - sb->start;

	do {
		alloc *= 2;
	} while (alloc < length + need);

	sb->start = (char*) REALLOC_OR_EXIT(sb->start, alloc + 1);
	sb->cur = sb->start + length;
	sb->end = sb->start + alloc;
}

static void sb_put(SB *sb, const char *bytes, int count)
{
	sb_need(sb, count);
	memcpy(sb->cur, bytes, count);
	sb->cur += count;
}

#define sb_putc(sb, c) do {         \
		if ((sb)->cur >= (sb)->end) \
			sb_grow(sb, 1);         \
		*(sb)->cur++ = (c);         \
	} while (0)

static void sb_puts(SB *sb, const char *str)
{
	sb_put(sb, str, strlen(str));
}

static void sb_put_space(SB *sb, int times)
{
	if (sb->space)
		while(--times >= 0)
			sb_puts(sb, sb->space);
}

static size_t sb_size(const SB *sb)
{
	assert(sb->cur >= sb->start);
	return sb->cur - sb->start;
}

static char *sb_finish(SB *sb)
{
	*sb->cur = 0;
	// The next assertion does not hold, if there was a sb_putc(sb, 0)
	// or sb_put(sb, "", 1). But ok, who does this?
	assert(sb->start <= sb->cur && strlen(sb->start) == sb_size(sb));
	return sb->start;
}

static void sb_free(SB *sb)
{
	FREE(sb->start);
	sb->space = sb->start = sb->cur = sb->end = NULL;
}

/*
 * Unicode helper functions
 *
 * These are taken from the ccan/charset module and customized a bit.
 * Putting them here means the compiler can (choose to) inline them,
 * and it keeps ccan/json from having a dependency.
 */

/*
 * Type for Unicode codepoints.
 * We need our own because wchar_t might be 16 bits.
 */
typedef uint32_t uchar_t;

/*
 * Validate a single UTF-8 character starting at @s.
 * The string must be null-terminated.
 *
 * If it's valid, return its length (1 thru 4).
 * If it's invalid or clipped, return 0.
 *
 * This function implements the syntax given in RFC3629, which is
 * the same as that given in The Unicode Standard, Version 6.0.
 *
 * It has the following properties:
 *
 *  * All codepoints U+0000..U+10FFFF may be encoded,
 *    except for U+D800..U+DFFF, which are reserved
 *    for UTF-16 surrogate pair encoding.
 *  * UTF-8 byte sequences longer than 4 bytes are not permitted,
 *    as they exceed the range of Unicode.
 *  * The sixty-six Unicode "non-characters" are permitted
 *    (namely, U+FDD0..U+FDEF, U+xxFFFE, and U+xxFFFF).
 */
static int utf8_validate_cz(const char *s)
{
	unsigned char c = *s++;

	if (c <= 0x7F) {        /* 00..7F */
		return 1;
	} else if (c <= 0xC1) { /* 80..C1 */
		/* Disallow overlong 2-byte sequence. */
		return 0;
	} else if (c <= 0xDF) { /* C2..DF */
		/* Make sure subsequent byte is in the range 0x80..0xBF. */
		if (((unsigned char)*s++ & 0xC0) != 0x80)
			return 0;

		return 2;
	} else if (c <= 0xEF) { /* E0..EF */
		/* Disallow overlong 3-byte sequence. */
		if (c == 0xE0 && (unsigned char)*s < 0xA0)
			return 0;

		/* Disallow U+D800..U+DFFF. */
		if (c == 0xED && (unsigned char)*s > 0x9F)
			return 0;

		/* Make sure subsequent bytes are in the range 0x80..0xBF. */
		if (((unsigned char)*s++ & 0xC0) != 0x80)
			return 0;
		if (((unsigned char)*s++ & 0xC0) != 0x80)
			return 0;

		return 3;
	} else if (c <= 0xF4) { /* F0..F4 */
		/* Disallow overlong 4-byte sequence. */
		if (c == 0xF0 && (unsigned char)*s < 0x90)
			return 0;

		/* Disallow codepoints beyond U+10FFFF. */
		if (c == 0xF4 && (unsigned char)*s > 0x8F)
			return 0;

		/* Make sure subsequent bytes are in the range 0x80..0xBF. */
		if (((unsigned char)*s++ & 0xC0) != 0x80)
			return 0;
		if (((unsigned char)*s++ & 0xC0) != 0x80)
			return 0;
		if (((unsigned char)*s++ & 0xC0) != 0x80)
			return 0;

		return 4;
	} else {                /* F5..FF */
		return 0;
	}
}

/* Validate a null-terminated UTF-8 string. */
static bool utf8_validate(const char *s)
{
	int len;

	for (; *s != 0; s += len) {
		len = utf8_validate_cz(s);
		if (len == 0)
			return false;
	}

	return true;
}

/*
 * Read a single UTF-8 character starting at @s,
 * returning the length, in bytes, of the character read.
 *
 * This function assumes input is valid UTF-8,
 * and that there are enough characters in front of @s.
 */
static int utf8_read_char(const char *s, uchar_t *out)
{
	const unsigned char *c = (const unsigned char*) s;

	assert(utf8_validate_cz(s));

	if (c[0] <= 0x7F) {
		/* 00..7F */
		*out = c[0];
		return 1;
	} else if (c[0] <= 0xDF) {
		/* C2..DF (unless input is invalid) */
		*out = ((uchar_t)c[0] & 0x1F) << 6 |
		       ((uchar_t)c[1] & 0x3F);
		return 2;
	} else if (c[0] <= 0xEF) {
		/* E0..EF */
		*out = ((uchar_t)c[0] &  0xF) << 12 |
		       ((uchar_t)c[1] & 0x3F) << 6  |
		       ((uchar_t)c[2] & 0x3F);
		return 3;
	} else {
		/* F0..F4 (unless input is invalid) */
		*out = ((uchar_t)c[0] &  0x7) << 18 |
		       ((uchar_t)c[1] & 0x3F) << 12 |
		       ((uchar_t)c[2] & 0x3F) << 6  |
		       ((uchar_t)c[3] & 0x3F);
		return 4;
	}
}

/*
 * Write a single UTF-8 character to @s,
 * returning the length, in bytes, of the character written.
 *
 * @unicode must be U+0000..U+10FFFF, but not U+D800..U+DFFF.
 *
 * This function will write up to 4 bytes to @out.
 */
static int utf8_write_char(uchar_t unicode, char *out)
{
	unsigned char *o = (unsigned char*) out;

	assert(unicode <= 0x10FFFF && !(unicode >= 0xD800 && unicode <= 0xDFFF));

	if (unicode <= 0x7F) {
		/* U+0000..U+007F */
		*o++ = unicode;
		return 1;
	} else if (unicode <= 0x7FF) {
		/* U+0080..U+07FF */
		*o++ = 0xC0 | unicode >> 6;
		*o++ = 0x80 | (unicode & 0x3F);
		return 2;
	} else if (unicode <= 0xFFFF) {
		/* U+0800..U+FFFF */
		*o++ = 0xE0 | unicode >> 12;
		*o++ = 0x80 | (unicode >> 6 & 0x3F);
		*o++ = 0x80 | (unicode & 0x3F);
		return 3;
	} else {
		/* U+10000..U+10FFFF */
		*o++ = 0xF0 | unicode >> 18;
		*o++ = 0x80 | (unicode >> 12 & 0x3F);
		*o++ = 0x80 | (unicode >> 6 & 0x3F);
		*o++ = 0x80 | (unicode & 0x3F);
		return 4;
	}
}

/*
 * Compute the Unicode codepoint of a UTF-16 surrogate pair.
 *
 * @uc should be 0xD800..0xDBFF, and @lc should be 0xDC00..0xDFFF.
 * If they aren't, this function returns false.
 */
static bool from_surrogate_pair(uint16_t uc, uint16_t lc, uchar_t *unicode)
{
	if (uc >= 0xD800 && uc <= 0xDBFF && lc >= 0xDC00 && lc <= 0xDFFF) {
		*unicode = 0x10000 + ((((uchar_t)uc & 0x3FF) << 10) | (lc & 0x3FF));
		return true;
	} else {
		return false;
	}
}

/*
 * Construct a UTF-16 surrogate pair given a Unicode codepoint.
 *
 * @unicode must be U+10000..U+10FFFF.
 */
static void to_surrogate_pair(uchar_t unicode, uint16_t *uc, uint16_t *lc)
{
	uchar_t n;

	assert(unicode >= 0x10000 && unicode <= 0x10FFFF);

	n = unicode - 0x10000;
	*uc = ((n >> 10) & 0x3FF) | 0xD800;
	*lc = (n & 0x3FF) | 0xDC00;
}

#define is_space(c) ((c) == '\t' || (c) == '\n' || (c) == '\r' || (c) == ' ')
#define is_digit(c) ((c) >= '0' && (c) <= '9')

static bool parse_value     (const char **sp, const char **se, JsonNode        **out);
static bool parse_string    (const char **sp, const char **se, char            **out);
static bool parse_number    (const char **sp, const char **se, double           *out, int *decimals);
static bool parse_array     (const char **sp, const char **se, JsonNode        **out);
static bool parse_object    (const char **sp, const char **se, JsonNode        **out);
static bool parse_hex16     (const char **sp, const char **se, uint16_t         *out);
static bool parse_comment   (const char **sp, const char **se, JsonNode        **out);

static bool expect_literal  (const char **sp, const char *str);
#define skip_space(s) while (is_space(**s)) (*(s))++

static void emit_value              (SB *out, const JsonNode *node, const JsonNode *comment);
static void emit_value_indented     (SB *out, const JsonNode *node, const JsonNode *comment, int indent_level);
static void emit_string             (SB *out, const char *str);
static void emit_number             (SB *out, double num, int decimals);
static void emit_array              (SB *out, const JsonNode *array, const JsonNode *comment);
static void emit_array_indented     (SB *out, const JsonNode *array, const JsonNode *comment, int indent_level);
static void emit_object             (SB *out, const JsonNode *object, const JsonNode *comment);
static void emit_object_indented    (SB *out, const JsonNode *object, const JsonNode *comment, int indent_level);
static bool emit_comment            (SB *out, const JsonNode *comment);
static bool emit_comment_indented   (SB *out, const JsonNode *comment);

static int write_hex16(char *out, uint16_t val);

static JsonNode *mknode(JsonTag tag);
static void append_node(JsonNode *parent, JsonNode *child);
static void prepend_node(JsonNode *parent, JsonNode *child);
static void append_member(JsonNode *object, char *key, JsonNode *value);

#define json_foreach_comment(i, object_or_array) json_foreach_and_all(i, object_or_array) \
                                             if (!json_is_comment(i)) continue; else

static const char dangling_comment_key[] = "dangling\tcomment\001\002\003";

/* Assertion-friendly validity checks */
static bool tag_is_valid(unsigned int tag);
static bool number_is_valid(const char *num);

JsonNode *json_decode(const char *json)
{
	const char *s = json;
	JsonNode *ret;

	skip_space(&s);
	if (!parse_value(&s, NULL, &ret))
		return NULL;

	skip_space(&s);
	if (*s != 0) {
		json_delete(ret);
		return NULL;
	}

	return json_strip_comments(ret, NULL);
}

JsonNode *json_decode_ex(const char *json, const char **problem, JsonNode **comments)
{
	const char *s = json;
	JsonNode *ret;

	skip_space(&s);
	if (!parse_value(&s, problem, &ret))
		return NULL;

	skip_space(&s);
	if (problem)
		*problem = s;

	if (*s != 0) {
		json_delete(ret);
		return NULL;
	}

	if (comments != JSON_WANT_EMBEDDED_COMMENTS) {

		JsonNode *found_comments = comments ? json_mkobject() : NULL;

		ret = json_strip_comments(ret, found_comments);

		if (found_comments) {
			if (json_first_child(found_comments)) // really found some.
				*comments = found_comments;
			else {
				json_delete(found_comments);
				*comments = found_comments = NULL;
			}
		}
	}

	return ret;
}

int json_get_line_number(const char *json, const char *problem, int *position_in_line)
{
	if (position_in_line)
		*position_in_line = -1;
	if (json == NULL || problem == NULL)
		return -1;
	if (problem < json || problem > json+strlen(json))
		return -2;

	int lino = 1;
	const char *last_line, *s = json;
	while ((s=strchr(last_line = s, '\n')) && s <= problem)
		s++, lino++;
	if (position_in_line)
		*position_in_line = problem - last_line;
	return lino;
}


char *json_encode(const JsonNode *node)
{
	return json_stringify(node, NULL);
}

char *json_encode_string(const char *str)
{
	SB sb;
	sb_init(&sb);

	emit_string(&sb, str);

	return sb_finish(&sb);
}

char *json_stringify(const JsonNode *node, const char *space)
{
	return json_stringify_ex(node, space, NULL);
}

char *json_stringify_ex(const JsonNode *node, const char *space, const JsonNode *comments)
{
	SB sb;
	sb_init(&sb);

	if (comments && comments->tag != JSON_OBJECT)
		comments = NULL;

	if (node != NULL)
	{
		sb.space = space;
		if (space != NULL)
			emit_value_indented(&sb, node, comments, 0);
		else
			emit_value(&sb, node, comments);
	}

	return sb_finish(&sb);
}

const JsonNode *json_delete_force(const JsonNode *node)
{
	return json_delete((/* non-const */ JsonNode*)node);
}

JsonNode *json_delete(JsonNode *node)
{
	if (node == NULL)
		return NULL;

	JsonNode *prev = json_remove_from_parent(node);	// frees key already.

	switch (node->tag) {
		case JSON_STRING:
		case JSON_LINE_COMMENT:
		case JSON_BLOCK_COMMENT:
			FREE(node->string_);
			break;
		case JSON_ARRAY:
		case JSON_OBJECT:
		{
			JsonNode *child;
			for (child = node->children.head; child != NULL; ) {
				child = json_delete(child);
			}
			break;
		}
		default:;
	}

	FREE(node);

	return prev;
}

// json_stip_comments() returns the node that caller must use in place of
// passed root in case root itself was a comment and has been stripped...
JsonNode *json_strip_comments(JsonNode *root, JsonNode *collector_object)
{
	if (root == NULL)
		return NULL;
	if (!collector_object || collector_object->tag != JSON_OBJECT)
		collector_object = NULL;

	if (json_is_container(root)) {
		static char * const nokeyyet = (char*) -1; //NULL;
		unsigned int idx = 0;
		char sidx[32];
		JsonNode *first = NULL;
		JsonNode *node = root->children.head, *next;
		for (;node; node = next) {
			next = node->next;
			if (json_is_comment(node)) {
				json_remove_from_parent(node);
				if (collector_object) {
					if (first == NULL)
						first = node;
					append_member(collector_object, nokeyyet, node);
				} else {
					json_delete(node);
				}
			}
			else {
				sprintf(sidx, "%u", idx++);
				char *key = root->tag == JSON_OBJECT ? node->key : sidx;
				for (;first; first = first->next) {
					if (first->key == nokeyyet)
						first->key = STRDUP_OR_EXIT(key);
				}

				JsonNode *coll = collector_object ? json_mkobject() : NULL;
				json_strip_comments(node, coll);
				if (coll && coll->children.head) {
					json_append_member(collector_object, key, coll);
				}
				else {
					json_delete(coll);
				}
				
			}
		}
		// mark dangling comments
		for (;first; first = next) {
			next = first->next;
			if (first->key == nokeyyet)
				first->key = STRDUP_OR_EXIT(dangling_comment_key);
		}
	}
	else if (json_is_comment(root)) {
		json_remove_from_parent(root);
		if (collector_object) {
			json_append_member(collector_object, "", root);
		} else {
			json_delete(root);
		}
		return NULL;
	}
	return root;
}

bool json_validate(const char *json, const char **problem)
{
	const char *s = json;

	skip_space(&s);
	if (!parse_value(&s, problem, NULL))
		return false;

	skip_space(&s);
	if (problem)
		*problem = s;
	return *s == 0;
}

const JsonNode *json_find_element(const JsonNode *array_or_object, int index)
{
	const JsonNode *element;
	int i = 0;

	if (array_or_object  == NULL)
		return NULL;
	if (array_or_object->tag == JSON_ARRAY) {
		json_foreach(element, array_or_object) {
			if (i++ == index)
				return element;
		}
	} else if (array_or_object->tag == JSON_OBJECT) {
		json_foreach(element, array_or_object) {
			if (atoi(element->key) == index)
				return element;
		}
	}

	return NULL;
}

static const JsonNode *find_member(const JsonNode *object_or_array, const char *name, bool no_comments)
{
	const JsonNode *member;

	if (object_or_array == NULL)
		return NULL;
	if (object_or_array->tag == JSON_OBJECT) {
		if (no_comments) {
			json_foreach(member, object_or_array) {
				if (strcmp(member->key, name) == 0)
					return member;
			}
		} else {
			json_foreach_comment(member, object_or_array) {
				if (member->key != NULL && strcmp(member->key, name) == 0)
					return member;
			}
		}
	} else if (object_or_array->tag == JSON_ARRAY
			&& is_digit(*name) && (name[0] != '0' || !name[1])) {
		// if name is a pure number (no leading/trailing spaces and no leading zeroes!) treat it as index
		char *end = NULL;
		int i = 0, index = strtol(name, &end, 10);	// no 0x prefix allowed.
		if (*end == '\0') {
			if (no_comments) {
				json_foreach(member, object_or_array) {
					if (i++ == index)
						return member;
				}
			} else {
				json_foreach_and_all(member, object_or_array) {
					if (!json_is_comment(member))
						i++;
					else if (i == index)
						return member;
				}
			}
		}
	}

	return NULL;
}

const JsonNode *json_find_member(const JsonNode *object_or_array, const char *name)
{
	return find_member(object_or_array, name, true);
}

const JsonNode *json_first_child(const JsonNode *node)
{
	if (node != NULL && json_is_container(node))
		return node->children.head;
	return NULL;
}

static JsonNode *mknode(JsonTag tag)
{
	JsonNode *ret = (JsonNode*) MALLOC_OR_EXIT(sizeof(JsonNode));
	memset(ret, 0, sizeof(*ret));
	ret->tag = tag;
	return ret;
}

JsonNode *json_mknull(void)
{
	return mknode(JSON_NULL);
}

JsonNode *json_mkbool(bool b)
{
	JsonNode *ret = mknode(JSON_BOOL);
	ret->bool_ = b;
	return ret;
}

static JsonNode *mkstring(char *s)
{
	JsonNode *ret = mknode(JSON_STRING);
	ret->string_ = s;
	return ret;
}

JsonNode *json_mkstring(const char *s)
{
	return mkstring(STRDUP_OR_EXIT(s));
}

JsonNode *json_mknumber(double n, int decimals)
{
	JsonNode *node = mknode(JSON_NUMBER);
	node->number_ = n;
	node->decimals_ = decimals;
	return node;
}

JsonNode *json_mkarray(void)
{
	return mknode(JSON_ARRAY);
}

JsonNode *json_mkobject(void)
{
	return mknode(JSON_OBJECT);
}

static void append_node(JsonNode *parent, JsonNode *child)
{
	child->parent = parent;
	child->prev = parent->children.tail;
	child->next = NULL;

	if (parent->children.tail != NULL)
		parent->children.tail->next = child;
	else
		parent->children.head = child;
	parent->children.tail = child;
}

static void prepend_node(JsonNode *parent, JsonNode *child)
{
	child->parent = parent;
	child->prev = NULL;
	child->next = parent->children.head;

	if (parent->children.head != NULL)
		parent->children.head->prev = child;
	else
		parent->children.tail = child;
	parent->children.head = child;
}

static void append_member(JsonNode *object, char *key, JsonNode *value)
{
	value->key = key;
	append_node(object, value);
}

JsonNode *json_append_element(JsonNode *array, JsonNode *element)
{
	assert(array->tag == JSON_ARRAY);
	assert(element->parent == NULL);

	append_node(array, element);
	return element;
}

JsonNode *json_prepend_element(JsonNode *array, JsonNode *element)
{
	assert(array->tag == JSON_ARRAY);
	assert(element->parent == NULL);

	prepend_node(array, element);
	return element;
}

JsonNode *json_append_member(JsonNode *object, const char *key, JsonNode *value)
{
	assert(object->tag == JSON_OBJECT);
	assert(value->parent == NULL);

	append_member(object, STRDUP_OR_EXIT(key), value);
	return value;
}

JsonNode *json_prepend_member(JsonNode *object, const char *key, JsonNode *value)
{
	assert(object->tag == JSON_OBJECT);
	assert(value->parent == NULL);

	value->key = STRDUP_OR_EXIT(key);
	prepend_node(object, value);
	return value;
}

JsonNode *json_append_comment(JsonNode *target, const char *comment)
{
	if (comment == NULL)
		return NULL;
	if (!json_is_container(target)) {
		assert(!"target not able to hold comments");
		return NULL;
	}
	JsonNode *node = mknode(strchr(comment, '\n') ? JSON_BLOCK_COMMENT : JSON_LINE_COMMENT);
	node->string_ = STRDUP_OR_EXIT(comment);
	append_node(target, node);
	return node;
}

const JsonNode *json_remove_from_parent_force(const JsonNode *node)
{
	return json_remove_from_parent((/* non-const */ JsonNode*) node);
}

JsonNode *json_remove_from_parent(JsonNode *node)
{
	if (node == NULL)
		return NULL;

	JsonNode *parent = node->parent;
	if (parent == NULL)
		return NULL;

	JsonNode *prev = node->prev;

	if (node->prev != NULL)
		node->prev->next = node->next;
	else
		parent->children.head = node->next;
	if (node->next != NULL)
		node->next->prev = node->prev;
	else
		parent->children.tail = node->prev;

	FREE(node->key);

	node->parent = NULL;
	node->prev = node->next = NULL;
	node->key = NULL;

	return prev;
}

static bool parse_value(const char **sp, const char **se, JsonNode **out)
{
	const char *s = *sp;

	if (se) *se = s;
	switch (*s) {
		case 'n':
			if (expect_literal(sp, "null")) {
				if (out)
					*out = json_mknull();
				return true;
			}
			return false;

		case 'f':
			if (expect_literal(sp, "false")) {
				if (out)
					*out = json_mkbool(false);
				return true;
			}
			return false;

		case 't':
			if (expect_literal(sp, "true")) {
				if (out)
					*out = json_mkbool(true);
				return true;
			}
			return false;

		case '\'':
		case '"': {
			char *str;
			if (parse_string(sp, se, out ? &str : NULL)) {
				if (out)
					*out = mkstring(str);
				return true;
			}
			return false;
		}

		case '/':
			return parse_comment(sp, se, out);

		case '[':
			return parse_array(sp, se, out);

		case '{':
			return parse_object(sp, se, out);

		default: {
			double num;
			int decimals = 0;
			if (parse_number(sp, se, out ? &num : NULL, &decimals)) {
				if (out)
					*out = json_mknumber(num, decimals);
				return true;
			}
			return false;
		}
	}
}

static bool parse_array(const char **sp, const char **se, JsonNode **out)
{
	const char *s = *sp;
	JsonNode *ret = out ? json_mkarray() : NULL;
	JsonNode *element = NULL, *comment = NULL;
	const char *no_comma_seen = NULL;

	if (se) *se = s;
	if (*s++ != '[')
		goto failure;

	for (;;) {
		skip_space(&s);

		if (!parse_comment(&s, se, out ? &comment : NULL))
			goto failure;
		if (comment)
			append_node(ret, comment);

		if (*s == ']') {
			s++;
			goto success;
		}

		if (!parse_value(&s, se, out ? &element : NULL))
			goto failure;
		skip_space(&s);

		if (out)
			json_append_element(ret, element);

		no_comma_seen = *s != ',' ? s : NULL;
		if (!no_comma_seen) {
			s++;
			skip_space(&s);
		}

		if (!parse_comment(&s, se, out ? &comment : NULL))
			goto failure;
		if (comment)
			append_node(ret, comment);

		if (*s == ']') {
			s++;
			goto success;
		}

		if (no_comma_seen) {
			if (se) *se = no_comma_seen;
			goto failure;
		}
		if (se) *se = s;
	}

success:
	*sp = s;
	if (out)
		*out = ret;
	return true;

failure:
	json_delete(ret);
	return false;
}

static bool parse_object(const char **sp, const char **se, JsonNode **out)
{
	const char *s = *sp;
	JsonNode *ret = out ? json_mkobject() : NULL;
	char *key = NULL;
	JsonNode *value = NULL, *comment = NULL;
	const char *no_comma_seen = NULL;

	if (se) *se = s;
	if (*s++ != '{')
		goto failure;

	for (;;) {
		skip_space(&s);

		if (!parse_comment(&s, se, out ? &comment : NULL))
			goto failure;
		if (comment)
			append_node(ret, comment);

		if (*s == '}') {
			s++;
			goto success;
		}

		if (!parse_string(&s, se, out ? &key : NULL)) {
			goto failure;
		}
		skip_space(&s);

		if (se) *se = s;
		if (*s++ != ':')
			goto failure;
		skip_space(&s);

		if (!parse_value(&s, se, out ? &value : NULL))
			goto failure;
		skip_space(&s);
		if (out)
			append_member(ret, key, value);
		key = NULL;	// owned by ret now.

		no_comma_seen = *s != ',' ? s : NULL;
		if (!no_comma_seen) {
			s++;
			skip_space(&s);
		}

		if (!parse_comment(&s, se, out ? &comment : NULL))
			goto failure;
		if (comment)
			append_node(ret, comment);

		if (*s == '}') {
			s++;
			goto success;
		}

		if (no_comma_seen) {
			if (se) *se = no_comma_seen;
			goto failure;
		}
		if (se) *se = s;
	}

success:
	*sp = s;
	if (out)
		*out = ret;
	return true;

failure:
	if (out) {
		FREE(key);
		json_delete(ret);
	}
	return false;
}

bool parse_comment(const char **sp, const char **se, JsonNode **out)
{
	const char *s = *sp, *end, *nested, *delim = "";
	size_t len = 0;
	JsonNode *ret = NULL;
	JsonTag tag = JSON_TAG_UNDEFINED;
	SB comment = { 0 };

	if (se) *se = s;
	while (*s++ == '/') {
		switch (*s++) {
		case '/':
			len = strcspn(s, "\n\r\f\v");
			*sp = s + len + (s[len] ? 1 : 0); // skip found char but not term zero.
			tag = tag == JSON_TAG_UNDEFINED ? JSON_LINE_COMMENT : JSON_BLOCK_COMMENT;
			break;
		case '*':
			end = strstr(s, "*/");
			if (end == NULL) {
				sb_free(&comment);
				return false;
			}
			if ((nested = strstr(s, "/*")) && nested < end) {
				if (se) *se = nested;
				sb_free(&comment);
				return false;
			}
			len = end - s;
			*sp = end + 2; // =strlen("*/")
			tag = JSON_BLOCK_COMMENT;
			break;
		default:
			s = NULL;
			break;
		}
		// now sp points behind comment. s points to comment. len = lenght of comment.
		if (s == NULL) {
			break;
		}
		if (out != NULL) {
			if (comment.start == NULL)
				sb_init(&comment);
			sb_puts(&comment, delim);
			sb_put(&comment, s, len);
			delim = "\n";
		}
		skip_space(sp);
		s = *sp;
		if (se) *se = s;
	}

	if (tag == JSON_TAG_UNDEFINED || out == NULL) {
		if (out)
			*out = NULL;
		sb_free(&comment);
		return true;
	}
	ret = mknode(tag);
	ret->string_ = sb_finish(&comment);
	*out = ret;
	return true;
}

bool parse_string(const char **sp, const char **se, char **out)
{
	const char *s = *sp;
	SB sb;
	char throwaway_buffer[4];
		/* enough space for a UTF-8 character */
	char *b, quote;

	if (se) *se = s;
	quote = *s++;
	if (quote != '"' && quote != '\'')
		return false;

	if (out) {
		sb_init(&sb);
		sb_need(&sb, sizeof(throwaway_buffer));
		b = sb.cur;
	} else {
		b = throwaway_buffer;
	}

	while (*s != quote) {
		unsigned char c = *s++;

		/* Parse next character, and write it to b. */
		if (c == '\\') {
			c = *s++;
			switch (c) {
				case '"':
				case '\'':
				case '\\':
				case '/':
					*b++ = c;
					break;
				case 'b':
					*b++ = '\b';
					break;
				case 'f':
					*b++ = '\f';
					break;
				case 'n':
					*b++ = '\n';
					break;
				case 'r':
					*b++ = '\r';
					break;
				case 't':
					*b++ = '\t';
					break;
				case 'v':
					*b++ = '\v';
					break;
				case 'u':
				{
					uint16_t uc, lc;
					uchar_t unicode;

					if (!parse_hex16(&s, se, &uc))
						goto failed;

					if (uc >= 0xD800 && uc <= 0xDFFF) {
						/* Handle UTF-16 surrogate pair. */
						if (*s++ != '\\' || *s++ != 'u' || !parse_hex16(&s, se, &lc))
							goto failed; /* Incomplete surrogate pair. */
						if (!from_surrogate_pair(uc, lc, &unicode))
							goto failed; /* Invalid surrogate pair. */
					} else if (uc == 0) {
						/* Disallow "\u0000". */
						goto failed;
					} else {
						unicode = uc;
					}

					b += utf8_write_char(unicode, b);
					break;
				}
				default:
					/* Invalid escape */
					goto failed;
			}
		} else if (c <= 0x1F) {
			/* Control characters are not allowed in string literals. */
			goto failed;
		} else {
			/* Validate and echo a UTF-8 character. */
			int len;

			s--;
			len = utf8_validate_cz(s);
			if (len == 0 || len > sizeof(throwaway_buffer))
				goto failed; /* Invalid UTF-8 character. */

			while (len--)
				*b++ = *s++;
		}

		/*
		 * Update sb to know about the new bytes,
		 * and set up b to write another character.
		 */
		if (out) {
			sb.cur = b;
			sb_need(&sb, sizeof(throwaway_buffer));
			b = sb.cur;
		} else {
			b = throwaway_buffer;
		}
	}
	s++;

	if (out)
		*out = sb_finish(&sb);
	*sp = s;
	return true;

failed:
	if (out)
		sb_free(&sb);
	return false;
}

/*
 * The JSON spec says that a number shall follow this precise pattern
 * (spaces and quotes added for readability):
 *	 '-'? (0 | [1-9][0-9]*) ('.' [0-9]+)? ([Ee] [+-]? [0-9]+)?
 *
 * However, some JSON parsers are more liberal.  For instance, PHP accepts
 * '.5' and '1.'.  JSON.parse accepts '+3'.
 *
 * This function takes the strict approach.
 */
bool parse_number(const char **sp, const char **se, double *out, int *decimals)
{
	const char *s = *sp;
	if (decimals != NULL) {
		(*decimals) = 0;
	}

	if (se) *se = s;

	/* '-'? */
	if (*s == '-')
		s++;

	/* (0 | [1-9][0-9]*) */
	if (*s == '0') {
		s++;
	} else {
		if (!is_digit(*s))
			return false;
		do {
			s++;
		} while (is_digit(*s));
	}

	/* ('.' [0-9]+)? */
	if (*s == '.') {
		s++;
		if (!is_digit(*s))
			return false;
		do {
			s++;
			if (decimals != NULL) {
				(*decimals)++;
			}
		} while (is_digit(*s));
	}

	/* ([Ee] [+-]? [0-9]+)? */
	if (*s == 'E' || *s == 'e') {
		s++;
		if (*s == '+' || *s == '-')
			s++;
		if (!is_digit(*s))
			return false;
		do {
			s++;
		} while (is_digit(*s));
	}

	if (out)
		*out = strtod(*sp, NULL);

	*sp = s;
	return true;
}

static void emit_value(SB *out, const JsonNode *node, const JsonNode *comment)
{
	assert(tag_is_valid(node->tag));
	switch (node->tag) {
		case JSON_NULL:
			sb_puts(out, "null");
			break;
		case JSON_BOOL:
			sb_puts(out, node->bool_ ? "true" : "false");
			break;
		case JSON_STRING:
			emit_string(out, node->string_);
			break;
		case JSON_NUMBER:
			emit_number(out, node->number_, node->decimals_);
			break;
		case JSON_ARRAY:
			emit_array(out, node, comment);
			break;
		case JSON_OBJECT:
			emit_object(out, node, comment);
			break;
		case JSON_LINE_COMMENT:
		case JSON_BLOCK_COMMENT:
			emit_comment(out, node);
			break;
		default:
			assert(false);
	}
}

void emit_value_indented(SB *out, const JsonNode *node, const JsonNode *comment, int indent_level)
{
	assert(tag_is_valid(node->tag));
	switch (node->tag) {
		case JSON_NULL:
			sb_puts(out, "null");
			break;
		case JSON_BOOL:
			sb_puts(out, node->bool_ ? "true" : "false");
			break;
		case JSON_STRING:
			emit_string(out, node->string_);
			break;
		case JSON_NUMBER:
			emit_number(out, node->number_, node->decimals_);
			break;
		case JSON_ARRAY:
			emit_array_indented(out, node, comment, indent_level);
			break;
		case JSON_OBJECT:
			emit_object_indented(out, node, comment, indent_level);
			break;
		case JSON_LINE_COMMENT:
		case JSON_BLOCK_COMMENT:
			emit_comment_indented(out, node);
			break;
		default:
			assert(false);
	}
}

static void emit_array_indented(SB *out, const JsonNode *array, const JsonNode *comment, int indent_level)
{
	const JsonNode *element, *dangling_comment;
	unsigned int idx = 0, indent_next = 0;
	const char *delimiter = NULL;
	char sidx[32];
	bool multiline_format = false;
	static const int max_elements_per_single_line = 10;
	int elements_in_single_line = 0;

	// If array has comments or non-empty containers then indent elements
	// on multi lines. Otherwise use a compact single line format.
	json_foreach_and_all(element, array) {
		idx++;
		if (json_is_container(element) && element->children.head)
			break;	// element is non-empty container.
		if (json_is_comment(element))
			break;	// element is comment.
		sprintf(sidx, "%u", idx-1);
		if (find_member(comment, sidx, false))
			break;	// element has comments.
	}

	dangling_comment = find_member(comment, dangling_comment_key, false);
	multiline_format = element || dangling_comment;

	if (idx == 0 && ! dangling_comment) {
		sb_puts(out, "[]");
		return;
	}

	sb_puts(out, multiline_format ? "[\n" : "[ ");
	indent_level++;
	delimiter = "";
	indent_next = multiline_format ? indent_level : 0;
	for (idx = 0, element = array->children.head; element != NULL; element = element->next) {
		sb_puts(out, delimiter); delimiter = "";
		sb_put_space(out, indent_next);

		if (emit_comment_indented(out, element)) {
			indent_next = indent_level;
			continue;
		}
		sprintf(sidx, "%u", idx++);
		if (emit_comment_indented(out, find_member(comment, sidx, false)))
			sb_put_space(out, indent_level);
		emit_value_indented(out, element, find_member(comment, sidx, true), indent_level);

		sprintf(sidx, "%u", idx);
		bool wrap = multiline_format;
		wrap = wrap && ( // current is non-empty container (cannot be comment at this point):
				(json_is_container(element) && element->children.head)
			||	// next element exists and is non-empty container or is or has comments:
				(element->next &&
				( (json_is_container(element->next) && element->next->children.head)
				|| json_is_comment(element->next)
				|| find_member(comment, sidx, true)))
			);
		wrap = wrap || ++elements_in_single_line >= max_elements_per_single_line;
		elements_in_single_line *= !wrap;
		delimiter   = wrap ? ",\n" : ", ";
		indent_next = wrap ? indent_level : 0;
	}
	indent_level--;
	if (dangling_comment) {
		sb_puts(out, delimiter);
		sb_put_space(out, indent_next);
		emit_comment_indented(out, dangling_comment);
		sb_put_space(out, !multiline_format ? indent_level : 0);
	}
	sb_puts(out,      multiline_format ? "\n" : " ");
	sb_put_space(out, multiline_format ? indent_level : 0);
	sb_putc(out, ']');
}

static void emit_array(SB *out, const JsonNode *array, const JsonNode *comment)
{
	const JsonNode *element;

	sb_putc(out, '[');
	json_foreach_and_all(element, array) {
		if (emit_comment(out, element))
			continue;
		emit_value(out, element, comment);
		if (element->next != NULL)
			sb_putc(out, ',');
	}
	sb_putc(out, ']');
}

static void emit_object(SB *out, const JsonNode *object, const JsonNode *comment)
{
	const JsonNode *member;

	sb_putc(out, '{');
	json_foreach_and_all(member, object) {
		if (emit_comment(out, member))
			continue;
		emit_string(out, member->key);
		sb_putc(out, ':');
		emit_value(out, member, comment);
		if (member->next != NULL)
			sb_putc(out, ',');
	}
	sb_putc(out, '}');
}

static void emit_object_indented(SB *out, const JsonNode *object, const JsonNode *comment, int indent_level)
{
	const JsonNode *member = object->children.head;
	const JsonNode *dangling_comment = find_member(comment, dangling_comment_key, false);

	if (member == NULL && ! dangling_comment) {
		sb_puts(out, "{}");
		return;
	}

	sb_puts(out, "{\n");
	indent_level++;
	for (; member != NULL; member = member->next) {
		sb_put_space(out, indent_level);
		if (emit_comment_indented(out, member))
			continue;
		if (emit_comment_indented(out, find_member(comment, member->key, false)))
			sb_put_space(out, indent_level);

		emit_string(out, member->key);
		sb_puts(out, ": ");
		emit_value_indented(out, member, find_member(comment, member->key, true), indent_level);

		sb_puts(out, member->next != NULL ? ",\n" : "\n");
	}
	if (dangling_comment) {
		sb_put_space(out, indent_level);
		emit_comment_indented(out, dangling_comment);
	}
	indent_level--;
	sb_put_space(out, indent_level);
	sb_putc(out, '}');
}

static bool emit_comment(SB *out, const JsonNode *object)
{
	if (object == NULL)
		return false;
	char *str = object->string_;
	size_t len;
	switch (object->tag) {
	case JSON_LINE_COMMENT:
		str += strspn(str, "\n"); // trim leading \n's
		len = strcspn(str, "\n"); // len until 1st \n or \0
		if (strspn(str + len, "\n") == strlen(str + len)) {
			// No more \n's in comment after trimming them on
			// left and right (and which we simply throw away):
			sb_puts(out, "//");
			sb_put(out, object->string_, len);
			sb_putc(out, '\n');
			break;
		}
		// Comment has new lines that can't be ignored.
		// Fall thru
	case JSON_BLOCK_COMMENT:
		sb_puts(out, "/*");
		for (;;) {
			// Scan comment for "*/" and "escape" them.
			char *bad = strstr(str, "*/");
			if (!bad)
				break;
			sb_put(out, str, bad - str);
			str = bad + 2;
			if (*str == 0)
				break; // lucky, "*/" was the last thing in comment - re-use it.
			sb_puts(out, "* /");
		}
		sb_puts(out, str);
		sb_puts(out, "*/");
		break;
	default:
		return false;
	}
	return true;
}

static bool emit_comment_indented(SB *out, const JsonNode *object)
{
	if (!emit_comment(out, object))
		return false;
	if (object->tag == JSON_BLOCK_COMMENT) {
		sb_puts(out, "\n");
	}
	return true;
}

static void emit_string(SB *out, const char *str)
{
	bool escape_unicode = false;
	const char *s = str;
	char *b;

	if (str == NULL) {
		sb_puts(out, "null");
		return;
	}

	assert(utf8_validate(str));

	/*
	 * 14 bytes is enough space to write up to two
	 * \uXXXX escapes and two quotation marks.
	 */
	sb_need(out, 14);
	b = out->cur;

	*b++ = '"';
	while (*s != 0) {
		unsigned char c = *s++;

		/* Encode the next character, and write it to b. */
		switch (c) {
			case '"':
				*b++ = '\\';
				*b++ = '"';
				break;
			case '\\':
				*b++ = '\\';
				*b++ = '\\';
				break;
			case '\b':
				*b++ = '\\';
				*b++ = 'b';
				break;
			case '\f':
				*b++ = '\\';
				*b++ = 'f';
				break;
			case '\n':
				*b++ = '\\';
				*b++ = 'n';
				break;
			case '\r':
				*b++ = '\\';
				*b++ = 'r';
				break;
			case '\t':
				*b++ = '\\';
				*b++ = 't';
				break;
			default: {
				int len;

				s--;
				len = utf8_validate_cz(s);

				if (len == 0) {
					/*
					 * Handle invalid UTF-8 character gracefully in production
					 * by writing a replacement character (U+FFFD)
					 * and skipping a single byte.
					 *
					 * This should never happen when assertions are enabled
					 * due to the assertion at the beginning of this function.
					 */
					assert(false);
					if (escape_unicode) {
						strcpy(b, "\\uFFFD");
						b += 6;
					} else {
						*b++ = 0xEF;
						*b++ = 0xBF;
						*b++ = 0xBD;
					}
					s++;
				} else if (c < 0x1F || (c >= 0x80 && escape_unicode)) {
					/* Encode using \u.... */
					uint32_t unicode;

					s += utf8_read_char(s, &unicode);

					if (unicode <= 0xFFFF) {
						*b++ = '\\';
						*b++ = 'u';
						b += write_hex16(b, unicode);
					} else {
						/* Produce a surrogate pair. */
						uint16_t uc, lc;
						assert(unicode <= 0x10FFFF);
						to_surrogate_pair(unicode, &uc, &lc);
						*b++ = '\\';
						*b++ = 'u';
						b += write_hex16(b, uc);
						*b++ = '\\';
						*b++ = 'u';
						b += write_hex16(b, lc);
					}
				} else {
					/* Write the character directly. */
					while (len--)
						*b++ = *s++;
				}

				break;
			}
		}

		/*
		 * Update *out to know about the new bytes,
		 * and set up b to write another encoded character.
		 */
		out->cur = b;
		sb_need(out, 14);
		b = out->cur;
	}
	*b++ = '"';

	out->cur = b;
}

static void emit_number(SB *out, double num, int decimals)
{
	/*
	 * This isn't exactly how JavaScript renders numbers,
	 * but it should produce valid JSON for reasonable numbers
	 * preserve precision well enough, and avoid some oddities
	 * like 0.3 -> 0.299999999999999988898 .
	 */
	char buf[64];
	sprintf(buf, "%.*f", decimals, num);

	if (number_is_valid(buf))
		sb_puts(out, buf);
	else
		sb_puts(out, "null");
}

static bool tag_is_valid(unsigned int tag)
{
	return (/* tag >= JSON_NULL && */ tag <= JSON_TAG_HIGH_VALUE);
}

static bool number_is_valid(const char *num)
{
	return parse_number(&num, NULL, NULL, NULL) && *num == 0;
}

static bool expect_literal(const char **sp, const char *str)
{
	const char *s = *sp;

	while (*str != '\0')
		if (*s++ != *str++)
			return false;

	*sp = s;
	return true;
}

/*
 * Parses exactly 4 hex characters (capital or lowercase).
 * Fails if any input chars are not [0-9A-Fa-f].
 */
static bool parse_hex16(const char **sp, const char **se, uint16_t *out)
{
	const char *s = *sp;
	uint16_t ret = 0;
	uint16_t i;
	uint16_t tmp;
	char c;

	if (se) *se = s;

	for (i = 0; i < 4; i++) {
		c = *s++;
		if (c >= '0' && c <= '9')
			tmp = c - '0';
		else if (c >= 'A' && c <= 'F')
			tmp = c - 'A' + 10;
		else if (c >= 'a' && c <= 'f')
			tmp = c - 'a' + 10;
		else
			return false;

		ret <<= 4;
		ret += tmp;
	}

	if (out)
		*out = ret;
	*sp = s;
	return true;
}

/*
 * Encodes a 16-bit number into hexadecimal,
 * writing exactly 4 hex chars.
 */
static int write_hex16(char *out, uint16_t val)
{
	const char *hex = "0123456789ABCDEF";

	*out++ = hex[(val >> 12) & 0xF];
	*out++ = hex[(val >> 8)  & 0xF];
	*out++ = hex[(val >> 4)  & 0xF];
	*out++ = hex[ val        & 0xF];

	return 4;
}

bool json_check(const JsonNode *node, char errmsg[256])
{
	#define problem(...) do { \
			if (errmsg != NULL) \
				snprintf(errmsg, 256, __VA_ARGS__); \
			return false; \
		} while (0)

	if (node->key != NULL && !utf8_validate(node->key))
		problem("key contains invalid UTF-8");

	if (!tag_is_valid(node->tag))
		problem("tag is invalid (%u)", node->tag);

	if (node->tag == JSON_BOOL) {
		if (node->bool_ != false && node->bool_ != true)
			problem("bool_ is %d which is neither false (%d) nor true (%d)",
					(int)node->bool_, (int)false, (int)true);
	} else if (node->tag == JSON_STRING) {
		if (node->string_ == NULL)
			problem("string_ is NULL");
		if (!utf8_validate(node->string_))
			problem("string_ contains invalid UTF-8");
		if (node->key != NULL)
			problem("key should be NULL but is 0x%08x", (unsigned int)node->key);
	} else if (node->tag == JSON_ARRAY || node->tag == JSON_OBJECT) {
		JsonNode *head = node->children.head;
		JsonNode *tail = node->children.tail;

		if (head == NULL || tail == NULL) {
			if (head != NULL)
				problem("tail is NULL, but head is not");
			if (tail != NULL)
				problem("head is NULL, but tail is not");
		} else {
			JsonNode *child;
			JsonNode *last = NULL;

			if (head->prev != NULL)
				problem("First child's prev pointer is not NULL");

			for (child = head; child != NULL; last = child, child = child->next) {
				if (child == node)
					problem("node is its own child");
				if (child->next == child)
					problem("child->next == child (cycle)");
				if (child->next == head)
					problem("child->next == head (cycle)");

				if (child->parent != node)
					problem("child does not point back to parent");
				if (child->next != NULL && child->next->prev != child)
					problem("child->next does not point back to child");

				if (node->tag == JSON_ARRAY && child->key != NULL)
					problem("Array element's key is not NULL");
				if (node->tag == JSON_OBJECT && child->key == NULL)
					problem("Object member's key is NULL");

				if (!json_check(child, errmsg))
					return false;
			}

			if (last != tail)
				problem("tail does not match pointer found by starting at head and following next links");
		}
	}
	else if (node->tag == JSON_LINE_COMMENT || node->tag == JSON_BLOCK_COMMENT) {
		if (node->string_ == NULL)
			problem("string_ is NULL");
		if (!utf8_validate(node->string_))
			problem("string_ contains invalid UTF-8");
		if (node->key != NULL)
			problem("key should be NULL but is 0x%08x", (unsigned int)node->key);
	}
	return true;

	#undef problem
}

bool json_get_number(const JsonNode *object_or_array, const char *name, double *out)
{
	const JsonNode *node = json_find_member(object_or_array, name);
	if (node && node->tag == JSON_NUMBER) {
		if (out != NULL)
			*out = node->number_;
		return true;
	}
	return false;
}

bool json_get_string(const JsonNode *object_or_array, const char *name, const char **out)
{
	const JsonNode *node = json_find_member(object_or_array, name);
	if (node && node->tag == JSON_STRING) {
		if (out != NULL)
			*out = node->string_;
		return true;
	}
	return false;
}

bool json_convert_type_force(const JsonNode *node, JsonTag new_type)
{
	return json_convert_type((/* non-const */ JsonNode*) node, new_type);
}

bool json_convert_type(JsonNode *node, JsonTag new_type)
{
	if (node->tag != JSON_STRING || new_type != JSON_NUMBER)
		return node->tag == new_type;

	const char *str = node->string_;
	double num;
	int decimals = 0;
	if (!parse_number(&str, NULL, &num, &decimals))
		return false;

	FREE(node->string_);
	node->number_ = num;
	node->decimals_ = decimals;
	node->tag = JSON_NUMBER;

	return true;
}


void json_free(void *a)
{
	FREE(a);
}
