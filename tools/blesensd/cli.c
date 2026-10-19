/*
 * blesensd's command-line client (blesensd status|list|watch|scan|pair|
 * cancel|forget), for the telnet and ssh shells: see main.c's usage.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "blesensd.h"

/* ------------------------------------------------------------ client CLI */

/* Does this JSON line have "key":"value"? */
static bool has_kv(const char *line, const char *key, const char *value)
{
	char pat[96];

	snprintf(pat, sizeof(pat), "\"%s\":\"%s\"", key, value);
	return strstr(line, pat) != NULL;
}

/* Is this line the end of the request? */
static bool client_done(const char *cmd, const char *arg, const char *line)
{
	if (!strcmp(cmd, "status"))
		return has_kv(line, "type", "status");
	if (has_kv(line, "type", "error") && has_kv(line, "cmd", cmd))
		return true;
	if (!strcmp(cmd, "pair"))
		return has_kv(line, "type", "pair") && has_kv(line, "addr", arg) &&
		       (has_kv(line, "state", "done") || has_kv(line, "state", "failed"));
	if (!strcmp(cmd, "forget"))
		return has_kv(line, "type", "forgotten") && has_kv(line, "addr", arg);
	if (!strcmp(cmd, "list") || !strcmp(cmd, "cancel"))
		return has_kv(line, "type", "ok") && has_kv(line, "cmd", cmd);
	return false;	/* watch, scan: until interrupted */
}

int bd_cli_main(const char *sock, int argc, char **argv)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	const char *cmd = argv[0], *arg = argc > 1 ? argv[1] : "";
	char req[128], buf[4096];
	size_t len = 0;
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	bool known = !strcmp(cmd, "status") || !strcmp(cmd, "list") || !strcmp(cmd, "watch") ||
		     !strcmp(cmd, "scan") || ((!strcmp(cmd, "pair") || !strcmp(cmd, "cancel") ||
					       !strcmp(cmd, "forget")) && bd_addr_ok(arg));

	if (!known) {
		fprintf(stderr, "blesensd: unknown request (status|list|watch|scan|pair ADDR|cancel ADDR|forget ADDR)\n");
		return 64;
	}
	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", sock);
	if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		fprintf(stderr, "blesensd: connect %s: %s\n", sock, strerror(errno));
		return 1;
	}
	if (!strcmp(cmd, "scan"))
		snprintf(req, sizeof(req), "watch\nscan on\n");
	else if (!strcmp(cmd, "pair") || !strcmp(cmd, "forget"))
		snprintf(req, sizeof(req), "watch\n%s %s\n", cmd, arg);	/* the outcome is an event */
	else
		snprintf(req, sizeof(req), "%s%s%s\n", cmd, *arg ? " " : "", arg);
	if (write(fd, req, strlen(req)) != (ssize_t)strlen(req))
		return 1;
	for (;;) {
		ssize_t n = read(fd, buf + len, sizeof(buf) - 1 - len);
		char *nl;

		if (n <= 0)
			break;
		len += (size_t)n;
		buf[len] = 0;
		while ((nl = strchr(buf, '\n'))) {
			bool done, failed;

			*nl = 0;
			printf("%s\n", buf);
			done = client_done(cmd, arg, buf);
			failed = has_kv(buf, "type", "error") || has_kv(buf, "state", "failed");
			len -= (size_t)(nl + 1 - buf);
			memmove(buf, nl + 1, len + 1);
			if (done) {
				fflush(stdout);
				close(fd);
				return failed ? 1 : 0;
			}
		}
		fflush(stdout);
		if (len == sizeof(buf) - 1)
			len = 0;	/* no line is this long */
	}
	close(fd);
	return 1;
}
