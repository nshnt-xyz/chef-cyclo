/*
 * test_bootsteps.c - sys/bootsteps.c (the boot splash's step tracker
 * without LVGL) against temporary OpenRC and run trees with an injected
 * clock: every row of the table (done, failed, partial), steps finishing
 * out of order, a failure in each step, sticky results, directories that
 * are missing and then appear, the 30 s cap, the marker appearing and
 * disappearing, and the log lines. Built without LVGL's include path
 * (Makefile: test-bootsteps).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <sys/resource.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "check.h"
#include "bootsteps.h"

static char root[128], openrc[160], run[160];
static int64_t g_now, g_boot;
static char g_log[32][160];
static int g_nlog;

static int64_t clk(void) { return g_now; }
static int64_t boot_clk(void) { return g_boot; }

static void log_fn(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	if (g_nlog < 32)
		vsnprintf(g_log[g_nlog++], sizeof(g_log[0]), fmt, ap);
	va_end(ap);
}

static void sh(const char *fmt, ...)
{
	char cmd[512];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(cmd, sizeof(cmd), fmt, ap);
	va_end(ap);
	if (system(cmd))
		abort();
}

static void tree_new(bool with_dirs)
{
	snprintf(root, sizeof(root), "/tmp/bootsteps-test-XXXXXX");
	if (!mkdtemp(root))
		abort();
	snprintf(openrc, sizeof(openrc), "%s/openrc", root);
	snprintf(run, sizeof(run), "%s/chef", root);
	sh("mkdir -p %s", root);
	if (with_dirs)
		sh("mkdir -p %s/started %s/failed %s", openrc, openrc, run);
	g_now = 1000;
	g_boot = 5000;
	g_nlog = 0;
}

static void svc(const char *dir, const char *name)
{
	sh("ln -s /nonexistent %s/%s/%s", openrc, dir, name);
}

static void unsvc(const char *dir, const char *name)
{
	sh("rm -f %s/%s/%s", openrc, dir, name);
}

static void init(struct bootsteps *b)
{
	struct bootsteps_config cfg = { .openrc = openrc, .run = run, .now = clk, .boot_ms = boot_clk,
					.log = log_fn };

	bootsteps_init(b, &cfg);
}

static void changed(struct bootsteps *b)
{
	struct pollfd p = { .fd = bootsteps_fd(b), .events = POLLIN };

	CHECK(poll(&p, 1, 1000) == 1);
	bootsteps_handle(b);
}

static bool logged(const char *s)
{
	int i;

	for (i = 0; i < g_nlog; i++)
		if (!strcmp(g_log[i], s))
			return true;
	return false;
}

static void test_initial(void)
{
	struct bootsteps b;

	tree_new(true);
	init(&b);
	CHECK(b.t.state[0] == BOOTSTEP_DONE);
	CHECK(bootsteps_finished(&b) == 1 && bootsteps_current(&b) == 1 && !bootsteps_all_finished(&b));
	CHECK(logged("boot splash: storage ready at 5.00 s"));
	CHECK(bootsteps_fd(&b) >= 0);
	CHECK(bootsteps_deadline(&b) == 1000 + BOOTSTEPS_CAP_MS);
	bootsteps_fini(&b);
	CHECK(bootsteps_fd(&b) == -1);
	sh("rm -rf %s", root);
}

static void test_each_row(void)
{
	struct bootsteps b;

	tree_new(true);
	init(&b);
	/* step 2: the marker, an empty file */
	sh("touch %s/modem-ready", run);
	changed(&b);
	CHECK(b.t.state[1] == BOOTSTEP_DONE && bootsteps_current(&b) == 2);
	/* step 3 needs both */
	svc("started", "sensord");
	changed(&b);
	CHECK(b.t.state[2] == BOOTSTEP_PENDING && bootsteps_current(&b) == 2);
	svc("started", "sensors-up");
	changed(&b);
	CHECK(b.t.state[2] == BOOTSTEP_DONE && bootsteps_current(&b) == 3);
	/* step 4 needs both */
	svc("started", "bluetooth");
	changed(&b);
	CHECK(b.t.state[3] == BOOTSTEP_PENDING);
	g_boot = 12310;
	svc("started", "bt-up");
	changed(&b);
	CHECK(b.t.state[3] == BOOTSTEP_DONE && bootsteps_current(&b) == 4);
	CHECK(logged("boot splash: Bluetooth ready at 12.31 s"));
	/* step 5, then step 6 follows at once */
	svc("started", "networkmanager");
	changed(&b);
	CHECK(b.t.state[4] == BOOTSTEP_DONE);
	CHECK(bootsteps_finished(&b) == BOOTSTEPS_N && bootsteps_current(&b) == BOOTSTEPS_N &&
	      bootsteps_all_finished(&b));
	CHECK(bootsteps_fd(&b) == -1 && bootsteps_deadline(&b) == -1);
	CHECK(!b.t.capped);
	bootsteps_fini(&b);
	sh("rm -rf %s", root);
}

static void test_out_of_order(void)
{
	struct bootsteps b;

	tree_new(true);
	svc("started", "networkmanager");
	svc("started", "sensord");
	svc("started", "sensors-up");
	init(&b);	/* already there at the start */
	CHECK(b.t.state[2] == BOOTSTEP_DONE && b.t.state[4] == BOOTSTEP_DONE && b.t.state[1] == BOOTSTEP_PENDING);
	CHECK(bootsteps_finished(&b) == 1 && bootsteps_current(&b) == 1);
	svc("started", "bluetooth");
	svc("started", "bt-up");
	sh("touch %s/modem-ready", run);
	changed(&b);
	CHECK(bootsteps_all_finished(&b));
	bootsteps_fini(&b);
	sh("rm -rf %s", root);
}

static void test_failures(void)
{
	static const struct {
		int step;
		const char *svcs[2];
		const char *label;
	} rows[] = {
		{ 1, { "modem-up", NULL }, "GPS modem failed" },
		{ 2, { "sensord", NULL }, "Sensors failed" },
		{ 2, { "sensors-up", NULL }, "Sensors failed" },
		{ 3, { "bt-up", NULL }, "Bluetooth failed" },
		{ 3, { "bluetooth", NULL }, "Bluetooth failed" },
		{ 4, { "networkmanager", NULL }, "Wi-Fi failed" },
	};
	size_t i;

	for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
		struct bootsteps b;
		char want[64];

		tree_new(true);
		init(&b);
		svc("failed", rows[i].svcs[0]);
		changed(&b);
		CHECK(b.t.state[rows[i].step] == BOOTSTEP_FAILED);
		CHECK(!strcmp(bootsteps_failed_label(rows[i].step), rows[i].label));
		snprintf(want, sizeof(want), "boot splash: %.*s failed at 5.00 s", (int)(strlen(rows[i].label) - 7),
			 rows[i].label);
		CHECK(logged(want));
		/* a failed step counts as finished for the bar and the text */
		CHECK(bootsteps_finished(&b) == (rows[i].step == 1 ? 2 : 1));
		bootsteps_fini(&b);
		sh("rm -rf %s", root);
	}
	CHECK(!strcmp(bootsteps_failed_label(0), "") && !strcmp(bootsteps_failed_label(5), ""));
	CHECK(!strcmp(bootsteps_label(0), "Mounting storage") && !strcmp(bootsteps_label(5), "Loading home"));
}

static void test_sticky(void)
{
	struct bootsteps b;
	int n;

	tree_new(true);
	init(&b);
	sh("touch %s/modem-ready", run);
	changed(&b);
	n = g_nlog;
	/* done, then the marker removed: still done, not logged again */
	sh("rm -f %s/modem-ready", run);
	changed(&b);
	CHECK(b.t.state[1] == BOOTSTEP_DONE && bootsteps_finished(&b) == 2 && g_nlog == n);
	/* done, then a failed entry: still done */
	svc("failed", "modem-up");
	changed(&b);
	CHECK(b.t.state[1] == BOOTSTEP_DONE && g_nlog == n);
	/* failed, then started: still failed */
	svc("failed", "networkmanager");
	changed(&b);
	CHECK(b.t.state[4] == BOOTSTEP_FAILED);
	n = g_nlog;
	svc("started", "networkmanager");
	unsvc("failed", "networkmanager");
	changed(&b);
	CHECK(b.t.state[4] == BOOTSTEP_FAILED && g_nlog == n);
	bootsteps_fini(&b);
	sh("rm -rf %s", root);
}

static void test_missing_dirs(void)
{
	struct bootsteps b;

	tree_new(false);
	init(&b);
	CHECK(b.t.state[0] == BOOTSTEP_DONE && bootsteps_fd(&b) >= 0);
	CHECK(bootsteps_deadline(&b) == 2000);	/* the retry, before the cap */
	g_now = 1500;
	bootsteps_tick(&b);	/* not yet due */
	CHECK(bootsteps_deadline(&b) == 2000);
	/* the trees appear without any event on a watch */
	sh("mkdir -p %s/started %s/failed %s", openrc, openrc, run);
	svc("started", "networkmanager");
	g_now = 2000;
	bootsteps_tick(&b);
	CHECK(b.t.state[4] == BOOTSTEP_DONE);
	CHECK(b.t.retry_at == -1 && bootsteps_deadline(&b) == 1000 + BOOTSTEPS_CAP_MS);
	/* and now watched: a marker is seen by its event */
	sh("touch %s/modem-ready", run);
	changed(&b);
	CHECK(b.t.state[1] == BOOTSTEP_DONE);
	bootsteps_fini(&b);
	sh("rm -rf %s", root);
}

static void test_dir_replaced(void)
{
	struct bootsteps b;

	tree_new(true);
	init(&b);
	sh("rm -rf %s/started && mkdir %s/started", openrc, openrc);
	changed(&b);	/* IN_DELETE_SELF/IN_IGNORED: watched again at once */
	CHECK(b.t.wd[0] >= 0 && b.t.retry_at == -1);
	svc("started", "networkmanager");
	changed(&b);
	CHECK(b.t.state[4] == BOOTSTEP_DONE);
	bootsteps_fini(&b);
	sh("rm -rf %s", root);
}

static void test_no_inotify(void)
{
	struct bootsteps b;
	struct rlimit old, lim;
	int fd = dup(0);

	/* no descriptor left for inotify at init: sources are still read, retried each second */
	tree_new(true);
	svc("started", "networkmanager");
	getrlimit(RLIMIT_NOFILE, &old);
	lim = old;
	lim.rlim_cur = fd + 1;
	CHECK(setrlimit(RLIMIT_NOFILE, &lim) == 0);
	init(&b);
	CHECK(bootsteps_fd(&b) == -1 && b.t.state[4] == BOOTSTEP_DONE && b.t.retry_at == 2000);
	CHECK(bootsteps_deadline(&b) == 2000);
	/* system() needs descriptors: lift the limit around it */
	CHECK(setrlimit(RLIMIT_NOFILE, &old) == 0);
	sh("touch %s/modem-ready", run);
	CHECK(setrlimit(RLIMIT_NOFILE, &lim) == 0);
	g_now = 2000;
	bootsteps_tick(&b);	/* still no descriptor: the rescan alone finds it */
	CHECK(b.t.state[1] == BOOTSTEP_DONE && bootsteps_fd(&b) == -1 && b.t.retry_at == 3000);
	CHECK(setrlimit(RLIMIT_NOFILE, &old) == 0);
	g_now = 3000;
	bootsteps_tick(&b);
	CHECK(bootsteps_fd(&b) >= 0 && b.t.retry_at == -1);
	svc("started", "sensord");
	svc("started", "sensors-up");
	changed(&b);
	CHECK(b.t.state[2] == BOOTSTEP_DONE);
	bootsteps_fini(&b);
	close(fd);
	sh("rm -rf %s", root);
}

static void test_cap(void)
{
	struct bootsteps b;

	tree_new(true);
	init(&b);
	svc("started", "networkmanager");
	changed(&b);
	g_now = 1000 + BOOTSTEPS_CAP_MS - 1;
	bootsteps_tick(&b);
	CHECK(!b.t.capped);
	g_now = 1000 + BOOTSTEPS_CAP_MS;
	bootsteps_tick(&b);
	CHECK(b.t.capped && bootsteps_fd(&b) == -1 && bootsteps_deadline(&b) == -1);
	CHECK(logged("boot splash: Starting GPS modem not finished after 30 s"));
	CHECK(logged("boot splash: Starting sensors not finished after 30 s"));
	CHECK(logged("boot splash: Starting Bluetooth not finished after 30 s"));
	CHECK(!logged("boot splash: Starting Wi-Fi not finished after 30 s"));
	CHECK(!logged("boot splash: Loading home not finished after 30 s"));
	{
		int n = g_nlog;

		bootsteps_tick(&b);
		CHECK(g_nlog == n);
	}
	bootsteps_fini(&b);
	sh("rm -rf %s", root);

	/* a link that came without an event is found by the cap's last look */
	tree_new(true);
	init(&b);
	sh("touch %s/modem-ready", run);
	g_now = 1000 + BOOTSTEPS_CAP_MS;
	bootsteps_tick(&b);
	CHECK(b.t.state[1] == BOOTSTEP_DONE && b.t.capped && !logged("boot splash: Starting GPS modem not finished after 30 s"));
	bootsteps_fini(&b);
	sh("rm -rf %s", root);

	/* the cap does not fire once everything finished */
	tree_new(true);
	init(&b);
	sh("touch %s/modem-ready", run);
	svc("started", "networkmanager");
	svc("started", "sensord");
	svc("started", "sensors-up");
	svc("started", "bt-up");
	svc("started", "bluetooth");
	bootsteps_rescan(&b);
	g_now += BOOTSTEPS_CAP_MS * 2;
	bootsteps_tick(&b);
	CHECK(!b.t.capped && bootsteps_all_finished(&b));
	bootsteps_fini(&b);
	sh("rm -rf %s", root);
}

static void test_symlinks_not_followed(void)
{
	struct bootsteps b;

	tree_new(true);
	/* a dangling link counts; the target is never read */
	svc("started", "networkmanager");
	init(&b);
	CHECK(b.t.state[4] == BOOTSTEP_DONE);
	bootsteps_fini(&b);
	sh("rm -rf %s", root);
}

int main(void)
{
	test_initial();
	test_each_row();
	test_out_of_order();
	test_failures();
	test_sticky();
	test_missing_dirs();
	test_dir_replaced();
	test_no_inotify();
	test_cap();
	test_symlinks_not_followed();
	return check_report("test_bootsteps");
}
