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
#include <stdarg.h>
#include <errno.h>
#include <sys/time.h>
#include <time.h>
#include <string.h>
#include <unistd.h>
#include <libgen.h>
#include <sys/stat.h>
#ifndef _WIN32
	#ifdef __mips__
		#define __USE_UNIX98
	#endif
#endif
#include <pthread.h>

#include "pilight.h"
#include "common.h"
#include "gc.h"
#include "log.h"

struct logqueue_t {
	char *line;
	struct logqueue_t *next;
} logqueue_t;

static pthread_mutex_t logqueue_lock;
static pthread_cond_t logqueue_signal;
static pthread_mutexattr_t logqueue_attr;

static struct logqueue_t *logqueue;
static struct logqueue_t *logqueue_head;
static unsigned int logqueue_number = 0;
static unsigned int loop = 1;
static unsigned int stop = 0;
static unsigned int pthinitialized = 0;
static unsigned int pthactive = 0;
static unsigned int pthfree = 0;
static pthread_t pth;

static char *logfile = NULL;
static int filelog = 1;
static int shelllog = 0;
static int loglevel = LOG_DEBUG;

void logwrite(char *line) {
	struct stat sb;
	FILE *lf = NULL;
	if(logfile != NULL) {
		if((stat(logfile, &sb)) == 0) {
			if(sb.st_nlink != 0 && sb.st_size > LOG_MAX_SIZE) {
				if(lf != NULL) {
					fclose(lf);
				}
				char tmp[strlen(logfile)+5];
				strcpy(tmp, logfile);
				strcat(tmp, ".old");
				rename(logfile, tmp);
			}
		}
		if((lf = fopen(logfile, "a")) == NULL) {
			filelog = 0;
		} else {
			fputs(line, lf);
			fflush(lf);
			fclose(lf);
			lf = NULL;
		}
	} else if(shelllog && pthinitialized) {
		fputs(line, stderr);
	}
}

static void init_logqueue() {
	if(pthinitialized == 0) {
		pthread_mutexattr_init(&logqueue_attr);
		pthread_mutexattr_settype(&logqueue_attr, PTHREAD_MUTEX_RECURSIVE);
		pthread_mutex_init(&logqueue_lock, &logqueue_attr);
		pthread_cond_init(&logqueue_signal, NULL);
		pthinitialized = 1;
	}
}

int log_gc(void) {
	if(shelllog == 1) {
		fprintf(stderr, "DEBUG: garbage collected log library\n");
	}

	stop = 1;
	loop = 0;

	if(pthinitialized == 1) {
		pthread_mutex_unlock(&logqueue_lock);
		pthread_cond_signal(&logqueue_signal);
	}

	/* Flush log queue to pilight.err file */
	if(pthactive == 0) {
		struct logqueue_t *tmp;
		while(logqueue) {
			tmp = logqueue;
			if(tmp->line != NULL) {
				if(filelog == 1 && logfile != NULL) {
					logwrite(tmp->line);
				} else {
					/* [ Datetime ] Progname: */
					/*  24 + 14 + 2 */
					size_t pos = 24+strlen(progname)+3;
					size_t len = strlen(tmp->line);
					memmove(&tmp->line[0], &tmp->line[pos], len-pos);
					/* Remove newline */
					tmp->line[(len-pos)-1] = '\0';
					logerror(tmp->line);
				}
				FREE(tmp->line);
			}
			logqueue = logqueue->next;
			FREE(tmp);
			logqueue_number--;
		}
		if(logqueue != NULL) {
			FREE(logqueue);
		}
		if(pthfree == 1) {
			pthread_join(pth, NULL);
		}
	} else {
		/* Flush log queue by log thread */
		while(logqueue_number > 0) {
			usleep(10);
		}
		while(pthactive > 0) {
			usleep(10);
		}
		pthread_join(pth, NULL);
	}
	if(logfile != NULL) {
		FREE(logfile);
	}
	return 1;
}

void logprintf(int prio, const char *format_str, ...) {
	if(loglevel < prio) {
		return;
	}
	struct timeval tv;
	struct tm tm, *ptm;
	va_list ap, apcpy;
	char fmt[64], buf[64], *line = MALLOC_OR_EXIT(128);
	int save_errno = -1, pos = 0, bytes = 0;

	memset(&tm, '\0', sizeof(struct tm));

	save_errno = errno;

	memset(line, '\0', 128);
	memset(buf, '\0',  64);

	if(loglevel >= prio) {
		gettimeofday(&tv, NULL);
#ifdef _WIN32
		ptm = gmtime(&tv.tv_sec);
#else
		ptm = gmtime_r(&tv.tv_sec, &tm);
#endif
		if (ptm) {
			strftime(fmt, sizeof(fmt), "%b %d %H:%M:%S", ptm);
			snprintf(buf, sizeof(buf), "%s:%06u", fmt, (unsigned int)tv.tv_usec);
		}
		pos += sprintf(line, "[%22.22s] %s: ", buf, progname);

		switch(prio) {
			case LOG_WARNING:
				pos += sprintf(&line[pos], "WARNING: ");
			break;
			case LOG_ERR:
				pos += sprintf(&line[pos], "ERROR: ");
			break;
			case LOG_INFO:
				pos += sprintf(&line[pos], "INFO: ");
			break;
			case LOG_NOTICE:
				pos += sprintf(&line[pos], "NOTICE: ");
			break;
			case LOG_DEBUG:
				pos += sprintf(&line[pos], "DEBUG: ");
			break;
			case LOG_STACK:
				pos += sprintf(&line[pos], "STACK: ");
			break;
			default:
			break;
		}

		va_copy(apcpy, ap);
		va_start(apcpy, format_str);
#ifdef _WIN32
		bytes = _vscprintf(format_str, apcpy);
#else
		bytes = vsnprintf(NULL, 0, format_str, apcpy);
#endif
		if(bytes == -1) {
			fprintf(stderr, "ERROR: unproperly formatted logprintf message %s\n", format_str);
		} else {
			va_end(apcpy);
			line = REALLOC_OR_EXIT(line, (size_t)bytes+(size_t)pos+3);
			va_start(ap, format_str);
			pos += vsprintf(&line[pos], format_str, ap);
			va_end(ap);
		}
		line[pos++]='\n';
		line[pos++]='\0';
	}
	if(shelllog == 1 && !pthinitialized) {
		fputs(line, stderr);
	}
#ifdef _WIN32
	if(prio == LOG_ERR && strstr(progname, "daemon") != NULL && pilight.running == 0) {
		MessageBox(NULL, line, "pilight :: error", MB_OK);
	}
#endif
	if(stop == 0 && pos > 0) {
		if(prio < LOG_DEBUG) {
			if(pthinitialized == 1) {
				pthread_mutex_lock(&logqueue_lock);
			}
			if(logqueue_number < 1024) {
				struct logqueue_t *node = MALLOC_OR_EXIT(sizeof(logqueue_t));
				node->line = line; line = NULL;
				node->next = NULL;

				if(logqueue_number == 0) {
					logqueue = node;
					logqueue_head = node;
				} else {
					logqueue_head->next = node;
					logqueue_head = node;
				}

				logqueue_number++;
			} else {
				fprintf(stderr, "log queue full\n");
			}
			if(pthinitialized == 1) {
				pthread_mutex_unlock(&logqueue_lock);
				pthread_cond_signal(&logqueue_signal);
			}
		}
	}
	FREE(line);
	errno = save_errno;
}

void *logloop(void *param) {
	pth = pthread_self();

	pthactive = 1;
	pthfree = 1;

	pthread_mutex_lock(&logqueue_lock);
	while(loop) {
		if(logqueue_number > 0) {
			pthread_mutex_lock(&logqueue_lock);

			logwrite(logqueue->line);

			struct logqueue_t *tmp = logqueue;
			FREE(tmp->line);
			logqueue = logqueue->next;
			FREE(tmp);
			logqueue_number--;
			pthread_mutex_unlock(&logqueue_lock);
		} else {
			pthread_cond_wait(&logqueue_signal, &logqueue_lock);
		}
	}

	pthactive = 0;
	return (void *)NULL;
}

void logperror(int prio, const char *s) {
	// int save_errno = errno;
	// if(logging == 0)
		// return;

	// if(s != NULL) {
		// logprintf(prio, "%s: %s", s, strerror(errno));
	// } else {
		// logprintf(prio, "%s", strerror(errno));
	// }
	// errno = save_errno;
}

void log_file_enable(void) {
	filelog = 1;
	init_logqueue();
}

void log_file_disable(void) {
	filelog = 0;
}

void log_shell_enable(void) {
	shelllog = 1;
}

void log_shell_disable(void) {
	shelllog = 0;
}

int log_file_set(const char *log) {
	struct stat s;
	struct stat sb;
	char *logpath = NULL;
	FILE *lf = NULL;

	if(log == NULL) {
		FREE(logpath);
		logpath = NULL;
		return EXIT_SUCCESS;
	}

	char buf[strlen(log) + 1];
	strcpy(buf, log);
	atomiclock();
	/* basename isn't thread safe and it requires a non-const ptr */
	char *filename = basename(buf);
	atomicunlock();

	size_t i = (strlen(log)-strlen(filename));
	logpath = REALLOC_OR_EXIT(logpath, i+1);
	memset(logpath, '\0', i+1);
	strncpy(logpath, log, i);

/*
 * dir stat doens't work on windows if path has a trailing slash
 */
#ifdef _WIN32
	if(logpath[i-1] == '\\' || logpath[i-1] == '/') {
		logpath[i-1] = '\0';
	}
#endif

	if(strcmp(filename, log) != 0) {
		int err = stat(logpath, &s);
		if(err == -1) {
			if(ENOENT == errno) {
				logprintf(LOG_ERR, "the log folder %s does not exist", logpath);
				FREE(logpath);
				return EXIT_FAILURE;
			} else {
				logprintf(LOG_ERR, "failed to run stat on log folder %s", logpath);
				FREE(logpath);
				return EXIT_FAILURE;
			}
		} else {
			if(S_ISDIR(s.st_mode)) {
				logfile = REALLOC_OR_EXIT(logfile, strlen(log)+1);
				strcpy(logfile, log);
			} else {
				logprintf(LOG_ERR, "the log folder %s does not exist", logpath);
				FREE(logpath);
				return EXIT_FAILURE;
			}
		}
	} else {
		logfile = REALLOC_OR_EXIT(logfile, strlen(log)+1);
		strcpy(logfile, log);
	}

	char tmp[strlen(logfile)+5];
	strcpy(tmp, logfile);
	strcat(tmp, ".old");

	if((stat(tmp, &sb)) == 0) {
		if(sb.st_nlink > 0) {
			if((stat(logfile, &sb)) == 0) {
				if(sb.st_nlink > 0) {
					remove(tmp);
					rename(logfile, tmp);
				}
			}
		}
	}

	if(lf == NULL && filelog == 1) {
		if((lf = fopen(logfile, "a")) == NULL) {
			filelog = 0;
			shelllog = 1;
			logprintf(LOG_ERR, "could not open logfile %s", logfile);
			FREE(logpath);
			FREE(logfile);
			exit(EXIT_FAILURE);
		} else {
			fclose(lf);
			lf = NULL;
		}
	}

	FREE(logpath);
	return EXIT_SUCCESS;
}

void log_level_set(int level) {
	loglevel = level;
}

int log_level_get(void) {
	return loglevel;
}

void logerror(const char *format_str, ...) {
	char line[1024];
	va_list ap;
	struct stat sb;
	FILE *f = NULL;
	char fmt[64], buf[64];
	struct timeval tv;
	struct tm tm;
	char date[128];
#ifdef _WIN32
	const char *errpath = "c:/ProgramData/pilight/pilight.err";
#else
	const char *errpath = "/var/log/pilight.err";
#endif
	memset(line, '\0', 1024);
	memset(&ap, '\0', sizeof(va_list));
	memset(&sb, '\0', sizeof(struct stat));
	memset(&tv, '\0', sizeof(struct timeval));
	memset(date, '\0', 128);
	memset(&tm, '\0', sizeof(struct tm));

	gettimeofday(&tv, NULL);
#ifdef _WIN32
		if((localtime(&tv.tv_sec)) != 0) {
#else
		if((localtime_r(&tv.tv_sec, &tm)) != 0) {
#endif
		strftime(fmt, sizeof(fmt), "%b %d %H:%M:%S", &tm);
		snprintf(buf, sizeof(buf), "%s:%03u", fmt, (unsigned int)tv.tv_usec);
	}

	sprintf(date, "[%22.22s] %s: ", buf, progname);
	strcat(line, date);
	va_start(ap, format_str);
	vsprintf(&line[strlen(line)], format_str, ap);
	strcat(line, "\n");

	if((stat(errpath, &sb)) >= 0) {
		if((f = fopen(errpath, "a")) == NULL) {
			return;
		}
	} else {
		if(sb.st_nlink == 0) {
			if(!(f = fopen(errpath, "a"))) {
				return;
			}
		}
		if(sb.st_size > LOG_MAX_SIZE) {
			if(f != NULL) {
				fclose(f);
			}
			char tmp[strlen(errpath)+5];
			strcpy(tmp, errpath);
			strcat(tmp, ".old");
			rename(errpath, tmp);
			if((f = fopen(errpath, "a")) == NULL) {
				return;
			}
		}
	}
	if(f != NULL) {
		fwrite(line, sizeof(char), strlen(line), f);
		fflush(f);
		fclose(f);
		f = NULL;
	}
	va_end(ap);
}
