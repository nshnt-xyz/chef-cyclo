/*
 * sensord's command-line client (sensord list|status|get|watch|
 * declination), for the telnet and ssh shells: see main.c's usage.
 */
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "sensord.h"

/* Client mode: one request, print the reply lines (the hello is not
 * printed). list/status/get stop after the first reply; watch streams
 * sample lines until ^C, or COUNT of them with -n. */
int sn_cli_main(const char *sock_path, long count, int argc, char **argv)
{
	const char *cmd = argv[0];
	bool watch = strcmp(cmd, "watch") == 0;
	char req[128], buf[4096];
	size_t have = 0;
	long samples = 0;
	int fd, rc = 0;

	if (watch || strcmp(cmd, "get") == 0) {
		if (argc < 2 || argc > 3) {
			sn_usage();
			return 64;
		}
		snprintf(req, sizeof(req), "%s %s%s%s\n", watch ? "claim" : "get", argv[1],
			 argc == 3 ? " " : "", argc == 3 ? argv[2] : "");
	} else if ((strcmp(cmd, "list") == 0 || strcmp(cmd, "status") == 0) && argc == 1) {
		snprintf(req, sizeof(req), "%s\n", cmd);
	} else if (strcmp(cmd, "declination") == 0 && argc <= 2) {
		snprintf(req, sizeof(req), "declination%s%.32s\n", argc == 2 ? " " : "",
			 argc == 2 ? argv[1] : "");
	} else {
		sn_usage();
		return 64;
	}
	fd = sn_sock_connect(sock_path);
	if (fd < 0) {
		fprintf(stderr, "sensord: connect %s: %s\n", sock_path, strerror(-fd));
		return 1;
	}
	if (write(fd, req, strlen(req)) < 0) {
		close(fd);
		return 1;
	}
	for (;;) {
		ssize_t n;
		char *line, *nl;

		if (have >= sizeof(buf) - 1)
			have = 0;	/* no line is this long; resync */
		n = read(fd, buf + have, sizeof(buf) - 1 - have);

		if (n <= 0) {
			if (!watch)
				rc = 1;
			break;
		}
		have += (size_t)n;
		buf[have] = '\0';
		line = buf;
		while ((nl = strchr(line, '\n'))) {
			*nl = '\0';
			if (strncmp(line, "{\"hello\"", 8) != 0 &&
			    !(watch && strncmp(line, "{\"ok\"", 5) == 0)) {
				puts(line);
				fflush(stdout);
				if (strncmp(line, "{\"err\"", 6) == 0)
					rc = 1;
				if (!watch || rc)
					goto out;
				if (strncmp(line, "{\"sensor\"", 9) == 0 && count > 0 &&
				    ++samples >= count)
					goto out;
			}
			line = nl + 1;
		}
		have = strlen(line);
		memmove(buf, line, have);
	}
out:
	close(fd);
	return rc;
}
