/*
 * timeview.c - where the current time came from, for the Time page. While
 * the page is visible it asks chrony (`chronyc -c -n -h <socket> tracking`,
 * posix_spawn, no shell) on entry and every 30 s, and parses the answer
 * strictly; the boot provenance in /run/chef/state/time is read on the
 * settings worker. All of it runs on the UI thread (the loop's fd watch and
 * LVGL timers) except the provenance read; nothing blocks.
 *
 * A query is settled when stdout reached EOF and the direct child was reaped
 * with exit 0, or on its 2 s deadline (whichever comes first, even for a
 * reaped child whose stdout a descendant still holds). The child is signalled
 * only before it is reaped.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <math.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <spawn.h>
#include <sys/stat.h>

#include "chefui.h"
#include "common/chef_paths.h"
#include "settings.h"
#include "sys.h"
#include "sysstate.h"
#include "timeview.h"

extern char **environ;

static struct {
	bool initialized, wanted, eof, reaped, child_ok, child_owned;
	pid_t pid;
	int fd;
	char output[4097];
	size_t used;
	int64_t deadline_ms;
	lv_timer_t *supervise, *refresh, *reap;
} tv = { .fd = -1 };

#ifdef CHEFUI_TEST
static const char *test_executable;
static pid_t (*test_waitpid)(pid_t, int *, int);
static unsigned test_kills;
static pid_t test_last_kill;

void sys_timeview_test_executable(const char *path)
{
	test_executable = path;
}

void sys_timeview_test_waitpid(pid_t (*fn)(pid_t, int *, int))
{
	test_waitpid = fn;
}

unsigned sys_timeview_test_kills(void)
{
	return test_kills;
}

pid_t sys_timeview_test_last_kill(void)
{
	return test_last_kill;
}

bool sys_timeview_test_reap_timer(void)
{
	return tv.reap != NULL;
}
#endif

static pid_t wait_child(int *status)
{
#ifdef CHEFUI_TEST
	if (test_waitpid)
		return test_waitpid(tv.pid, status, WNOHANG);
#endif
	return waitpid(tv.pid, status, WNOHANG);
}

static int64_t mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void verified_clear(void)
{
	if (!sys_state.timeview.verified && sys_state.timeview.source == SYS_TIME_UNKNOWN &&
	    sys_state.timeview.reference_epoch == 0)
		return;
	sys_state.timeview.verified = false;
	sys_state.timeview.source = SYS_TIME_UNKNOWN;
	sys_state.timeview.reference_epoch = 0;
	sys_changed(SYS_TIME_VIEW);
}

static void close_output(void)
{
	if (tv.fd < 0)
		return;
	chefui_unwatch_fd(tv.fd);
	close(tv.fd);
	tv.fd = -1;
}

/* Reap the direct child if it is done. The pid is ours only until waitpid
 * returns it: ownership (and so any signal) ends here, and a child that is
 * already gone (ECHILD) is a failure, never a success. */
static void reap_child(void)
{
	int status = 0;
	pid_t p;

	if (!tv.child_owned)
		return;
	p = wait_child(&status);
	if (p == tv.pid) {
		tv.child_ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
	} else if (p < 0 && errno == ECHILD) {
		tv.child_ok = false;
	} else {
		return;
	}
	tv.child_owned = false;
	tv.reaped = true;
}

static void reap_tick(lv_timer_t *t)
{
	(void)t;
	reap_child();
	if (!tv.child_owned && tv.reap) {
		lv_timer_delete(tv.reap);
		tv.reap = NULL;
	}
}

/* Repeats once a second until the child is reaped; deleted in reap_tick(). */
static void reap_timer_start(void)
{
	if (tv.child_owned && !tv.reap)
		tv.reap = lv_timer_create(reap_tick, 1000, NULL);
}

static void timeout_query(void)
{
	if (tv.child_owned) {
		/* posix_spawn put the child in its own group (SETPGROUP, pgroup 0, and
		 * the spawn fails if that fails), so while we own it pgid == pid and
		 * the group kill covers it. Not signalled again once reaped. */
		(void)kill(-tv.pid, SIGKILL);
#ifdef CHEFUI_TEST
		test_kills++;
		test_last_kill = tv.pid;
#endif
	}
	close_output();
	verified_clear();
	if (tv.supervise) {
		lv_timer_delete(tv.supervise);
		tv.supervise = NULL;
	}
	reap_timer_start();
}

static void query_finish(void)
{
	struct sys_time_report report;
	double now = (double)time(NULL);
	bool ok = tv.reaped && tv.child_ok && tv.eof &&
		sys_time_parse_tracking(tv.output, tv.used, now, &report);

	if (tv.supervise) {
		lv_timer_delete(tv.supervise);
		tv.supervise = NULL;
	}
	if (!ok) {
		verified_clear();
		return;
	}
	sys_state.timeview.source = report.source;
	sys_state.timeview.reference_epoch = report.reference_epoch;
	sys_state.timeview.verified = true;
	sys_changed(SYS_TIME_VIEW);
}

static void supervise_tick(lv_timer_t *t);

static void output_ready(int fd, short revents, void *user)
{
	char buf[512];

	(void)revents;
	(void)user;
	for (;;) {
		ssize_t n = read(fd, buf, sizeof(buf));

		if (n > 0) {
			if ((size_t)n > sizeof(tv.output) - 1 - tv.used) {
				tv.used = sizeof(tv.output);
				timeout_query();
				return;
			}
			memcpy(tv.output + tv.used, buf, (size_t)n);
			tv.used += (size_t)n;
			tv.output[tv.used] = 0;
			continue;
		}
		if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
			tv.eof = true;
			close_output();
		}
		break;
	}
	supervise_tick(tv.supervise);
}

/* The 20 ms supervision timer runs while the query is unsettled: the
 * deadline applies even to a reaped child whose stdout is still held open. */
static void supervise_tick(lv_timer_t *t)
{
	(void)t;
	reap_child();
	if (mono_ms() >= tv.deadline_ms) {
		timeout_query();
		return;
	}
	if (tv.reaped && tv.eof)
		query_finish();
}

static bool query_start(void)
{
	int pfd[2], nullfd, e;
	posix_spawn_file_actions_t fa;
	posix_spawnattr_t attr;
	sigset_t empty, defaults;
	char *argv[] = { (char *)(
#ifdef CHEFUI_TEST
		test_executable ? test_executable :
#endif
		"/usr/bin/chronyc"), "-c", "-n", "-h", "/run/chrony/chronyd.sock",
		"tracking", NULL };
	char *envp[] = { "LC_ALL=C", "PATH=/usr/bin:/bin", NULL };

	if (tv.child_owned) {
		reap_child();
		if (tv.child_owned) {
			reap_timer_start();
			return false;
		}
		if (tv.reap) {
			lv_timer_delete(tv.reap);
			tv.reap = NULL;
		}
	}
	if (tv.supervise || !tv.wanted)
		return false;
	if (pipe2(pfd, O_CLOEXEC | O_NONBLOCK))
		goto failed;
	nullfd = open("/dev/null", O_RDWR | O_CLOEXEC);
	if (nullfd < 0) {
		close(pfd[0]);
		close(pfd[1]);
		goto failed;
	}
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, pfd[1], STDOUT_FILENO);
	posix_spawn_file_actions_adddup2(&fa, nullfd, STDIN_FILENO);
	posix_spawn_file_actions_adddup2(&fa, nullfd, STDERR_FILENO);
	posix_spawn_file_actions_addclose(&fa, pfd[0]);
	posix_spawn_file_actions_addclose(&fa, pfd[1]);
	posix_spawn_file_actions_addclose(&fa, nullfd);
	posix_spawnattr_init(&attr);
	sigemptyset(&empty);
	sigfillset(&defaults);
	posix_spawnattr_setsigmask(&attr, &empty);
	posix_spawnattr_setsigdefault(&attr, &defaults);
	posix_spawnattr_setpgroup(&attr, 0);
	posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF |
				 POSIX_SPAWN_SETPGROUP);
	e = posix_spawn(&tv.pid, argv[0], &fa, &attr, argv, envp);
	posix_spawnattr_destroy(&attr);
	posix_spawn_file_actions_destroy(&fa);
	close(pfd[1]);
	close(nullfd);
	if (e) {
		close(pfd[0]);
		errno = e;
		goto failed;
	}
	tv.child_owned = true;
	tv.reaped = false;
	tv.child_ok = false;
	tv.eof = false;
	tv.used = 0;
	tv.output[0] = 0;
	tv.fd = pfd[0];
	tv.deadline_ms = mono_ms() + 2000;
	chefui_watch_fd(tv.fd, POLLIN | POLLHUP | POLLERR, output_ready, NULL);
	tv.supervise = lv_timer_create(supervise_tick, 20, NULL);
	return true;
failed:
	verified_clear();
	return false;
}

static void refresh_tick(lv_timer_t *t)
{
	(void)t;
	if (!tv.wanted) {
		if (tv.refresh) {
			lv_timer_delete(tv.refresh);
			tv.refresh = NULL;
		}
		return;
	}
	if (tv.refresh)
		lv_timer_reset(tv.refresh);
	query_start();
}

static void boot_done(enum sys_time_source source, void *user)
{
	(void)user;
	if (source != sys_state.timeview.boot_source) {
		sys_state.timeview.boot_source = source;
		sys_changed(SYS_TIME_VIEW);
	}
}

int sys_timeview_init(void)
{
	tv.initialized = true;
	ui_store_on_time_boot(boot_done, NULL);
	return 0;
}

void sys_timeview_want(bool visible)
{
	tv.wanted = visible;
	if (!visible) {
		ui_store_time_boot_cancel();
		if (tv.refresh) {
			lv_timer_delete(tv.refresh);
			tv.refresh = NULL;
		}
		if (tv.supervise)
			timeout_query();
		else
			verified_clear();
		return;
	}
	if (!tv.initialized)
		sys_timeview_init();
	ui_store_time_boot(NULL);	/* boot provenance, on the worker */
	if (!tv.refresh)
		tv.refresh = lv_timer_create(refresh_tick, 30000, NULL);
	query_start();
}

/* Tests only: production has no teardown path, the process exits. */
void sys_timeview_stop(void)
{
	tv.wanted = false;
	if (tv.refresh) {
		lv_timer_delete(tv.refresh);
		tv.refresh = NULL;
	}
	if (tv.supervise)
		timeout_query();
}

bool sys_time_format(int hour, int minute, bool clock_24h, char *digits,
		     size_t digits_size, char meridiem[3])
{
	if (!digits || !digits_size)
		return false;
	meridiem[0] = 0;
	if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
		snprintf(digits, digits_size, "--:--");
		return false;
	}
	if (clock_24h)
		snprintf(digits, digits_size, "%02d:%02d", hour, minute);
	else {
		snprintf(digits, digits_size, "%d:%02d", hour % 12 ? hour % 12 : 12, minute);
		snprintf(meridiem, 3, "%s", hour < 12 ? "am" : "pm");
	}
	return true;
}

void sys_time_full(int hour, int minute, bool clock_24h, char *out, size_t size)
{
	char digits[8], meridiem[3];

	if (!sys_time_format(hour, minute, clock_24h, digits, sizeof(digits), meridiem)) {
		snprintf(out, size, "--:--");
		return;
	}
	if (meridiem[0])
		snprintf(out, size, "%s %s", digits, meridiem);
	else
		snprintf(out, size, "%s", digits);
}

static bool number(const char *s, double *v)
{
	char *end;

	if (!s || !*s)
		return false;
	*v = strtod(s, &end);
	return end != s && !*end && isfinite(*v);
}

bool sys_time_parse_tracking(const char *csv, size_t len, double query_epoch,
			     struct sys_time_report *out)
{
	char copy[4097], *field[14], *p;
	struct sys_time_report r = { .source = SYS_TIME_UNKNOWN };
	double v;
	int stratum;

	if (!csv || !out || !len || len >= sizeof(copy) || csv[len - 1] != '\n' ||
	    memchr(csv, 0, len) || !isfinite(query_epoch))
		return false;
	memcpy(copy, csv, len);
	copy[len] = 0;
	if (strchr(copy, '\n') != copy + len - 1 || strchr(copy, '\r'))
		return false;
	copy[len - 1] = 0;
	p = copy;
	for (int i = 0; i < 14; i++) {
		field[i] = p;
		char *comma = strchr(p, ',');
		if (i < 13) {
			if (!comma)
				return false;
			*comma = 0;
			p = comma + 1;
		} else if (comma)
			return false;
		if (!*field[i] || strcspn(field[i], "\t ") != strlen(field[i]))
			return false;
	}
	if (strlen(field[0]) != 8)
		return false;
	for (int i = 0; i < 8; i++)
		if (!isxdigit((unsigned char)field[0][i]))
			return false;
	if (!number(field[2], &v) || v < 1 || v > 15 || floor(v) != v)
		return false;
	stratum = (int)v;
	if (!number(field[3], &r.reference_epoch) || r.reference_epoch < 1767225600.0 ||
	    r.reference_epoch >= 4102444800.0 || r.reference_epoch > query_epoch + 5.0)
		return false;
	for (int i = 4; i <= 12; i++)
		if (!number(field[i], &v))
			return false;
	/* chrony prints the refid in upper case */
	if (strcmp(field[13], "Normal") || !stratum || !strcasecmp(field[0], "00000000") ||
	    !strcasecmp(field[0], "7F7F0101") || !strcmp(field[1], "LOCL"))
		return false;
	if (!strcasecmp(field[0], "47505300") && !strcmp(field[1], "GPS"))
		r.source = SYS_TIME_GPS;
	else {
		unsigned char addr[16];

		if (inet_pton(AF_INET, field[1], addr) == 1 || inet_pton(AF_INET6, field[1], addr) == 1)
			r.source = SYS_TIME_NETWORK;
	}
	if (r.source == SYS_TIME_UNKNOWN)
		return false;
	*out = r;
	return true;
}

enum sys_time_source sys_time_parse_boot(const char *text, size_t len)
{
	char copy[513], *line, *save = NULL;
	char *source = NULL, *reason = NULL, *age = NULL;
	int fields = 0;
	if (!text || !len || len > 512 || text[len - 1] != '\n' || memchr(text, 0, len))
		return SYS_TIME_UNKNOWN;
	memcpy(copy, text, len);
	copy[len] = 0;
	if (strchr(copy, '\r'))
		return SYS_TIME_UNKNOWN;
	for (line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char *eq = strchr(line, '=');
		if (!eq || eq == line || !eq[1] || strchr(eq + 1, '='))
			return SYS_TIME_UNKNOWN;
		*eq++ = 0;
		if (!strcmp(line, "source") && !source)
			source = eq;
		else if (!strcmp(line, "reason") && !reason)
			reason = eq;
		else if (!strcmp(line, "saved_age_s") && !age)
			age = eq;
		else
			return SYS_TIME_UNKNOWN;
		fields++;
	}
	if (fields != 3 || !source || !reason || !age)
		return SYS_TIME_UNKNOWN;
	for (const char *p = reason; *p; p++) {
		if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
		      (*p >= '0' && *p <= '9') || *p == '-' || *p == '_'))
			return SYS_TIME_UNKNOWN;
	}
	if (strcmp(age, "na")) {
		if (!*age || (age[0] == '0' && age[1]))
			return SYS_TIME_UNKNOWN;
		for (const char *p = age; *p; p++) {
			if (*p < '0' || *p > '9')
				return SYS_TIME_UNKNOWN;
		}
	}
	if (!strcmp(source, "rtc-offset"))
		return SYS_TIME_RESTORED;
	if (!strcmp(source, "floor"))
		return SYS_TIME_FLOOR;
	if (!strcmp(source, "none"))
		return SYS_TIME_NONE;
	return SYS_TIME_UNKNOWN;
}

enum sys_time_source sys_time_read_boot(const char *path)
{
	char b[513];
	struct stat st;
	size_t used;
	int fd = open(path ? path : CHEF_STATE_TIME, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return SYS_TIME_UNKNOWN;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 || st.st_size > 512) {
		close(fd);
		return SYS_TIME_UNKNOWN;
	}
	used = 0;
	while (used < sizeof(b)) {
		ssize_t n = read(fd, b + used, sizeof(b) - used);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		used += (size_t)n;
	}
	close(fd);
	if (used > 512)
		return SYS_TIME_UNKNOWN;
	return sys_time_parse_boot(b, used);
}
