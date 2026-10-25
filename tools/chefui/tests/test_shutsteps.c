/*
 * test_shutsteps.c - sys/shutsteps.c (the shutdown screen's step tracker
 * without LVGL) against temporary OpenRC and run trees with an injected
 * clock: every row of the table, the four state directories, a service
 * still stopping, steps finishing out of order, a service never started,
 * softlevel written in place and by rename, the labels and every reason
 * (including a missing and a garbage marker), and the log lines. The
 * machinery shared with the boot splash (missing directories, a replaced
 * directory, no inotify, the retry) is tested once, in test_bootsteps.c.
 * Built without LVGL's include path (Makefile: test-shutsteps).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "check.h"
#include "shutsteps.h"
#include "steptest.h"

static const char *const dirs[] = { "started", "starting", "stopping", "inactive", NULL };

/* A shutting-down tree: every service started, softlevel still default. */
static void tree_new(bool with_dirs)
{
	steptest_tree(with_dirs ? dirs : NULL, 125020);
}

static void softlevel(const char *text)
{
	sh("printf '%s' > %s/softlevel", text, openrc);
}

static void init(struct shutsteps *s)
{
	struct shutsteps_config cfg = { .openrc = openrc, .now = clk, .boot_ms = boot_clk, .log = log_fn };

	shutsteps_init(s, &cfg);
}

static void changed(struct shutsteps *s)
{
	CHECK(steptest_readable(shutsteps_fd(s)));
	shutsteps_handle(s);
}

static bool done(const struct shutsteps *s, int step)
{
	return s->t.state[step] == STEPTRACK_DONE;
}

/* lstat is wrapped (Makefile: -Wl,--wrap=lstat): a service's move from one
 * state directory to the next, made in the middle of a scan. When a probe
 * of <openrc>/<g_move_to>/networkmanager has just come back, the service
 * is linked into g_move_to and unlinked from g_move_from once, as OpenRC
 * does it (new state first); the probe's own answer is unchanged. */
int __real_lstat(const char *path, struct stat *st);
static const char *g_move_from, *g_move_to;

int __wrap_lstat(const char *path, struct stat *st)
{
	int rc = __real_lstat(path, st);
	char want[512];

	if (g_move_to) {
		snprintf(want, sizeof(want), "%s/%s/networkmanager", openrc, g_move_to);
		if (!strcmp(path, want)) {
			const char *from = g_move_from, *to = g_move_to;

			g_move_from = g_move_to = NULL;
			svc(to, "networkmanager");
			unsvc(from, "networkmanager");
		}
	}
	return rc;
}

/* The services of steps 2 to 5, all started. */
static void start_all(void)
{
	svc("started", "networkmanager");
	svc("started", "bt-up");
	svc("started", "bluetooth");
	svc("started", "sensord");
	svc("started", "sensors-up");
	svc("started", "modem-up");
}

static void test_initial(void)
{
	struct shutsteps s;

	tree_new(true);
	start_all();
	init(&s);
	CHECK(!done(&s, 0) && !done(&s, 1) && !done(&s, 2) && !done(&s, 3) && !done(&s, 4));
	CHECK(shutsteps_finished(&s) == 0 && shutsteps_current(&s) == 0);
	CHECK(g_nlog == 0);
	CHECK(shutsteps_fd(&s) >= 0);
	CHECK(shutsteps_deadline(&s) == -1);
	shutsteps_fini(&s);
	CHECK(shutsteps_fd(&s) == -1);
	sh("rm -rf %s", root);
}

/* every row of the table: its sources, and only them, finish it */
static void test_each_row(void)
{
	struct shutsteps s;

	tree_new(true);
	start_all();
	init(&s);

	/* step 1: softlevel reads shutdown (first line, newline ignored) */
	softlevel("default\n");
	shutsteps_rescan(&s);
	CHECK(!done(&s, 0));
	softlevel("shutdown\n");
	shutsteps_rescan(&s);
	CHECK(done(&s, 0) && shutsteps_current(&s) == 1 && shutsteps_finished(&s) == 1);
	CHECK(logged("shutdown screen: state saved at 125.02 s"));

	/* step 2: networkmanager */
	unsvc("started", "networkmanager");
	shutsteps_rescan(&s);
	CHECK(done(&s, 1) && shutsteps_current(&s) == 2);
	CHECK(logged("shutdown screen: Wi-Fi stopped at 125.02 s"));

	/* step 3 needs bt-up and bluetooth */
	unsvc("started", "bt-up");
	shutsteps_rescan(&s);
	CHECK(!done(&s, 2) && shutsteps_current(&s) == 2);
	unsvc("started", "bluetooth");
	shutsteps_rescan(&s);
	CHECK(done(&s, 2) && shutsteps_current(&s) == 3);
	CHECK(logged("shutdown screen: Bluetooth stopped at 125.02 s"));

	/* step 4 needs sensord and sensors-up */
	unsvc("started", "sensors-up");
	shutsteps_rescan(&s);
	CHECK(!done(&s, 3));
	unsvc("started", "sensord");
	shutsteps_rescan(&s);
	CHECK(done(&s, 3) && shutsteps_current(&s) == 4);
	CHECK(logged("shutdown screen: sensors stopped at 125.02 s"));

	/* step 5: modem-up; the modem-ready marker is not a source */
	sh("touch %s/modem-ready", run);
	shutsteps_rescan(&s);
	CHECK(!done(&s, 4));
	CHECK(logged("shutdown screen: GPS modem stopped at 125.02 s") == false);
	unsvc("started", "modem-up");
	shutsteps_rescan(&s);
	CHECK(done(&s, 4) && logged("shutdown screen: GPS modem stopped at 125.02 s"));

	/* step 6 is shown: current is 5, the bar is full, nothing is watched */
	CHECK(shutsteps_current(&s) == 5 && shutsteps_finished(&s) == SHUTSTEPS_N);
	CHECK(shutsteps_fd(&s) == -1 && shutsteps_deadline(&s) == -1);
	CHECK(g_nlog == 5);
	sh("rm -rf %s", root);
}

/* a service counts as stopped only when absent from all four directories */
static void test_state_directories(void)
{
	struct shutsteps s;
	int i;

	tree_new(true);
	softlevel("shutdown\n");
	for (i = 0; i < 4; i++) {
		svc(dirs[i], "networkmanager");
		init(&s);
		CHECK(!done(&s, 1));
		unsvc(dirs[i], "networkmanager");
		shutsteps_rescan(&s);
		CHECK(done(&s, 1));
		shutsteps_fini(&s);
		g_nlog = 0;
	}
	sh("rm -rf %s", root);
}

/* stopping/ is not stopped yet; the event path sees it leave */
static void test_stopping_then_gone(void)
{
	struct shutsteps s;

	tree_new(true);
	svc("started", "modem-up");
	svc("stopping", "modem-up");
	softlevel("shutdown\n");
	init(&s);
	CHECK(done(&s, 0) && !done(&s, 4));
	unsvc("started", "modem-up");
	changed(&s);
	CHECK(!done(&s, 4));	/* still in stopping/ */
	unsvc("stopping", "modem-up");
	changed(&s);
	CHECK(done(&s, 4));
	shutsteps_fini(&s);
	sh("rm -rf %s", root);
}

/* steps finish out of order; the shown step is the first unfinished one */
static void test_out_of_order(void)
{
	struct shutsteps s;

	tree_new(true);
	start_all();
	init(&s);
	/* chef-state shutdown stops sensord before openrc shutdown begins */
	unsvc("started", "sensord");
	unsvc("started", "sensors-up");
	changed(&s);
	CHECK(done(&s, 3) && shutsteps_finished(&s) == 0 && shutsteps_current(&s) == 0);
	unsvc("started", "modem-up");
	changed(&s);
	CHECK(done(&s, 4) && shutsteps_current(&s) == 0);
	softlevel("shutdown\n");
	changed(&s);
	CHECK(done(&s, 0) && shutsteps_finished(&s) == 1 && shutsteps_current(&s) == 1);
	unsvc("started", "bt-up");
	unsvc("started", "bluetooth");
	changed(&s);
	CHECK(done(&s, 2) && !done(&s, 1) && shutsteps_finished(&s) == 1);
	unsvc("started", "networkmanager");
	changed(&s);
	CHECK(shutsteps_finished(&s) == SHUTSTEPS_N && shutsteps_current(&s) == 5);
	shutsteps_fini(&s);
	sh("rm -rf %s", root);
}

/* a service that failed at boot or never started is already stopped */
static void test_never_started(void)
{
	struct shutsteps s;

	tree_new(true);
	sh("mkdir -p %s/failed", openrc);
	svc("failed", "modem-up");
	softlevel("shutdown\n");
	init(&s);
	CHECK(done(&s, 0) && done(&s, 1) && done(&s, 2) && done(&s, 3) && done(&s, 4));
	CHECK(shutsteps_current(&s) == 5 && shutsteps_finished(&s) == SHUTSTEPS_N);
	CHECK(g_nlog == 5);
	shutsteps_fini(&s);
	sh("rm -rf %s", root);
}

/* softlevel written in place and by rename; other contents never count */
static void test_softlevel_writes(void)
{
	struct shutsteps s;

	tree_new(true);
	start_all();
	init(&s);
	/* in place: created empty, then written (CLOSE_WRITE) */
	sh("printf 'default\\n' > %s/softlevel", openrc);
	changed(&s);
	CHECK(!done(&s, 0));
	sh("printf 'shutdown\\n' > %s/softlevel", openrc);
	changed(&s);
	CHECK(done(&s, 0));
	shutsteps_fini(&s);

	/* by rename */
	sh("rm -f %s/softlevel", openrc);
	init(&s);
	CHECK(!done(&s, 0));
	sh("printf 'shutdown\\n' > %s/softlevel.tmp && mv %s/softlevel.tmp %s/softlevel", openrc, openrc, openrc);
	changed(&s);
	CHECK(done(&s, 0));
	shutsteps_fini(&s);

	/* first line only, and no prefix match */
	sh("rm -f %s/softlevel", openrc);
	softlevel("shutdown-not\n");
	init(&s);
	CHECK(!done(&s, 0));
	softlevel("default\nshutdown\n");
	shutsteps_rescan(&s);
	CHECK(!done(&s, 0));
	softlevel("shutdown");	/* no newline */
	shutsteps_rescan(&s);
	CHECK(done(&s, 0));
	shutsteps_fini(&s);

	/* a symlink is never followed */
	sh("rm -f %s/softlevel; printf 'shutdown\\n' > %s/real; ln -s real %s/softlevel", openrc, root, openrc);
	init(&s);
	CHECK(!done(&s, 0));
	shutsteps_fini(&s);
	sh("rm -rf %s", root);
}

static void test_labels(void)
{
	CHECK(!strcmp(shutsteps_label(0, SHUTSTEPS_POWEROFF), "Saving state"));
	CHECK(!strcmp(shutsteps_label(1, SHUTSTEPS_POWEROFF), "Stopping Wi-Fi"));
	CHECK(!strcmp(shutsteps_label(2, SHUTSTEPS_POWEROFF), "Stopping Bluetooth"));
	CHECK(!strcmp(shutsteps_label(3, SHUTSTEPS_POWEROFF), "Stopping sensors"));
	CHECK(!strcmp(shutsteps_label(4, SHUTSTEPS_POWEROFF), "Stopping GPS modem"));
	CHECK(!strcmp(shutsteps_label(5, SHUTSTEPS_POWEROFF), "Powering off"));
	CHECK(!strcmp(shutsteps_label(5, SHUTSTEPS_REBOOT), "Restarting"));
	CHECK(!strcmp(shutsteps_label(5, SHUTSTEPS_UNKNOWN), "Shutting down"));
	/* the reason only matters for step 6 */
	CHECK(!strcmp(shutsteps_label(4, SHUTSTEPS_REBOOT), "Stopping GPS modem"));
	CHECK(!strcmp(shutsteps_label(-1, SHUTSTEPS_REBOOT), "") && !strcmp(shutsteps_label(6, SHUTSTEPS_REBOOT), ""));
}

static void marker(const char *text)
{
	sh("printf '%s' > %s/shutting-down", text, run);
}

static void test_reasons(void)
{
	bool present;

	tree_new(true);
	/* missing */
	CHECK(shutsteps_read_reason(run, &present) == SHUTSTEPS_UNKNOWN && !present);
	CHECK(shutsteps_read_reason(run, NULL) == SHUTSTEPS_UNKNOWN);
	marker("poweroff\n");
	CHECK(shutsteps_read_reason(run, &present) == SHUTSTEPS_POWEROFF && present);
	marker("reboot\n");
	CHECK(shutsteps_read_reason(run, &present) == SHUTSTEPS_REBOOT && present);
	marker("reboot");
	CHECK(shutsteps_read_reason(run, NULL) == SHUTSTEPS_REBOOT);
	marker("unknown\n");
	CHECK(shutsteps_read_reason(run, &present) == SHUTSTEPS_UNKNOWN && present);
	/* garbage: present, unknown */
	marker("");
	CHECK(shutsteps_read_reason(run, &present) == SHUTSTEPS_UNKNOWN && present);
	marker("halt\n");
	CHECK(shutsteps_read_reason(run, &present) == SHUTSTEPS_UNKNOWN && present);
	marker("poweroffX\n");
	CHECK(shutsteps_read_reason(run, NULL) == SHUTSTEPS_UNKNOWN);
	marker("\npoweroff\n");
	CHECK(shutsteps_read_reason(run, NULL) == SHUTSTEPS_UNKNOWN);
	sh("printf '\\377\\376\\000poweroff' > %s/shutting-down", run);
	CHECK(shutsteps_read_reason(run, NULL) == SHUTSTEPS_UNKNOWN);
	/* a symlink is not followed; a directory is unreadable */
	sh("rm -f %s/shutting-down; printf 'reboot\\n' > %s/real; ln -s real %s/shutting-down", run, run, run);
	CHECK(shutsteps_read_reason(run, &present) == SHUTSTEPS_UNKNOWN && present);
	sh("rm -f %s/shutting-down; mkdir %s/shutting-down", run, run);
	CHECK(shutsteps_read_reason(run, &present) == SHUTSTEPS_UNKNOWN && present);
	/* exists but cannot be read (root reads anything): present, unknown */
	if (geteuid() != 0) {
		sh("rmdir %s/shutting-down; printf 'reboot\\n' > %s/shutting-down; chmod 000 %s/shutting-down", run, run, run);
		CHECK(shutsteps_read_reason(run, &present) == SHUTSTEPS_UNKNOWN && present);
	}
	CHECK(!strcmp(shutsteps_reason_name(SHUTSTEPS_POWEROFF), "poweroff"));
	CHECK(!strcmp(shutsteps_reason_name(SHUTSTEPS_REBOOT), "reboot"));
	CHECK(!strcmp(shutsteps_reason_name(SHUTSTEPS_UNKNOWN), "unknown"));
	sh("rm -rf %s", root);
}

/* OpenRC links a service's new state before it unlinks the old one: a scan
 * between every link and unlink never sees the service stopped early. */
static void test_lifecycle_never_stopped_early(void)
{
	struct shutsteps s;

	tree_new(true);
	softlevel("shutdown\n");
	svc("starting", "networkmanager");
	init(&s);
	/* starting -> started */
	CHECK(!done(&s, 1));
	svc("started", "networkmanager");
	shutsteps_rescan(&s);
	CHECK(!done(&s, 1));
	unsvc("starting", "networkmanager");
	shutsteps_rescan(&s);
	CHECK(!done(&s, 1));
	/* started -> inactive -> stopping -> gone */
	svc("inactive", "networkmanager");
	shutsteps_rescan(&s);
	CHECK(!done(&s, 1));
	unsvc("started", "networkmanager");
	shutsteps_rescan(&s);
	CHECK(!done(&s, 1));
	svc("stopping", "networkmanager");
	shutsteps_rescan(&s);
	CHECK(!done(&s, 1));
	unsvc("inactive", "networkmanager");
	shutsteps_rescan(&s);
	CHECK(!done(&s, 1));
	unsvc("stopping", "networkmanager");
	shutsteps_rescan(&s);
	CHECK(done(&s, 1));
	shutsteps_fini(&s);
	sh("rm -rf %s", root);
}

/* The same move inside one scan (the probe order matters here): a scan that
 * looks at the new state first, with the old one still there, never calls
 * the service stopped; one that looks at the target first would. */
static void test_move_inside_a_scan(void)
{
	static const struct { const char *from, *to; } moves[] = {
		{ "starting", "started" }, { "inactive", "stopping" }, { "started", "stopping" },
	};
	struct shutsteps s;
	unsigned i;

	for (i = 0; i < sizeof(moves) / sizeof(moves[0]); i++) {
		tree_new(true);
		softlevel("shutdown\n");
		svc(moves[i].from, "networkmanager");
		init(&s);
		CHECK(!done(&s, 1));
		g_move_from = moves[i].from;
		g_move_to = moves[i].to;
		shutsteps_rescan(&s);
		CHECK(!done(&s, 1));
		g_move_from = g_move_to = NULL;
		shutsteps_fini(&s);
		sh("rm -rf %s", root);
	}
}

/* The tracker starts with no descriptor of its own: fd 0 of the process
 * (here /dev/zero) is neither closed by init nor by fini. */
static void test_fd0_survives(void)
{
	struct shutsteps s;
	int saved = dup(0), z = open("/dev/zero", O_RDONLY);

	CHECK(saved >= 0 && z >= 0 && dup2(z, 0) == 0);
	close(z);
	tree_new(true);
	init(&s);
	CHECK(shutsteps_fd(&s) > 0 && fcntl(0, F_GETFD) >= 0);
	shutsteps_fini(&s);
	CHECK(fcntl(0, F_GETFD) >= 0);
	dup2(saved, 0);
	close(saved);
	sh("rm -rf %s", root);
}

int main(void)
{
	test_initial();
	test_each_row();
	test_state_directories();
	test_stopping_then_gone();
	test_out_of_order();
	test_never_started();
	test_softlevel_writes();
	test_labels();
	test_reasons();
	test_lifecycle_never_stopped_early();
	test_move_inside_a_scan();
	test_fd0_survives();
	return check_report("test_shutsteps");
}
