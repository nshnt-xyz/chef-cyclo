/*
 * chef_log.c - see chef_log.h. The kmsg write is the one every daemon
 * carried as kmsg_note() before it moved onto tools/common/; it is the
 * fallback now, behind the syslog socket.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "chef_log.h"

#define CHEF_LOG_SOCKET "/dev/log"
#define CHEF_LOG_KMSG "/dev/kmsg"
#define CHEF_LOG_MSG_MAX 321	/* a message is cut at 320 bytes */
#define CHEF_LOG_PRI 30		/* LOG_DAEMON | LOG_INFO */

static const char *log_tag = "chef";
static bool log_echo;
static const char *log_kmsg = CHEF_LOG_KMSG;
static const char *log_socket_set;	/* chef_log_set_socket() */
static const char *log_socket_env;	/* CHEF_LOG_SOCKET */
static bool log_env_read;
static int log_fd = -1;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

static void log_close(void)
{
	if (log_fd >= 0)
		close(log_fd);
	log_fd = -1;
}

void chef_log_init(const char *tag, bool echo_stderr)
{
	pthread_mutex_lock(&log_lock);
	log_tag = tag;
	log_echo = echo_stderr;
	log_socket_env = getenv("CHEF_LOG_SOCKET");
	log_env_read = true;
	log_close();
	pthread_mutex_unlock(&log_lock);
}

void chef_log_set_socket(const char *path)
{
	pthread_mutex_lock(&log_lock);
	log_socket_set = path;
	log_close();
	pthread_mutex_unlock(&log_lock);
}

void chef_log_set_kmsg(const char *path)
{
	pthread_mutex_lock(&log_lock);
	log_kmsg = path ? path : CHEF_LOG_KMSG;
	pthread_mutex_unlock(&log_lock);
}

static const char *socket_path(void)
{
	if (log_socket_set)
		return log_socket_set;
	if (!log_env_read) {	/* a caller that never ran chef_log_init() */
		log_socket_env = getenv("CHEF_LOG_SOCKET");
		log_env_read = true;
	}
	return log_socket_env && *log_socket_env ? log_socket_env : CHEF_LOG_SOCKET;
}

static int log_connect(void)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	const char *path = socket_path();

	log_close();
	if (strlen(path) >= sizeof(sa.sun_path))
		return -1;
	strcpy(sa.sun_path, path);
	log_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (log_fd < 0)
		return -1;
	if (connect(log_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		log_close();
		return -1;
	}
	return 0;
}

/* syslogd gone, restarted (a new socket at the path) or not up yet */
static bool lost_connection(int e)
{
	return e == ECONNREFUSED || e == ENOTCONN || e == ENOENT || e == ECONNRESET || e == EPIPE;
}

/* 0 once syslogd has the datagram; -1 means "use kmsg". */
static int log_send(const char *dgram, size_t len)
{
	if (log_fd < 0 && log_connect() < 0)
		return -1;
	if (send(log_fd, dgram, len, MSG_DONTWAIT | MSG_NOSIGNAL) >= 0)
		return 0;
	if (!lost_connection(errno))
		return -1;	/* EAGAIN (queue full) and the rest */
	if (log_connect() < 0)
		return -1;
	return send(log_fd, dgram, len, MSG_DONTWAIT | MSG_NOSIGNAL) >= 0 ? 0 : -1;
}

void chef_log(const char *fmt, ...)
{
	char buf[CHEF_LOG_MSG_MAX], dgram[CHEF_LOG_MSG_MAX + 96];
	va_list ap;
	int fd, n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	pthread_mutex_lock(&log_lock);
	if (log_echo)
		fprintf(stderr, "%s: %s\n", log_tag, buf);
	n = snprintf(dgram, sizeof(dgram), "<%d>%.48s[%ld]: %s", CHEF_LOG_PRI, log_tag, (long)getpid(), buf);
	if (n < 0 || log_send(dgram, (size_t)n < sizeof(dgram) ? (size_t)n : sizeof(dgram) - 1) < 0) {
		fd = open(log_kmsg, O_WRONLY | O_APPEND | O_CLOEXEC);
		if (fd >= 0) {
			dprintf(fd, "%s: %s\n", log_tag, buf);
			close(fd);
		}
	}
	pthread_mutex_unlock(&log_lock);
}
