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

void array_free(char ***array, int len);
int isrunning(const char *program);
void atomicinit(void);
void atomiclock(void);
void atomicunlock(void);
unsigned int explode(const char *str, const char *delimiter, char ***output);
char *str_join(const char *delimiter, size_t argc, const char * const *argv);
int isNumeric(const char *str);
int nrDecimals(const char *str);
int name2uid(char const *name);
int which(const char *program);
int ishex(int x);
const char *rstrstr(const char* haystack, const char* needle);
void alpha_random(char *s, const int len);
int urldecode(const char *s, char *dec);
char *urlencode(const char *str);
char *base64encode(const char *src, size_t len);
char *base64decode(const char *src, size_t len, size_t *decsize);
char *hostname(void);
char *distroname(void);
void rmsubstr(char *s, const char *r);
char *genuuid(const char *ifname);
int file_exists(const char *fil);
int path_exists(const char *fil);
char *uniq_space(char *str);

#ifdef __FreeBSD__
int findproc(const char *name, const char *args, int loosely);
#else
pid_t findproc(const char *name, const char *args, int loosely);
#endif

int vercmp(const char *val, const char *ref);
int str_replace(const char *search, const char *replace, char **str);
int strcicmp(char const *a, char const *b);

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

#endif
