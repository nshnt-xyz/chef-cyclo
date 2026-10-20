/*
 * blesensd's command-line client (blesensd status|list|watch|scan|pair|
 * cancel|forget), for the telnet and ssh shells: see main.c's usage.
 */
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "blesensd.h"
#include "common/json.h"

/* ------------------------------------------------------------ client CLI */

/* Does this line have "key":"value"? */
static bool has(const struct json_obj *o, const char *key, const char *value)
{
	char v[64];

	return json_get_string(o, key, v, sizeof(v)) && !strcmp(v, value);
}

/* Is this line the end of the request? */
static bool client_done(const char *cmd, const char *arg, const struct json_obj *o)
{
	if (!strcmp(cmd, "status"))
		return has(o, "type", "status");
	if (has(o, "type", "error") && has(o, "cmd", cmd))
		return true;
	if (!strcmp(cmd, "pair"))
		return has(o, "type", "pair") && has(o, "addr", arg) &&
		       (has(o, "state", "done") || has(o, "state", "failed"));
	if (!strcmp(cmd, "forget"))
		return has(o, "type", "forgotten") && has(o, "addr", arg);
	if (!strcmp(cmd, "list") || !strcmp(cmd, "cancel"))
		return has(o, "type", "ok") && has(o, "cmd", cmd);
	return false;	/* watch, scan: until interrupted */
}

/* Client mode: one request, the reply lines printed raw (the hello is
 * not). Exit 0 at the request's end, 1 on an error line, a failed pairing
 * or no connection, 64 on a usage error. */
int bd_cli_main(const char *sock, int argc, char **argv)
{
	const char *cmd = argv[0], *arg = argc > 1 ? argv[1] : "";
	char req[128], buf[4096];
	size_t len = 0;
	int fd;
	bool known = !strcmp(cmd, "status") || !strcmp(cmd, "list") || !strcmp(cmd, "watch") ||
		     !strcmp(cmd, "scan") || ((!strcmp(cmd, "pair") || !strcmp(cmd, "cancel") ||
					       !strcmp(cmd, "forget")) && bd_addr_ok(arg));

	if (!known) {
		fprintf(stderr, "blesensd: unknown request (status|list|watch|scan|pair ADDR|cancel ADDR|forget ADDR)\n");
		return 64;
	}
	fd = linesrv_connect(sock);
	if (fd < 0) {
		fprintf(stderr, "blesensd: connect %s: %s\n", sock, strerror(-fd));
		return 1;
	}
	if (!strcmp(cmd, "scan"))
		snprintf(req, sizeof(req), "watch\nscan on\n");
	else if (!strcmp(cmd, "pair") || !strcmp(cmd, "forget"))
		snprintf(req, sizeof(req), "watch\n%s %s\n", cmd, arg);	/* the outcome is an event */
	else
		snprintf(req, sizeof(req), "%s%s%s\n", cmd, *arg ? " " : "", arg);
	if (write(fd, req, strlen(req)) != (ssize_t)strlen(req)) {
		close(fd);
		return 1;
	}
	for (;;) {
		ssize_t n = read(fd, buf + len, sizeof(buf) - 1 - len);
		char *nl;

		if (n <= 0)
			break;
		len += (size_t)n;
		buf[len] = 0;
		while ((nl = strchr(buf, '\n'))) {
			static struct json_obj o;
			bool json, done = false, failed = false;

			*nl = 0;
			json = json_parse(&o, buf, strlen(buf)) == JSON_OK;
			if (!json || !has(&o, "type", "hello")) {
				printf("%s\n", buf);
				if (json) {
					done = client_done(cmd, arg, &o);
					failed = has(&o, "type", "error") || has(&o, "state", "failed");
				}
			}
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
