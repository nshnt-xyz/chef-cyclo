/*
 * test_timeview.c - the Time page's chrony client against fixture
 * executables: success, EOF before exit, the 2 s deadline, a descendant
 * holding stdout, a failing or stuck waitpid (ECHILD, a child that cannot
 * be reaped yet) and recovery. Real time; no chrony.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "check.h"
#include "fakeloop.h"
#include "sys.h"
#include "sysstate.h"
#include "timeview.h"

#define CSV "47505300,GPS,1,1791448320.000,0,0,0,0,0,0,0,0,64,Normal\\n"

static char dir[] = "/tmp/chef-timeview-test.XXXXXX";
static char prog[256], pidfile[256];

static void script(const char *body)
{
	FILE *f = fopen(prog, "w");

	CHECK(f != NULL);
	if (!f)
		return;
	fputs("#!/bin/sh\n", f);
	fputs(body, f);
	fclose(f);
	chmod(prog, 0700);
}

static int64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static bool verified(void *unused)
{
	(void)unused;
	return sys_state.timeview.verified;
}

static bool unverified(void *unused)
{
	(void)unused;
	return !sys_state.timeview.verified;
}

static bool no_reap_timer(void *unused)
{
	(void)unused;
	return !sys_timeview_test_reap_timer();
}

/* waitpid stand-in: 0 real, 1 the child is "gone" (ECHILD), 2 not reapable yet */
static int wait_mode;

static pid_t hook_waitpid(pid_t pid, int *status, int options)
{
	if (wait_mode == 1) {
		errno = ECHILD;
		return -1;
	}
	if (wait_mode == 2)
		return 0;
	return waitpid(pid, status, options);
}

static void kill_descendant(void)
{
	char pidbuf[32] = { 0 };
	int fd = open(pidfile, O_RDONLY);

	if (fd < 0)
		return;
	if (read(fd, pidbuf, sizeof(pidbuf) - 1) > 0) {
		pid_t child = (pid_t)strtol(pidbuf, NULL, 10);

		if (child > 1)
			kill(child, SIGKILL);	/* fixture-owned descendant */
	}
	close(fd);
}

int main(void)
{
	char body[1024];
	unsigned kills;
	int64_t t0;

	CHECK(mkdtemp(dir) != NULL);
	snprintf(prog, sizeof(prog), "%s/chronyc-fixture", dir);
	snprintf(pidfile, sizeof(pidfile), "%s/descendant.pid", dir);
	fakeloop_init(false);
	sys_timeview_test_executable(prog);
	sys_timeview_test_waitpid(hook_waitpid);
	sys_timeview_init();

	/* success, over chronyd's localhost command port; leaving clears the
	 * verified state */
	snprintf(body, sizeof(body), "echo \"$*\" > %s/args\nprintf '" CSV "'\n", dir);
	script(body);
	sys_timeview_want(true);
	CHECK(fakeloop_run_until(verified, NULL, 1000));
	CHECK(sys_state.timeview.source == SYS_TIME_GPS);
	CHECK(sys_state.timeview.reference_epoch == 1791448320.0);
	sys_timeview_want(false);
	CHECK(!sys_state.timeview.verified);
	{
		char path[300], args[128] = { 0 };
		FILE *f;

		snprintf(path, sizeof(path), "%s/args", dir);
		f = fopen(path, "r");
		CHECK(f != NULL);
		if (f) {
			CHECK(fgets(args, sizeof(args), f) != NULL);
			fclose(f);
			unlink(path);
		}
		CHECK(strcmp(args, "-c -n -h 127.0.0.1 tracking\n") == 0);
	}

	/* EOF strictly before exit: stdout is closed, the child lives on for 0.3 s */
	script("printf '" CSV "'\nexec >&-\nsleep 0.3\nexit 0\n");
	kills = sys_timeview_test_kills();
	t0 = now_ms();
	sys_timeview_want(true);
	CHECK(fakeloop_run_until(verified, NULL, 1500));
	CHECK(now_ms() - t0 >= 250);		/* it waited for the exit */
	CHECK(sys_timeview_test_kills() == kills);
	sys_timeview_want(false);

	/* the 2 s deadline before the child exits: a verified state is cleared at the
	 * deadline, the query is killed once and the page still wants updates */
	script("printf '" CSV "'\n");
	sys_timeview_want(true);
	CHECK(fakeloop_run_until(verified, NULL, 1000));
	script("printf '" CSV "'\nsleep 5\n");
	kills = sys_timeview_test_kills();
	t0 = now_ms();
	sys_timeview_want(true);		/* a new query, the old result still shown */
	CHECK(sys_state.timeview.verified);
	CHECK(fakeloop_run_until(unverified, NULL, 2700));
	CHECK(now_ms() - t0 >= 1900 && now_ms() - t0 <= 2700);
	CHECK(sys_timeview_test_kills() == kills + 1);
	fakeloop_run_for(1200);			/* the reap timer collects the killed child */
	CHECK(!sys_timeview_test_reap_timer());
	sys_timeview_want(false);

	/* the direct child exits 0 but a descendant holds stdout open: rejected at the
	 * deadline, the descriptor removed, and nothing is signalled (the child was
	 * reaped, and its pid is no longer ours) */
	snprintf(body, sizeof(body), "printf '" CSV "'\n(sleep 8) & echo $! > '%s'\nexit 0\n", pidfile);
	script(body);
	sys_state.timeview.verified = true;
	sys_state.timeview.source = SYS_TIME_GPS;
	kills = sys_timeview_test_kills();
	t0 = now_ms();
	sys_timeview_want(true);
	CHECK(fakeloop_run_until(unverified, NULL, 2700));
	CHECK(now_ms() - t0 >= 1900);
	CHECK(fakeloop_watched() == 0);
	CHECK(sys_timeview_test_kills() == kills);
	kill_descendant();
	sys_timeview_want(false);

	/* a later query with a good fixture recovers */
	script("printf '" CSV "'\n");
	sys_timeview_want(true);
	CHECK(fakeloop_run_until(verified, NULL, 1000));
	sys_timeview_want(false);

	/* a child that is already gone (ECHILD) is a failure, not an exit 0 */
	script("printf '" CSV "'\n");
	sys_state.timeview.verified = true;
	sys_state.timeview.source = SYS_TIME_GPS;
	wait_mode = 1;
	kills = sys_timeview_test_kills();
	sys_timeview_want(true);
	CHECK(fakeloop_run_until(unverified, NULL, 1000));
	CHECK(sys_timeview_test_kills() == kills);
	fakeloop_run_for(300);
	CHECK(!sys_state.timeview.verified);	/* it stays rejected */
	wait_mode = 0;
	sys_timeview_want(false);

	/* a child that cannot be reaped yet after the deadline: the reap timer keeps
	 * firing (it does not dangle), and a new query starts as soon as it is reaped */
	script("sleep 8\n");
	wait_mode = 2;
	kills = sys_timeview_test_kills();
	sys_timeview_want(true);
	fakeloop_run_for(2300);			/* past the deadline */
	CHECK(sys_timeview_test_kills() == kills + 1);
	CHECK(sys_timeview_test_reap_timer());
	fakeloop_run_for(2200);			/* two more reap ticks: still pending, not deleted */
	CHECK(sys_timeview_test_reap_timer());
	CHECK(sys_timeview_test_kills() == kills + 1);	/* and never signalled again */
	wait_mode = 0;				/* the child can be reaped now */
	CHECK(fakeloop_run_until(no_reap_timer, NULL, 1500));
	script("printf '" CSV "'\n");
	t0 = now_ms();
	sys_timeview_want(true);		/* spawns at once, not after 30 s */
	CHECK(fakeloop_run_until(verified, NULL, 1000));
	CHECK(now_ms() - t0 < 1000);
	sys_timeview_want(false);

	/* leaving and re-entering right after a reap spawns in the same call */
	script("sleep 5\n");
	sys_timeview_want(true);
	fakeloop_run_for(100);
	sys_timeview_want(false);		/* killed, reap timer armed */
	fakeloop_run_for(100);			/* the killed child is reapable by now */
	script("printf '" CSV "'\n");
	t0 = now_ms();
	sys_timeview_want(true);
	CHECK(fakeloop_run_until(verified, NULL, 600));
	CHECK(now_ms() - t0 < 600);
	sys_timeview_stop();
	unlink(prog);
	unlink(pidfile);
	rmdir(dir);
	return check_report("test-timeview");
}
