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
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#ifdef _WIN32
	#include <winsock2.h>
	#include <windows.h>
	#include <ws2tcpip.h>
	#include <winsvc.h>
	#include <shellapi.h>
	#define MSG_NOSIGNAL 0
#else
	#include <sys/socket.h>
	#include <sys/time.h>
	#include <netinet/in.h>
	#include <netinet/tcp.h>
	#include <netdb.h>
	#include <arpa/inet.h>
	#ifdef __mips__
		#define __USE_UNIX98
	#endif
#endif
#include <pthread.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <ctype.h>
#include <dirent.h>

#include "libs/pilight/core/pilight.h"
#include "libs/pilight/core/threads.h"
#include "libs/pilight/core/datetime.h"
#include "libs/pilight/core/common.h"
#include "libs/pilight/core/network.h"
#include "libs/pilight/core/gc.h"
#include "libs/pilight/core/log.h"
#include "libs/pilight/core/options.h"
#include "libs/pilight/core/socket.h"
#include "libs/pilight/core/json.h"
#include "libs/pilight/core/irq.h"
#include "libs/pilight/core/ssdp.h"
#include "libs/pilight/core/dso.h"
#include "libs/pilight/core/firmware.h"
#include "libs/pilight/core/proc.h"
#include "libs/pilight/core/ntp.h"
#include "libs/pilight/core/config.h"

#ifdef EVENTS
	#include "libs/pilight/events/events.h"
#endif

#ifdef WEBSERVER
	#ifdef WEBSERVER_HTTPS
		#include "libs/polarssl/polarssl/md5.h"
	#endif
	#include "libs/pilight/core/webserver.h"
#endif

#include "libs/pilight/config/hardware.h"
#include "libs/pilight/config/registry.h"
#include "libs/pilight/config/devices.h"
#include "libs/pilight/config/settings.h"
#include "libs/pilight/config/gui.h"

#include "libs/wiringx/wiringX.h"

#ifdef _WIN32
static char server_name[40];
#endif

typedef struct clients_t {
	char uuid[UUID_LENGTH];
	int id;
	int receiver;
	int config;
	int core;
	int stats;
	int forward;
	char media[8];
	double cpu;
	double ram;
	struct clients_t *next;
} clients_t;

static struct clients_t *clients_head = NULL;

typedef struct sendqueue_t {
	unsigned int id;
	char *protoname;
	char *settings;
	char *message;
	enum origin_t origin;
	struct protocol_t *protopt;
	int code[MAXPULSESTREAMLENGTH];
	int length;
	char uuid[UUID_LENGTH];
	struct sendqueue_t *next;
} sendqueue_t;

static struct sendqueue_t * volatile sendqueue_head = NULL;
static struct sendqueue_t * volatile sendqueue_tail = NULL;
static int sendqueue_number = 0;
static pthread_mutex_t sendqueue_lock;
static pthread_cond_t sendqueue_signal;
static pthread_mutexattr_t sendqueue_attr;
static unsigned short sendqueue_init = 0;

typedef struct recvqueue_t {
	int raw[MAXPULSESTREAMLENGTH];
	int rawlen;
	int hwtype;
	int plslen;
	struct recvqueue_t *next;
} recvqueue_t;

static struct recvqueue_t * volatile recvqueue_head = NULL;
static struct recvqueue_t * volatile recvqueue_tail = NULL;
static int recvqueue_number = 0;
static pthread_mutex_t recvqueue_lock;
static pthread_cond_t recvqueue_signal;
static pthread_mutexattr_t recvqueue_attr;
static unsigned short recvqueue_init = 0;

typedef struct bcqueue_t {
	struct JsonNode *jmessage;
	char *protoname;
	enum origin_t origin;
	struct bcqueue_t *next;
} bcqueue_t;

static struct bcqueue_t * volatile bcqueue_head = NULL;
static struct bcqueue_t * volatile bcqueue_tail = NULL;
static int bcqueue_number = 0;
static pthread_mutex_t bcqueue_lock;
static pthread_cond_t bcqueue_signal;
static pthread_mutexattr_t bcqueue_attr;
static unsigned short bcqueue_init = 0;

static struct protocol_t *procProtocol;

static char *configtmp = NULL;		/* Name of the config file being used */

static pid_t pid;			/* The pid_file and pid of this daemon */
#ifndef _WIN32
static const char *pid_file;
#endif

static int standalone = 0;		/* Are we running standalone */

static int nodaemon = 0;		/* Daemonize or not */

static int stacktracer = 0;		/* Run tracktracer */

static int threadprofiler = 0;		/* Run thread profiler */

static int volatile running = 1;	/* Are we already running */

static int volatile sending = 0;	/* Are we currently sending code */

static int sockfd = 0;			/* Socket identifier to the server if we are running as client */

static pthread_t logpth;		/* Log thread pointer */
static int verbosity = LOG_DEBUG;

#ifdef _WIN32
	static int console = 0;
	static int oldverbosity = LOG_NOTICE;
#endif

/* By default the reveiver is turned to wait-mode while
	sending to prevent receivin the sent data.
	For debug reasons it might be helpful to
	receive self-sent-messages:  */
static int receive_while_sending = 0;

/* While loop conditions */
static unsigned short main_loop = 1;

/* Do we need to connect to a master server:port? */
static char *master_server = NULL;
static unsigned short master_port = 0;
struct socket_callback_t socket_callback;

#ifdef WEBSERVER
/* Do we enable the webserver */
static int webserver_enable = WEBSERVER_ENABLE;
static int webgui_websockets = WEBGUI_WEBSOCKETS;
/* On what port does the webserver run */
static int webserver_http_port = WEBSERVER_HTTP_PORT;
/* The webroot of pilight */
static const char *webserver_root = NULL;
#endif

#define protocol_can_hwtype(p, t) ((p) != NULL && ((p)->hwtype == (t) || (p)->hwtype == HWINTERNAL || (t) == HWINTERNAL))

static void client_remove(int id) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	struct clients_t *curr, **prev_next = &clients_head;

	for(curr = clients_head; curr != NULL; curr = *(prev_next = &curr->next)) {

		if(curr->id == id) {
			*prev_next = curr->next;
			FREE(curr);
			break;
		}
	}
}

static void broadcast_queue(const char *protoname, const struct JsonNode *json, enum origin_t origin) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	if(main_loop == 1) {
		pthread_mutex_lock(&bcqueue_lock);
		if(bcqueue_number > 1024) {
			logprintf(LOG_ERR, "broadcast queue full (1024 entries)");
		} else {
			struct bcqueue_t *bnode = NULL;
			CONFIG_ALLOC_UNNAMED_NODE(bnode);

			char *jstr = json_stringify(json, NULL);
			bnode->jmessage = json_decode(jstr);
			if(json_find_member(bnode->jmessage, "uuid") == NULL && strlen(pilight_uuid) > 0) {
				json_append_member(bnode->jmessage, "uuid", json_mkstring(pilight_uuid));
			}
			json_free(jstr);
			jstr = NULL;

			bnode->protoname = STRDUP_OR_EXIT(protoname);
			bnode->origin = origin;

			CONFIG_APPEND_NODE_TO_TAIL(bnode, bcqueue_head, bcqueue_tail);
			bcqueue_number++;
		}
		pthread_mutex_unlock(&bcqueue_lock);
		pthread_cond_signal(&bcqueue_signal);
	}
}

static void *broadcast(void *param) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	int broadcasted = 0;

	while(main_loop) {
		struct bcqueue_t *bcqueue = NULL;
		pthread_mutex_lock(&bcqueue_lock);
		logprintf(LOG_DEBUG, "%s::locked", __FUNCTION__);

		while(main_loop != 0 && (bcqueue = bcqueue_head) == NULL) {
			logprintf(LOG_DEBUG, "%s::waiting", __FUNCTION__);
			pthread_cond_wait(&bcqueue_signal, &bcqueue_lock);
		}
		if(main_loop == 0) {
			pthread_mutex_unlock(&bcqueue_lock);
			logprintf(LOG_DEBUG, "%s::unlocked", __FUNCTION__);
			continue;
		}

		CONFIG_REMOVE_NODE_FROM_HEAD(bcqueue, bcqueue_head, bcqueue_tail);
		bcqueue_number--;
		pthread_mutex_unlock(&bcqueue_lock);
		logprintf(LOG_DEBUG, "%s::unlocked", __FUNCTION__);

		broadcasted = 0;
		struct JsonNode *jret = NULL;
		const char *origin = NULL;

		if(json_find_string(bcqueue->jmessage, "origin", &origin) == 0) {
			if(strcmp(origin, "core") == 0) {
				double tmp = 0;
				json_find_number(bcqueue->jmessage, "type", &tmp);
				char *conf = json_stringify(bcqueue->jmessage, NULL);

				struct clients_t *tmp_clients = clients_head;
				for(; tmp_clients; tmp_clients = tmp_clients->next) {
					if(((int)tmp < 0 && tmp_clients->core == 1) ||
					   ((int)tmp >= 0 && tmp_clients->config == 1) ||
						 ((int)tmp == PROCESS && tmp_clients->stats == 1)) {
						socket_write(tmp_clients->id, conf);
						broadcasted = 1;
					}
				}
				if(pilight.runmode == ADHOC && sockfd > 0) {
					struct JsonNode *jupdate = json_decode(conf);
					json_append_member(jupdate, "action", json_mkstring("update"));
					char *ret = json_stringify(jupdate, NULL);
					socket_write(sockfd, ret);
					broadcasted = 1;
					json_delete(jupdate);
					json_free(ret);
				}
				if(broadcasted == 1) {
					logprintf(LOG_DEBUG, "broadcasted: %s", conf);
				}
				json_free(conf);
			} else {
				/* Update the config */
				if(devices_update(bcqueue->protoname, bcqueue->jmessage, bcqueue->origin, &jret) == 0) {
					char *tmp = json_stringify(jret, NULL);

					struct clients_t *tmp_clients = clients_head;
					for(;tmp_clients; tmp_clients = tmp_clients->next) {
						if(tmp_clients->config != 1) {
							continue;
						}
						unsigned short match1 = 0;
						struct JsonNode *jtmp = json_decode(tmp);
						const struct JsonNode *jdevices = json_find_member(jtmp, "devices");
						const struct JsonNode *jchilds = NULL;
						json_foreach(jchilds, jdevices) {
							unsigned short match2 = 0;
							if(jchilds->tag == JSON_STRING) {
								struct gui_values_t *gui_values = NULL;
								if(!(gui_values = gui_media(jchilds->string_)) ||
								    strcmp(tmp_clients->media, "all") == 0) {
									match1 = 1;
									match2 = 1;
								} else for(; gui_values; gui_values = gui_values->next) {
									if(gui_values->type == JSON_STRING && (
									    strcmp(gui_values->string_, tmp_clients->media) == 0 ||
									    strcmp(gui_values->string_, "all") == 0)) {
										match1 = 1;
										match2 = 1;
									}
								}
							}
							if(match2 == 0) {
								jchilds = json_delete_force(jchilds);
							}
						}
						if(match1 == 1) {
							char *conf = json_stringify(jtmp, NULL);
							socket_write(tmp_clients->id, conf);
							logprintf(LOG_DEBUG, "broadcasted: %s", conf);
							json_free(conf);
						}
						json_delete(jtmp);
					}

					json_free(tmp);
					json_delete(jret);
				}

				/* The settings objects inside the broadcast queue is only of interest for the
				   internal pilight functions. For the outside world we only communicate the
				   message part of the queue so we remove the settings */
				char *internal = json_stringify(bcqueue->jmessage, NULL);

				json_delete_force(json_find_member(bcqueue->jmessage, "settings"));

				const struct JsonNode *tmp = json_find_member(bcqueue->jmessage, "action");
				if(tmp != NULL && tmp->tag == JSON_STRING && strcmp(tmp->string_, "update") == 0) {
					json_delete_force(tmp);
				}

				char *out = json_stringify(bcqueue->jmessage, NULL);
				if(strcmp(bcqueue->protoname, "pilight_firmware") == 0) {
					const struct JsonNode *code = NULL;
					if((code = json_find_member(bcqueue->jmessage, "message")) != NULL) {
						firmware_t fw;
						firmware_from_json(&fw, code);
						if(fw.version > 0) {
							firmware_to_registry(&fw);

							struct JsonNode *jmessage = json_mkobject();
							json_append_member(jmessage, "values", firmware_to_json(&fw, json_mkobject()));
							json_append_member(jmessage, "origin", json_mkstring("core"));
							json_append_member(jmessage, "type", json_mknumber(FIRMWARE, 0));
							pilight.broadcast("pilight-firmware", jmessage, FW);
							json_delete(jmessage);
							jmessage = NULL;
						}
						firmware_free(&fw);
					}
				}
				broadcasted = 0;

				int nrchilds = 0;
				const struct JsonNode *childs = NULL;
				json_foreach(childs, bcqueue->jmessage) {
					nrchilds++;
				}

				/* Write the message to all receivers */
				struct clients_t *tmp_clients = clients_head;
				for(; tmp_clients; tmp_clients = tmp_clients->next) {
					if(tmp_clients->receiver == 1 && tmp_clients->forward == 0) {
						if(strcmp(out, "{}") != 0 && nrchilds > 1) {
							socket_write(tmp_clients->id, out);
							broadcasted = 1;
						}
					}
				}

				if(pilight.runmode == ADHOC && sockfd > 0) {
					struct JsonNode *jupdate = json_decode(internal);
					json_append_member(jupdate, "action", json_mkstring("update"));
					char *ret = json_stringify(jupdate, NULL);
					socket_write(sockfd, ret);
					broadcasted = 1;
					json_delete(jupdate);
					json_free(ret);
				}
				if((broadcasted == 1 || nodaemon == 1) && (strcmp(out, "{}") != 0 && nrchilds > 1)) {
					logprintf(LOG_DEBUG, "broadcasted: %s", out);
				}
				json_free(internal);
				json_free(out);
			}
		}

		FREE(bcqueue->protoname);
		json_delete(bcqueue->jmessage);
		FREE(bcqueue);
	}
	return (void *)NULL;
}

static void receive_queue(int *raw, int rawlen, int plslen, int hwtype) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	int i = 0;

	if(main_loop == 1) {
		if(rawlen > countof(((struct recvqueue_t*)0)->raw))
		{
			logprintf(LOG_ERR, "received a too long message (%d raw items)",rawlen);
			return;
		}
		pthread_mutex_lock(&recvqueue_lock);
		if(recvqueue_number > 1024) {
			logprintf(LOG_ERR, "receiver queue full (1024 entries)");
		} else {
			struct recvqueue_t *rnode = NULL;
			CONFIG_ALLOC_UNNAMED_NODE(rnode);

			for(i=0;i<rawlen;i++) {
				rnode->raw[i] = raw[i];
			}
			rnode->rawlen = rawlen;
			rnode->plslen = plslen;
			rnode->hwtype = hwtype;

			CONFIG_APPEND_NODE_TO_TAIL(rnode, recvqueue_head, recvqueue_tail);
			recvqueue_number++;
		}
		pthread_mutex_unlock(&recvqueue_lock);
		pthread_cond_signal(&recvqueue_signal);
	}
}

static char* get_timestamp(char *buf, size_t maxlen, const time_t *timer) {
	if(buf == NULL || maxlen < 64) {
		return NULL;
	}
        time_t loc = timer == NULL ? time(NULL) : *timer, gmt;
        struct tm now, utc;
#ifdef _WIN32
        now = *localtime(&loc);
        utc = *gmtime(&loc);
#else
        localtime_r(&loc, &now);
        gmtime_r(&loc, &utc);
#endif
        gmt = mktime(&utc);
	int len = strftime(buf, maxlen, "%Y-%m-%d %H:%M:%S", &now);
        int dif = loc >= gmt ? (int)(loc - gmt) : (int)(gmt - loc);
	len += sprintf(buf+len, " (%s%02d:%02d:%02d)", loc >= gmt ? "+" : "-",
				dif/(60*60), (dif/60)%60, dif%60);
	// assert(len+1 < maxlen);
	return buf;
}

static void add_timestamp(JsonNode *object, const char *timestamp_key, const time_t *timer) {
	if(timestamp_key == NULL) {
		timestamp_key = "timestamp";
	}
	char timbuf[64];
	if(!json_find_member(object, timestamp_key) && get_timestamp(timbuf, sizeof(timbuf), timer) != NULL) {
		json_append_member(object, timestamp_key, json_mkstring(timbuf));
	}
}

static void receiver_create_message(protocol_t *protocol, const char *msg_name, size_t msg_id) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	if(protocol->message == NULL)
		return;
	char *valid = json_stringify(protocol->message, NULL);
	json_delete(protocol->message);
	protocol->message = NULL;
	if(valid == NULL)
		return;

	struct JsonNode *jmessage = json_mkobject();

	json_append_member(jmessage, "message", json_decode(valid));
	json_append_member(jmessage, "origin", json_mkstring("receiver"));

	char origin_msg_id[strlen(msg_name) + 64];
	sprintf(origin_msg_id, "%s#%u", msg_name, msg_id);
	json_append_member(jmessage, "origin-msg-id", json_mkstring(origin_msg_id));

	json_append_member(jmessage, "protocol", json_mkstring(protocol->id));
	if(strlen(pilight_uuid) > 0) {
		json_append_member(jmessage, "uuid", json_mkstring(pilight_uuid));
	}
	if(protocol->repeats > -1) {
		json_append_member(jmessage, "repeats", json_mknumber(protocol->repeats, 0));
	}

	add_timestamp(jmessage, "received-timestamp", NULL);
	static size_t total_message_count = 0;
	json_append_member(jmessage, "received-total",
		json_mknumber(__sync_add_and_fetch(&total_message_count, 1), 0));

	broadcast_queue(protocol->id, jmessage, RECEIVER);
	json_delete(jmessage);
	json_free(valid);
}

static void receive_parse_api(struct JsonNode *code, int hwtype) {
	struct protocol_t *protocol = NULL;
	struct protocols_t *pnode = protocols;

	static size_t code_count = 0;
	size_t code_id = ++code_count;

	for(; pnode != NULL; pnode = pnode->next) {
		protocol = pnode->listener;

		if(protocol->hwtype == hwtype && protocol->parseCommand != NULL) {
			protocol->parseCommand(code);
			receiver_create_message(protocol, "code", code_id);
		}
	}
}

static void *receive_parse_code(void *param) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	while(main_loop != 0) {
		recvqueue_t *recvqueue = NULL;
		pthread_mutex_lock(&recvqueue_lock);
		logprintf(LOG_DEBUG, "%s::locked", __FUNCTION__);

		while(main_loop != 0 && (recvqueue = recvqueue_head) == NULL) {
			logprintf(LOG_DEBUG, "%s::waiting", __FUNCTION__);
			pthread_cond_wait(&recvqueue_signal, &recvqueue_lock);
		}
		if(main_loop == 0) {
			pthread_mutex_unlock(&recvqueue_lock);
			logprintf(LOG_DEBUG, "%s::unlocked", __FUNCTION__);
			continue;
		}

		CONFIG_REMOVE_NODE_FROM_HEAD(recvqueue, recvqueue_head, recvqueue_tail);
		recvqueue_number--;

		static size_t receive_count = 0;
		size_t receive_id = ++receive_count;

		pthread_mutex_unlock(&recvqueue_lock);
		logprintf(LOG_DEBUG, "%s::unlocked", __FUNCTION__);

		const struct protocols_t *pnode;
		for(pnode = protocols; pnode != NULL && main_loop; pnode = pnode->next) {
			struct protocol_t *protocol = pnode->listener;

			if(protocol == NULL || protocol->validate == NULL || !protocol_can_hwtype(protocol, recvqueue->hwtype)) {
				continue;
			}

			if(recvqueue->rawlen < MAXPULSESTREAMLENGTH) {
				protocol->raw = recvqueue->raw;
			}
			protocol->rawlen = recvqueue->rawlen;
			if(protocol->validate() != 0) {
				continue;
			}

			logprintf(LOG_DEBUG, "possible %s protocol", protocol->id);

			/* Reset # of repeats after a certain delay (currently 0.5 sec) */
			protocol->first = protocol->second;
			gettimeofday(&protocol->second, NULL);
			if(can_timeval_diff(protocol->first, protocol->second) &&
					get_timeval_diff_usec(protocol->first, protocol->second) > 500000) {
				protocol->repeats = 0;
			}

			protocol->repeats++;
			if(protocol->parseCode != NULL) {
				logprintf(LOG_DEBUG, "recevied pulse length of %d", recvqueue->plslen);
				logprintf(LOG_DEBUG, "caught minimum # of repeats %d of %s", protocol->repeats, protocol->id);
				logprintf(LOG_DEBUG, "called %s parseRaw()", protocol->id);
				protocol->parseCode();
				receiver_create_message(protocol, "pulse", receive_id);
			}
		}

		FREE(recvqueue);
	}
	return (void *)NULL;
}

static void *send_code(void *param) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	int i = 0;

	/* Make sure the pilight sender gets
	   the highest priority available */
#ifdef _WIN32
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#else
	struct sched_param sched = { 0 };
	sched.sched_priority = 80;
	pthread_setschedparam(pthread_self(), SCHED_FIFO, &sched);
#endif

	while(main_loop != 0) {
		static sendqueue_t *sendqueue;
		pthread_mutex_lock(&sendqueue_lock);
		logprintf(LOG_DEBUG, "%s::locked", __FUNCTION__);

		while(main_loop != 0 && (sendqueue = sendqueue_head) == NULL) {
			logprintf(LOG_DEBUG, "%s::waiting", __FUNCTION__);
			pthread_cond_wait(&sendqueue_signal, &sendqueue_lock);
		}
		if(main_loop == 0) {
			pthread_mutex_unlock(&sendqueue_lock);
			logprintf(LOG_DEBUG, "%s::unlocked", __FUNCTION__);
			continue;
		}

		CONFIG_REMOVE_NODE_FROM_HEAD(sendqueue, sendqueue_head, sendqueue_tail);
		sendqueue_head = sendqueue->next;
		sendqueue_number--;
		pthread_mutex_unlock(&sendqueue_lock);
		logprintf(LOG_DEBUG, "%s::unlocked", __FUNCTION__);

		sending = 1;

		struct protocol_t *protocol = sendqueue->protopt;
		struct hardware_t *hw = NULL;

		struct JsonNode *message = NULL;

		if(sendqueue->message != NULL && strcmp(sendqueue->message, "{}") != 0) {
			struct JsonNode *jmessage = json_decode(sendqueue->message);
			if(jmessage == NULL) {
				logprintf(LOG_ERR, "failed to send message '%s': not json", sendqueue->message);
			} else {
				if(message == NULL) {
					message = json_mkobject();
				}
				json_append_member(message, "origin", json_mkstring("sender"));
				json_append_member(message, "protocol", json_mkstring(protocol->id));
				json_append_member(message, "message", jmessage);
				if(strlen(sendqueue->uuid) > 0) {
					json_append_member(message, "uuid", json_mkstring(sendqueue->uuid));
				}
				json_append_member(message, "repeat", json_mknumber(1, 0));
				if(receive_while_sending) {
					json_append_member(message, "receive-while-sending", json_mknumber(receive_while_sending, 0));
				}
			}
		}
		if(sendqueue->settings != NULL && strcmp(sendqueue->settings, "{}") != 0) {
			struct JsonNode *jsettings = json_decode(sendqueue->settings);
			if(jsettings == NULL) {
				logprintf(LOG_ERR, "failed to send settings '%s': not json", sendqueue->settings);
			} else {
				if(message == NULL) {
					message = json_mkobject();
				}
				json_append_member(message, "settings", jsettings);
			}
		}

		const struct conf_hardware_t *tmp_confhw = conf_hardware;
		for (; tmp_confhw; tmp_confhw = tmp_confhw->next) {
			if(protocol->hwtype == tmp_confhw->hardware->hwtype) {
				hw = tmp_confhw->hardware;
				break;
			}
		}
		if(hw != NULL) {
			if((hw->comtype == COMOOK || hw->comtype == COMPLSTRAIN) && hw->sendOOK != NULL) {
				int receiver_wait = hw->receiveOOK != NULL || hw->receivePulseTrain != NULL;
				if(receive_while_sending) {
					receiver_wait = 0;
				}
				if(receiver_wait) {
					hw->wait = 1;
					pthread_cond_signal(&hw->signal);
					pthread_mutex_lock(&hw->lock);
				}
				logprintf(LOG_DEBUG, "**** RAW CODE ****");
				if(log_level_get() >= LOG_DEBUG) {
					for(i=0;i<sendqueue->length;i++) {
						printf("%d ", sendqueue->code[i]);
					}
					printf("\n");
				}
				logprintf(LOG_DEBUG, "**** RAW CODE ****");

				if(hw->sendOOK(sendqueue->code, sendqueue->length, protocol->txrpt) == 0) {
					logprintf(LOG_DEBUG, "successfully send %s code", protocol->id);
				} else {
					logprintf(LOG_ERR, "failed to send code");
				}
				if(strcmp(protocol->id, "raw") == 0) {
					int plslen = sendqueue->code[sendqueue->length-1]/PULSE_DIV;
					receive_queue(sendqueue->code, sendqueue->length, plslen, -1);
				}
				if(receiver_wait) {
					hw->wait = 0;
					pthread_mutex_unlock(&hw->lock);
					pthread_cond_signal(&hw->signal);
				}
			} else if(hw->comtype == COMAPI && hw->sendAPI != NULL) {
				if(message != NULL) {
					if(hw->sendAPI(message) == 0) {
						logprintf(LOG_DEBUG, "successfully send %s command", protocol->id);
					} else {
						logprintf(LOG_ERR, "failed to send command");
					}
				}
			}
		} else {
			if(strcmp(protocol->id, "raw") == 0) {
				int plslen = sendqueue->code[sendqueue->length-1]/PULSE_DIV;
				receive_queue(sendqueue->code, sendqueue->length, plslen, -1);
			}
		}
		if(message != NULL) {
			broadcast_queue(sendqueue->protoname, message, sendqueue->origin);
			json_delete(message);
			message = NULL;
		}

		sending = 0;

		FREE(sendqueue->message);
		FREE(sendqueue->settings);
		FREE(sendqueue->protoname);
		FREE(sendqueue);
	}
	return (void *)NULL;
}

static int send_queue_nolock(struct JsonNode *json, enum origin_t origin);

/* Send a specific code */
static int send_queue(struct JsonNode *json, enum origin_t origin) {
	pthread_mutex_lock(&sendqueue_lock);

	int ret = -1, need_signal = 1;
	if(sendqueue_number > 1024) {
		logprintf(LOG_ERR, "send queue full (1024 entries)");
	} else {
		int need_signal = sendqueue_number;
		ret = send_queue_nolock(json, origin);
		need_signal -= sendqueue_number;
	}

	pthread_mutex_unlock(&sendqueue_lock);
	if(need_signal != 0) {
		pthread_cond_signal(&sendqueue_signal);
	}
	return ret;
}

static int send_queue_nolock(struct JsonNode *json, enum origin_t origin) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	int raw[MAXPULSESTREAMLENGTH];
	struct timeval tcurrent;
	struct clients_t *tmp_clients = NULL;
	const char *uuid = NULL;
	char *buffer = NULL;
	/* Hold the final protocol struct */
	struct protocol_t *protocol = NULL;

#ifdef _WIN32
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#else
	/* Make sure the pilight sender gets
	   the highest priority available */
	struct sched_param sched = { 0 };
	sched.sched_priority = 80;
	pthread_setschedparam(pthread_self(), SCHED_FIFO, &sched);
#endif

	const struct JsonNode *jcode = NULL;
	const struct JsonNode *jprotocols = NULL;
	const struct JsonNode *jprotocol = NULL;

	buffer = json_stringify(json, NULL);
	if(buffer == NULL) {
		logprintf(LOG_ERR, "failed to generate json code for sending");
		return -1;
	}

	tmp_clients = clients_head;
	for(;tmp_clients; tmp_clients = tmp_clients->next) {
		if(tmp_clients->forward == 1) {
			socket_write(tmp_clients->id, buffer);
		}
	}
	json_free(buffer);

	if((jcode = json_find_member(json, "code")) == NULL) {
		logprintf(LOG_ERR, "sender did not send any codes");
		json_delete_force(jcode);
		return -1;
	}

	if((jprotocols = json_find_member(jcode, "protocol")) == NULL) {
		logprintf(LOG_ERR, "sender did not provide a protocol name");
		json_delete_force(jcode);
		return -1;
	}

	/* If we matched a protocol and are not already sending, continue */
	json_find_string(jcode, "uuid", &uuid);
	if(uuid != NULL && strcmp(uuid, pilight_uuid) != 0) {
		return 0;	// already sending.
	}

	protocol = NULL;
	json_foreach(jprotocol, jprotocols) {
		if(jprotocol->tag == JSON_STRING) {
			struct protocols_t *pnode = protocols;
			/* Retrieve the used protocol */
			for(; pnode; pnode = pnode->next) {
				/* Check if the protocol exists */
				if(protocol_device_exists(pnode->listener, jprotocol->string_) == 0) {
					protocol = pnode->listener;
					break;
				}
			}
			if(pnode != NULL) {
				break;
			}
		}
	}
	if(protocol == NULL || protocol->createCode == NULL || main_loop == 0) {
		return -1;	// no protocol or can't send.
	}

	memset(raw, 0, sizeof(raw));
	protocol->raw = raw;

	/* Let the protocol create his code */
	if(protocol->createCode(jcode) != 0) {
		return -1;
	}

	struct sendqueue_t *mnode = NULL;
	CONFIG_ALLOC_UNNAMED_NODE(mnode);

	gettimeofday(&tcurrent, NULL);
	mnode->origin = origin;
	mnode->id = 1000000 * (unsigned int)tcurrent.tv_sec + (unsigned int)tcurrent.tv_usec;
	mnode->message = NULL;
	if(protocol->message != NULL) {
		char *jsonstr = json_stringify(protocol->message, NULL);
		if(jsonstr == NULL) {
			logprintf(LOG_ERR, "failed to generate json for protocol %s command", protocol->id);
		} else {
			mnode->message = jsonstr;
			jsonstr = NULL;
		}
		json_delete(protocol->message);
		protocol->message = NULL;
	}

	mnode->length = protocol->rawlen;
	memcpy(mnode->code, protocol->raw, sizeof(int)*protocol->rawlen);

	mnode->protoname = STRDUP_OR_EXIT(protocol->id);
	mnode->protopt = protocol;

	struct options_t *tmp_options = protocol->options;
	const char *stmp = NULL;
	struct JsonNode *jsettings = json_mkobject();
	const struct JsonNode *jtmp = NULL;
	for(; tmp_options; tmp_options = tmp_options->next) {
		if(tmp_options->conftype == DEVICES_SETTING) {
			if(tmp_options->vartype == JSON_NUMBER &&
			  (jtmp = json_find_member(jcode, tmp_options->name)) != NULL &&
			   jtmp->tag == JSON_NUMBER) {
				json_append_member(jsettings, tmp_options->name, json_mknumber(jtmp->number_, jtmp->decimals_));
			} else if(tmp_options->vartype == JSON_STRING && json_find_string(jcode, tmp_options->name, &stmp) == 0) {
				json_append_member(jsettings, tmp_options->name, json_mkstring(stmp));
			}
		}
	}
	mnode->settings = json_stringify(jsettings, NULL);
	json_delete(jsettings);
	jsettings = NULL;

	if(uuid != NULL) {
		strcpy(mnode->uuid, uuid);
	} else {
		memset(mnode->uuid, '\0', UUID_LENGTH);
	}

	CONFIG_APPEND_NODE_TO_TAIL(mnode, sendqueue_head, sendqueue_tail);
	sendqueue_number++;

	return 0;
}

#ifdef WEBSERVER
static void client_webserver_parse_code(int i, char buffer[BUFFER_SIZE]) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	int sd = socket_get_clients(i);
	int x = 0;
	FILE *f;
	unsigned char *p = NULL;
	unsigned char buff[BUFFER_SIZE];
	char *cache = NULL;
	char *path = NULL, buf[INET_ADDRSTRLEN+1];
	char *mimetype = NULL;
	struct stat sb;
	struct sockaddr_in sockin;
	socklen_t len = sizeof(sockin);

	if(getsockname(sd, (struct sockaddr *)&sockin, &len) == -1) {
		logprintf(LOG_ERR, "could not determine server ip address");
	} else {
		if(strstr(buffer, " HTTP/") == NULL) {
			return;
		}
		p = buff;
		if(strstr(buffer, "/logo.png") != NULL) {
			path = MALLOC_OR_EXIT(strlen(webserver_root)+strlen("logo.png")+2);
			sprintf(path, "%s/logo.png", webserver_root);
			if((f = fopen(path, "rb"))) {
				fstat(fileno(f), &sb);
				mimetype = webserver_mimetype("image/png");
				webserver_create_header(&p, "200 OK", mimetype, (unsigned int)sb.st_size);
				send(sd, (const char *)buff, (size_t)(p-buff), MSG_NOSIGNAL);
				x = 0;
				cache = MALLOC_OR_EXIT(BUFFER_SIZE);
				memset(cache, '\0', BUFFER_SIZE);
				while(!feof(f)) {
					x = (int)fread(cache, 1, BUFFER_SIZE, f);
					send(sd, (const char *)cache, (size_t)x, MSG_NOSIGNAL);
				}
				fclose(f);
				FREE(cache);
				FREE(mimetype);
			} else {
				logprintf(LOG_WARNING, "pilight logo not found");
			}
			FREE(path);
		} else {
		    /* Catch all webserver page to inform users on which port the webserver runs */
			mimetype = webserver_mimetype("text/html");
			webserver_create_header(&p, "200 OK", mimetype, (unsigned int)BUFFER_SIZE);
			send(sd, (const char *)buff, (size_t)(p-buff), MSG_NOSIGNAL);
			if(webserver_enable == 1) {
				cache = MALLOC_OR_EXIT(BUFFER_SIZE);
				memset(cache, '\0', BUFFER_SIZE);
				memset(&buf, '\0', INET_ADDRSTRLEN+1);
				inet_ntop(AF_INET, (void *)&(sockin.sin_addr), buf, INET_ADDRSTRLEN+1);
				sprintf(cache, "<html><head><title>pilight</title></head>"
							   "<body><center><img src=\"logo.png\"><br />"
							   "<p style=\"color: #0099ff; font-weight: 800px;"
							   "font-family: Verdana; font-size: 20px;\">"
							   "The pilight webgui is located at "
							   "<a style=\"text-decoration: none; color: #0099ff;"
							   "font-weight: 800px; font-family: Verdana; font-size:"
							   "20px;\" href=\"http://%s:%d\">http://%s:%d</a></p>"
							   "</center></body></html>",
							   buf, webserver_http_port, buf, webserver_http_port);
				send(sd, (const char *)cache, strlen(cache), MSG_NOSIGNAL);
				FREE(cache);
			} else {
				send(sd, "<body><center><img src=\"logo.png\"></center></body></html>", 57, MSG_NOSIGNAL);
			}
			FREE(mimetype);
		}
	}
}
#endif

static int control_device(struct devices_t *dev, const char *state, const struct JsonNode *values, enum origin_t origin) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	struct devices_settings_t *sett = NULL;
	struct devices_values_t *val = NULL;
	struct options_t *opt = NULL;
	struct protocols_t *tmp_protocols = NULL;

	struct JsonNode *code = json_mkobject();
	struct JsonNode *json = json_mkobject();
	struct JsonNode *jprotocols = json_mkarray();

	/* Check all protocol options */
	for (tmp_protocols = dev->protocols; tmp_protocols; tmp_protocols = tmp_protocols->next) {
		json_append_element(jprotocols, json_mkstring(tmp_protocols->name));
		for(opt = tmp_protocols->listener->options; opt; opt = opt->next) {
			for (sett = dev->settings; sett; sett = sett->next) {
				/* Retrieve the device id's */
				if(strcmp(sett->name, "id") == 0) {
					for (val = sett->values; val; val = val->next) {
						if((opt->conftype == DEVICES_ID)
						   && strcmp(val->name, opt->name) == 0
						   && json_find_member(code, opt->name) == NULL) {
							if(val->type == JSON_STRING) {
								json_append_member(code, val->name, json_mkstring(val->string_));
							} else if(val->type == JSON_NUMBER) {
								json_append_member(code, val->name, json_mknumber(val->number_, val->decimals));
							}
						}
					}
				}
				if(strcmp(sett->name, opt->name) == 0 && opt->conftype == DEVICES_SETTING) {
					val = sett->values;
					if(json_find_member(code, opt->name) == NULL) {
						if(val->type == JSON_STRING) {
							json_append_member(code, opt->name, json_mkstring(val->string_));
						} else if(val->type == JSON_NUMBER) {
							json_append_member(code, opt->name, json_mknumber(val->number_, val->decimals));
						}
					}
				}
			}
		}
		for (; values; values = values->next) {
			for (opt = tmp_protocols->listener->options; opt; opt = opt->next) {
				if((opt->conftype == DEVICES_VALUE || opt->conftype == DEVICES_OPTIONAL)
				   && strcmp(values->key, opt->name) == 0
				   && json_find_member(code, opt->name) == NULL) {
					if(values->tag == JSON_STRING) {
						json_append_member(code, values->key, json_mkstring(values->string_));
					} else if(values->tag == JSON_NUMBER) {
						json_append_member(code, values->key, json_mknumber(values->number_, values->decimals_));
					}
				}
			}
		}
		/* Send the new device state */
		if(state != NULL) {
			for (opt = tmp_protocols->listener->options; opt; opt = opt->next) {
				if(json_find_member(code, opt->name) == NULL) {
					if(opt->conftype == DEVICES_STATE && opt->argtype == OPTION_NO_VALUE
					   && strcmp(opt->name, state) == 0) {
						json_append_member(code, opt->name, json_mknumber(1, 0));
						break;
					} else if(opt->conftype == DEVICES_STATE && opt->argtype == OPTION_HAS_VALUE) {
						json_append_member(code, opt->name, json_mkstring(state));
						break;
					}
				}
			}
		}
	}

	/* Construct the right json object */
	json_append_member(code, "protocol", jprotocols);
	if(dev->dev_uuid != NULL && (dev->protocols->listener->hwtype == SENSOR
	   || dev->protocols->listener->hwtype == HWRELAY)) {
		json_append_member(code, "uuid", json_mkstring(dev->dev_uuid));
	}
	json_append_member(json, "code", code);
	json_append_member(json, "action", json_mkstring("send"));

	if(send_queue(json, origin) == 0) {
		json_delete(json);
		return 0;
	}

	json_delete(json);
	return -1;
}

/* Parse the incoming buffer from the client */
static void socket_parse_data(int i, char *buffer) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	struct sockaddr_in address;
	struct JsonNode *json = NULL;
	const struct JsonNode *options = NULL;
	struct clients_t *tmp_clients = NULL;
	struct clients_t *client = NULL;
	int sd = -1;
	int addrlen = sizeof(address);
	const char *action = NULL, *media = NULL, *status = NULL;
	int error = 0, exists = 0;

	if(pilight.runmode == ADHOC) {
		sd = sockfd;
	} else {
		sd = socket_get_clients(i);
		getpeername(sd, (struct sockaddr*)&address, (socklen_t*)&addrlen);
	}

	if(strcmp(buffer, "HEART") == 0) {
		socket_write(sd, "BEAT");
	} else {
		if(pilight.runmode != ADHOC) {
			logprintf(LOG_DEBUG, "socket recv: %s", buffer);
		}
		/* Serve static webserver page. This is the only request that is
		   expected not to be a json object */
#ifdef WEBSERVER
		if(strstr(buffer, " HTTP/")) {
			client_webserver_parse_code(i, buffer);
			socket_close(sd);
		} else if(json_validate(buffer, NULL) == true) {
#else
		if(json_validate(buffer, NULL) == true) {
#endif
			json = json_decode(buffer);
			if((json_find_string(json, "action", &action)) == 0) {
				tmp_clients = clients_head;
				for(; tmp_clients; tmp_clients = tmp_clients->next) {
					if(tmp_clients->id == sd) {
						exists = 1;
						client = tmp_clients;
						break;
					}
				}
				if(strcmp(action, "identify") == 0) {
					/* Check if client doesn't already exist */
					if(exists == 0) {
						client = MALLOC_OR_EXIT(sizeof(struct clients_t));
						client->core = 0;
						client->config = 0;
						client->receiver = 0;
						client->forward = 0;
						client->stats = 0;
						client->cpu = 0;
						client->ram = 0;
						strcpy(client->media, "all");
						client->next = NULL;
						client->id = sd;
						memset(client->uuid, '\0', sizeof(client->uuid));
					}
					if(json_find_string(json, "media", &media) == 0) {
						if(strcmp(media, "all") == 0 ||
						   strcmp(media, "mobile") == 0 ||
						   strcmp(media, "desktop") == 0 ||
						   strcmp(media, "web") == 0) {
							strcpy(client->media, media);
						}
					} else {
						strcpy(client->media, "all");
					}
					const char *t = NULL;
					if(json_find_string(json, "uuid", &t) == 0) {
						strcpy(client->uuid, t);
					}
					const struct JsonNode *childs = NULL;
					options = json_find_member(json, "options");
					json_foreach(childs, options) {
						if(strcmp(childs->key, "core") == 0 && childs->tag == JSON_NUMBER) {
							client->core = (int)childs->number_ == 1;
						} else if(strcmp(childs->key, "stats") == 0 && childs->tag == JSON_NUMBER) {
							client->stats = (int)childs->number_ == 1;
						} else if(strcmp(childs->key, "receiver") == 0 && childs->tag == JSON_NUMBER) {
							client->receiver = (int)childs->number_ == 1;
						} else if(strcmp(childs->key, "config") == 0 && childs->tag == JSON_NUMBER) {
							client->config = (int)childs->number_ == 1;
						} else if(strcmp(childs->key, "forward") == 0 && childs->tag == JSON_NUMBER) {
							client->forward = (int)childs->number_ == 1;
						} else {
							error = 1;
							break;
						}
					}
					if(exists == 0) {
						if(error == 1) {
							FREE(client);
						} else {
							CONFIG_APPEND_NODE_TO_LIST(client, clients_head);
						}
					}
					socket_write(sd, "{\"status\":\"success\"}");
				} else if(strcmp(action, "send") == 0) {
					if(send_queue(json, SENDER) == 0) {
						socket_write(sd, "{\"status\":\"success\"}");
					} else {
						socket_write(sd, "{\"status\":\"failed\"}");
					}
				} else if(strcmp(action, "control") == 0) {
					const struct JsonNode *code = NULL;
					struct devices_t *dev = NULL;
					const char *device = NULL;
					if((code = json_find_member(json, "code")) == NULL || code->tag != JSON_OBJECT) {
						logprintf(LOG_ERR, "client did not send any codes");
					} else if(json_find_string(code, "device", &device) != 0) {
						logprintf(LOG_ERR, "client did not send a device");
					} else if(devices_get(device, &dev) == 0) {
						/* Check if the given device and location exists in the config file */
						const char *state = NULL;
						const struct JsonNode *values = NULL;

						json_find_string(code, "state", &state);
						if((values = json_find_member(code, "values")) != NULL) {
							values = json_first_child(values);
						}

						if(control_device(dev, state, values, SENDER) == 0) {
							socket_write(sd, "{\"status\":\"success\"}");
						} else {
							socket_write(sd, "{\"status\":\"failed\"}");
						}
					} else {
						logprintf(LOG_ERR, "the device \"%s\" does not exist", device);
					}
				} else if(strcmp(action, "registry") == 0) {
					const struct JsonNode *value = NULL;
					const char *type = NULL;
					const char *key = NULL;
					const char *sval = NULL;
					double nval = 0.0;
					int dec = 0;
					if(json_find_string(json, "type", &type) != 0) {
						logprintf(LOG_ERR, "client did not send a type of action");
					} else if(strcmp(type, "set") == 0) {
						if(json_find_string(json, "key", &key) != 0) {
							logprintf(LOG_ERR, "client did not send a registry key");
							socket_write(sd, "{\"status\":\"failed\"}");
						} else if((value = json_find_member(json, "value")) == NULL) {
							logprintf(LOG_ERR, "client did not send a registry value");
							socket_write(sd, "{\"status\":\"failed\"}");
						} else if(value->tag == JSON_NUMBER) {
							if(registry_set_number(key, value->number_, value->decimals_) == 0) {
								socket_write(sd, "{\"status\":\"success\"}");
							} else {
								socket_write(sd, "{\"status\":\"failed\"}");
							}
						} else if(value->tag == JSON_STRING) {
							if(registry_set_string(key, value->string_) == 0) {
								socket_write(sd, "{\"status\":\"success\"}");
							} else {
								socket_write(sd, "{\"status\":\"failed\"}");
							}
						} else {
							logprintf(LOG_ERR, "registry value can only be a string or number");
							socket_write(sd, "{\"status\":\"failed\"}");
						}
					} else if(strcmp(type, "remove") == 0) {
						if(json_find_string(json, "key", &key) != 0) {
							logprintf(LOG_ERR, "client did not send a registry key");
							socket_write(sd, "{\"status\":\"failed\"}");
						} else if(registry_remove_value(key) == 0) {
							socket_write(sd, "{\"status\":\"success\"}");
						} else {
							socket_write(sd, "{\"status\":\"failed\"}");
						}
					} else if(strcmp(type, "get") == 0) {
						if(json_find_string(json, "key", &key) != 0) {
							logprintf(LOG_ERR, "client did not send a registry key");
							socket_write(sd, "{\"status\":\"failed\"}");
						} else if(registry_get_number(key, &nval, &dec) == 0) {
							struct JsonNode *jsend = json_mkobject();
							json_append_member(jsend, "message", json_mkstring("registry"));
							json_append_member(jsend, "value", json_mknumber(nval, dec));
							json_append_member(jsend, "key", json_mkstring(key));
							char *output = json_stringify(jsend, NULL);
							socket_write(sd, output);
							json_free(output);
							json_delete(jsend);
						} else if(registry_get_string(key, &sval) == 0) {
							struct JsonNode *jsend = json_mkobject();
							json_append_member(jsend, "message", json_mkstring("registry"));
							json_append_member(jsend, "value", json_mkstring(sval));
							json_append_member(jsend, "key", json_mkstring(key));
							char *output = json_stringify(jsend, NULL);
							socket_write(sd, output);
							json_free(output);
							json_delete(jsend);
						} else {
							logprintf(LOG_ERR, "registry key '%s' doesn't exists", key);
							socket_write(sd, "{\"status\":\"failed\"}");
						}
					}
				} else if(strcmp(action, "request config") == 0) {
					struct JsonNode *jsend = json_mkobject();
					struct JsonNode *jconfig = NULL;
					if(client->forward == 1) {
						jconfig = config_print(CONFIG_FORWARD, client->media);
					} else {
						jconfig = config_print(CONFIG_INTERNAL, client->media);
					}
					json_append_member(jsend, "message", json_mkstring("config"));
					json_append_member(jsend, "config", jconfig);
					char *output = json_stringify(jsend, NULL);
					str_replace("%", "%%", &output);
					socket_write(sd, output);
					json_free(output);
					json_delete(jsend);
				} else if(strcmp(action, "request values") == 0) {
					struct JsonNode *jsend = json_mkobject();
					struct JsonNode *jvalues = devices_values(client->media);
					json_append_member(jsend, "message", json_mkstring("values"));
					json_append_member(jsend, "values", jvalues);
					char *output = json_stringify(jsend, NULL);
					socket_write(sd, output);
					json_free(output);
					json_delete(jsend);
				/*
				 * Parse received codes from nodes
				 */
				} else if(strcmp(action, "update") == 0) {
					const struct JsonNode *jvalues = NULL;
					const char *pname = NULL;
					if((jvalues = json_find_member(json, "values")) != NULL) {
						exists = 0;
						tmp_clients = clients_head;
						for(; tmp_clients; tmp_clients = tmp_clients->next) {
							if(tmp_clients->id == sd) {
								exists = 1;
								client = tmp_clients;
								break;
							}
						}
						if(exists) {
							json_find_number(jvalues, "ram", &client->ram);
							json_find_number(jvalues, "cpu", &client->cpu);
						}
					}
					if(json_find_string(json, "protocol", &pname) == 0) {
						broadcast_queue(pname, json, MASTER);
					}
				} else {
					error = 1;
				}
			} else if((json_find_string(json, "status", &status)) == 0) {
				tmp_clients = clients_head;
				for(; tmp_clients; tmp_clients = tmp_clients->next) {
					if(tmp_clients->id == sd) {
						exists = 1;
						client = tmp_clients;
						break;
					}
				}
				if(strcmp(status, "success") == 0) {
					logprintf(LOG_DEBUG, "client \"%s\" successfully executed our latest request", client->uuid);
				} else if(strcmp(status, "failed") == 0) {
					logprintf(LOG_DEBUG, "client \"%s\" failed executing our latest request", client->uuid);
				}
			} else {
				error = 1;
			}
			json_delete(json);
		}
	}
	if(error == 1) {
		client_remove(sd);
		socket_close(sd);
	}
}

static void socket_client_disconnected(int i) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	client_remove(socket_get_clients(i));
}

static void *receivePulseTrain(void *param) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	struct rawcode_t r;
	r.length = 0;
	int plslen = 0;
#ifdef _WIN32
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#else
	/* Make sure the pilight receiving gets
	   the highest priority available */
	struct sched_param sched = { 0 };
	sched.sched_priority = 70;
	pthread_setschedparam(pthread_self(), SCHED_FIFO, &sched);
#endif

	struct hardware_t *hw = (hardware_t *)param;
	hw->running = 1;

	while(main_loop != 0 && hw->receivePulseTrain != NULL && hw->stop == 0) {
		pthread_mutex_lock(&hw->lock);
		if(hw->wait != 0) {
			logprintf(LOG_DEBUG, "%s::waiting", __FUNCTION__);
			pthread_cond_wait(&hw->signal, &hw->lock);
			logprintf(LOG_DEBUG, "%s::working", __FUNCTION__);
			continue;
		}
		if(main_loop == 0 || hw->wait != 0) {
			pthread_mutex_unlock(&hw->lock);
			continue;
		}

		r.length = 0;
		hw->receivePulseTrain(&r);
		if(r.length > 0) {
			plslen = r.pulses[r.length-1]/PULSE_DIV;
			receive_queue(r.pulses, r.length, plslen, hw->hwtype);
		} else if(r.length < 0) {
			hw->init();
			sleep(1);
		}
		pthread_mutex_unlock(&hw->lock);
	}
	hw->running = 0;
	return (void *)NULL;
}

void dump_rawcode(const rawcode_t *r, const hardware_t *hw) {

	int ii;
	for(ii = 0; ii < r->length; ii++) {
		if ((ii & 15) == 0)
			fprintf(stdout, "\n%4d:", ii);
		fprintf(stdout, " %4d", r->pulses[ii]);
	}
	if(ii)
		fprintf(stdout, "\n%4d: min/maxgaplen=%d/%d, min/maxrawlen=%d/%d\n",
			ii, hw->mingaplen, hw->maxgaplen, hw->minrawlen, hw->maxrawlen);
}

static void *receiveOOK(void *param) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

#ifdef _WIN32
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#else
	/* Make sure the pilight receiving gets
	   the highest priority available */
	struct sched_param sched = { 0 };
	sched.sched_priority = 70;
	pthread_setschedparam(pthread_self(), SCHED_FIFO, &sched);
#endif

	hardware_t *hw = (hardware_t *)param;
	hw->running = 1;
	pthread_mutex_lock(&hw->lock);
	logprintf(LOG_DEBUG, "%s::working", __FUNCTION__);
	while(main_loop != 0 && hw->receiveOOK != NULL && hw->stop == 0) {

		if(hw->wait != 0) {
			logprintf(LOG_DEBUG, "%s::waiting", __FUNCTION__);
			pthread_cond_wait(&hw->signal, &hw->lock);
			logprintf(LOG_DEBUG, "%s::working", __FUNCTION__);
			continue;
		}

		rawcode_t r;
		r.length = 0;
		int duration;
		while ((duration = hw->receiveOOK()) > 0) {

			if(r.length >= countof(r.pulses)) {
				r.length = 0;
			}
			r.pulses[r.length++] = duration;
			if(duration < hw->mingaplen) {
				continue;
			}

		//	dump_rawcode(&r, hw);

			/* Let's do a little filtering here as well */
			if(r.length >= hw->minrawlen && r.length <= hw->maxrawlen) {
				int plslen = duration < hw->maxgaplen ? duration/PULSE_DIV : 0;
				receive_queue(r.pulses, r.length, plslen, hw->hwtype);
			}
			break;
		}

		if(duration < 0) {
			/* Hardware failure */
			struct timeval tp;
			struct timespec ts = { 0 };
			gettimeofday(&tp, NULL);
			ts.tv_sec = tp.tv_sec + 1;
			ts.tv_nsec = tp.tv_usec * 1000;
			logprintf(LOG_DEBUG, "%s::waiting", __FUNCTION__);
			pthread_cond_timedwait(&hw->signal, &hw->lock, &ts);
			logprintf(LOG_DEBUG, "%s::working", __FUNCTION__);
		}

	}
	pthread_mutex_unlock(&hw->lock);
	hw->running = 0;
	return (void *)NULL;
}

static void *clientize(void *param) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	struct ssdp_list_t *ssdp_list = NULL;
	struct JsonNode *json = NULL;
	struct JsonNode *joptions = NULL;
	const struct JsonNode *jchilds = NULL;
	char *recvBuff = NULL, *output = NULL;
	const char *message = NULL, *action = NULL;
	const char *origin = NULL, *protocol = NULL;
	int client_loop = 0, config_synced = 0;

	while(main_loop) {

		if(client_loop == 1) {
			logprintf(LOG_NOTICE, "connection to main pilight daemon lost");
			logprintf(LOG_NOTICE, "trying to reconnect...");
			sleep(1);
		}

		client_loop = 1;
		config_synced = 0;

		ssdp_list = NULL;
		if(master_server != NULL && master_port > 0) {
			if((sockfd = socket_connect(master_server, master_port)) == -1) {
				logprintf(LOG_ERR, "could not connect to pilight-daemon");
				continue;
			}
		} else if(ssdp_seek(&ssdp_list) == -1) {
			logprintf(LOG_NOTICE, "no pilight ssdp connections found");
			continue;
		} else {
			if((sockfd = socket_connect(ssdp_list->ip, ssdp_list->port)) == -1) {
				logprintf(LOG_ERR, "could not connect to pilight-daemon");
				continue;
			}
		}
		if(ssdp_list) {
			ssdp_free(ssdp_list);
		}

		json = json_mkobject();
		joptions = json_mkobject();
		json_append_member(json, "action", json_mkstring("identify"));
		json_append_member(joptions, "receiver", json_mknumber(1, 0));
		json_append_member(joptions, "forward", json_mknumber(1, 0));
		json_append_member(joptions, "config", json_mknumber(1, 0));
		json_append_member(json, "uuid", json_mkstring(pilight_uuid));
		json_append_member(json, "options", joptions);
		output = json_stringify(json, NULL);
		if(socket_write(sockfd, output) != (strlen(output)+strlen(EOSS))) {
			json_free(output);
			json_delete(json);
			continue;
		}
		json_free(output);
		json_delete(json);

		if(socket_read(sockfd, &recvBuff, 1) != 0
		   || strcmp(recvBuff, "{\"status\":\"success\"}") != 0) {
			continue;
		}
		logprintf(LOG_DEBUG, "socket recv: %s", recvBuff);

		json = json_mkobject();
		json_append_member(json, "action", json_mkstring("request config"));
		output = json_stringify(json, NULL);
		if(socket_write(sockfd, output) != (strlen(output)+strlen(EOSS))) {
			json_free(output);
			json_delete(json);
			continue;
		}
		json_free(output);
		json_delete(json);

		if(socket_read(sockfd, &recvBuff, 0) == 0) {
			logprintf(LOG_DEBUG, "socket recv: %s", recvBuff);
			if(json_validate(recvBuff, NULL) == true) {
				json = json_decode(recvBuff);
				if(json_find_string(json, "message", &message) == 0) {
					if(strcmp(message, "config") == 0) {
						const struct JsonNode *jconfig = NULL;
						if((jconfig = json_find_member(json, "config")) != NULL) {
							gui_gc();
							devices_gc();
#ifdef EVENTS
							rules_gc();
#endif
							registry_gc();

							json_foreach(jchilds, jconfig) {
								if(strcmp(jchilds->key, "devices") != 0) {
									jchilds = json_delete_force(jchilds);
								}
							}

							if(config_parse(jconfig) == EXIT_SUCCESS) {
								logprintf(LOG_DEBUG, "loaded master configuration");
								config_synced = 1;
							} else {
								logprintf(LOG_WARNING, "failed to load master configuration");
							}
						}
					}
				}
				json_delete(json);
			}
		}

		while(client_loop && config_synced) {
			if(sockfd <= 0) {
				break;
			}
			if(main_loop == 0) {
				client_loop = 0;
				break;
			}

			int n = socket_read(sockfd, &recvBuff, 1);
			if(n == -1) {
				sockfd = 0;
				break;
			} else if(n == 1) {
				continue;
			}

			logprintf(LOG_DEBUG, "socket recv: %s", recvBuff);
			char **array = NULL;
			unsigned int z = explode(recvBuff, "\n", &array), q = 0;
			for(q=0;q<z;q++) {
				if((json = json_decode(array[q])) == NULL) {
					continue;
				}
				if(json_find_string(json, "action", &action) == 0) {
					if(strcmp(action, "send") == 0 ||
					   strcmp(action, "control") == 0) {
						socket_parse_data(sockfd, array[q]);
					}
				} else if(json_find_string(json, "origin", &origin) == 0 &&
						json_find_string(json, "protocol", &protocol) == 0) {
						if(strcmp(origin, "receiver") == 0 ||
						   strcmp(origin, "sender") == 0) {
							broadcast_queue(protocol, json, NODE);
					}
				}
				json_delete(json);
			}
			array_free(&array, z);
		}
	}

	if(recvBuff != NULL) {
		FREE(recvBuff);
	}

	return NULL;
}

#ifndef _WIN32
static void save_pid(pid_t npid) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	int f = 0;
	if((f = open(pid_file, O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR)) != -1) {
		lseek(f, 0, SEEK_SET);
		char buffer[BUFFER_SIZE];
		sprintf(buffer, "%d", npid);
		int i = strlen(buffer);
		if(write(f, buffer, i) != i) {
			logprintf(LOG_WARNING, "could not store pid in '%s' (%s)", pid_file, strerror(errno));
		}
		close(f);
	}
}

static void daemonize(void) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	log_file_enable();
	log_shell_disable();
	/* Get the pid of the fork */

	pid_t npid = fork();
	switch(npid) {
		case 0:
		break;
		case -1:
			logprintf(LOG_ERR, "could not daemonize program");
			exit(1);
		break;
		default:
			save_pid(npid);
			logprintf(LOG_INFO, "daemon started with pid: %d", npid);
			exit(0);
		break;
	}
}
#endif

/* Garbage collector of main program */
static int main_gc(void) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	running = 0;
	pilight.running = 0;
	main_loop = 0;

	/* If we are running in node mode, the clientize
	   thread is waiting for a response from the main
	   daemon. This means we can't gracefull stop that
	   thread. However, by sending a HEART message the
	   main daemon will response with a BEAT. This allows
	   us to stop the socket_read function and properly
	   stop the clientize thread. */
	if(pilight.runmode == ADHOC) {
		socket_write(sockfd, "HEART");
	}

	pilight.runmode = STANDALONE;

	while(sending) {
		usleep(1000);
	}

#ifdef EVENTS
	events_gc();
#endif

	if(recvqueue_init == 1) {
		pthread_mutex_unlock(&recvqueue_lock);
		pthread_cond_signal(&recvqueue_signal);
		usleep(1000);
	}

	if(sendqueue_init == 1) {
		pthread_mutex_unlock(&sendqueue_lock);
		pthread_cond_signal(&sendqueue_signal);
	}

	if(bcqueue_init == 1) {
		pthread_mutex_unlock(&bcqueue_lock);
		pthread_cond_signal(&bcqueue_signal);
	}

	struct clients_t *tmp_clients;
	while(clients_head) {
		tmp_clients = clients_head;
		clients_head = clients_head->next;
		FREE(tmp_clients);
	}

#ifndef _WIN32
	if(running == 0) {
		/* Remove the stale pid file */
		if(access(pid_file, F_OK) != -1) {
			if(remove(pid_file) != -1) {
				logprintf(LOG_INFO, "removed stale pid_file '%s'", pid_file);
			} else {
				logprintf(LOG_ERR, "could not remove stale pid file '%s' (%s)", pid_file, strerror(errno));
			}
		}
	}

	FREE(pid_file);
	pid_file = NULL;
#endif
#ifdef WEBSERVER
	if(webserver_enable == 1) {
		webserver_gc();
	}
	webserver_root = NULL;
#endif

	FREE(master_server);

	datetime_gc();
	ssdp_gc();
	options_gc();
	socket_gc();

	config_gc();
	protocol_gc();
	ntp_gc();
	whitelist_free();
	threads_gc();
#ifndef _WIN32
	wiringXGC();
#endif
	dso_gc();
	log_gc();
	FREE(configtmp);

	gc_clear();
	FREE(progname);
	xfree();

#ifdef _WIN32
	WSACleanup();
	if(console == 1) {
		FreeConsole();
	}
#endif

	return 0;
}

static void procProtocolInit(void) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	protocol_register(&procProtocol);
	protocol_set_id(procProtocol, "process");
	protocol_device_add(procProtocol, "process", "pilight proc. API");
	procProtocol->devtype = PROCESS;
	procProtocol->hwtype = API;
	procProtocol->multipleId = 0;
	procProtocol->config = 0;

	options_add(&procProtocol->options, 'c', "cpu", OPTION_HAS_VALUE, DEVICES_VALUE, JSON_NUMBER, NULL, NULL);
	options_add(&procProtocol->options, 'r', "ram", OPTION_HAS_VALUE, DEVICES_VALUE, JSON_NUMBER, NULL, NULL);
}

#ifndef _WIN32
#pragma GCC diagnostic push  // require GCC 4.6
#pragma GCC diagnostic ignored "-Wcast-qual"
#endif
void registerVersion(void) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	registry_remove_value("pilight.version");
	registry_set_string("pilight.version.current", (char *)PILIGHT_VERSION);
}
#ifndef _WIN32
#pragma GCC diagnostic pop   // require GCC 4.6
#endif

#ifdef _WIN32
static void closeconsole(void) {
	log_shell_disable();
	verbosity = oldverbosity;
	FreeConsole();
	console = 0;
}

static BOOL CtrlHandler(DWORD fdwCtrlType) {
	closeconsole();
	return TRUE;
}

static void openconsole(void) {
	DWORD lpMode;
	AllocConsole();
	freopen("CONOUT$", "w", stdout);
	freopen("CONOUT$", "w", stderr);
	HWND hWnd = GetConsoleWindow();
	if(hWnd != NULL) {
		GetConsoleMode(hWnd, &lpMode);
		SetConsoleMode(hWnd, lpMode & ~ENABLE_PROCESSED_INPUT);
		SetConsoleCtrlHandler((PHANDLER_ROUTINE)CtrlHandler, TRUE);
		HMENU hMenu = GetSystemMenu(hWnd, FALSE);
		if(hMenu != NULL) {
			RemoveMenu(hMenu, SC_CLOSE, MF_GRAYED);
			RemoveMenu(hMenu, SC_MINIMIZE, MF_GRAYED);
			RemoveMenu(hMenu, SC_MAXIMIZE, MF_GRAYED);
		}

		console = 1;
		oldverbosity = verbosity;
		verbosity = LOG_DEBUG;
		log_level_set(verbosity);
		log_shell_enable();
	}
}
#endif

static void *pilight_stats(void *param) {
	int checkram = 0, checkcpu = 0, i = -1, x = 0, watchdog = 1, stats = 1;
	settings_find_number("watchdog-enable", &watchdog);
	settings_find_number("stats-enable", &stats);

	while(main_loop) {
		if(pilight.runmode == STANDALONE) {
			registerVersion();
		}

		if(stats == 1) {
			double cpu = 0.0, ram = 0.0;
			time_t cpu_ram_time;
			cpu = getCPUUsage();
			ram = getRAMUsage();
			time(&cpu_ram_time);

			if(threadprofiler == 1) {
				threads_cpu_usage(1);
			}
			if(watchdog == 1 && (i > -1) && (cpu > 60)) {
				if(nodaemon == 1 && threadprofiler == 0) {
					threads_cpu_usage(x);
					x ^= 1;
				}
				if(checkcpu == 0) {
					if(cpu > 90) {
						logprintf(LOG_CRIT, "cpu usage way too high %f%%", cpu);
					} else {
						logprintf(LOG_WARNING, "cpu usage too high %f%%", cpu);
					}
					logprintf(LOG_WARNING, "checking again in 10 seconds");
					sleep(10);
				} else {
					if(cpu > 90) {
						logprintf(LOG_ALERT, "cpu usage still way too high %f%%, exiting", cpu);
					} else {
						logprintf(LOG_CRIT, "cpu usage still too high %f%%, stopping", cpu);
					}
				}
				if(checkcpu == 1) {
					if(cpu > 90) {
						exit(EXIT_FAILURE);
					} else {
						main_gc();
						break;
					}
				}
				checkcpu = 1;
			} else if(watchdog == 1 && (i > -1) && (ram > 60)) {
				if(checkram == 0) {
					if(ram > 90) {
						logprintf(LOG_CRIT, "ram usage way too high %f%%", ram);
						exit(EXIT_FAILURE);
					} else {
						logprintf(LOG_WARNING, "ram usage too high %f%%", ram);
					}
					logprintf(LOG_WARNING, "checking again in 10 seconds");
					sleep(10);
				} else {
					if(ram > 90) {
						logprintf(LOG_ALERT, "ram usage still way too high %f%%, exiting", ram);
					} else {
						logprintf(LOG_CRIT, "ram usage still too high %f%%, stopping", ram);
					}
				}
				if(checkram == 1) {
					if(ram > 90) {
						exit(EXIT_FAILURE);
					} else {
						main_gc();
						break;
					}
				}
				checkram = 1;
			} else {
				checkcpu = 0;
				checkram = 0;
				if((i > 0 && i%3 == 0) || (i == -1)) {
					procProtocol->message = json_mkobject();
					struct JsonNode *code = json_mkobject();
					json_append_member(code, "cpu", json_mknumber(cpu, 16));
					if(ram > 0) {
						json_append_member(code, "ram", json_mknumber(ram, 16));
					}
					logprintf(LOG_DEBUG, "cpu: %f%%, ram: %f%%", cpu, ram);
					json_append_member(procProtocol->message, "values", code);
					json_append_member(procProtocol->message, "origin", json_mkstring("core"));
					json_append_member(procProtocol->message, "type", json_mknumber(PROCESS, 0));
					struct clients_t *tmp_clients = clients_head;
					for(; tmp_clients; tmp_clients = tmp_clients->next) {
						if(tmp_clients->cpu > 0 && tmp_clients->ram > 0) {
							logprintf(LOG_DEBUG, "- client: %s cpu: %f%%, ram: %f%%",
								tmp_clients->uuid, tmp_clients->cpu, tmp_clients->ram);
						}
					}
					add_timestamp(procProtocol->message, "timestamp", &cpu_ram_time);
					pilight.broadcast(procProtocol->id, procProtocol->message, STATS);
					json_delete(procProtocol->message);
					procProtocol->message = NULL;

					i = 0;
					x = 0;
				}
				i++;
			}
		}
		sleep(1);
	}
	return (void *)NULL;
}

static int adjust_hardware_limits() {

	conf_hardware_t *tmp_confhw = conf_hardware;
	for(; tmp_confhw; tmp_confhw = tmp_confhw->next) {
		hardware_t *hw = tmp_confhw->hardware;
		if(hw == NULL || hw->init == NULL || hw->comtype != COMOOK)
			continue;

		const struct protocols_t *tmp = protocols;
		for(; tmp; tmp = tmp->next) {
			protocol_t *listener = tmp->listener;
			if(!protocol_can_hwtype(listener, hw->hwtype))
				continue;

			if(listener->rawlen > 0) {
				logprintf(LOG_EMERG, "%s: setting \"rawlen\" length is not allowed, "
					"use the \"minrawlen\" and \"maxrawlen\" instead", listener->id);
				return EXIT_FAILURE;
			}

			if(listener->maxrawlen > hw->maxrawlen) {
				hw->maxrawlen = listener->maxrawlen;
			}
			if(listener->minrawlen > 0 && listener->minrawlen < hw->minrawlen) {
				hw->minrawlen = listener->minrawlen;
			}
			if(listener->maxgaplen > hw->maxgaplen) {
				hw->maxgaplen = listener->maxgaplen;
			}
			if(listener->mingaplen > 0 && listener->mingaplen < hw->mingaplen) {
				hw->mingaplen = listener->mingaplen;
			}
		}
	}
	return EXIT_SUCCESS;
}

static int start_pilight(int argc, char **argv) {
	struct options_t *options = NULL;
	struct ssdp_list_t *ssdp_list = NULL;

	pid_t other_pid_running = 0;
	int itmp = 0, show_default = 0, show_version = 0, show_help = 0;
#ifndef _WIN32
	int f = 0;
#endif
	const char *stmp = NULL;
	char *args = NULL, *p = NULL;
	int port = 0;

	wiringXLog = logprintf;

	progname = STRDUP_OR_EXIT("pilight-daemon");
	configtmp = STRDUP_OR_EXIT(CONFIG_FILE);

	options_add(&options, 'H', "help", OPTION_NO_VALUE, 0, JSON_NULL, NULL, NULL);
	options_add(&options, 'V', "version", OPTION_NO_VALUE, 0, JSON_NULL, NULL, NULL);
	options_add(&options, 'D', "nodaemon", OPTION_NO_VALUE, 0, JSON_NULL, NULL, NULL);
	options_add(&options, 'C', "config", OPTION_HAS_VALUE, 0, JSON_NULL, NULL, NULL);
	options_add(&options, 'S', "server", OPTION_HAS_VALUE, 0, JSON_NULL, NULL, "^(([0-9]|[1-9][0-9]|1[0-9]{2}|2[0-4][0-9]|25[0-5]).){3}([0-9]|[1-9][0-9]|1[0-9]{2}|2[0-4][0-9]|25[0-5])$");
	options_add(&options, 'P', "port", OPTION_HAS_VALUE, 0, JSON_NULL, NULL, "[0-9]{1,4}");
	options_add(&options, 256, "stacktracer", OPTION_NO_VALUE, 0, JSON_NULL, NULL, NULL);
	options_add(&options, 257, "threadprofiler", OPTION_NO_VALUE, 0, JSON_NULL, NULL, NULL);
	options_add(&options, 258, "debuglevel", OPTION_HAS_VALUE, 0, JSON_NULL, NULL, "[01]{1}");
	// options_add(&options, 258, "memory-tracer", OPTION_NO_VALUE, 0, JSON_NULL, NULL, NULL);

	while(1) {
		int c = options_parse(&options, argc, argv, 1, &args);
		if(c == -1) {
			break;
		}
		if(c == -2) {
			show_help = 1;
			break;
		}
		switch(c) {
			case 'H':
				show_help = 1;
			break;
			case 'V':
				show_version = 1;
			break;
			case 'C':
				configtmp = REALLOC_OR_EXIT(configtmp, strlen(args)+1);
				strcpy(configtmp, args);
			break;
			case 'S':
				master_server = STRDUP_OR_EXIT(args);
			break;
			case 'P':
				master_port = (unsigned short)atoi(args);
			break;
			case 'D':
				nodaemon = 1;
				verbosity = LOG_DEBUG;
			break;
			case 257:
				threadprofiler = 1;
				verbosity = LOG_ERR;
				nodaemon = 1;
			break;
			case 256:
				verbosity = LOG_STACK;
				stacktracer = 1;
				nodaemon = 1;
			break;
			case 258:
				pilight.debuglevel = atoi(args);
				nodaemon = 1;
				verbosity = LOG_DEBUG;
			break;
			default:
				show_default = 1;
			break;
		}
	}
	options_delete(options);
	if(show_help == 1) {
		char help[1024];
		char tabs[5];
#ifdef _WIN32
		strcpy(tabs, "\t");
#else
		strcpy(tabs, "\t\t");
#endif
		sprintf(help, "Usage: %s [options]\n"
									"\t -H --help\t\t\tdisplay usage summary\n"
									"\t -V --version\t%sdisplay version\n"
									"\t -C --config\t%sconfig file\n"
									"\t -S --server=x.x.x.x\t\tconnect to server address\n"
									"\t -P --port=xxxx\t%sconnect to server port\n"
									"\t -D --nodaemon\t%sdo not daemonize and\n"
									"\t\t\t%sshow debug information\n"
									"\t    --stacktracer\t\tshow internal function calls\n"
									"\t    --threadprofiler\t\tshow per thread cpu usage\n"
									"\t    --debuglevel\t\tshow additional development info\n",
									progname, tabs, tabs, tabs, tabs, tabs);
#ifdef _WIN32
		MessageBox(NULL, help, "pilight :: info", MB_OK);
#else
		printf("%s", help);
#endif
		goto clear;
	}
	if(show_version == 1) {
#ifdef HASH
	#ifdef _WIN32
			char version[50];
			snprintf(version, 50, "%s version %s", progname, HASH);
			MessageBox(NULL, version, "pilight :: info", MB_OK);
	#else
			printf("%s version %s\n", progname, HASH);
	#endif
#else
	#ifdef _WIN32
			char version[50];
			snprintf(version, 50, "%s version %s", progname, PILIGHT_VERSION);
			MessageBox(NULL, version, "pilight :: info", MB_OK);
	#else
			printf("%s version %s\n", progname, PILIGHT_VERSION);
	#endif
#endif
		goto clear;
	}
	if(show_default == 1) {
#ifdef _WIN32
		char def[50];
		snprintf(def, 50, "Usage: %s [options]\n", progname);
		MessageBox(NULL, def, "pilight :: info", MB_OK);
#else
		printf("Usage: %s [options]\n", progname);
#endif
		goto clear;
	}

#ifdef _WIN32
	if(nodaemon == 1) {
		openconsole();
		console = 1;
	}
#endif

	datetime_init();
	atomicinit();
	procProtocolInit();

#ifndef _WIN32
	if(geteuid() != 0) {
		printf("%s requires root privileges in order to run\n", progname);
		FREE(progname);
		exit(EXIT_FAILURE);
	}
#endif

	/* Run main garbage collector when quiting the daemon */
	gc_attach(main_gc);

	/* Catch all exit signals for gc */
	gc_catch();

	int nrdevs = 0, x = 0;
	char **devs = NULL;
	if((nrdevs = inetdevs(&devs)) > 0) {
		for(x=0;x<nrdevs;x++) {
			if((p = genuuid(devs[x])) == NULL) {
				logprintf(LOG_ERR, "could not generate the device uuid");
			} else {
				strcpy(pilight_uuid, p);
				FREE(p);
				break;
			}
		}
	}
	array_free(&devs, nrdevs);

	firmware.version = 0;
	firmware.lpf = 0;
	firmware.hpf = 0;
	firmware.method = NULL;
	firmware.raw_version = 0;

	log_level_set(LOG_INFO);
	log_file_enable();
	log_shell_disable();

	if(nodaemon == 1) {
		log_level_set(verbosity);
		log_shell_enable();
	}

#ifdef _WIN32
	if((pid = check_instances(L"pilight-daemon")) != -1) {
		logprintf(LOG_NOTICE, "pilight is already running");
		goto clear;
	}
#endif

	if((pid = isrunning("pilight-raw")) != -1) {
		logprintf(LOG_NOTICE, "pilight-raw instance found (%d)", (int)pid);
		goto clear;
	}

	if((pid = isrunning("pilight-debug")) != -1) {
		logprintf(LOG_NOTICE, "pilight-debug instance found (%d)", (int)pid);
		goto clear;
	}

	if(config_set_file(configtmp) == EXIT_FAILURE) {
		return EXIT_FAILURE;
	}

	protocol_init();
	config_init();

	/* Export certain daemon function to global usage */
	pilight.broadcast = &broadcast_queue;
	pilight.send = &send_queue;
	pilight.control = &control_device;
	pilight.receive = &receive_parse_api;

	if(config_read() != EXIT_SUCCESS) {
		goto clear;
	}

	registerVersion();

#ifdef WEBSERVER
	#ifdef WEBSERVER_HTTPS
	const char *pemfile = NULL;
	int pem_free = 0;
	if(settings_find_string("pem-file", &pemfile) != 0) {
		pemfile = PEM_FILE;
	}

	char *content = NULL;
	unsigned char md5sum[17];
	char md5conv[33];
	int i = 0;
	p = (char *)md5sum;
	if(file_exists(pemfile) != 0) {
		logprintf(LOG_ERR, "missing webserver SSL private key file '%s'", pemfile);
		goto clear;
	}
	if(file_get_contents(pemfile, &content) != -1) {
		md5((const unsigned char *)content, strlen((char *)content), (unsigned char *)p);
		for(i = 0; i < 32; i+=2) {
			sprintf(&md5conv[i], "%02x", md5sum[i/2] );
		}
		if(strcmp(md5conv, PILIGHT_PEM_MD5) == 0) {
			registry_set_number("webserver.ssl.certificate.secure", 0, 0);
		} else {
			registry_set_number("webserver.ssl.certificate.secure", 1, 0);
		}
		registry_set_string("webserver.ssl.certificate.location", pemfile);
		FREE(content);
	}

	#endif

	settings_find_number("webserver-enable", &webserver_enable);
	settings_find_number("webserver-http-port", &webserver_http_port);
	if(settings_find_string("webserver-root", &webserver_root) != 0) {
		webserver_root = WEBSERVER_ROOT;
	}
#endif

	settings_find_number("receive-while-sending", &receive_while_sending);

#ifndef _WIN32
	FREE(pid_file);
	pid_file = NULL;
	if(settings_find_string("pid-file", &pid_file) != 0) {
		pid_file = PID_FILE;
	}
	pid_file = STRDUP_OR_EXIT(pid_file);

	if((f = open(pid_file, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR)) != -1) {
		char buffer[BUFFER_SIZE] = { 0 };
		if(read(f, buffer, BUFFER_SIZE) == -1) {
			logprintf(LOG_WARNING, "could not read pid_file '%s' (%s)", pid_file, strerror(errno));
		} else {
			//If the file is empty, create a new process
			other_pid_running = atoi(buffer);
			if(!other_pid_running) {
				running = 0;
			} else {
				//Check if the process is running
				kill(other_pid_running, 0);
				//If not, create a new process
				if(errno == ESRCH) {
					other_pid_running = 0;
					running = 0;
				}
			}
		}
		close(f);
	} else {
		logprintf(LOG_ERR, "could not open / create pid_file '%s' (%s)", pid_file, strerror(errno));
		goto clear;
	}
#endif

	if(settings_find_number("log-level", &itmp) == 0) {
		log_level_set(itmp);
	}

	if(settings_find_string("log-file", &stmp) == 0) {
		if(log_file_set(stmp) == EXIT_FAILURE) {
			goto clear;
		}
	}

#ifdef HASH
	logprintf(LOG_INFO, "version %s", HASH);
#else
	logprintf(LOG_INFO, "version %s", PILIGHT_VERSION);
#endif

	if(nodaemon == 1 || running == 1) {
		log_file_disable();
		log_shell_enable();

		if(nodaemon == 1) {
			log_level_set(verbosity);
		} else {
			log_level_set(LOG_ERR);
		}
	}

#ifndef _WIN32
	if(running == 1) {
		nodaemon = 1;
		logprintf(LOG_NOTICE, "already active (pid %d)", other_pid_running);
		log_level_set(LOG_NOTICE);
		log_shell_disable();
		goto clear;
	}
#endif
	if (adjust_hardware_limits() != EXIT_SUCCESS) {
		goto clear;
	}

	settings_find_number("port", &port);
	settings_find_number("standalone", &standalone);

	pilight.runmode = STANDALONE;
	if(standalone == 0 || (master_server != NULL && master_port > 0)) {
		if(master_server != NULL && master_port > 0) {
			if((sockfd = socket_connect(master_server, master_port)) == -1) {
				logprintf(LOG_NOTICE, "pilight daemon not found @%s, waiting for it to come online", master_server);
			} else {
				logprintf(LOG_INFO, "a pilight daemon was found, clientizing");
			}
			pilight.runmode = ADHOC;
		} else if(ssdp_seek(&ssdp_list) == -1) {
			logprintf(LOG_INFO, "no pilight daemon found, daemonizing");
		} else {
			logprintf(LOG_NOTICE, "a pilight daemon was found @%s, clientizing", ssdp_list->ip);
			pilight.runmode = ADHOC;
		}
		if(ssdp_list) {
			ssdp_free(ssdp_list);
		}
	}

	if(pilight.runmode == STANDALONE) {
		socket_start((unsigned short)port);
		if(standalone == 0) {
			ssdp_start();
		}
	}

#ifndef _WIN32
	if(nodaemon == 0) {
		daemonize();
	} else {
		save_pid(getpid());
	}
#endif

	pilight.running = 1;

	/* Threads are unable to survive forks properly.
	 * Therefor, we queue all messages until we're
	 * able to fork properly.
	 */
	threads_create(&logpth, NULL, &logloop, (void *)NULL);

	pthread_mutexattr_init(&sendqueue_attr);
	pthread_mutexattr_settype(&sendqueue_attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&sendqueue_lock, &sendqueue_attr);
	pthread_cond_init(&sendqueue_signal, NULL);
	sendqueue_init = 1;

	pthread_mutexattr_init(&recvqueue_attr);
	pthread_mutexattr_settype(&recvqueue_attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&recvqueue_lock, &recvqueue_attr);
	pthread_cond_init(&recvqueue_signal, NULL);
	recvqueue_init = 1;

	pthread_mutexattr_init(&bcqueue_attr);
	pthread_mutexattr_settype(&bcqueue_attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&bcqueue_lock, &bcqueue_attr);
	pthread_cond_init(&bcqueue_signal, NULL);
	bcqueue_init = 1;

	/* Run certain daemon functions from the socket library */
	socket_callback.client_disconnected_callback = &socket_client_disconnected;
	socket_callback.client_connected_callback = NULL;
	socket_callback.client_data_callback = &socket_parse_data;

	/* Start threads library that keeps track of all threads used */
	threads_start();

	/* The daemon running in client mode, register a seperate thread that
	   communicates with the server */
	if(pilight.runmode == ADHOC) {
		threads_register("node", &clientize, (void *)NULL, 0);
	} else {
		/* Register a seperate thread for the socket server */
		threads_register("socket", &socket_wait, (void *)&socket_callback, 0);
		if(standalone == 0) {
			threads_register("ssdp", &ssdp_wait, (void *)NULL, 0);
		}
	}
	threads_register("sender", &send_code, (void *)NULL, 0);
	threads_register("broadcaster", &broadcast, (void *)NULL, 0);

	struct conf_hardware_t *tmp_confhw = conf_hardware;
	while(tmp_confhw && main_loop) {
		if(tmp_confhw->hardware->init) {
			if(tmp_confhw->hardware->init() == EXIT_FAILURE) {
				if(main_loop == 1) {
					logprintf(LOG_ERR, "could not initialize %s hardware module", tmp_confhw->hardware->id);
					goto clear;
				} else {
					break;
				}
			}
			if(main_loop == 1) {
				tmp_confhw->hardware->wait = 0;
				tmp_confhw->hardware->stop = 0;
				if(tmp_confhw->hardware->comtype == COMOOK) {
					threads_register(tmp_confhw->hardware->id, &receiveOOK, (void *)tmp_confhw->hardware, 0);
				} else if(tmp_confhw->hardware->comtype == COMPLSTRAIN) {
					threads_register(tmp_confhw->hardware->id, &receivePulseTrain, (void *)tmp_confhw->hardware, 0);
				} else if(tmp_confhw->hardware->comtype == COMAPI) {
					threads_register(tmp_confhw->hardware->id, tmp_confhw->hardware->receiveAPI, (void *)tmp_confhw->hardware, 0);
				}
			} else {
				break;
			}
		}
		if(main_loop == 0) {
			break;
		}
		tmp_confhw = tmp_confhw->next;
	}
	if(main_loop == 0) {
		goto clear;
	}

	threads_register("receive parser", &receive_parse_code, (void *)NULL, 0);

#ifdef EVENTS
	if(pilight.runmode == STANDALONE) {
		/* Register a seperate thread in which the daemon communicates the events library */
		threads_register("events client", &events_clientize, (void *)NULL, 0);
		threads_register("events loop", &events_loop, (void *)NULL, 0);
	}
#endif

#ifdef WEBSERVER
	settings_find_number("webgui-websockets", &webgui_websockets);

	/* Register a seperate thread for the webserver */
	if(webserver_enable == 1 && pilight.runmode == STANDALONE) {
		webserver_start();
		/* Register a seperate thread in which the webserver communicates the main daemon */
		threads_register("webserver client", &webserver_clientize, (void *)NULL, 0);
		if(webgui_websockets == 1) {
			threads_register("webserver broadcast", &webserver_broadcast, (void *)NULL, 0);
		}
	} else {
		webserver_enable = 0;
	}
#endif

	const char *ntpsync = NULL;
	if(settings_find_string("ntpserver0", &ntpsync) == 0) {
		threads_register("ntp sync", &ntpthread, NULL, 0);
	}

#ifdef _WIN32
	threads_register("stats", &pilight_stats, NULL, 0);
#endif

	return EXIT_SUCCESS;

clear:
	if(nodaemon == 0) {
		log_level_set(LOG_NOTICE);
		log_shell_disable();
	}
	if(main_loop == 1) {
		main_gc();
	}
	return EXIT_FAILURE;
}

#ifdef _WIN32
#define ID_TRAYICON 100
#define ID_QUIT 101
#define ID_SEPARATOR 102
#define ID_INSTALL_SERVICE 103
#define ID_REMOVE_SERVICE 104
#define ID_START 105
#define ID_STOP 106
#define ID_WEBGUI 107
#define ID_CONSOLE 108
#define ID_ICON 200
static NOTIFYICONDATA TrayIcon;

static LRESULT CALLBACK WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  char buf[200], *service_argv[] = {__argv[0], NULL};
  POINT pt;
  HMENU hMenu;

  switch(msg) {
    case WM_CREATE:
			if(start_pilight(__argc, __argv) == EXIT_FAILURE) {
				if(main_loop == 1) {
					main_gc();
				}
				exit(EXIT_FAILURE);
			}
			running = 1;
		break;
    case WM_COMMAND:
      switch(LOWORD(wParam)) {
        case ID_QUIT:
					if(main_loop == 1) {
						main_gc();
					}
          Shell_NotifyIcon(NIM_DELETE, &TrayIcon);
          PostQuitMessage(0);
				break;
				case ID_STOP:
					if(running == 1) {
						if(main_loop == 1) {
							main_gc();
						}
						running = 0;
						MessageBox(NULL, "pilight stopped", "pilight :: notice", MB_OK);
					}
				break;
				case ID_CONSOLE:
					if(console == 0) {
						openconsole();
					} else {
						closeconsole();
					}
				break;
				case ID_START:
					if(running == 0) {
						main_loop = 1;
						if(start_pilight(1, service_argv) == EXIT_FAILURE) {
							if(main_loop == 1) {
								main_gc();
								MessageBox(NULL, "pilight failed to start", "pilight :: error", MB_OK);
							}
						}
						running = 1;
						MessageBox(NULL, "pilight started", "pilight :: notice", MB_OK);
					}
				break;
				case ID_WEBGUI:
					if(webserver_http_port == 80) {
						snprintf(buf, sizeof(buf), "http://localhost");
					} else {
						snprintf(buf, sizeof(buf), "http://localhost:%d", webserver_http_port);
					}
					ShellExecute(NULL, "open", buf, NULL, NULL, SW_SHOWNORMAL);
				break;
      }
		break;
    case WM_USER:
      switch(lParam) {
        case WM_RBUTTONUP:
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
          hMenu = CreatePopupMenu();

          AppendMenu(hMenu, MF_STRING | MF_GRAYED, ID_SEPARATOR, server_name);
					AppendMenu(hMenu, MF_SEPARATOR, ID_SEPARATOR, "");
          AppendMenu(hMenu, MF_STRING | MF_GRAYED, ID_SEPARATOR, ((pilight.runmode == ADHOC) ? "Ad-Hoc mode" : "Standalone mode"));
					AppendMenu(hMenu, MF_SEPARATOR, ID_SEPARATOR, "");
					if(pilight.runmode == STANDALONE) {
						AppendMenu(hMenu, MF_STRING | (running ? 0 : MF_GRAYED), ID_WEBGUI, "Open webGUI");
					}
					if(console == 0) {
						AppendMenu(hMenu, MF_STRING, ID_CONSOLE, "Open console");
					} else {
						AppendMenu(hMenu, MF_STRING, ID_CONSOLE, "Close console");
					}
          AppendMenu(hMenu, MF_SEPARATOR, ID_SEPARATOR, "");

					// snprintf(buf, sizeof(buf), "pilight is %s", running ? "running" : "stopped");

					// AppendMenu(hMenu, MF_STRING | MF_GRAYED, ID_SEPARATOR, buf);
          // AppendMenu(hMenu, MF_SEPARATOR, ID_SEPARATOR, "");

					// AppendMenu(hMenu, MF_STRING | (running ? 0 : MF_GRAYED), ID_STOP, "Stop pilight");
					// AppendMenu(hMenu, MF_STRING | (running ? MF_GRAYED : 0), ID_START, "Start pilight");

          AppendMenu(hMenu, MF_STRING, ID_QUIT, "Exit");

          GetCursorPos(&pt);
          SetForegroundWindow(hWnd);
          TrackPopupMenu(hMenu, 0, pt.x, pt.y, 0, hWnd, NULL);
          PostMessage(hWnd, WM_NULL, 0, 0);
          DestroyMenu(hMenu);
				break;
      }
		break;
  }

  return DefWindowProc(hWnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmdline, int show) {
	pilight.running = 0;
	pilight.debuglevel = 0;

	HWND hWnd;
  WNDCLASS cls;
  MSG msg;
#ifdef HASH
	snprintf(server_name, sizeof(server_name), "pilight-daemon %s", HASH);
#else
	snprintf(server_name, sizeof(server_name), "pilight-daemon %s", PILIGHT_VERSION);
#endif
  memset(&cls, 0, sizeof(cls));
  cls.lpfnWndProc = (WNDPROC)WindowProc;
  cls.hIcon = LoadIcon(NULL, IDI_APPLICATION);
  cls.lpszClassName = server_name;

  RegisterClass(&cls);
  hWnd = CreateWindow(cls.lpszClassName, server_name, 0, 0, 0, 0, 0, NULL, NULL, NULL, NULL);
  ShowWindow(hWnd, SW_HIDE);

  TrayIcon.cbSize = sizeof(TrayIcon);
  TrayIcon.uID = ID_TRAYICON;
  TrayIcon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  TrayIcon.hIcon = LoadImage(GetModuleHandle(NULL), MAKEINTRESOURCE(ID_ICON), IMAGE_ICON, 16, 16, 0);
  TrayIcon.hWnd = hWnd;
  snprintf(TrayIcon.szTip, sizeof(TrayIcon.szTip), "%s", server_name);

  TrayIcon.uCallbackMessage = WM_USER;
  Shell_NotifyIcon(NIM_ADD, &TrayIcon);

  while(GetMessage(&msg, hWnd, 0, 0) && main_loop == 1) {
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }
	return 0;
}
#else
int main(int argc, char **argv) {
	pilight.running = 0;
	pilight.debuglevel = 0;

	int ret = start_pilight(argc, argv);
	if(ret == EXIT_SUCCESS) {
		pilight_stats((void *)NULL);
	}
	return ret;
}
#endif
