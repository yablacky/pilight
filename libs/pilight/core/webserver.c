/*
	Copyright (C) 2013 CurlyMo

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
#include <unistd.h>
#include <getopt.h>
#include <string.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <assert.h>
#include <signal.h>
#include <stdarg.h>
#include <pthread.h>
#ifdef _WIN32
#else
	#ifdef __mips__
		#define __USE_UNIX98
	#endif
	#include <pwd.h>
#endif

#include "../config/settings.h"
#include "../config/registry.h"
#include "threads.h"
#include "sha256cache.h"
#include "pilight.h"
#include "network.h"
#include "mongoose.h"
#include "gc.h"
#include "log.h"
#include "json.h"
#include "socket.h"
#include "webserver.h"
#include "ssdp.h"
#include "fcache.h"

#ifdef WEBSERVER_HTTPS
static int webserver_https_port = WEBSERVER_HTTPS_PORT;
#endif
static int webserver_http_port = WEBSERVER_HTTP_PORT;
static int webserver_cache = 1;
static int webgui_websockets = WEBGUI_WEBSOCKETS;
static const char *webserver_authentication_username = NULL;
static const char *webserver_authentication_password = NULL;
static unsigned short webserver_loop = 1;
static const char *webserver_root = NULL;
#ifdef WEBSERVER_HTTPS
static struct mg_server *mgserver[WEBSERVER_WORKERS+1];
#else
static struct mg_server *mgserver[WEBSERVER_WORKERS];
#endif
static char *recvBuff = NULL;

static int sockfd = 0;

typedef enum {
	WELCOME,
	IDENTIFY,
	REJECT,
	SYNC
} steps_t;

typedef struct webqueue_t {
	char *message;
	struct webqueue_t *next;
} webqueue_t;

static struct webqueue_t *webqueue; // head.
static struct webqueue_t *webqueue_tail;

static pthread_mutex_t webqueue_lock;
static pthread_cond_t webqueue_signal;
static pthread_mutexattr_t webqueue_attr;
static unsigned short webqueue_init = 0;

static int webqueue_number = 0;

int webserver_gc(void) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	int i = 0;

	while(webqueue_number > 0) {
		struct webqueue_t *tmp = webqueue;
		FREE(webqueue->message);
		webqueue = webqueue->next;
		FREE(tmp);
		webqueue_number--;
	}
	webqueue = webqueue_tail = NULL;

	webserver_loop = 0;

	if(webqueue_init == 1) {
		pthread_mutex_unlock(&webqueue_lock);
		pthread_cond_signal(&webqueue_signal);
	}

	webserver_root = NULL;
	webserver_authentication_password = NULL;
	webserver_authentication_username = NULL;

#ifdef WEBSERVER_HTTPS
	for(i=0;i<WEBSERVER_WORKERS+1;i++) {
#else
	for(i=0;i<WEBSERVER_WORKERS;i++) {
#endif
		if(mgserver[i] != NULL) {
			mg_wakeup_server(mgserver[i]);
		}
		usleep(100);
		mg_destroy_server(&mgserver[i]);
	}

	fcache_gc();
	sha256cache_gc();
	logprintf(LOG_DEBUG, "garbage collected webserver library");
	return 1;
}

struct filehandler_t {
	unsigned char *bytes;
	FILE *fp;
	unsigned int ptr;
	unsigned int length;
	unsigned short free;
};

void webserver_create_header(unsigned char **p, const char *message, const char *mimetype, unsigned int len) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	*p += sprintf((char *)*p,
		"HTTP/1.0 %s\r\n"
		"Server: pilight\r\n"
		"Content-Type: %s\r\n",
		message, mimetype);
	*p += sprintf((char *)*p,
		"Content-Length: %u\r\n\r\n",
		len);
}

static void webserver_create_404(const char *in, unsigned char **p) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	const char mimetype[] = "text/html";
	webserver_create_header(p, "404 Not Found", mimetype, (unsigned int)(202+strlen((const char *)in)));
	*p += sprintf((char *)*p, "<!DOCTYPE HTML PUBLIC \"-//IETF//DTD HTML 2.0//EN\">\x0d\x0a"
		"<html><head>\x0d\x0a"
		"<title>404 Not Found</title>\x0d\x0a"
		"</head><body>\x0d\x0a"
		"<h1>Not Found</h1>\x0d\x0a"
		"<p>The requested URL %s was not found on this server.</p>\x0d\x0a"
		"</body></html>",
		(const char *)in);
}

char *webserver_mimetype(const char *str) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	return STRDUP_OR_EXIT(str);
}

static int webserver_auth_handler(struct mg_connection *conn) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	if(webserver_authentication_username != NULL && webserver_authentication_password != NULL) {
		return mg_authorize_input(conn, webserver_authentication_username, webserver_authentication_password, mg_get_option(mgserver[0], "auth_domain"));
	} else {
		return MG_TRUE;
	}
}

static int webserver_parse_rest(struct mg_connection *conn) {
	char **array = NULL, **array1 = NULL;
	struct JsonNode *jobject = json_mkobject();
	struct JsonNode *jcode = json_mkobject();
	struct JsonNode *jvalues = json_mkobject();
	int a = 0, b = 0, c = 0, has_protocol = 0;

	if(strcmp(conn->request_method, "POST") == 0) {
		conn->query_string = conn->content;
	}
	if(strcmp(conn->request_method, "GET") == 0 && conn->query_string == NULL) {
		conn->query_string = conn->content;
	}
	if(conn->query_string != NULL) {
		struct devices_t *dev = NULL;
		char decoded[strlen(conn->query_string)+1];

		if(urldecode(conn->query_string, decoded) == -1) {
			char *z = "{\"message\":\"failed\",\"error\":\"cannot decode url\"}";
			mg_send_data(conn, z, strlen(z));
			return MG_TRUE;
		}
		
		char state[16], *p = NULL;
		a = explode((char *)decoded, "&", &array), b = 0, c = 0;
		memset(state, 0, 16);
	
		if(a > 1) {
			int type = 0; //0 = SEND, 1 = CONTROL, 2 = REGISTRY
			json_append_member(jobject, "code", jcode);

			if(strcmp(conn->uri, "/send") == 0) {
				type = 0;
				json_append_member(jobject, "action", json_mkstring("send"));
			} else if(strcmp(conn->uri, "/control") == 0) {
				type = 1;
				json_append_member(jobject, "action", json_mkstring("control"));
			} else if(strcmp(conn->uri, "/registry") == 0) {
				type = 2;
				json_append_member(jobject, "action", json_mkstring("registry"));
			}

			for(b=0;b<a;b++) {
				c = explode(array[b], "=", &array1);
				if(c == 2) {
					if(strcmp(array1[0], "protocol") == 0) {
						struct JsonNode *jprotocol = json_mkarray();
						json_append_element(jprotocol, json_mkstring(array1[1]));
						json_append_member(jcode, "protocol", jprotocol);
						has_protocol = 1;
					} else if(strcmp(array1[0], "state") == 0) {
						strcpy(state, array1[1]);
					} else if(strcmp(array1[0], "device") == 0) {
						if(devices_get(array1[1], &dev) != 0) {
							char *z = "{\"message\":\"failed\",\"error\":\"device does not exists\"}";
							mg_send_data(conn, z, strlen(z));
							goto clear1;
						}
					} else if(strncmp(array1[0], "values", 6) == 0) {
						char name[255], *ptr = name;
						if(sscanf(array1[0], "values[%254[a-z]]", ptr) != 1) {
							char *z = "{\"message\":\"failed\",\"error\":\"values should be passed like this \'values[dimlevel]=10\'\"}";
							mg_send_data(conn, z, strlen(z));
							goto clear1;
						} else {
							if(isNumeric(array1[1]) == 0) {
								json_append_member(jvalues, name, json_mknumber(atof(array1[1]), nrDecimals(array1[1])));
							} else {
								json_append_member(jvalues, name, json_mkstring(array1[1]));
							}
						}
					} else if(isNumeric(array1[1]) == 0) {
						json_append_member(jcode, array1[0], json_mknumber(atof(array1[1]), nrDecimals(array1[1])));
					} else {
						json_append_member(jcode, array1[0], json_mkstring(array1[1]));
					}
				}
				array_free(&array1, c);
			}

			if(type == 0) {
				if(has_protocol == 0) {
					char *z = "{\"message\":\"failed\",\"error\":\"no protocol was send\"}";
					mg_send_data(conn, z, strlen(z));
					goto clear2;
				}
				if(pilight.send != NULL) {
					if(pilight.send(jobject, SENDER) == 0) {
						char *z = "{\"message\":\"success\"}";
						mg_send_data(conn, z, strlen(z));
						goto clear2;
					}
				}
			} else if(type == 1) {
				if(pilight.control != NULL) {
					if(dev == NULL) {
						char *z = "{\"message\":\"failed\",\"error\":\"no device was send\"}";
						mg_send_data(conn, z, strlen(z));
						goto clear2;
					} else {
						if(strlen(state) > 0) {
							p = state;
						}
						if(pilight.control(dev, p, json_first_child(jvalues), SENDER) == 0) {
							char *z = "{\"message\":\"success\"}";
							mg_send_data(conn, z, strlen(z));
							goto clear2;
						}
					}
				}
			} else if(type == 2) {
				const struct JsonNode *value = NULL;
				const char *type = NULL;
				const char *key = NULL;
				const char *sval = NULL;
				double nval = 0.0;
				int dec = 0;
				if(json_find_string(jcode, "type", &type) != 0) {
					logprintf(LOG_ERR, "client did not send a type of action");
					char *z = "{\"message\":\"failed\",\"error\":\"client did not send a type of action\"}";
					mg_send_data(conn, z, strlen(z));
					goto clear2;
				} else {
					if(strcmp(type, "set") == 0) {
						if(json_find_string(jcode, "key", &key) != 0) {
							logprintf(LOG_ERR, "client did not send a registry key");
							char *z = "{\"message\":\"failed\",\"error\":\"client did not send a registry key\"}";
							mg_send_data(conn, z, strlen(z));
							goto clear2;
						} else if((value = json_find_member(jcode, "value")) == NULL) {
							logprintf(LOG_ERR, "client did not send a registry value");
							char *z = "{\"message\":\"failed\",\"error\":\"client did not send a registry value\"}";
							mg_send_data(conn, z, strlen(z));
							goto clear2;
						} else {
							if(value->tag == JSON_NUMBER) {
								if(registry_set_number(key, value->number_, value->decimals_) == 0) {
									char *z = "{\"message\":\"success\"}";
									mg_send_data(conn, z, strlen(z));
									goto clear2;
								} else {
									char *z = "{\"message\":\"failed\"}";
									mg_send_data(conn, z, strlen(z));
									goto clear2;
								}
							} else if(value->tag == JSON_STRING) {
								if(registry_set_string(key, value->string_) == 0) {
									char *z = "{\"message\":\"success\"}";
									mg_send_data(conn, z, strlen(z));
									goto clear2;
								} else {
									char *z = "{\"message\":\"failed\"}";
									mg_send_data(conn, z, strlen(z));
									goto clear2;
								}
							} else {
								logprintf(LOG_ERR, "registry value can only be a string or number");
								char *z = "{\"message\":\"failed\",\"error\":\"registry value can only be a string or number\"}";
								mg_send_data(conn, z, strlen(z));
								goto clear2;
							}
						}
					} else if(strcmp(type, "remove") == 0) {
						if(json_find_string(jcode, "key", &key) != 0) {
							logprintf(LOG_ERR, "client did not send a registry key");
							char *z = "{\"message\":\"failed\",\"error\":\"client did not send a registry key\"}";
							mg_send_data(conn, z, strlen(z));
							goto clear2;
						} else {
							if(registry_remove_value(key) == 0) {
								char *z = "{\"message\":\"success\"}";
								mg_send_data(conn, z, strlen(z));
								goto clear2;
							} else {
								char *z = "{\"message\":\"failed\"}";
								mg_send_data(conn, z, strlen(z));
								goto clear2;
							}
						}
					} else if(strcmp(type, "get") == 0) {
						if(json_find_string(jcode, "key", &key) != 0) {
							logprintf(LOG_ERR, "client did not send a registry key");
							char *z = "{\"message\":\"failed\",\"error\":\"client did not send a registry key\"}";
							mg_send_data(conn, z, strlen(z));
							goto clear2;
						} else {
							if(registry_get_number(key, &nval, &dec) == 0) {
								char *out = NULL;
								int len = snprintf(NULL, 0, "%*.f", dec, nval);
								out = MALLOC_OR_EXIT(len+1);
								snprintf(out, len, "%*.f", dec, nval);
								mg_send_data(conn, out, len);
								FREE(out);
								goto clear2;
							} else if(registry_get_string(key, &sval) == 0) {
								mg_send_data(conn, sval, strlen(sval));
								goto clear2;
							} else {
								logprintf(LOG_ERR, "registry key '%s' doesn't exists", key);
								char *z = "{\"message\":\"failed\",\"error\":\"registry key doesn't exists\"}";
								mg_send_data(conn, z, strlen(z));
								goto clear2;
							}
						}
					}
				}			
			}
			json_delete(jvalues);
			json_delete(jobject);
		}
		array_free(&array, a);
	}
	char *z = "{\"message\":\"failed\"}";
	mg_send_data(conn, z, strlen(z));
	return MG_TRUE;
	
clear1:
	array_free(&array1, c);

clear2:
	array_free(&array, a);
	json_delete(jvalues);
	json_delete(jobject);

	return MG_TRUE;
}

static int webserver_request_handler(struct mg_connection *conn) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	char *request = NULL;
	char *ext = NULL;
	char *mimetype = NULL;
	int size = 0;
	unsigned char buffer[4096];
	unsigned char *p;
	struct filehandler_t *filehandler = (struct filehandler_t *)conn->connection_param;
	unsigned int chunk = WEBSERVER_CHUNK_SIZE;
	struct stat st;

	if(!conn->is_websocket) {
		if(filehandler != NULL) {
			char buff[WEBSERVER_CHUNK_SIZE];
			if((filehandler->length-filehandler->ptr) < chunk) {
				chunk = (unsigned int)(filehandler->length-filehandler->ptr);
			}
			if(filehandler->fp != NULL) {
				chunk = (unsigned int)fread(buff, sizeof(char), WEBSERVER_CHUNK_SIZE-1, filehandler->fp);
				mg_send_data(conn, buff, (int)chunk);
			} else {
				mg_send_data(conn, &filehandler->bytes[filehandler->ptr], (int)chunk);
			}
			filehandler->ptr += chunk;

			if(filehandler->ptr == filehandler->length || conn->wsbits != 0) {
				if(filehandler->fp != NULL) {
					fclose(filehandler->fp);
					filehandler->fp = NULL;
				}
				if(filehandler->free) {
					FREE(filehandler->bytes);
				}
				FREE(filehandler);
				conn->connection_param = NULL;
				return MG_TRUE;
			} else {
				return MG_MORE;
			}
		} else if(conn->uri != NULL) {
			if(strcmp(conn->uri, "/send") == 0 || strcmp(conn->uri, "/control") == 0 || strcmp(conn->uri, "/registry") == 0) {
				return webserver_parse_rest(conn);
			} else if(strcmp(conn->uri, "/config") == 0) {
				char media[15];
				int internal = CONFIG_USER;
				strcpy(media, "web");
				if(conn->query_string != NULL) {
					sscanf(conn->query_string, "media=%14s%*[ \n\r]", media);
					if(strstr(conn->query_string, "internal") != NULL) {
						internal = CONFIG_INTERNAL;
					}
				}
				JsonNode *jsend = config_print(internal, media);
				if(jsend != NULL) {
					char *output = json_stringify(jsend, NULL);
					mg_send_data(conn, output, strlen(output));
					json_delete(jsend);
					json_free(output);
				}
				jsend = NULL;
				return MG_TRUE;
			} else if(strcmp(conn->uri, "/values") == 0) {
				char media[15];
				strcpy(media, "web");
				if(conn->query_string != NULL) {
					sscanf(conn->query_string, "media=%14s%*[ \n\r]", media);
				}
				JsonNode *jsend = devices_values(media);
				if(jsend != NULL) {
					char *output = json_stringify(jsend, NULL);
					mg_send_data(conn, output, strlen(output));
					json_delete(jsend);
					json_free(output);
				}
				jsend = NULL;
				return MG_TRUE;
			} else if(strcmp(&conn->uri[(rstrstr(conn->uri, "/")-conn->uri)], "/") == 0) {
				char indexes[255];
				strcpy(indexes, mg_get_option(mgserver[0], "index_files"));

				char **array = NULL;
				unsigned int n = explode((char *)indexes, ",", &array), q = 0;
				/* Check if the webserver_root is terminated by a slash. If not, than add it */
				for(q=0;q<n;q++) {
					size_t l = strlen(webserver_root)+strlen(conn->uri)+strlen(array[q])+4;
					request = REALLOC_OR_EXIT(request, l);
					memset(request, '\0', l);
					if(webserver_root[strlen(webserver_root)-1] == '/') {
#ifdef __FreeBSD__
						sprintf(request, "%s/%s%s", webserver_root, conn->uri, array[q]);
#else
						sprintf(request, "%s%s%s", webserver_root, conn->uri, array[q]);
#endif
					} else {
						sprintf(request, "%s/%s%s", webserver_root, conn->uri, array[q]);
					}
					if(access(request, F_OK) == 0) {
						break;
					}
				}
				array_free(&array, n);
			} else if(webserver_root != NULL && conn->uri != NULL) {
				size_t wlen = strlen(webserver_root)+strlen(conn->uri)+2;
				request = MALLOC_OR_EXIT(wlen);
				memset(request, '\0', wlen);
				/* If a file was requested add it to the webserver path to create the absolute path */
				if(webserver_root[strlen(webserver_root)-1] == '/') {
					if(conn->uri[0] == '/')
						sprintf(request, "%s%s", webserver_root, conn->uri);
					else
						sprintf(request, "%s/%s", webserver_root, conn->uri);
				} else {
					if(conn->uri[0] == '/')
						sprintf(request, "%s/%s", webserver_root, conn->uri);
					else
						sprintf(request, "%s/%s", webserver_root, conn->uri);
				}
			}
			if(request == NULL) {
				return MG_FALSE;
			}

			char *dot = NULL;
			/* Retrieve the extension of the requested file and create a mimetype accordingly */
			dot = strrchr(request, '.');
			if(!dot || dot == request) {
				mimetype = webserver_mimetype("text/plain");
			} else {
				ext = REALLOC_OR_EXIT(ext, strlen(dot)+1);
				strcpy(ext, dot+1);

				if(strcmp(ext, "html") == 0) {
					mimetype = webserver_mimetype("text/html");
				} else if(strcmp(ext, "xml") == 0) {
					mimetype = webserver_mimetype("text/xml");
				} else if(strcmp(ext, "png") == 0) {
					mimetype = webserver_mimetype("image/png");
				} else if(strcmp(ext, "gif") == 0) {
					mimetype = webserver_mimetype("image/gif");
				} else if(strcmp(ext, "ico") == 0) {
					mimetype = webserver_mimetype("image/x-icon");
				} else if(strcmp(ext, "jpg") == 0) {
					mimetype = webserver_mimetype("image/jpg");
				} else if(strcmp(ext, "css") == 0) {
					mimetype = webserver_mimetype("text/css");
				} else if(strcmp(ext, "js") == 0) {
					mimetype = webserver_mimetype("text/javascript");
				} else {
					mimetype = webserver_mimetype("text/plain");
				}
			}
			FREE(ext);

			memset(buffer, '\0', 4096);
			p = buffer;

			if(access(request, F_OK) == 0) {
				stat(request, &st);
				if(webserver_cache == 1 && st.st_size <= MAX_CACHE_FILESIZE &&
				  fcache_get_size(request, &size) != 0 && fcache_add(request) != 0) {
					FREE(mimetype);
					goto filenotfound;
				}
			} else {
				FREE(mimetype);
				goto filenotfound;
			}

			const char *cl = NULL;
			if((cl = mg_get_header(conn, "Content-Length"))) {
				if(atoi(cl) > MAX_UPLOAD_FILESIZE) {
					char line[1024] = {'\0'};
					FREE(mimetype);
					mimetype = webserver_mimetype("text/plain");
					sprintf(line, "Webserver Warning: POST Content-Length of %d bytes exceeds the limit of %d bytes in Unknown on line 0", MAX_UPLOAD_FILESIZE, atoi(cl));
					webserver_create_header(&p, "200 OK", mimetype, (unsigned int)strlen(line));
					mg_write(conn, buffer, (int)(p-buffer));
					mg_write(conn, line, (int)strlen(line));
					FREE(mimetype);
					FREE(request);
					return MG_TRUE;
				}
			}

			stat(request, &st);
			if(webserver_cache == 0 || st.st_size > MAX_CACHE_FILESIZE) {
				FILE *fp = fopen(request, "rb");
				fseek(fp, 0, SEEK_END);
				size = (int)ftell(fp);
				fseek(fp, 0, SEEK_SET);
				if(strstr(mimetype, "text") != NULL || st.st_size < WEBSERVER_CHUNK_SIZE) {
					webserver_create_header(&p, "200 OK", mimetype, (unsigned int)size);
					mg_write(conn, buffer, (int)(p-buffer));
					size_t total = 0;
					chunk = 0;
					unsigned char buff[1024];
					while(total < size) {
						chunk = (unsigned int)fread(buff, sizeof(char), 1024, fp);
						mg_write(conn, buff, (int)chunk);
						total += chunk;
					}
					fclose(fp);
				} else {
					if(filehandler == NULL) {
						filehandler = MALLOC_OR_EXIT(sizeof(struct filehandler_t));
						filehandler->bytes = NULL;
						filehandler->length = (unsigned int)size;
						filehandler->ptr = 0;
						filehandler->free = 0;
						filehandler->fp = fp;
						conn->connection_param = filehandler;
					}
					char buff[WEBSERVER_CHUNK_SIZE];
					if(filehandler != NULL) {
						if((filehandler->length-filehandler->ptr) < chunk) {
							chunk = (filehandler->length-filehandler->ptr);
						}
						chunk = (unsigned int)fread(buff, sizeof(char), WEBSERVER_CHUNK_SIZE, fp);
						mg_send_data(conn, buff, (int)chunk);
						filehandler->ptr += chunk;

						FREE(mimetype);
						FREE(request);
						if(filehandler->ptr == filehandler->length || conn->wsbits != 0) {
							if(filehandler->fp != NULL) {
								fclose(filehandler->fp);
								filehandler->fp = NULL;
							}
							FREE(filehandler);
							conn->connection_param = NULL;
							return MG_TRUE;
						} else {
							return MG_MORE;
						}
					}
				}

				FREE(mimetype);
				FREE(request);
				return MG_TRUE;
			} else {
				if(fcache_get_size(request, &size) == 0) {
					if(strstr(mimetype, "text") != NULL) {
						webserver_create_header(&p, "200 OK", mimetype, (unsigned int)size);
						mg_write(conn, buffer, (int)(p-buffer));
						mg_write(conn, fcache_get_bytes(request), size);
						FREE(mimetype);
						FREE(request);
						return MG_TRUE;
					} else {
						if(filehandler == NULL) {
							filehandler = MALLOC_OR_EXIT(sizeof(struct filehandler_t));
							filehandler->bytes = fcache_get_bytes(request);
							filehandler->length = (unsigned int)size;
							filehandler->ptr = 0;
							filehandler->free = 0;
							filehandler->fp = NULL;
							conn->connection_param = filehandler;
						}
						chunk = WEBSERVER_CHUNK_SIZE;
						if(filehandler != NULL) {
							if((filehandler->length-filehandler->ptr) < chunk) {
								chunk = (filehandler->length-filehandler->ptr);
							}
							mg_send_data(conn, &filehandler->bytes[filehandler->ptr], (int)chunk);
							filehandler->ptr += chunk;

							FREE(mimetype);
							FREE(request);
							if(filehandler->ptr == filehandler->length || conn->wsbits != 0) {
								FREE(filehandler);
								conn->connection_param = NULL;
								return MG_TRUE;
							} else {
								return MG_MORE;
							}
						}
					}
				}
			}
			FREE(mimetype);
			FREE(request);
		}
	} else if(webgui_websockets == 1) {
		char input[conn->content_len+1];
		strncpy(input, conn->content, conn->content_len);
		input[conn->content_len] = '\0';

		if(json_validate(input, NULL) == true) {
			JsonNode *json = json_decode(input);
			const char *action = NULL;
			if(json_find_string(json, "action", &action) == 0) {
				if(strcmp(action, "request config") == 0) {
					JsonNode *jsend = config_print(CONFIG_INTERNAL, "web");
					char *output = json_stringify(jsend, NULL);
					size_t output_len = strlen(output);
					mg_websocket_write(conn, 1, output, output_len);
					json_free(output);
					json_delete(jsend);
				} else if(strcmp(action, "request values") == 0) {
					JsonNode *jsend = devices_values("web");
					char *output = json_stringify(jsend, NULL);
					size_t output_len = strlen(output);
					mg_websocket_write(conn, 1, output, output_len);
					json_free(output);
					json_delete(jsend);
				} else if(strcmp(action, "control") == 0 || strcmp(action, "registry") == 0) {
					/* Write all codes coming from the webserver to the daemon */
					socket_write(sockfd, input);
				}
			}
			json_delete(json);
		}
		return MG_TRUE;
	}
	return MG_MORE;

filenotfound:
	logprintf(LOG_WARNING, "(webserver) could not read %s", request);
	webserver_create_404(conn->uri, &p);
	mg_write(conn, buffer, (int)(p-buffer));
	FREE(mimetype);
	FREE(request);
	return MG_TRUE;
}

static int webserver_connect_handler(struct mg_connection *conn) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	char ip[17];
	strcpy(ip, conn->remote_ip);
	if(whitelist_check(conn->remote_ip) != 0) {
		logprintf(LOG_NOTICE, "rejected client, ip: %s, port: %d", ip, conn->remote_port);
		return MG_FALSE;
	} else {
		logprintf(LOG_INFO, "client connected, ip %s, port %d", ip, conn->remote_port);
		return MG_TRUE;
	}
	return MG_FALSE;
}

static void *webserver_worker(void *param) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);
	while(webserver_loop) {
		mg_poll_server(mgserver[(intptr_t)param], 1000);
	}
	return NULL;
}

static void webserver_queue(char *message) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	pthread_mutex_lock(&webqueue_lock);
	if(webqueue_number <= 1024) {
		struct webqueue_t *wnode = MALLOC_OR_EXIT(sizeof(struct webqueue_t));
		wnode->message = STRDUP_OR_EXIT(message);

		if(webqueue_number == 0) {
			webqueue = wnode;
			webqueue_tail = wnode;
		} else {
			webqueue_tail->next = wnode;
			webqueue_tail = wnode;
		}

		webqueue_number++;
	} else {
		logprintf(LOG_ERR, "webserver queue full");
	}
	pthread_mutex_unlock(&webqueue_lock);
	pthread_cond_signal(&webqueue_signal);
}

void *webserver_broadcast(void *param) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	int i = 0;
	pthread_mutex_lock(&webqueue_lock);
	struct mg_connection *c = NULL;

	while(webserver_loop) {
		if(webqueue_number > 0) {
			pthread_mutex_lock(&webqueue_lock);

			logprintf(LOG_STACK, "%s::unlocked", __FUNCTION__);

#ifdef WEBSERVER_HTTPS
			for(i=0;i<WEBSERVER_WORKERS+1;i++) {
#else
			for(i=0;i<WEBSERVER_WORKERS;i++) {
#endif
				for(c=mg_next(mgserver[i], NULL); c != NULL; c = mg_next(mgserver[i], c)) {
					if(c->is_websocket && webserver_loop == 1) {
						mg_websocket_write(c, 1, webqueue->message, strlen(webqueue->message));
					}
				}
			}

			struct webqueue_t *tmp = webqueue;
			FREE(webqueue->message);
			webqueue = webqueue->next;
			FREE(tmp);
			webqueue_number--;
			pthread_mutex_unlock(&webqueue_lock);
		} else {
			pthread_cond_wait(&webqueue_signal, &webqueue_lock);
		}
	}
	return (void *)NULL;
}

void *webserver_clientize(void *param) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	unsigned int failures = 0;
	while(webserver_loop && failures <= 5) {
		struct ssdp_list_t *ssdp_list = NULL;
		int standalone = 0;
		settings_find_number("standalone", &standalone);
		if(ssdp_seek(&ssdp_list) == -1 || standalone == 1) {
			logprintf(LOG_NOTICE, "no pilight ssdp connections found");
			char server[16] = "127.0.0.1";
			if((sockfd = socket_connect(server, (unsigned short)socket_get_port())) == -1) {
				logprintf(LOG_ERR, "could not connect to pilight-daemon");
				failures++;
				continue;
			}
		} else {
			if((sockfd = socket_connect(ssdp_list->ip, ssdp_list->port)) == -1) {
				logprintf(LOG_ERR, "could not connect to pilight-daemon");
				failures++;
				continue;
			}
		}
		if(ssdp_list != NULL) {
			ssdp_free(ssdp_list);
		}

		struct JsonNode *jclient = json_mkobject();
		struct JsonNode *joptions = json_mkobject();
		json_append_member(jclient, "action", json_mkstring("identify"));
		json_append_member(joptions, "config", json_mknumber(1, 0));
		json_append_member(joptions, "core", json_mknumber(1, 0));
		json_append_member(jclient, "options", joptions);
		json_append_member(jclient, "media", json_mkstring("web"));
		char *out = json_stringify(jclient, NULL);
		socket_write(sockfd, out);
		json_free(out);
		json_delete(jclient);

		if(socket_read(sockfd, &recvBuff, 0) != 0
			 || strcmp(recvBuff, "{\"status\":\"success\"}") != 0) {
				failures++;
			continue;
		}
		failures = 0;
		while(webserver_loop) {
			if(webgui_websockets == 1) {
				if(socket_read(sockfd, &recvBuff, 0) != 0) {
					break;
				} else {
					char **array = NULL;
					unsigned int n = explode(recvBuff, "\n", &array), i = 0;
					for(i=0;i<n;i++) {
						webserver_queue(array[i]);
					}
					array_free(&array, n);
				}
			} else {
				sleep(1);
			}
		}
	}

	if(recvBuff != NULL) {
		FREE(recvBuff);
		recvBuff = NULL;
	}
	if(sockfd > 0) {
		socket_close(sockfd);
	}
	return 0;
}

static int webserver_handler(struct mg_connection *conn, enum mg_event ev) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	if(webserver_loop == 1) {
		if(ev == MG_REQUEST || (ev == MG_POLL && !conn->is_websocket)) {
			if(ev == MG_POLL ||
				(conn->is_websocket == 0 && webserver_connect_handler(conn) == MG_TRUE) ||
				conn->is_websocket == 1) {
				return webserver_request_handler(conn);
			} else {
				return MG_FALSE;
			}
		} else if(ev == MG_AUTH) {
			return webserver_auth_handler(conn);
		} else {
			return MG_FALSE;
		}
	} else {
		return MG_FALSE;
	}
}

int webserver_start(void) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	if(webqueue_init == 0) {
		pthread_mutexattr_init(&webqueue_attr);
		pthread_mutexattr_settype(&webqueue_attr, PTHREAD_MUTEX_RECURSIVE);
		pthread_mutex_init(&webqueue_lock, &webqueue_attr);
		pthread_cond_init(&webqueue_signal, NULL);
		webqueue_init = 1;
	}

	/* Check on what port the webserver needs to run */
	settings_find_number("webserver-http-port", &webserver_http_port);

#ifdef WEBSERVER_HTTPS
	settings_find_number("webserver-https-port", &webserver_https_port);
#endif

	if (settings_find_string("webserver-root", &webserver_root) != 0 || *webserver_root == 0) {
		/* If no webserver port was set or is empty, use the default webserver port */
		webserver_root = WEBSERVER_ROOT;
	}

	settings_find_number("webgui-websockets", &webgui_websockets);

	/* Do we turn on webserver caching. This means that all requested files are
	   loaded into the memory so they aren't read from the FS anymore */
	settings_find_number("webserver-cache", &webserver_cache);
	settings_find_string("webserver-authentication-password", &webserver_authentication_password);
	settings_find_string("webserver-authentication-username", &webserver_authentication_username);

	int z = 0;
#ifdef WEBSERVER_HTTPS
	char *pemfile = NULL;
	if(settings_find_string("pem-file", &pemfile) != 0) {
		pemfile = STRDUP_OR_EXIT(PEM_FILE)
	} else {
		pemfile = STRDUP_OR_EXIT(pem_file);
	}

	char ssl[BUFFER_SIZE];
	char id[64];
	memset(ssl, '\0', sizeof(ssl));

	sprintf(id, "%d", z);
	snprintf(ssl, BUFFER_SIZE, "ssl://%d:%s", webserver_https_port, pemfile);
	mgserver[z] = mg_create_server((void *)id, webserver_handler);
	mg_set_option(mgserver[z], "listening_port", ssl);
	mg_set_option(mgserver[z], "auth_domain", "pilight");
	char msg[25];
	sprintf(msg, "webserver worker #%d", z);
	threads_register(msg, &webserver_worker, (void *)(intptr_t)z, 0);
	z = 1;
	FREE(pemfile);
#endif

	char webport[10] = {'\0'};
	sprintf(webport, "%d", webserver_http_port);

	int i = 0;
	for(i=z;i<WEBSERVER_WORKERS+z;i++) {
		char id[64];
		sprintf(id, "%d", i);
		mgserver[i] = mg_create_server((void *)id, webserver_handler);
		mg_set_option(mgserver[i], "listening_port", webport);
		mg_set_option(mgserver[i], "auth_domain", "pilight");
		char msg[64];
		sprintf(msg, "webserver worker #%d", i);
		threads_register(msg, &webserver_worker, (void *)(intptr_t)i, 0);
	}

#ifdef WEBSERVER_HTTPS	
	logprintf(LOG_DEBUG, "webserver listening to port %d", webserver_https_port);
#endif
	logprintf(LOG_DEBUG, "webserver listening to port %s", webport);

	return 0;
}
