/* Host tests for tools/common/chef_log.c: the "<tag>: <msg>\n" line in
 * the kmsg file (a temp file standing in for /dev/kmsg), the 320-byte
 * message bound, the stderr echo and a missing kmsg that is not an error.
 * Build/run: see tools/common/Makefile ("make test").
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "chef_log.h"

static int g_failures;
static int g_tests;

#define CHECK(cond) do { \
	g_tests++; \
	if (!(cond)) { \
		g_failures++; \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	} \
} while (0)

static size_t slurp(const char *path, char *buf, size_t len)
{
	int fd = open(path, O_RDONLY);
	ssize_t n;

	if (fd < 0)
		return 0;
	n = read(fd, buf, len - 1);
	close(fd);
	if (n < 0)
		n = 0;
	buf[n] = '\0';
	return (size_t)n;
}

int main(void)
{
	char dir[] = "/tmp/chef-log-test-XXXXXX", kmsg[64], errf[64], buf[1024], msg[400];
	int fd, saved;

	if (!mkdtemp(dir)) {
		perror("mkdtemp");
		return 1;
	}
	snprintf(kmsg, sizeof(kmsg), "%s/kmsg", dir);
	snprintf(errf, sizeof(errf), "%s/stderr", dir);
	chef_log_set_kmsg(kmsg);

	/* no kmsg file: nothing happens (open without O_CREAT) */
	chef_log_init("buttond", false);
	chef_log("before %d", 1);
	CHECK(access(kmsg, F_OK) != 0);

	fd = open(kmsg, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	close(fd);
	chef_log("ready: %s, long %u ms", "/run/chef/buttond.sock", 1500u);
	CHECK(slurp(kmsg, buf, sizeof(buf)) && !strcmp(buf, "buttond: ready: /run/chef/buttond.sock, long 1500 ms\n"));

	/* the message is cut at 320 bytes; the tag and newline come on top */
	memset(msg, 'x', 360);
	msg[360] = '\0';
	chef_log("%s", msg);
	CHECK(slurp(kmsg, buf, sizeof(buf)) == strlen("buttond: ") + 320 + 1);
	CHECK(!strncmp(buf, "buttond: xxx", 12) && buf[strlen(buf) - 1] == '\n' && buf[strlen(buf) - 2] == 'x');

	/* echo: the same line on stderr */
	fflush(stderr);
	saved = dup(2);
	fd = open(errf, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	dup2(fd, 2);
	close(fd);
	chef_log_init("powerd", true);
	chef_log("level %s", "L2");
	chef_log_init("powerd", false);
	chef_log("quiet");
	fflush(stderr);
	dup2(saved, 2);
	close(saved);
	CHECK(slurp(errf, buf, sizeof(buf)) && !strcmp(buf, "powerd: level L2\n"));
	CHECK(slurp(kmsg, buf, sizeof(buf)) && !strncmp(buf, "powerd: quiet\n", 14));

	chef_log_set_kmsg(NULL);
	unlink(kmsg);
	unlink(errf);
	rmdir(dir);
	printf("test-chef-log: %d/%d checks passed\n", g_tests - g_failures, g_tests);
	return g_failures ? 1 : 0;
}
