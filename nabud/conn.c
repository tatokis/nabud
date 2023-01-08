/*-
 * Copyright (c) 2022, 2023 Jason R. Thorpe.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Connection abstraction.
 *
 * Connections can be either over a serial interface to a real NABU,
 * but eventually also be over a socket to support NABU emulators.
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>

#include "adaptor.h"
#include "conn.h"
#include "image.h"
#include "log.h"
#include "retronet.h"

static pthread_mutex_t conn_list_mutex = PTHREAD_MUTEX_INITIALIZER;
static LIST_HEAD(, nabu_connection) conn_list =
    LIST_HEAD_INITIALIZER(conn_list);
unsigned int conn_count;

static void
conn_insert(struct nabu_connection *conn)
{
	assert(! conn->on_list);

	pthread_mutex_lock(&conn_list_mutex);
	LIST_INSERT_HEAD(&conn_list, conn, link);
	conn->on_list = true;
	conn_count++;
	pthread_mutex_unlock(&conn_list_mutex);
}

static void
conn_remove(struct nabu_connection *conn)
{
	if (conn->on_list) {
		pthread_mutex_lock(&conn_list_mutex);
		LIST_REMOVE(conn, link);
		conn->on_list = false;
		conn_count--;
		pthread_mutex_unlock(&conn_list_mutex);
	}
}

/*
 * conn_thread --
 *	Worker thread that handles NABU connections.
 */
static void *
conn_thread(void *arg)
{
	struct nabu_connection *conn = arg;

	/* Just run the Adaptor event loop until it returns. */
	adaptor_event_loop(conn);

	/*
	 * If we got there, the connection was cancelled or aborted,
	 * so so ahead and destroy it now.
	 */
	conn_destroy(conn);

	return NULL;
}

/*
 * conn_create_common --
 *	Common connection-creation duties.
 */
static void
conn_create_common(char *name, int fd, unsigned int channel, conn_type type,
    void *(*func)(void *))
{
	struct nabu_connection *conn;

	conn = calloc(1, sizeof(*conn));
	if (conn == NULL) {
		log_error("[%s] Unable to allocate connection structure.",
		    name);
		close(fd);
		return;
	}

	conn->type = type;
	pthread_mutex_init(&conn->mutex, NULL);

	if (! conn_io_init(&conn->io, name, fd, func)) {
		/* Error already logged. */
		goto bad;
	}

	/*
	 * If a channel was specified, set it now.
	 */
	if (channel != 0) {
		image_channel_select(conn, (int16_t)channel);
	}

	if (! conn_io_start(&conn->io)) {
		/* Error already logged. */
		goto bad;
	}

	conn_insert(conn);
	return;

 bad:
	conn_destroy(conn);
	return;
}

#define	NABU_NATIVE_BPS		111000
#define	NABU_FALLBACK_BPS	115200

/*
 * conn_add_serial --
 *	Add a serial connection.
 */
void
conn_add_serial(char *path, unsigned int channel)
{
	struct termios t;
	int fd;

	log_info("Creating Serial connection on %s.", path);

	fd = open(path, O_RDWR | O_NONBLOCK | O_NOCTTY);
	if (fd < 0) {
		log_error("Unable to open %s: %s", path, strerror(errno));
		return;
	}

	if (tcgetattr(fd, &t) < 0) {
		log_error("tcgetattr() failed on %s: %s", path,
		    strerror(errno));
		goto bad;
	}

	/*
	 * The native protocol is 8N1 @ 111000 baud, but it's much
	 * more reliable if we use 2 stop bits.  Otherwise, the NABU
	 * can get out of sync when receiving a stream of bytes in
	 * a packet.
	 */
	cfmakeraw(&t);
	t.c_cflag &= ~(CSIZE | PARENB | PARODD);
	t.c_cflag |= CLOCAL | CS8 | CSTOPB;
	if (cfsetspeed(&t, NABU_NATIVE_BPS) < 0) {
		log_error("cfsetspeed(NABU_NATIVE_BPS) on %s failed.",
		    path);
		goto bad;
	}

	if (tcsetattr(fd, TCSANOW, &t) < 0) {
		/*
		 * If we failed to set the native NABU baud rate
		 * (it's a little of an odd-ball after all), then
		 * try the fall back.  But add an extra stop bit
		 * so that the NABU's UART has a better chance of
		 * re-synchronizing with the next start bit.
		 */
		log_info("Failed to 8N2-%d on %s; falling back to 8N2-%d.",
		    NABU_NATIVE_BPS, path, NABU_FALLBACK_BPS);
		if (cfsetspeed(&t, NABU_FALLBACK_BPS)) {
			log_error("cfsetspeed(NABU_FALLBACK_BPS) on %s failed.",
			    path);
			goto bad;
		}
		if (tcsetattr(fd, TCSANOW, &t) < 0) {
			log_error("Failed to set 8N2-%d on %s.",
			    NABU_FALLBACK_BPS, path);
			goto bad;
		}
	}

	conn_create_common(path, fd, channel, CONN_TYPE_SERIAL, conn_thread);
 	return;
 bad:
	close(fd);
}

/*
 * conn_tcp_thread --
 *	Worker thread that handles accepting TCP connections from
 *	NABU emulators (like MAME).
 */
static void *
conn_tcp_thread(void *arg)
{
	struct nabu_connection *conn = arg;
	struct image_channel *chan;
	char host[NI_MAXHOST];
	struct sockaddr_storage peerss;
	socklen_t peersslen;
	int sock, v;

	for (;;) {
		peersslen = sizeof(peerss);
		if (! conn_io_accept(&conn->io, (struct sockaddr *)&peerss,
				     &peersslen, &sock)) {
			/* Error already logged. */
			break;
		}

		/* Disable Nagle. */
		v = 1;
		setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &v, sizeof(v));

		/* Get the numeric peer name string. */
		v = getnameinfo((struct sockaddr *)&peerss,
		    peersslen, host, sizeof(host), NULL, 0,
		    NI_NUMERICHOST);
		if (v) {
			log_error("[%s] getnameinfo() failed: %s",
			    conn_name(conn), gai_strerror(v));
			close(sock);
			continue;
		}

		log_info("[%s] Creating TCP connection for %s.",
		    conn_name(conn), host);

		pthread_mutex_lock(&conn->mutex);
		chan = conn->l_channel;
		pthread_mutex_unlock(&conn->mutex);

		conn_create_common(strdup(host), sock,
		    chan != NULL ? chan->number : 0, CONN_TYPE_TCP,
		    conn_thread);
	}

	/* Error on the listen socket -- He's dead, Jim. */
	conn_destroy(conn);

	return NULL;
}

/*
 * conn_add_tcp --
 *	Add a TCP listener.  This creates a "connection" that simply
 *	listens for incoming connections from the network and in-turn
 *	creates new connections to service them.
 */
void
conn_add_tcp(char *portstr, unsigned int channel)
{
	int sock;
	long port;
	char name[sizeof("IPv4-65536")];

	log_info("Creating TCP listener on port %s.", portstr);

	port = strtol(portstr, NULL, 10);
	if (port < 1 || port > UINT16_MAX) {
		log_error("Invalid TCP port number: %s", portstr);
		return;
	}

	struct sockaddr_in sin = {
		.sin_len = sizeof(sin),
		.sin_family = AF_INET,
		.sin_port = htons((in_port_t)port),
		.sin_addr = { .s_addr = htonl(INADDR_ANY) },
	};

	snprintf(name, sizeof(name), "IPv4-%ld", port);
	sock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock >= 0) {
		if (bind(sock, (struct sockaddr *)&sin, sizeof(sin)) == 0) {
			if (listen(sock, 8) == 0) {
				conn_create_common(strdup(name), sock, channel,
				    CONN_TYPE_LISTENER, conn_tcp_thread);
				sock = -1;
			} else {
				log_error("Unable to listen on IPv4 socket: %s",
				    strerror(errno));
			}
		} else {
			log_error("Unable to bind IPv4 socket: %s",
			    strerror(errno));
		}
	} else {
		log_error("Unable to create IPv4 socket: %s",
		    strerror(errno));
	}
	if (sock >= 0) {
		close(sock);
	}

#ifdef PF_INET6
	struct sockaddr_in6 sin6 = {
		.sin6_len = sizeof(sin6),
		.sin6_family = AF_INET6,
		.sin6_port = htons((in_port_t)port),
		.sin6_addr = IN6ADDR_ANY_INIT,
	};

	snprintf(name, sizeof(name), "IPv6-%ld", port);
	sock = socket(PF_INET6, SOCK_STREAM, IPPROTO_TCP);
	if (sock >= 0) {
		if (bind(sock, (struct sockaddr *)&sin6, sizeof(sin6)) == 0) {
			if (listen(sock, 8) == 0) {
				conn_create_common(strdup(name), sock, channel,
				    CONN_TYPE_LISTENER, conn_tcp_thread);
				sock = -1;
			} else {
				log_error("Unable to listen on IPv6 socket: %s",
				    strerror(errno));
			}
		} else {
			log_error("Unable to bind IPv6 socket: %s",
			    strerror(errno));
		}
	} else {
		log_error("Unable to create IPv6 socket: %s",
		    strerror(errno));
	}
	if (sock >= 0) {
		close(sock);
	}
#endif /* PF_INET6 */
}

/*
 * conn_destroy --
 *	Destroy a connection structure.
 */
void
conn_destroy(struct nabu_connection *conn)
{
	conn_remove(conn);

	image_release(conn_set_last_image(conn, NULL));
	rn_file_closeall(conn);

	pthread_mutex_destroy(&conn->mutex);

	conn_io_fini(&conn->io);

	free(conn);
}

/*
 * conn_get_last_image --
 *	Return the last image used by the connection.
 */
struct nabu_image *
conn_get_last_image(struct nabu_connection *conn)
{
	struct nabu_image *img;

	pthread_mutex_lock(&conn->mutex);
	img = conn->l_last_image;
	pthread_mutex_unlock(&conn->mutex);

	return img;
}

/*
 * conn_set_last_image --
 *	Set the specified image as the most-recent.  Returns
 *	the old value.
 */
struct nabu_image *
conn_set_last_image(struct nabu_connection *conn, struct nabu_image *img)
{
	struct nabu_image *oimg;

	pthread_mutex_lock(&conn->mutex);
	oimg = conn->l_last_image;
	conn->l_last_image = img;
	pthread_mutex_unlock(&conn->mutex);

	return oimg;
}

/*
 * conn_set_last_image_if --
 *	Like conn_set_last_image(), but only if the last image
 *	matches the specified match value.
 */
struct nabu_image *
conn_set_last_image_if(struct nabu_connection *conn, struct nabu_image *match,
    struct nabu_image *img)
{
	struct nabu_image *oimg;

	pthread_mutex_lock(&conn->mutex);
	if (conn->l_last_image == match) {
		oimg = conn->l_last_image;
		conn->l_last_image = img;
	} else {
		oimg = NULL;
	}
	pthread_mutex_unlock(&conn->mutex);

	return oimg;
}

/*
 * conn_get_channel --
 *	Return the connection's currently-selected channel.
 */
struct image_channel *
conn_get_channel(struct nabu_connection *conn)
{
	struct image_channel *chan;

	pthread_mutex_lock(&conn->mutex);
	chan = conn->l_channel;
	pthread_mutex_unlock(&conn->mutex);

	return chan;
}

/*
 * conn_set_channel --
 *	Set the specified channel as the connection's selected channel.
 */
void
conn_set_channel(struct nabu_connection *conn, struct image_channel *chan)
{
	pthread_mutex_lock(&conn->mutex);
	conn->l_channel = chan;
	pthread_mutex_unlock(&conn->mutex);
}

/*
 * conn_get_selected_file --
 *	Return the selected file on this connection, or NULL if
 *	no file is selected.  Caller must free the returned string.
 */
static const char *
conn_get_selected_file_logic(struct nabu_connection *conn)
{
	if (conn->l_selected_file != NULL) {
		return conn->l_selected_file;
	}
	if (conn->l_channel != NULL) {
		return conn->l_channel->default_file;
	}
	return NULL;
}

char *
conn_get_selected_file(struct nabu_connection *conn)
{
	size_t len;
	const char *sel;
	char *cp;

	for (cp = NULL, len = 0;;) {
		/*
		 * Avoid allocating memory while holding the
		 * channel lock.  First, figure out what selection
		 * we're going to use and drop the lock.  Then,
		 * allocate space for the name, re-acquire the
		 * lock, and determine the selection again.  If
		 * the selection will still fit, copy the string
		 * and return.  Otherwise, try again.
		 */

		pthread_mutex_lock(&conn->mutex);
		sel = conn_get_selected_file_logic(conn);
		if (sel != NULL) {
			len = strlen(sel);
		}
		pthread_mutex_unlock(&conn->mutex);
		if (sel == NULL) {
			return NULL;
		}

		cp = malloc(len + 1);
		if (cp == NULL) {
			return NULL;
		}

		pthread_mutex_lock(&conn->mutex);
		sel = conn_get_selected_file_logic(conn);
		if (sel != NULL && strlen(sel) <= len) {
			strcpy(cp, sel);
		} else {
			sel = NULL;
		}
		pthread_mutex_unlock(&conn->mutex);
		if (sel != NULL) {
			return cp;
		}
		free(cp);
	}
}

/*
 * conn_set_selected_file --
 *	Set the selected file for the connection.
 */
void
conn_set_selected_file(struct nabu_connection *conn, const char *name)
{
	char *cp = name == NULL ? NULL : strdup(name);
	char *oname;

	pthread_mutex_lock(&conn->mutex);
	oname = conn->l_selected_file;
	conn->l_selected_file = cp;
	pthread_mutex_unlock(&conn->mutex);

	if (oname != NULL) {
		free(oname);
	}
}
