/*
 * gps-manager's command-line client on the lease socket (server.c's
 * header has the protocol). It never prints the hello:
 *
 *   status, retry, enable, disable   print the one reply line; exit 0 on
 *                                    a non-error reply, 1 on an error reply
 *                                    or no connection
 *   watch, hold KIND...              print every later line raw (one JSON
 *                                    object per line) until EOF or ^C;
 *                                    hold sends its leases, then watch
 *   usage errors                     exit 64
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common/json.h"
#include "common/linesrv.h"
#include "gps-manager.h"

void gm_usage(void)
{
	fprintf(stderr,
		"usage: gps-manager [-S SOCK] [-d DIR] [-q QMUX_SOCK] [-o OWNER_LOCK] [-b BINDIR] [-s STATE_CMD]\n"
		"                   [-g GRACE_S] [-v]\n"
		"       gps-manager [-S SOCK] status | watch | retry | enable | disable | hold KIND...\n"
		"                                                                    (KIND: map, ride)\n");
}

/* The "type" of one daemon line, or "" when it has none. */
static const char *line_type(const char *line, size_t len, char *buf, size_t buflen)
{
	struct json_obj o;

	if (json_parse(&o, line, len) || !json_get_string(&o, "type", buf, buflen))
		buf[0] = '\0';
	return buf;
}

/* MSG_NOSIGNAL: a connection the manager refused (too many clients) is an
 * error message and exit 1, not death by SIGPIPE. */
static int send_request(int fd, const char *sock_path, const char *line)
{
	if (send(fd, line, strlen(line), MSG_NOSIGNAL) < 0) {
		fprintf(stderr, "gps-manager: send to %s: %s\n", sock_path, strerror(errno));
		return -1;
	}
	return 0;
}

int gm_cli_main(const char *sock_path, int argc, char **argv)
{
	bool once = strcmp(argv[0], "status") == 0 || strcmp(argv[0], "retry") == 0 ||
		    strcmp(argv[0], "enable") == 0 || strcmp(argv[0], "disable") == 0;
	bool hold = strcmp(argv[0], "hold") == 0;
	char buf[4096], type[16];
	size_t len = 0;
	int fd, i;

	if (!once && !hold && strcmp(argv[0], "watch") != 0) {
		gm_usage();
		return 64;
	}
	if (hold && argc < 2) {
		gm_usage();
		return 64;
	}
	fd = linesrv_connect(sock_path);
	if (fd < 0) {
		fprintf(stderr, "gps-manager: connect %s: %s\n", sock_path, strerror(-fd));
		return 1;
	}
	/* hold: its leases, then watch; otherwise the one command */
	for (i = hold ? 1 : 0; i < (hold ? argc : 1); i++) {
		snprintf(buf, sizeof(buf), hold ? "lease %s\n" : "%s\n", argv[i]);
		if (send_request(fd, sock_path, buf)) {
			close(fd);
			return 1;
		}
	}
	if (hold && send_request(fd, sock_path, "watch\n")) {
		close(fd);
		return 1;
	}
	for (;;) {
		ssize_t n = read(fd, buf + len, sizeof(buf) - len);
		char *nl;

		if (n <= 0)
			break;
		len += (size_t)n;
		while ((nl = memchr(buf, '\n', len))) {
			size_t used = (size_t)(nl - buf) + 1;

			line_type(buf, used - 1, type, sizeof(type));
			if (strcmp(type, "hello") != 0) {
				if (fwrite(buf, 1, used, stdout) != used)
					goto out;
				fflush(stdout);
				if (once) {
					close(fd);
					return strcmp(type, "error") == 0 ? 1 : 0;
				}
			}
			memmove(buf, buf + used, len - used);
			len -= used;
		}
		if (len == sizeof(buf)) {	/* no line is this long: pass it on */
			if (!once)
				fwrite(buf, 1, len, stdout);
			len = 0;
		}
	}
out:
	close(fd);
	return once ? 1 : 0;	/* closed before the reply: no answer */
}
