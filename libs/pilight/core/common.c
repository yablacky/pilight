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

#ifndef __FreeBSD__
	#define _GNU_SOURCE
#endif
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <libgen.h>
#ifdef _WIN32
	#if _WIN32_WINNT < 0x0501
		#undef _WIN32_WINNT
		#define _WIN32_WINNT 0x0501
	#endif
	#include <winsock2.h>
	#include <windows.h>
	#include <psapi.h>
	#include <tlhelp32.h>
	#include <ws2tcpip.h>
	#include <iphlpapi.h>
#else
	#include <sys/socket.h>
	#include <sys/time.h>
	#include <netinet/in.h>
	#include <netinet/tcp.h>
	#include <netdb.h>
	#include <arpa/inet.h>
	#include <sys/wait.h>
	#include <net/if.h>
	#include <ifaddrs.h>
	#include <pwd.h>
	#include <sys/mount.h>
	#include <sys/ioctl.h>
#endif

#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#ifndef __USE_XOPEN
	#define __USE_XOPEN
#endif
#include <sys/time.h>
#include <time.h>
#include <pthread.h>

#include "../config/settings.h"
#include "mem.h"
#include "common.h"
#include "network.h"
#include "log.h"

char *progname = NULL;
#ifndef _WIN32
static int procmounted = 0;
#endif

static const char base64table[] = {
	'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H',
	'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P',
	'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X',
	'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f',
	'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n',
	'o', 'p', 'q', 'r', 's', 't', 'u', 'v',
	'w', 'x', 'y', 'z', '0', '1', '2', '3',
	'4', '5', '6', '7', '8', '9', '+', '/'
};

static pthread_mutex_t atomic_lock;
static pthread_mutexattr_t atomic_attr;

void atomicinit(void) {
	pthread_mutexattr_init(&atomic_attr);
	pthread_mutexattr_settype(&atomic_attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&atomic_lock, &atomic_attr);
}

void atomiclock(void) {
	pthread_mutex_lock(&atomic_lock);
}

void atomicunlock(void) {
	pthread_mutex_unlock(&atomic_lock);
}

char **array_init(size_t len, const char *initial_value) {
	if(len == 0)
		return NULL;
	char **array = MALLOC_OR_EXIT(len * sizeof(char*));
	while(--len >= 0) {
		array[len] = initial_value ? STRDUP_OR_EXIT(initial_value) : NULL;
	}
	return array;
}

size_t array_push(char ***array, size_t len, const char *str, int str_len) {
	*array = REALLOC_OR_EXIT(*array, sizeof(char *)*(len + 1));
	if(str_len < 0)
		(*array)[len] = STRDUP_OR_EXIT(str);
	else {
		(*array)[len] = MALLOC_OR_EXIT(str_len + 1);
		memcpy((*array)[len], str, str_len);
		(*array)[len][str_len] = '\0';
	}
	return len + 1;
}

void array_free(char ***array, size_t len) {
	while(len-- > 0)	// note: (--len >= 0) goes wrong for unsigned types!
		FREE((*array)[len]);
	if(array) {
		FREE((*array));
		*array = NULL;
	}
}

size_t explode(const char *str, const char *delimiter, char ***output) {
	if(str == NULL || output == NULL) {
		return 0;
	}
	if(delimiter == NULL) {
		return array_push(output, 0, str, -1);
	}
	size_t n = 0;
	size_t delimiter_len = strlen(delimiter);
	const char *found = NULL;

	while((found = strstr(str, delimiter)) != NULL) {
		n = array_push(output, n, str, found - str);
		str = found + delimiter_len;
	}

	// Ignore the last part if emtpy (--> empty string generates emtpy array rather array with one empty string).
	if(*str) {
		n = array_push(output, n, str, -1);
	}
	return n;
}

/**
 * Concat all strings in a given array to one string divided by a given delimiter.
 * All NULL input strings are ignored (and do not generate a delimiter).
 * @param char* delimiter The string to use between input strings. NULL is same as emtpy.
 * @param size_t argc Number of elements in argv.
 * @param char** argv The input strings. NULL is ok if argc is 0.
 * @return char* The joined string, allocated with malloc. Caller must free it.
 */
char *str_join(const char *delimiter, size_t argc, const char * const *argv) {
	char *result = NULL;
	const char *glue = "";
	size_t ii = 0, ac = 0, len = 0;
	if(argc > 0) {
		if(delimiter == NULL) {
			delimiter = "";
		}
		for(ii = 0; ii < argc; ii++) {
			if(argv[ii] != NULL) {
				len += strlen(argv[ii]); 
				ac++;
			}
		}
		if(ac > 0) {
			len += strlen(delimiter) * (ac - 1);
		}
	}
	
	result = MALLOC_OR_EXIT(len+1);
	*result = '\0';
	for(ii = 0; ii < argc; ii++) {
		if(argv[ii] != NULL) {
			strcat(result, glue);
			strcat(result, argv[ii]);
			glue = delimiter;
		}
	}
	return result;
}

#ifdef _WIN32
int check_instances(const wchar_t *prog) {
	HANDLE m_hStartEvent = CreateEventW(NULL, FALSE, FALSE, prog);
	if(m_hStartEvent == NULL) {
		CloseHandle(m_hStartEvent);
		return 0;
	}

	if(GetLastError() == ERROR_ALREADY_EXISTS) {
		CloseHandle(m_hStartEvent);
		m_hStartEvent = NULL;
		return 0;
	}
	return -1;
}

int setenv(const char *name, const char *value, int overwrite) {
	if(name == NULL) {
		errno = EINVAL;
		return -1;
	}
	if(overwrite == 0 && getenv(name) != NULL) {
		return 0; // name already defined and not allowed to overwrite. Treat as OK.
	}
	if(value == NULL) {
		return unsetenv(name);
	}
	char c[strlen(name)+1+strlen(value)+1]; // one for "=" + one for term zero
	strcat(c, name);
	strcat(c, "=");
	strcat(c, value);
	return putenv(c);
}

int unsetenv(const char *name) {
	if(name == NULL) {
		errno = EINVAL;
		return -1;
	}
	char c[strlen(name)+1+1]; // one for "=" + one for term zero
	strcat(c, name);
	strcat(c, "=");
	return putenv(c);
}

int isrunning(const char *program) {
	DWORD aiPID[1000], iCb = 1000;
	DWORD iCbneeded = 0;
	int iNumProc = 0, i = 0;
	char szName[MAX_PATH];
	int iLenP = 0;
	HANDLE hProc;
	HMODULE hMod;

	iLenP = strlen(program);
	if(iLenP < 1 || iLenP > MAX_PATH)
		return -1;

	if(EnumProcesses(aiPID, iCb, &iCbneeded) <= 0) {
		return -1;
	}

	iNumProc = iCbneeded / sizeof(DWORD);

	for(i=0;i<iNumProc;i++) {
		strcpy(szName, "Unknown");
		hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, aiPID[i]);

		if(hProc) {
			if(EnumProcessModules(hProc, &hMod, sizeof(hMod), &iCbneeded)) {
				GetModuleBaseName(hProc, hMod, szName, MAX_PATH);
			}
		}
		CloseHandle(hProc);

		if(strstr(szName, program) != NULL) {
			return aiPID[i];
		}
	}

	return -1;
}
#else
int isrunning(const char *program) {
	int pid = findproc(program, NULL, 1);
	return pid > 0 ? pid : -1;
}
#endif

#ifdef __FreeBSD__
int findproc(const char *cmd, const char *args, int loosely) {
#else
pid_t findproc(const char *cmd, const char *args, int loosely) {
#endif
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

#ifndef _WIN32
	DIR* dir;
	struct dirent* ent;
	char fname[512], cmdline[1024];
	int fd = 0, ptr = 0, i = 0, y = '\n';

	if(procmounted == 0) {
		if(!(dir = opendir("/proc"))) {
			logprintf(LOG_ERR, "/proc filesystem not properly mounted");
			return -1;
		}
		for(i = 0; readdir(dir) != NULL; i++)
			;
		closedir(dir);
		if(i == 2) {
#ifdef __FreeBSD__
			mount("procfs", "/proc", 0, "");
#else
			mount("proc", "/proc", "procfs", 0, "");
#endif
			if((dir = opendir("/proc"))) {
				for(i = 0; readdir(dir) != NULL; i++)
					;
				closedir(dir);
				if(i == 2) {
					logprintf(LOG_ERR, "/proc filesystem not properly mounted");
					return -1;
				}
			}
		}
		procmounted = 1;
	}
	if(!(dir = opendir("/proc"))) {
		return -1;
	}
	while((ent = readdir(dir)) != NULL) {
		if(isNumeric(ent->d_name) != 0) {
			continue;
		}
		snprintf(fname, sizeof(fname), "/proc/%s/cmdline", ent->d_name);
		if((fd = open(fname, O_RDONLY, 0)) < 0) {
			continue;
		}
		ptr = (int)read(fd, cmdline, sizeof(cmdline)-1);
		close(fd);
		if(ptr < 0) {
			continue;
		}
		i = 0, y = '\n';
		/* Replace all NULL terminators for newlines */
		for(i=0;i<ptr-1;i++) {
			if(i < ptr && cmdline[i] == '\0') {
				cmdline[i] = (char)y;
				y = ' ';
			}
		}

		/* Check if program matches */
		char **array = NULL;
		unsigned int n = explode(cmdline, "\n", &array);
		if(n > 0
		    && loosely ? strstr(array[0], cmd) != NULL : strcmp(array[0], cmd) == 0
		    && (args == NULL || (n >= 2 && strcmp(array[1], args) == 0))) {
			array_free(&array, n);
			closedir(dir);
			return (pid_t)atol(ent->d_name);
		}
		array_free(&array, n);
	}
	closedir(dir);
#endif
	return -1;
}

int isNumeric(const char *s) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	if(s == NULL || *s == '\0' || *s == ' ')
		return -1;

	char *p = NULL;
	strtod(s, &p);
	return (*p == '\0') ? 0 : -1;
}

int nrDecimals(const char *s) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	unsigned int b = 0, c = strlen(s), i = 0;
	int a = 0;
	for(i=0;i<c;i++) {
		if(b == 1) {
			a++;
		}
		if(s[i] == '.') {
			b = 1;
		}
	}
	return a;
}

int name2uid(char const *name) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

#ifndef _WIN32
	if(name != NULL) {
		struct passwd *pwd = getpwnam(name); /* don't free, see getpwnam() for details */
		if(pwd) {
			return (int)pwd->pw_uid;
		}
	}
#endif
	return -1;
}

int which(const char *program) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	char path[1024];
	strcpy(path, getenv("PATH"));
	char **array = NULL;
	unsigned int n = 0, i = 0;
	int found = -1;

	n = explode(path, ":", &array);
	for(i=0;i<n;i++) {
		char exec[strlen(array[i])+8];
		strcpy(exec, array[i]);
		strcat(exec, "/");
		strcat(exec, program);

		if(access(exec, X_OK) != -1) {
			found = 0;
			break;
		}
	}
	for(i=0;i<n;i++) {
		FREE(array[i]);
	}
	if(n > 0) {
		FREE(array);
	}
	return found;
}

int ishex(int x) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	return(x >= '0' && x <= '9') || (x >= 'a' && x <= 'f') || (x >= 'A' && x <= 'F');
}

const char *rstrstr(const char* haystack, const char* needle) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	char* loc = 0;
	char* found = 0;
	size_t pos = 0;

	while ((found = strstr(haystack + pos, needle)) != 0) {
		loc = found;
		pos = (size_t)((found - haystack) + 1);
	}

	return loc;
}

void alpha_random(char *s, const int len) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	static const char alphanum[] =
			"0123456789"
			"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
			"abcdefghijklmnopqrstuvwxyz";
	int i = 0;

	for(i = 0; i < len; ++i) {
			s[i] = alphanum[(unsigned int)rand() % (sizeof(alphanum) - 1)];
	}

	s[len] = 0;
}

int urldecode(const char *s, char *dec) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	char *o = NULL;
	const char *end = s + strlen(s);
	int c = 0;

	for(o = dec; s <= end; o++) {
		c = *s++;
		if(c == '+') {
			c = ' ';
		} else if(c == '%' && (!ishex(*s++) || !ishex(*s++)	|| !sscanf(s - 2, "%2x", &c))) {
			return -1;
		}
		if(dec) {
			sprintf(o, "%c", c);
		}
	}

	return (int)(o - dec);
}

static char to_hex(char code) {
  static char hex[] = "0123456789abcdef";
  return hex[code & 15];
}

char *urlencode(const char *pstr) {
	char *buf = MALLOC_OR_EXIT(strlen(pstr) * 3 + 1), *pbuf = buf;
	while(*pstr) {
		if(isalnum(*pstr) || *pstr == '-' || *pstr == '_' || *pstr == '.' || *pstr == '~')
			*pbuf++ = *pstr;
		else if(*pstr == ' ')
			*pbuf++ = '+';
		else
			*pbuf++ = '%', *pbuf++ = to_hex(*pstr >> 4), *pbuf++ = to_hex(*pstr & 15);
		pstr++;
	}
	*pbuf = '\0';
	return buf;
}

char *base64decode(const char *src, size_t len, size_t *decsize) {
  unsigned int i = 0;
  unsigned int j = 0;
  unsigned int l = 0;
  size_t size = 0;
  char *dec = NULL;
  char buf[3];
  char tmp[4];

  dec = NULL;

  while(len--) {
    if('=' == src[j]) {
			break;
		}
    if(!(isalnum(src[j]) || src[j] == '+' || src[j] == '/')) {
			break;
		}

    tmp[i++] = src[j++];

    if(i == 4) {
      for(i = 0; i < 4; ++i) {
        for(l = 0; l < 64; ++l) {
          if(tmp[i] == base64table[l]) {
            tmp[i] = (char)l;
            break;
          }
        }
      }

      buf[0] = (char)((tmp[0] << 2) + ((tmp[1] & 0x30) >> 4));
      buf[1] = (char)(((tmp[1] & 0xf) << 4) + ((tmp[2] & 0x3c) >> 2));
      buf[2] = (char)(((tmp[2] & 0x3) << 6) + tmp[3]);

      dec = REALLOC_OR_EXIT(dec, size + 3);
      for(i = 0; i < 3; ++i) {
        dec[size++] = buf[i];
      }

      i = 0;
    }
  }

  if(i > 0) {
    for(j = i; j < 4; ++j) {
      tmp[j] = '\0';
    }

    for(j = 0; j < 4; ++j) {
			for(l = 0; l < 64; ++l) {
				if(tmp[j] == base64table[l]) {
					tmp[j] = (char)l;
					break;
				}
			}
    }

    buf[0] = (char)((tmp[0] << 2) + ((tmp[1] & 0x30) >> 4));
    buf[1] = (char)(((tmp[1] & 0xf) << 4) + ((tmp[2] & 0x3c) >> 2));
    buf[2] = (char)(((tmp[2] & 0x3) << 6) + tmp[3]);

    dec = REALLOC_OR_EXIT(dec, (size_t)(size + (size_t)(i - 1)));
    for(j = 0; (j < i - 1); ++j) {
      dec[size++] = buf[j];
    }
  }

  dec = REALLOC_OR_EXIT(dec, size + 1);
  dec[size] = '\0';

  if(decsize != NULL) {
		*decsize = size;
	}

  return dec;
}

char *base64encode(const char *src, size_t len) {
  unsigned int i = 0;
  unsigned int j = 0;
  char *enc = NULL;
  size_t size = 0;
  char buf[4];
  char tmp[3];

  enc = NULL;

  while(len--) {
    tmp[i++] = *(src++);

    if(i == 3) {
      buf[0] = (char)((tmp[0] & 0xfc) >> 2);
      buf[1] = (char)(((tmp[0] & 0x03) << 4) + ((tmp[1] & 0xf0) >> 4));
      buf[2] = (char)(((tmp[1] & 0x0f) << 2) + ((tmp[2] & 0xc0) >> 6));
      buf[3] = (char)(tmp[2] & 0x3f);

      enc = REALLOC_OR_EXIT(enc, size + 4);
      for(i = 0; i < 4; ++i) {
        enc[size++] = base64table[(int)buf[i]];
      }

      i = 0;
    }
  }

  if(i > 0) {
    for(j = i; j < 3; ++j) {
      tmp[j] = '\0';
    }

		buf[0] = (char)((tmp[0] & 0xfc) >> 2);
		buf[1] = (char)(((tmp[0] & 0x03) << 4) + ((tmp[1] & 0xf0) >> 4));
		buf[2] = (char)(((tmp[1] & 0x0f) << 2) + ((tmp[2] & 0xc0) >> 6));
		buf[3] = (char)(tmp[2] & 0x3f);

    for(j = 0; (j < i + 1); ++j) {
      enc = REALLOC_OR_EXIT(enc, size+1);
      enc[size++] = base64table[(int)buf[j]];
    }

    while((i++ < 3)) {
      enc = REALLOC_OR_EXIT(enc, size+1);
      enc[size++] = '=';
    }
  }

  enc = REALLOC_OR_EXIT(enc, size+1);
  enc[size] = '\0';

  return enc;
}

size_t rmsubstr(char *str, const char *rem) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	size_t cnt = 0;
	size_t len = strlen(rem);
	while((str = strstr(str, rem)) != NULL) {
		memmove(str, str+len, strlen(str+len) + 1);
		cnt++;
	}
	return cnt;
}

char *hostname(void) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	char name[255] = {'\0'}, *dot = NULL;

	gethostname(name, sizeof(name)-1);
	dot = strchr(name, '.');
	if (dot != NULL) {
		*dot = '\0';
	}
	return STRDUP_OR_EXIT(name);
}

char *distroname(void) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	const char *dist = NULL;

#ifdef _WIN32
	dist =  "Windows";
#elif defined(__FreeBSD__)
	dist = "FreeBSD/0.0";
#else
	int rc = 1;
	struct stat sb;
	if((rc = stat("/etc/redhat-release", &sb)) == 0) {
		dist = "RedHat/0.0";
	} else if((rc = stat("/etc/SuSE-release", &sb)) == 0) {
		dist = "SuSE/0.0";
	} else if((rc = stat("/etc/mandrake-release", &sb)) == 0) {
		dist = "Mandrake/0.0";
	} else if((rc = stat("/etc/debian-release", &sb)) == 0) {
		dist = "Debian/0.0";
	} else if((rc = stat("/etc/debian_version", &sb)) == 0) {
		dist = "Debian/0.0";
	} else {
		dist = "Unknown/0.0";
	}
#endif
	if(strlen(dist) > 0) {
		return STRDUP_OR_EXIT(dist);
	} else {
		return NULL;
	}
}

/* The UUID is either generated from the
   processor serial number or from the
   onboard LAN controller mac address */
char *genuuid(const char *ifname) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	char mac[ETH_ALEN], *upnp_id = NULL, *p = mac;
	char serial[UUID_LENGTH+1];

	memset(serial, '\0', UUID_LENGTH+1);
#ifndef _WIN32
	char a[1024];
	FILE *fp = fopen("/proc/cpuinfo", "r");
	if(fp != NULL) {
		while(!feof(fp)) {
			if(fgets(a, 1024, fp) == 0) {
				break;
			}
			if(strstr(a, "Serial") != NULL) {
				sscanf(a, "Serial          : %16s%*[ \n\r]", (char *)&serial);
				if(strlen(serial) > 0 &&
					 ((isNumeric(serial) == EXIT_SUCCESS && atoi(serial) > 0) ||
					  (isNumeric(serial) == EXIT_FAILURE))) {
					memmove(&serial[5], &serial[4], 16);
					serial[4] = '-';
					memmove(&serial[8], &serial[7], 13);
					serial[7] = '-';
					memmove(&serial[11], &serial[10], 10);
					serial[10] = '-';
					memmove(&serial[14], &serial[13], 7);
					serial[13] = '-';
					upnp_id = MALLOC_OR_EXIT(UUID_LENGTH+1);
					strcpy(upnp_id, serial);
					fclose(fp);
					return upnp_id;
				}
			}
		}
		fclose(fp);
	}

#endif
	if(dev2mac(ifname, &p) == 0) {
		upnp_id = MALLOC_OR_EXIT(UUID_LENGTH+1);
		memset(upnp_id, '\0', UUID_LENGTH+1);
		snprintf(upnp_id, UUID_LENGTH,
				"0000-%02x-%02x-%02x-%02x%02x%02x",
				(unsigned char)p[0], (unsigned char)p[1], (unsigned char)p[2],
				(unsigned char)p[3], (unsigned char)p[4], (unsigned char)p[5]);
		return upnp_id;
	}

	return NULL;
}

/* Check if a given file exists */
int file_exists(const char *filename) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	struct stat sb;
	return stat(filename, &sb);
}

/* Check if a given path exists */
int path_exists(const char *fil) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	struct stat s;
	char tmp[strlen(fil)+1];
	strcpy(tmp, fil);

	atomiclock();
	/* basename isn't thread safe and it requires a non-const ptr */
	char *filename = basename(tmp);
	atomicunlock();

	char path[(strlen(tmp)-strlen(filename))+1];
	size_t i = (strlen(tmp)-strlen(filename));

	memset(path, '\0', sizeof(path));
	memcpy(path, tmp, i);
	snprintf(path, i, "%s", tmp);

/*
 * dir stat doens't work on windows if path has a trailing slash
 */
#ifdef _WIN32
	if(i > 0 && path[i-1] == '\\' || path[i-1] == '/') {
		path[i-1] = '\0';
	}
#endif

	if(strcmp(filename, tmp) != 0) {
		int err = stat(path, &s);
		if(err == -1) {
			if(ENOENT == errno) {
				return EXIT_FAILURE;
			} else {
				return EXIT_FAILURE;
			}
		} else {
			if(S_ISDIR(s.st_mode)) {
				return EXIT_SUCCESS;
			} else {
				return EXIT_FAILURE;
			}
		}
	}
	return EXIT_SUCCESS;
}

/* Copyright (C) 1995 Ian Jackson <iwj10@cus.cam.ac.uk> */
/* Copyright (C) 1995 Ian Jackson <iwj10@cus.cam.ac.uk> */
//  1: val > ref
// -1: val < ref
//  0: val == ref
int vercmp(const char *val, const char *ref) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	int vc, rc;
	long vl, rl;
	const char *vp, *rp;
	const char *vsep, *rsep;

	if(!val) {
		val = "";
	}
	if(!ref) {
		ref = "";
	}
	while(1) {
		vp = val;
		while(*vp && !isdigit(*vp)) {
			vp++;
		}
		rp = ref;
		while(*rp && !isdigit(*rp)) {
			rp++;
		}
		while(1) {
			vc =(val == vp) ? 0 : *val++;
			rc =(ref == rp) ? 0 : *ref++;
			if(!rc && !vc) {
				break;
			}
			if(vc && !isalpha(vc)) {
				vc += 256;
			}
			if(rc && !isalpha(rc)) {
				rc += 256;
			}
			if(vc != rc) {
				return vc - rc;
			}
		}
		val = vp;
		ref = rp;
		vl = 0;
		if(isdigit(*vp)) {
			vl = strtol(val, (char**)&val, 10);
		}
		rl = 0;
		if(isdigit(*rp)) {
			rl = strtol(ref, (char**)&ref, 10);
		}
		if(vl != rl) {
			return (int)(vl - rl);
		}

		vc = *val;
		rc = *ref;
		vsep = strchr(".-", vc);
		rsep = strchr(".-", rc);

		if((vsep && !rsep) || !*val) {
			return 0;
		}

		if((!vsep && rsep) || !*ref) {
			return +1;
		}

		if(!*val && !*ref) {
			return 0;
		}
	}
}

char *uniq_space(char *str) {
	char *from = str, *to = str;

	while((*to++) == (*from++)) {
		if(to[-1] == ' ' || (to[-1] == '\t' && (to[-1] = ' '))) // convert tab to blank.
			while(*from == ' ' || *from == '\t')
				from++;
	}
	return str;
}

size_t str_replacen(const char *search, const char *replace, char **str) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	if(!str || !*str) {
		return 0;
	}
	if(!search || !*search) {
		return 0;
	}
	char *target = *str;
	size_t match = 0;
	size_t rlen = replace ? strlen(replace) : 0;
	if(rlen == 0) {
		match = rmsubstr(target, search);
		if(match) {
			*str = target = REALLOC_OR_EXIT(target, strlen(target)+1);
		}
		return match;
	}

	size_t slen = strlen(search);
	if(slen == rlen) {
		while((target = strstr(target, search)) != 0) {
			memcpy(target, replace, rlen);
			target += slen;
			match++;
		}
		return match;
	}

	while((target = strstr(target, search)) != 0) {
		target += slen;
		match++;
	}
	if(match) {
		char *source = *str, *src;
		char *dst = target = MALLOC_OR_EXIT(strlen(source) + match*rlen - match*slen);

		while((src = strstr(source, search)) != 0) {
			size_t len = src - source;
			memcpy(dst, source, len);
			memcpy(dst += len, replace, rlen);
			dst += rlen;
			source = src + slen;
		}
		strcpy(dst, source);

		FREE(*str);
		*str = target;
	}
	return match;
}

int str_replace(const char *search, const char *replace, char **str) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	// Generate strange return value...
	return str_replacen(search, replace, str) ? (int) strlen(*str) : -1;
}

int stricmp(char const *a, char const *b) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	for(;; a++, b++) {
			int d = *a - *b;
			if(d != 0)
				d = tolower(*a) - tolower(*b);
			if(d != 0 || !*a)
				return d;
	}
}

// Despite the content read is null-terminated, return the file size or -1
// on error. Caller must treat other negative return values as unsigned!
int file_get_contents(const char *file, char **content) {
	FILE *fp = NULL;
	size_t bytes = 0;
	struct stat st;

	if((fp = fopen(file, "rb")) == NULL) {
		logprintf(LOG_ERR, "cannot open file '%s' for reading (%s)", file, strerror(errno));
		return -1;
	}

	fstat(fileno(fp), &st);
	bytes = (size_t)st.st_size;

	if (content != NULL) {
		if((*content = MALLOC(bytes+1)) == NULL) {
			fprintf(stderr, "out of memory while reading file: '%s'\n", file);
			fclose(fp);
			return -1;
		}

		if(fread(*content, sizeof(char), bytes, fp) == -1) {
			logprintf(LOG_ERR, "cannot read file: '%s' (%s)", file, strerror(errno));
			fclose(fp);
			FREE(*content);
			return -1;
		}
		(*content)[bytes] = '\0';
	}

	fclose(fp);
	int ret = (int) bytes;
	if (ret == -1)	// overflow (file size is 0xFFFFFFFF bytes, which is rare...)
		ret = (int) (bytes - 1);	// return one less is also wrong but it is really rare...
	return ret;
}

int json_find_number(const JsonNode *object_or_array, const char *name, double *out) {
	// incredible but json_find_number must return 0 on success and 1 on failure...
	return json_get_number(object_or_array, name, out) ? 0 : 1;
}
int json_find_string(const JsonNode *object_or_array, const char *name, const char **out) {
	// incredible but json_find_string must return 0 on success and 1 on failure...
	return json_get_string(object_or_array, name, out) ? 0 : 1;
}

char *str_trim_right(char *str, const char *trim_away) {
	char *end = str;
	if (str != NULL && trim_away != NULL && *trim_away != '\0') {
		for (end += strlen(str); end > str && strchr(trim_away, end[-1]) != NULL; --end)
			;
		*end = '\0';
	}
	return str;
}

char *str_trim_left(char *str, const char *trim_away) {
	return str != NULL && trim_away != NULL && *trim_away != '\0'
		? str + strspn(str, trim_away)
		: str;
}

char *str_trim(char *str, const char *trim_away) {
	return str_trim_left(str_trim_right(str, trim_away), trim_away);
}

int checkdnsrr(const char *domain, const char *type)
{
	logprintf(LOG_STACK, "%s(%s,%s)", __FUNCTION__, domain ? domain : "(NULL)", type ? type : "(NULL)");
	return 0;	// NYI.
}

/**
 * Check if an email address is valid.
 * @param char* address The email address to check. Leading+trailing white spaces are accepted.
 * @param bool allow_lists If true, addr is allowed to be a comma separated list of email addresses.
 * @param bool check_domain_can_mail If true, check if domain exists in DNS system and accepts email.
 * @return int Values < 0 indicate error. Values >= 0 indicate success.
 * Where -1: syntax error, -2: DNS check for a domain failed, -8: severe internal error.
 */
int check_email_addr(const char *address, int allow_lists, int check_domain_can_mail) {
	if(address == NULL) {
		return -1;
	}

	#define ALPHANUMERICS "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"

	static const char white_spaces[] = " \t";
	static const char local_valid_chars[] = ALPHANUMERICS ".!#$%&'*+-\\/=?^_`{|}~";
	static const char domain_valid_chars[] = ALPHANUMERICS ".-"; // . and - are not allowed everywhere.

	const char *at = NULL, *domain_name = NULL;
	char *addr = NULL;
	char **addr_list = NULL;
	size_t addr_count = explode(address, ",", &addr_list), ii = 0;
	int ret = -1;
	for(ii = 0; ii < addr_count; ii++) {
		ret = -1;	// assume error
		if (allow_lists == 0 && addr_count > 1) {
			break;	// only one address allowed.
		}
		addr = addr_list[ii];
		addr = str_trim(addr, white_spaces);

		// Now check addr. First some quick pre-checks:
		at = strchr(addr, '@');
		if(at == NULL || strstr(addr, "..") != NULL) {
			break;	// no @ found or two consecutive dots found somewhere.
		}
		domain_name = at+1;

		if(at == addr || at[-1] == '.' || addr[0] == '.') {
			break;	// local part emtpy, or starts or ends with a dot.
		}
		if(addr+strspn(addr, local_valid_chars) < at) {
			break;	// local part contains invalid chars.
		}
		if(strchr(domain_name, '.') == NULL || strlen(domain_name) < 5) {
			break;	// not at least one dot in domain or domain is not at least 2 x 2 chars.
		}
		if(strchr(".-", domain_name[0]) != NULL || strchr(".-", domain_name[strlen(domain_name)-1]) != NULL) {
			break;	// domain starts or ends with a dot or dash.
		}
		if(strstr(domain_name, ".-") != NULL || strstr(domain_name, "-.") != NULL) {
			break;	// a dot delimited domain part starts or ends with a dash.
		}
		if(domain_name[strspn(domain_name, domain_valid_chars)] != '\0') {
			break;	// domain contains invalid chars.
		}

		// Finally check domain name against DNS if desired:

		if(check_domain_can_mail != 0 && checkdnsrr(domain_name, "MX") < 0) {
			ret = -2;
			break;	// domain not found or does not accept emails.
		}

		ret = 0; // Address ok (and all ok if this is the last one in the list).
	}
	array_free(&addr_list, addr_count);
	return ret;

	#undef ALPHANUMERICS
}
