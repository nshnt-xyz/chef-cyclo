/*
 * gps-manager's command-line client: status, watch, retry, enable,
 * disable and hold KIND... on the lease socket (server.c's header has the
 * protocol).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "gps-manager.h"

static int sock_connect(const char *path)
{
	struct sockaddr_un sa;
	int fd;

	if (strlen(path) >= sizeof(sa.sun_path))
		return -ENAMETOOLONG;
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -errno;
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	strcpy(sa.sun_path, path);
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		int err = errno;

		close(fd);
		return -err;
	}
	return fd;
}

void gm_usage(void)
{
	fprintf(stderr,
		"usage: gps-manager [-S SOCK] [-d DIR] [-q QMUX_SOCK] [-o OWNER_LOCK] [-b BINDIR] [-s STATE_CMD]\n"
		"                   [-g GRACE_S] [-v]\n"
		"       gps-manager [-S SOCK] status | watch | retry | enable | disable | hold KIND...\n"
		"                                                                    (KIND: map, ride)\n");
}

int gm_cli_main(const char *sock_path, int argc, char **argv)
{
	int fd;
	char buf[512];
	bool once = strcmp(argv[0], "status") == 0 || strcmp(argv[0], "retry") == 0 ||
		    strcmp(argv[0], "enable") == 0 || strcmp(argv[0], "disable") == 0;
	int i;

	if (!once && strcmp(argv[0], "watch") != 0 && strcmp(argv[0], "hold") != 0) {
		gm_usage();
		return 64;
	}
	if (strcmp(argv[0], "hold") == 0 && argc < 2) {
		gm_usage();
		return 64;
	}
	fd = sock_connect(sock_path);
	if (fd < 0) {
		fprintf(stderr, "gps-manager: connect %s: %s\n", sock_path, strerror(-fd));
		return 1;
	}
	if (strcmp(argv[0], "hold") == 0) {
		for (i = 1; i < argc; i++) {
			snprintf(buf, sizeof(buf), "lease %s\n", argv[i]);
			if (write(fd, buf, strlen(buf)) < 0)
				return 1;
		}
		if (write(fd, "watch\n", 6) < 0)
			return 1;
	} else {
		snprintf(buf, sizeof(buf), "%s\n", argv[0]);
		if (write(fd, buf, strlen(buf)) < 0)
			return 1;
	}
	for (;;) {
		ssize_t n = read(fd, buf, sizeof(buf));

		if (n <= 0)
			break;
		if (fwrite(buf, 1, (size_t)n, stdout) != (size_t)n)
			break;
		fflush(stdout);
		if (once && memchr(buf, '\n', (size_t)n)) {
			close(fd);
			return strncmp(buf, "ok", 2) == 0 ? 0 : 1;
		}
	}
	close(fd);
	return 0;
}
