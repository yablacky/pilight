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
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <limits.h>
#include <errno.h>
#include <time.h>
#include <math.h>
#include <string.h>

#include "libs/pilight/core/threads.h"
#include "libs/pilight/core/pilight.h"
#include "libs/pilight/core/network.h"
#include "libs/pilight/core/config.h"
#include "libs/pilight/core/log.h"
#include "libs/pilight/core/datetime.h"
#include "libs/pilight/core/ssdp.h"
#include "libs/pilight/core/socket.h"
#include "libs/pilight/core/threads.h"
#include "libs/pilight/core/irq.h"
#include "libs/pilight/core/dso.h"
#include "libs/pilight/core/gc.h"

#include "libs/pilight/protocols/protocol.h"

#include "libs/pilight/events/events.h"

#include "libs/pilight/config/hardware.h"

#ifndef _WIN32
	#include "libs/wiringx/wiringX.h"
#endif

static unsigned short main_loop = 1;
static unsigned short linefeed = 0;
static int min_pulses = 0;
static int max_pulses = -1;
static int pulses_per_line = 0;
static int show_duty_info = 0;
static int show_state_info = 0;
static int recv_prio = 50;

static const char spare_fmt[] = " %5s";
static const char spare_fmt2[] = " %7s";
static const char pulse_fmt[] = " %5d";
static const char pulse_fmt2[] = " %5d|%c";
static const char line_fmt[] = "\n%*d:";

#define STAT_VAL(a, b) ((100*(a) + ((b)>>1)) / (b))
#define stat_fmt " %3d"

static pthread_t logpth;

static char *get_timestamp(char *buf, size_t len)
{
	struct timeval now;
	gettimeofday(&now, NULL);
	strftime(buf, len, "%F %H:%M:%S", localtime(&now.tv_sec));
	sprintf(buf+strlen(buf), ".%06u", (unsigned int) now.tv_usec);
	return buf;
}

int main_gc(void) {
	log_shell_disable();
	main_loop = 0;

	datetime_gc();
	ssdp_gc();
#ifdef EVENTS
	events_gc();
#endif
	options_gc();
	socket_gc();

	config_gc();
	protocol_gc();
	whitelist_free();
	threads_gc();

#ifndef _WIN32
	wiringXGC();
#endif
	dso_gc();
	log_gc();
	gc_clear();

	FREE(progname);
	xfree();

#ifdef _WIN32
	WSACleanup();
#endif

	return EXIT_SUCCESS;
}

#define ESC			"\033"
#define COLOR_NORMAL		ESC "[m"
#define COLOR_BOLD		ESC "[1m"
#define COLOR_RED		ESC "[31m"
#define COLOR_GREEN		ESC "[32m"
#define COLOR_YELLOW		ESC "[33m"
#define COLOR_BLUE		ESC "[34m"
#define COLOR_MAGENTA		ESC "[35m"
#define COLOR_CYAN		ESC "[36m"
#define COLOR_WHITE		ESC "[37m"
#define COLOR_BOLD_RED		ESC "[1;31m"
#define COLOR_BOLD_GREEN	ESC "[1;32m"
#define COLOR_BOLD_YELLOW	ESC "[1;33m"
#define COLOR_BOLD_BLUE		ESC "[1;34m"
#define COLOR_BOLD_MAGENTA	ESC "[1;35m"
#define COLOR_BOLD_CYAN		ESC "[1;36m"
#define COLOR_BOLD_WHITE	ESC "[1;37m"

/*
 * Print formatted pulse information. Takes formatting information from global variables.
 * @param const int* pulses Ring-buffer with pulse durations.
 * @param unsigned int buflen Size of ring-buffer.
 * @param unsigned int start Position in ring-buffer of first pulse to print.
 * @param unsigned int count Number of pulses to print.
 * @param const char * hw_id Hardware that the pulses are from.
 * @return void.
 */
static void print_pulses(const int *pulses, unsigned int buflen, unsigned int start, unsigned int count, const char *hw_id)
{
	static size_t lines = 0;

	if(count < min_pulses || count > buflen || (max_pulses > 0 && count > max_pulses)) {
		return;
	}
	while(start >= buflen) {
		start -= buflen;
	}
	const int *pend = pulses + buflen;
	const int *pp = pulses + start;
	const int *pline = pp;
	static char last_val = 0;

	size_t dura_total_sum = 0, dura_line_sum = 0;
	int len = printf("%6u %s:", ++lines, hw_id) - 1; // don't count the ':' - is added by line_fmt.
	int ii, jj;
	for(ii = jj = 0; ii < count; ii++) {
		if(ii > 0 && ii % pulses_per_line == 0) {
			if(show_duty_info) {
				fputs(" |", stdout);
				for(;jj < ii; jj++) {
					printf(stat_fmt, STAT_VAL(*pline, dura_line_sum));
					if(++pline >= pend)
						pline -= buflen;
				}
			}
			dura_line_sum = 0;
			printf(line_fmt, len, ii);
		}
		char val = 'H';
		if (*pp < 0) { *(int*)pp = -*pp; val = 'L'; }

		if (!show_state_info) {
			printf(pulse_fmt, *pp);
		}
		// values should iterate. Same value indicates problem:
		else if (last_val && last_val == val) {
			fputs(val == 'H' ? COLOR_MAGENTA : COLOR_RED, stdout);
			printf(pulse_fmt2, *pp, val);
			fputs(COLOR_NORMAL, stdout);
		} else {
			printf(pulse_fmt2, *pp, val);
		}
		last_val = val;

		dura_line_sum += *pp;
		dura_total_sum += *pp;
		if (++pp >= pend)
			pp -= buflen;
	}
	while(ii++ % pulses_per_line) {
		printf(show_state_info ? spare_fmt2 : spare_fmt, ".");
	}
	if(show_duty_info) {
		fputs(" |", stdout);
		for(;jj < count; jj++) {
			printf(stat_fmt, STAT_VAL(*pline, dura_line_sum));
			if(++pline >= pend)
				pline -= buflen;
		}
	}
	printf(line_fmt, len, count);
	putc('\n', stdout);
}

static int ring_pulses[4096];
static volatile size_t ring_pulses_wr, ring_pulses_rd;

void *receiveOOK(void *param) {
	int duration = 0, iLoop = 0;
	size_t	lines = 0;

#ifdef _WIN32
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#else
	if (recv_prio != -1) {
		struct sched_param sched =  { 0 };
		sched.sched_priority = recv_prio;
		if(pthread_setschedparam(pthread_self(), SCHED_FIFO, &sched) != 0) {
			logprintf(LOG_WARNING, "Failed to set receiver thread priority %d", recv_prio);
		}
	}
#endif

	struct hardware_t *hw = (hardware_t *)param;
	if(min_pulses == 0 && max_pulses < 0) {
		while(main_loop && hw->receiveOOK) {
			duration = hw->receiveOOK();
			if(duration > 0) {
				char val = 0;
				if (show_state_info) {
					val = duration & (1 << 30) ? 'H' : 'L';
					duration &= ~(1 << 30);
				}
				iLoop++;
				if(linefeed == 1) {
					if(duration > 5100) {
						if (show_state_info)
							printf(" %d|%c -#: %d\n%6u %s: ",duration, val, iLoop, lines, hw->id);
						else
							printf(" %d -#: %d\n%6u %s: ",duration, iLoop, lines, hw->id);
						iLoop = 0;
						lines++;
					} else {
						if (show_state_info)
							printf(" %d|%c", duration, val);
						else
							printf(" %d", duration);
					}
				} else {
					if (show_state_info)
						printf("%6u %s: %d|%c\n", lines, hw->id, duration, val);
					else
						printf("%6u %s: %d\n", lines, hw->id, duration);
					lines++;
				}
			}
		}
		return NULL;
	}

	/*
	 * Receive pulses but don't print them immediately: Printing (the write() system call
	 * that printing implies and the analysing we do while printing) interferes too much
	 * with that high-prio ISR thread. So write pulses to a ring buffer and let a different
	 * thread read and print them.
	 * ring buffer is written in (wrapped-around) chunks of this format:
	 * struct {
	 *	int	length;		// chunk not valid while length == 0
	 *	(char*) hardware_id;
	 *	int	pulses[length];	// (with show_state_info: <0: low, >0: high)
	 * }
	 */
	int ring_len = countof(ring_pulses);
	int *ring_end = ring_pulses + ring_len;
	int *p1, *pp = ring_pulses;
	int pulse_count = 0;

	*(p1 = pp) = pulse_count = 0;
	if(++pp >= ring_end)
		pp -= ring_len;
	*pp = (int) hw->id;
	if(++pp >= ring_end)
		pp -= ring_len;
	ring_pulses_wr += 2;
	while(main_loop && hw->receiveOOK) {
		duration = hw->receiveOOK();
		if(duration > 0) {
			int durax = duration;
			if (show_state_info) {
				duration &= ~(1 << 30);
				durax = durax & (1 << 30) ? duration : -duration;
			}

			*pp = durax; //duration;
			if(++pp >= ring_end)
				pp -= ring_len;
			pulse_count++;
			ring_pulses_wr++;

			int overflow = 0;
			if(duration > 5100 || pulse_count >= MAXPULSESTREAMLENGTH ||
					(overflow = ring_pulses_wr + 3 - ring_pulses_rd >= ring_len)) {
				*pp = 0;	// length of the NEXT chunk must be set 0 before...
				*p1 = pulse_count;	// setting length of PREV chunk non-0.

				if (overflow) {
					fprintf(stderr, "\nring buffer overflow. Ignoring 5 seconds.\n");
					sleep(5);
				}

				p1 = pp;
				if(++pp >= ring_end)
					pp -= ring_len;
				*pp = (int) hw->id;
				if(++pp >= ring_end)
					pp -= ring_len;
				pulse_count = 0;
				ring_pulses_wr += 2;
			}
		}
	}

	return NULL;
}

void *receivePulseTrain(void *param) {
	struct rawcode_t r;
	int i = 0;

	struct hardware_t *hw = (hardware_t *)param;
	while(main_loop && hw->receivePulseTrain) {
		hw->receivePulseTrain(&r);
		if(r.length == -1) {
			main_gc();
			break;
		}
		if(min_pulses > 0 || max_pulses > 0) {
			print_pulses(r.pulses, r.length, 0, r.length, hw->id);
		} else {
			for(i=0;i<r.length;i++) {
				if(linefeed == 1) {
					printf(" %d", r.pulses[i]);
					if(r.pulses[i] > 5100) {
						printf(" -# %d\n %s:", i+1, hw->id);
					}
				} else {
					printf("%s: %d\n", hw->id, r.pulses[i]);
				}
			}
		}
	};
	return NULL;
}

int main(int argc, char **argv) {
	// memtrack();

	atomicinit();
	struct options_t *options = NULL;
	char *args = NULL;
	char *configtmp = STRDUP_OR_EXIT(CONFIG_FILE);
	pid_t pid = 0;

	gc_attach(main_gc);

	/* Catch all exit signals for gc */
	gc_catch();
	progname = STRDUP_OR_EXIT("pilight-raw");

#ifndef _WIN32
	if(geteuid() != 0) {
		printf("%s requires root privileges in order to run\n", progname);
		FREE(progname);
		exit(EXIT_FAILURE);
	}
#endif

	log_shell_enable();
	log_file_disable();
	log_level_set(LOG_NOTICE);

#ifndef _WIN32
	wiringXLog = logprintf;
#endif

	options_add(&options, 'H', "help", OPTION_NO_VALUE, 0, JSON_NULL, NULL, NULL);
	options_add(&options, 'V', "version", OPTION_NO_VALUE, 0, JSON_NULL, NULL, NULL);
	options_add(&options, 'C', "config", OPTION_HAS_VALUE, 0, JSON_NULL, NULL, NULL);
	options_add(&options, 'L', "linefeed", OPTION_NO_VALUE, 0, JSON_NULL, NULL, NULL);
	options_add(&options, 'm', "minpulses", OPTION_HAS_VALUE, 0, JSON_NULL, NULL, NULL);
	options_add(&options, 'M', "maxpulses", OPTION_HAS_VALUE, 0, JSON_NULL, NULL, NULL);
	options_add(&options, 'p', "pulsesperpline", OPTION_HAS_VALUE, 0, JSON_NULL, NULL, NULL);
	options_add(&options, 'd', "pulseduty", OPTION_NO_VALUE, 0, JSON_NULL, NULL, NULL);
	options_add(&options, 's', "pulsestate", OPTION_NO_VALUE, 0, JSON_NULL, NULL, NULL);
	options_add(&options, 'Z', "prio", OPTION_HAS_VALUE, 0, JSON_NULL, NULL, NULL);

	int	lo_prio = sched_get_priority_min(SCHED_FIFO),
		hi_prio = sched_get_priority_max(SCHED_FIFO);
	recv_prio = (hi_prio + lo_prio) / 2;
	while (1) {
		int c;
		c = options_parse(&options, argc, argv, 1, &args);
		if(c == -1)
			break;
		if(c == -2)
			c = 'H';
		switch (c) {
			case 'H':
				printf("Usage: %s [options]\n", progname);
				printf("\t -H --help\t\tdisplay usage summary\n");
				printf("\t -V --version\t\tdisplay version\n");
				printf("\t -L --linefeed\t\tstructure raw printout\n");
				printf("\t -m --minpulses count\t  Print nothing if not at least count pulses. Implies --linefeed.\n");
				printf("\t -M --maxpulses count\t  Print nothing if more than count pulses. Implies --linefeed.\n");
				printf("\t -p --pulsesperline count Pulses to print per line (10 by default). Implies --linefeed.\n");
				printf("\t -d --pulseduty           Print pulse duty (ratio information) per line.\n");
				printf("\t -s --pulsestate          Print pulse status(High or Low).\n");
				printf("\t -Z --prio n\t\tThread priority (%d .. %d) default=%d. -1=do not set prio.\n", lo_prio, hi_prio, recv_prio);
				printf("\t -C --config file\t\tUse that config file\n");
				goto close;
			break;
			case 'Z':
				recv_prio = atoi(args);
				if(recv_prio != -1 && (recv_prio < lo_prio || recv_prio > hi_prio)) {
					printf("%s: --prio must be %d .. %d or -1.\n", progname, lo_prio, hi_prio);
					goto close;
				}
			break;
			case 'L':
				linefeed = 1;
			break;
			case 'V':
				printf("%s v%s\n", progname, PILIGHT_VERSION);
				goto close;
			break;
			case 'C':
				configtmp = REALLOC_OR_EXIT(configtmp, strlen(args)+1);
				strcpy(configtmp, args);
			break;
			case 'm':
				min_pulses = atoi(args);
				if(min_pulses < 1) {
					printf("%s: --minpulses must be 1 or more.\n", progname);
					goto close;
				}
				break;
			case 'M':
				max_pulses = atoi(args);
				if(max_pulses < 1) {
					printf("%s: --maxpulses must be 1 or more.\n", progname);
					goto close;
				}
				break;
			case 'p':
				pulses_per_line = atoi(args);
				if(pulses_per_line < 1) {
					printf("%s: --pulsesperline must be 1 or more.\n", progname);
					goto close;
				}
				break;
			case 'd':
				show_duty_info = 1;
				break;
			case 's':
				show_state_info = 1;
				break;
			default:
				printf("Usage: %s [options]\n", progname);
				goto close;
			break;
		}
	}
	options_delete(options);

	if(max_pulses >0 && max_pulses < min_pulses) {
		printf("%s: --max_pulses %d is less than --minpulses %d\n", progname, max_pulses, min_pulses);
		goto close;
	}

	if(pulses_per_line > 0 || min_pulses > 0 || max_pulses > 0) {
		linefeed = 1;
	}
	if(linefeed == 1) {
		if(pulses_per_line > 0) {
			if(min_pulses < 1) {
				min_pulses = 1;
			}
		} else if(min_pulses > 0 || max_pulses > 0) {
			pulses_per_line = 10;
		}
	}

#ifdef _WIN32
	if((pid = check_instances(L"pilight-raw")) != -1) {
		logprintf(LOG_NOTICE, "pilight-raw is already running");
		goto close;
	}
#endif

	if((pid = isrunning("pilight-daemon")) != -1) {
		logprintf(LOG_NOTICE, "pilight-daemon instance found (%d)", (int)pid);
		goto close;
	}

	if((pid = isrunning("pilight-debug")) != -1) {
		logprintf(LOG_NOTICE, "pilight-debug instance found (%d)", (int)pid);
		goto close;
	}

	if(config_set_file(configtmp) == EXIT_FAILURE) {
		logprintf(LOG_ERR, "config file '%s' could not be used", configtmp);
		goto close;
	}

	irq_read_ex_enable(show_state_info);

	protocol_init();
	config_init();
	if(config_read() != EXIT_SUCCESS) {
		goto close;
	}

        threads_create(&logpth, NULL, &logloop, (void *)NULL);
	log_file_enable();
	log_file_disable();

	/* Start threads library that keeps track of all threads used */

	threads_start();

	struct conf_hardware_t *tmp_confhw = conf_hardware;
	while(tmp_confhw) {
		if(tmp_confhw->hardware->init) {
			if(tmp_confhw->hardware->init() == EXIT_FAILURE) {
				logprintf(LOG_ERR, "could not initialize %s hardware mode", tmp_confhw->hardware->id);
				goto close;
			}
			if(tmp_confhw->hardware->comtype == COMOOK) {
				threads_register(tmp_confhw->hardware->id, &receiveOOK, (void *)tmp_confhw->hardware, 0);
			} else if(tmp_confhw->hardware->comtype == COMPLSTRAIN) {
				threads_register(tmp_confhw->hardware->id, &receivePulseTrain, (void *)tmp_confhw->hardware, 0);
			}
		}
		tmp_confhw = tmp_confhw->next;
	}

	int start = 0, idle = 0;
	while(main_loop) {
		for (;;) {
			volatile int *p1 = ring_pulses + start;
			int len = *p1;
			if (len == 0)
				break;
			ring_pulses_rd++;
			if(++start >= countof(ring_pulses))
				start -= countof(ring_pulses);
			const char *hw_id = (const char*) (ring_pulses[start]);
			ring_pulses_rd++;
			if(++start >= countof(ring_pulses))
				start -= countof(ring_pulses);
			if(idle > 2 * 5 && len > min_pulses && (max_pulses < 0 || len <= max_pulses)) {
				char buf[128];
				printf("----------------------------------------------------------"
					" [%s]\n",get_timestamp(buf, sizeof(buf)));
				idle = 0;
			}
			print_pulses(ring_pulses, countof(ring_pulses), start, len, hw_id);
			ring_pulses_rd += len;
			start += len;
			while(start >= countof(ring_pulses))
				start -= countof(ring_pulses);
		}
		usleep(200000);	// 0.2 (1/5)sec.
		idle++;
	}

close:
	if(args != NULL) {
		FREE(args);
	}
	if(main_loop == 1) {
		main_gc();
	}
	FREE(configtmp);
	return (EXIT_FAILURE);
}
