/*
 * chef_log.c - see chef_log.h. The kmsg write is the one every daemon
 * carried as kmsg_note() before it moved onto tools/common/.
 */
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>

#include "chef_log.h"

#define CHEF_LOG_KMSG "/dev/kmsg"
#define CHEF_LOG_MSG_MAX 321	/* a message is cut at 320 bytes */

static const char *log_tag = "chef";
static bool log_echo;
static const char *log_kmsg = CHEF_LOG_KMSG;

void chef_log_init(const char *tag, bool echo_stderr)
{
	log_tag = tag;
	log_echo = echo_stderr;
}

void chef_log_set_kmsg(const char *path)
{
	log_kmsg = path ? path : CHEF_LOG_KMSG;
}

void chef_log(const char *fmt, ...)
{
	char buf[CHEF_LOG_MSG_MAX];
	va_list ap;
	int fd, n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if (log_echo)
		fprintf(stderr, "%s: %s\n", log_tag, buf);
	fd = open(log_kmsg, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return;
	dprintf(fd, "%s: %s\n", log_tag, buf);
	close(fd);
}
