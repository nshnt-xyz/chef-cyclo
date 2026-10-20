/* Host tests for tools/common/chef_log.c: the "<30>tag[pid]: msg" datagram
 * on a temp socket standing in for /dev/log; the "<tag>: <msg>\n" kmsg
 * fallback (a temp file standing in for /dev/kmsg) when there is no
 * socket, when its queue is full (without blocking) and when it went away;
 * reconnecting to a socket that came back; CHEF_LOG_SOCKET; the 320-byte
 * message bound, the stderr echo and a missing kmsg that is not an error.
 * Nothing here reaches the host's /dev/log.
 * Build/run: see tools/common/Makefile ("make test").
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
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

static void truncate_file(const char *path)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);

	if (fd >= 0)
		close(fd);
}

/* a stand-in syslogd socket: bound, never read unless asked */
static int bind_dgram(const char *path)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);

	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
	unlink(path);
	if (fd < 0 || bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		perror("bind");
		exit(1);
	}
	return fd;
}

/* the next queued datagram, or "" when there is none */
static size_t recv_one(int fd, char *buf, size_t len)
{
	ssize_t n = recv(fd, buf, len - 1, MSG_DONTWAIT);

	if (n < 0)
		n = 0;
	buf[n] = '\0';
	return (size_t)n;
}

static double now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(void)
{
	char dir[] = "/tmp/chef-log-test-XXXXXX", kmsg[64], errf[64], sock[64], missing[64];
	char buf[1024], want[128], msg[400];
	int fd, saved, srv, i;
	double t0;

	if (!mkdtemp(dir)) {
		perror("mkdtemp");
		return 1;
	}
	snprintf(kmsg, sizeof(kmsg), "%s/kmsg", dir);
	snprintf(errf, sizeof(errf), "%s/stderr", dir);
	snprintf(sock, sizeof(sock), "%s/log", dir);
	snprintf(missing, sizeof(missing), "%s/no-such-log", dir);
	chef_log_set_kmsg(kmsg);

	/* CHEF_LOG_SOCKET, read by chef_log_init(): no socket there and no
	 * kmsg file, so nothing happens (kmsg is opened without O_CREAT) */
	setenv("CHEF_LOG_SOCKET", missing, 1);
	chef_log_init("buttond", false);
	chef_log("before %d", 1);
	CHECK(access(kmsg, F_OK) != 0);

	/* no socket: the kmsg line, exactly as before syslog */
	truncate_file(kmsg);
	chef_log("ready: %s, long %u ms", "/run/chef/buttond.sock", 1500u);
	CHECK(slurp(kmsg, buf, sizeof(buf)) && !strcmp(buf, "buttond: ready: /run/chef/buttond.sock, long 1500 ms\n"));

	/* the socket CHEF_LOG_SOCKET names: one datagram in the wire form,
	 * nothing in kmsg */
	srv = bind_dgram(missing);
	truncate_file(kmsg);
	chef_log("ready: %s", "/run/chef/buttond.sock");
	snprintf(want, sizeof(want), "<30>buttond[%ld]: ready: /run/chef/buttond.sock", (long)getpid());
	CHECK(recv_one(srv, buf, sizeof(buf)) && !strcmp(buf, want));
	CHECK(slurp(kmsg, buf, sizeof(buf)) == 0);
	close(srv);
	unlink(missing);

	/* chef_log_set_socket() wins over the environment */
	srv = bind_dgram(sock);
	chef_log_set_socket(sock);
	chef_log_init("buttond", false);
	chef_log("set %s", "socket");
	snprintf(want, sizeof(want), "<30>buttond[%ld]: set socket", (long)getpid());
	CHECK(recv_one(srv, buf, sizeof(buf)) && !strcmp(buf, want));
	CHECK(slurp(kmsg, buf, sizeof(buf)) == 0);

	/* the message is cut at 320 bytes; the header comes on top */
	memset(msg, 'x', 360);
	msg[360] = '\0';
	chef_log("%s", msg);
	snprintf(want, sizeof(want), "<30>buttond[%ld]: ", (long)getpid());
	CHECK(recv_one(srv, buf, sizeof(buf)) == strlen(want) + 320);
	CHECK(!strncmp(buf, want, strlen(want)) && buf[strlen(buf) - 1] == 'x');

	/* a full receive queue (syslogd stuck): kmsg, and no block */
	t0 = now_s();
	for (i = 0; i < 100000 && slurp(kmsg, buf, sizeof(buf)) == 0; i++)
		chef_log("flood %d", i);
	CHECK(i > 0 && i < 100000);
	CHECK(!strncmp(buf, "buttond: flood ", 15));
	truncate_file(kmsg);
	chef_log("still full");
	CHECK(slurp(kmsg, buf, sizeof(buf)) && !strcmp(buf, "buttond: still full\n"));
	CHECK(now_s() - t0 < 5.0);
	while (recv_one(srv, buf, sizeof(buf)))
		;
	truncate_file(kmsg);
	chef_log("drained");
	snprintf(want, sizeof(want), "<30>buttond[%ld]: drained", (long)getpid());
	CHECK(recv_one(srv, buf, sizeof(buf)) && !strcmp(buf, want));
	CHECK(slurp(kmsg, buf, sizeof(buf)) == 0);

	/* syslogd restarted (a new socket at the same path): reconnected,
	 * nothing lost to kmsg */
	close(srv);
	srv = bind_dgram(sock);
	chef_log("after restart");
	snprintf(want, sizeof(want), "<30>buttond[%ld]: after restart", (long)getpid());
	CHECK(recv_one(srv, buf, sizeof(buf)) && !strcmp(buf, want));
	CHECK(slurp(kmsg, buf, sizeof(buf)) == 0);

	/* syslogd gone: kmsg; back: the socket again */
	close(srv);
	unlink(sock);
	chef_log("during the gap");
	CHECK(slurp(kmsg, buf, sizeof(buf)) && !strcmp(buf, "buttond: during the gap\n"));
	truncate_file(kmsg);
	chef_log("still gone");
	CHECK(slurp(kmsg, buf, sizeof(buf)) && !strcmp(buf, "buttond: still gone\n"));
	srv = bind_dgram(sock);
	truncate_file(kmsg);
	chef_log("back");
	snprintf(want, sizeof(want), "<30>buttond[%ld]: back", (long)getpid());
	CHECK(recv_one(srv, buf, sizeof(buf)) && !strcmp(buf, want));
	CHECK(slurp(kmsg, buf, sizeof(buf)) == 0);

	/* echo: "tag: msg" on stderr whichever way the line went */
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
	snprintf(want, sizeof(want), "<30>powerd[%ld]: level L2", (long)getpid());
	CHECK(recv_one(srv, buf, sizeof(buf)) && !strcmp(buf, want));
	snprintf(want, sizeof(want), "<30>powerd[%ld]: quiet", (long)getpid());
	CHECK(recv_one(srv, buf, sizeof(buf)) && !strcmp(buf, want));
	CHECK(slurp(kmsg, buf, sizeof(buf)) == 0);

	close(srv);
	chef_log_set_socket(missing);	/* never /dev/log, even on the way out */
	chef_log_set_kmsg(NULL);
	unlink(sock);
	unlink(kmsg);
	unlink(errf);
	rmdir(dir);
	printf("test-chef-log: %d/%d checks passed\n", g_tests - g_failures, g_tests);
	return g_failures ? 1 : 0;
}
