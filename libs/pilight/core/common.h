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

#ifndef _COMMON_H_
#define _COMMON_H_

#ifndef _WIN32
	#include <sys/types.h>
	#include <ifaddrs.h>
	#include <sys/socket.h>
#endif
#include <pthread.h>
#include <stdint.h>

#include "pilight.h"

extern char *progname;

#ifdef _WIN32
#define sleep(a) Sleep(a*1000)
int check_instances(const wchar_t *prog);
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);
int isrunning(const char *program);
#endif

/**
 * Calculate number of elements (not bytes like sizeof) in a
 * compile time allocated buffer. Do not use it for pointers.
 */
#ifndef countof
#define countof(buffer) (sizeof(buffer) / sizeof(buffer[0]))
#endif

/**
 * Initialize an array of strings.
 * @param int len Initial size of array.
 * @param char* initial_value Initial value for all array elements.
 * @return char ** The array. Must be freed by calling array_free().
 */
char **array_init(size_t len, const char *initial_value);

/**
 * Append a string to an array of strings.
 * @param char*** Address of the array.
 * @param int len Current size of array.
 * @param char *str The string to append.
 * @param int str_len Length of the string str. Pass -1 for zero-terminated strings.
 * @return int New size of the array.
 */
size_t array_push(char ***array, size_t len, const char *str, int str_len);

/**
 * Destroy an array from array_init().
 * @param char*** Address of the array.
 * @param int len Current size of array.
 */
void array_free(char ***array, size_t len);

/**
 * Split a string and create array of sub-strings.
 * Empty sub-strings are not ignored except it is the last sub-string.  Due to this,
 * an empty string generates an empty array (not an array with one empty element).
 * @param char *str The string to split.
 * @param char *delimiter Where to split.
 * @param char ***output Address of array to setup with sub strings.
 * @return int Size of the created array.
 */
unsigned int explode(const char *str, const char *delimiter, char ***output);

/**
 * Concat all strings in a given array to one string divided by a given delimiter.
 * All NULL input strings are ignored (and do not generate a delimiter).
 * @param char* delimiter The string to use between input strings. NULL is same as emtpy.
 * @param size_t argc Number of elements in argv.
 * @param char** argv The input strings. NULL is ok if argc is 0.
 * @return char* The joined string, allocated with malloc. Caller must free it.
 */
char *str_join(const char *delimiter, size_t argc, const char * const *argv);

int isrunning(const char *program);
int which(const char *program);

void atomicinit(void);
void atomiclock(void);
void atomicunlock(void);

int isNumeric(const char *str);
int nrDecimals(const char *str);
int name2uid(char const *name);
int ishex(int x);
/**
 * Find right-most position of needle in haystack.
 * @param char* haystack Where to search.
 * @param char* needle What to search.
 * @return char* Position in haystack where "last" needle starts.
 */
const char *rstrstr(const char* haystack, const char* needle);

/**
 * Remove sub-strings from a string.
 * @param char *s Where to remove from.
 * @param char *s What to remove.
 * @return Number of removes.
 */ 
size_t rmsubstr(char *s, const char *r);

/**
 * Search and replace sub-strings in an allocated string.
 * @param char* search What to search.
 * @param char* replace Replacement string.
 * @param char** str Address of string to modify; changed on return.
 * @param int Number of replacements done.
 */
size_t str_replacen(const char *search, const char *replace, char **str);

/**
 * Like str_replacen() but different return value semantics.
 * @return int Lenght of result string if there was a replacement. -1 otherwise.
 */
int str_replace(const char *search, const char *replace, char **str);

char *str_trim_left(char *str, const char *trim_away);
char *str_trim_right(char *str, const char *trim_away);
char *str_trim(char *str, const char *trim_away);

void alpha_random(char *s, const int len);
int strcicmp(char const *a, char const *b);

/**
 * Collapse consecutive white space chars to a single blank char (converts tabs to blank).
 * @param char* str String to modify in place.
 * @return char* The result string.
 */
char *uniq_space(char *str);

int urldecode(const char *s, char *dec);
char *urlencode(const char *str);
char *base64encode(const char *src, size_t len);
char *base64decode(const char *src, size_t len, size_t *decsize);
char *hostname(void);
char *distroname(void);
char *genuuid(const char *ifname);
int file_exists(const char *fil);
int path_exists(const char *fil);

#ifdef __FreeBSD__
int findproc(const char *name, const char *args, int loosely);
#else
pid_t findproc(const char *name, const char *args, int loosely);
#endif

int vercmp(const char *val, const char *ref);

// Despite the content read is null-terminated, return the file size or -1
// on error. Caller must treat other negative return values as unsigned!
int file_get_contents(const char *file, char **content);

/*
 * WARNING: In pilight the return value of many functions is vice-versa thinking:
 * 0 means found, non-null means not found.
 * This is in particular for the next _find_ calls (json_get_... work as expected):
 */
int json_find_number(const JsonNode *object_or_array, const char *name, double *out);
int json_find_string(const JsonNode *object_or_array, const char *name, const char **out);

/*
 * can_timeval_diff() checks for initialized monotone time values and
 * is a recommended sanity call that can be done before calling
 * get_timeval_diff_usec() which just * omputes without overflow checks.
 */
#define can_timeval_diff(olderT, newerT)				( \
	(olderT).tv_sec != 0 &&            /* very 1st call */		  \
	(newerT).tv_sec >= (olderT).tv_sec /* system-time change? */	)

#define get_timeval_diff_usec(olderT, newerT)			( \
	( (newerT).tv_sec  - (olderT).tv_sec) * 1000000UL	  \
	+ (newerT).tv_usec - (olderT).tv_usec			)

int check_email_addr(const char *addr, int allow_lists, int check_domain_can_mail);

#endif
