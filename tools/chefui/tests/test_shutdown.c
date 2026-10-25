/*
 * test_shutdown.c - the shutdown screen (apps/settings/shutdown.c) over
 * the app on a headless LVGL display with test-controlled time: the frozen
 * captures' texts and fills, the screen shown at start and on the marker's
 * arrival, the walk through the six steps over temporary OpenRC and run
 * trees, the reasons, the panel lit from off, touch swallowed, Power short
 * ignored, auto-sleep held off, the boot splash replaced, the marker's
 * removal back to the hub, a missing run directory, and every fd and timer
 * released at the end.
 */
#include "covertest.h"
#include "shutdown.h"
#include "splash.h"

static int changed_calls, ended_calls;
static char done_dir[256];

static void changed(void)
{
	changed_calls++;
	app_splash_ended();
}

static void splash_ended(void)
{
	ended_calls++;
	app_splash_ended();
}

static void tree_new(void)
{
	covertest_tree("shutdown");
	snprintf(done_dir, sizeof(done_dir), "%s/chef-settings", run);
	sh("mkdir -p %s/started %s/starting %s/stopping %s/inactive %s", openrc, openrc, openrc, openrc, run);
	changed_calls = ended_calls = 0;
}

static void tree_drop(void)
{
	sh("rm -rf %s", root);
}

static struct shutdown_config cfg(int frozen, enum shutsteps_reason reason)
{
	struct shutdown_config c = { .parent = lv_layer_sys(), .openrc = openrc, .run = run, .frozen = frozen,
				     .frozen_reason = reason, .now = pf_now, .boot_ms = pf_now, .log = log_fn,
				     .screen_is_on = pf_on, .screen_set = pf_set, .changed = changed };

	return c;
}

/* The shutdown cover is the last child of the system layer. */
static lv_obj_t *cover(void)
{
	lv_obj_t *l = lv_layer_sys();

	return lv_obj_get_child(l, lv_obj_get_child_count(l) - 1);
}

static bool shows(const char *step, const char *count, int fill)
{
	return cover_shows(cover(), step, count, fill);
}

static int timers(void)
{
	int n = 0;

	for (lv_timer_t *t = lv_timer_get_next(NULL); t; t = lv_timer_get_next(t))
		n++;
	return n;
}

/* The marker, made as chef-state makes it: a temp file, renamed. */
static void marker_make(const char *reason)
{
	sh("printf '%s\\n' > %s/shutting-down.tmp && mv -f %s/shutting-down.tmp %s/shutting-down", reason, run, run, run);
}

static void marker_drop(void)
{
	sh("rm -f %s/shutting-down", run);
}

static void softlevel(const char *level)
{
	sh("printf '%s\\n' > %s/softlevel", level, openrc);
}

static void all_started(void)
{
	svc("started", "networkmanager");
	svc("started", "bt-up");
	svc("started", "bluetooth");
	svc("started", "sensord");
	svc("started", "sensors-up");
	svc("started", "modem-up");
}

/* --------------------------------------------------------------- tests */

static void test_frozen(void)
{
	static const struct { int k; enum shutsteps_reason why; const char *step, *count; int fill; } rows[] = {
		{ 0, SHUTSTEPS_POWEROFF, "Saving state\xE2\x80\xA6", "1 / 6", 0 },
		{ 1, SHUTSTEPS_POWEROFF, "Stopping Wi-Fi\xE2\x80\xA6", "2 / 6", 140 },
		{ 4, SHUTSTEPS_POWEROFF, "Stopping GPS modem\xE2\x80\xA6", "5 / 6", 560 },
		{ 5, SHUTSTEPS_POWEROFF, "Powering off\xE2\x80\xA6", "6 / 6", 840 },
		{ 6, SHUTSTEPS_REBOOT, "Restarting\xE2\x80\xA6", "6 / 6", 840 },
		{ 6, SHUTSTEPS_UNKNOWN, "Shutting down\xE2\x80\xA6", "6 / 6", 840 },
	};
	int timers_before = timers();

	tree_new();
	sys_state.device.metadata_known = true;
	strcpy(sys_state.device.version, "v0.9");
	strcpy(sys_state.device.commit, "96e8c87f00000000000000000000000000000000");
	for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
		struct shutdown_config c = cfg(rows[i].k, rows[i].why);

		CHECK(shutdown_start(&c) && shutdown_active());
		CHECK(shows(rows[i].step, rows[i].count, rows[i].fill));
		CHECK(!strcmp(cover_text(cover(), 1), "v0.9 \xC2\xB7 96e8c87f"));
		CHECK(fakeloop_watched() == 0 && nlogs == 0 && changed_calls == 0);
		advance(60000);		/* nothing ends it */
		CHECK(shutdown_active() && screen_on);
		shutdown_stop();
		CHECK(!shutdown_active() && nlogs == 0);
	}
	CHECK(timers() == timers_before);
	sys_state.device.metadata_known = false;
	tree_drop();
}

static void test_shown_at_start(void)
{
	struct shutdown_config c;
	int timers_before = timers();

	tree_new();
	all_started();
	marker_make("poweroff");
	c = cfg(-1, SHUTSTEPS_UNKNOWN);
	CHECK(shutdown_start(&c) && shutdown_active());
	CHECK(lv_obj_get_parent(cover()) == lv_layer_sys() && lv_obj_is_clickable(cover()));
	CHECK(shows("Saving state\xE2\x80\xA6", "1 / 6", 0));
	CHECK(logged("shutdown screen: shown at") && logged("s (poweroff)"));
	CHECK(changed_calls == 1);
	CHECK(fakeloop_watched() == 2);		/* the marker and the steps */
	shutdown_stop();
	CHECK(!shutdown_active() && fakeloop_watched() == 0 && timers() == timers_before);
	CHECK(logged("shutdown screen: ended at") && logged("(terminated)"));
	tree_drop();
}

static void test_walk(void)
{
	struct shutdown_config c;

	tree_new();
	all_started();
	c = cfg(-1, SHUTSTEPS_UNKNOWN);
	CHECK(shutdown_start(&c));
	CHECK(!shutdown_active() && fakeloop_watched() == 1 && nlogs == 0);
	marker_make("poweroff");
	advance(20);
	CHECK(shutdown_active() && shows("Saving state\xE2\x80\xA6", "1 / 6", 0) && fakeloop_watched() == 2);
	/* chef-state shutdown stops sensord before openrc shutdown begins: out of order */
	unsvc("started", "sensord");
	unsvc("started", "sensors-up");
	advance(20);
	CHECK(shows("Saving state\xE2\x80\xA6", "1 / 6", 0));
	softlevel("shutdown");
	advance(20);
	CHECK(shows("Stopping Wi-Fi\xE2\x80\xA6", "2 / 6", 140) && logged("shutdown screen: state saved at"));
	unsvc("started", "networkmanager");
	advance(20);
	CHECK(shows("Stopping Bluetooth\xE2\x80\xA6", "3 / 6", 280) && logged("shutdown screen: Wi-Fi stopped at"));
	unsvc("started", "bt-up");
	advance(20);
	CHECK(shows("Stopping Bluetooth\xE2\x80\xA6", "3 / 6", 280));
	unsvc("started", "bluetooth");
	advance(20);
	/* sensors were already stopped: the walk skips to the modem */
	CHECK(shows("Stopping GPS modem\xE2\x80\xA6", "5 / 6", 560) && logged("shutdown screen: Bluetooth stopped at") &&
	      logged("shutdown screen: sensors stopped at"));
	unsvc("started", "modem-up");
	advance(20);
	CHECK(shows("Powering off\xE2\x80\xA6", "6 / 6", 840) && logged("shutdown screen: GPS modem stopped at"));
	CHECK(fakeloop_watched() == 1);		/* the steps are all through: only the marker */
	advance(60000);				/* step 6 stays until chef-settings stops */
	CHECK(shutdown_active() && shows("Powering off\xE2\x80\xA6", "6 / 6", 840));
	shutdown_stop();
	CHECK(fakeloop_watched() == 0);
	tree_drop();
}

static void test_reasons(void)
{
	static const struct { const char *text, *label; } rows[] = {
		{ "poweroff", "Powering off\xE2\x80\xA6" }, { "reboot", "Restarting\xE2\x80\xA6" },
		{ "unknown", "Shutting down\xE2\x80\xA6" }, { "halt", "Shutting down\xE2\x80\xA6" },
	};

	for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
		struct shutdown_config c;

		tree_new();
		softlevel("shutdown");
		marker_make(rows[i].text);
		c = cfg(-1, SHUTSTEPS_UNKNOWN);
		CHECK(shutdown_start(&c));
		CHECK(shows(rows[i].label, "6 / 6", 840));
		shutdown_stop();
		tree_drop();
	}
}

static void test_screen_off_start(void)
{
	struct shutdown_config c;
	int sets;

	tree_new();
	all_started();
	pf_set(false);
	sets = screen_sets;
	c = cfg(-1, SHUTSTEPS_UNKNOWN);
	CHECK(shutdown_start(&c) && !screen_on);
	marker_make("poweroff");
	advance(20);
	CHECK(shutdown_active() && screen_on && screen_sets == sets + 1);	/* lit by the screen path */
	CHECK(shows("Saving state\xE2\x80\xA6", "1 / 6", 0));
	/* already lit: no second call */
	sets = screen_sets;
	marker_drop();
	advance(20);
	marker_make("poweroff");
	advance(20);
	CHECK(shutdown_active() && screen_sets == sets);
	shutdown_stop();
	tree_drop();
}

static void test_input_and_sleep(void)
{
	struct shutdown_config c;
	int64_t last;
	int sets, depth;

	tree_new();
	all_started();
	app_set_sleep(30);
	app_open(APP_PAGE_DISPLAY, "Display");	/* depth 1: Power would be Back */
	depth = app_depth();
	advance(10000);
	c = cfg(-1, SHUTSTEPS_UNKNOWN);
	CHECK(shutdown_start(&c));
	marker_make("poweroff");
	advance(20);
	CHECK(shutdown_active());
	last = app.sleep.last_ms;
	app_on_touch_frame(&(struct chefui_contact){ .id = 0, .x = 5, .y = 5 }, 1);
	CHECK(app.sleep.last_ms == last);	/* swallowed: no activity */
	CHECK(app_on_button("power.short") && app_depth() == depth);	/* claimed and ignored: no Back, no screen off */
	sets = screen_sets;
	advance(60000);				/* well past auto-sleep */
	CHECK(screen_on && screen_sets == sets);
	marker_drop();				/* a chef-reboot that gave up */
	advance(20);
	CHECK(!shutdown_active() && changed_calls == 2 && app.sleep.last_ms > last);
	CHECK(logged("shutdown screen: ended at") && logged("(marker removed)"));
	CHECK(app_on_button("power.short"));	/* Back, as on any page */
	advance(25000);
	CHECK(screen_on);
	advance(8000);
	CHECK(!screen_on);			/* auto-sleep runs again */
	pf_set(true);
	app_back();
	shutdown_stop();
	tree_drop();
}

static void test_marker_removal(void)
{
	struct shutdown_config c;
	lv_obj_t *layer = lv_layer_sys();
	uint32_t children = lv_obj_get_child_count(layer);

	tree_new();
	all_started();
	marker_make("reboot");
	c = cfg(-1, SHUTSTEPS_UNKNOWN);
	CHECK(shutdown_start(&c) && shutdown_active());
	CHECK(lv_obj_get_child_count(layer) == children + 1);
	marker_drop();
	advance(20);
	CHECK(!shutdown_active() && lv_obj_get_child_count(layer) == children);
	CHECK(fakeloop_watched() == 1);		/* the marker watch stays */
	/* and it comes back with a new marker */
	marker_make("poweroff");
	advance(20);
	CHECK(shutdown_active() && shows("Saving state\xE2\x80\xA6", "1 / 6", 0));
	shutdown_stop();
	CHECK(lv_obj_get_child_count(layer) == children);
	tree_drop();
}

static void test_replaces_splash(void)
{
	struct shutdown_config c;
	struct splash_config s;
	char p[300];
	struct stat st;

	tree_new();
	s = (struct splash_config){ .parent = lv_screen_active(), .openrc = openrc, .run = run, .done_dir = done_dir,
				    .image_info = "/nonexistent", .frozen = -1, .now = pf_now, .boot_ms = pf_now,
				    .log = log_fn, .ended = splash_ended };
	CHECK(splash_start(&s) && splash_active());
	c = cfg(-1, SHUTSTEPS_UNKNOWN);
	CHECK(shutdown_start(&c) && !shutdown_active() && splash_active());
	marker_make("poweroff");
	advance(20);
	CHECK(shutdown_active() && !splash_active() && ended_calls == 1);
	CHECK(logged("boot splash: ended at") && logged("boot splash: ended by shutdown at"));
	snprintf(p, sizeof(p), "%s/splash-done", done_dir);
	CHECK(stat(p, &st) == 0);
	shutdown_stop();
	CHECK(fakeloop_watched() == 0);
	tree_drop();
}

static void test_missing_run_dir(void)
{
	struct shutdown_config c;

	tree_new();
	all_started();
	sh("rm -rf %s", run);
	c = cfg(-1, SHUTSTEPS_UNKNOWN);
	CHECK(shutdown_start(&c) && !shutdown_active());
	CHECK(fakeloop_watched() == 1);		/* inotify exists, with no watch yet */
	sh("mkdir -p %s", run);
	advance(1100);				/* the retry watches it */
	CHECK(!shutdown_active());
	marker_make("poweroff");
	advance(20);
	CHECK(shutdown_active());
	shutdown_stop();
	tree_drop();
}

/* The run directory goes away while watched: the marker is still seen once
 * it is back (the watch is made again, not just forgotten). */
static void test_run_dir_replaced(void)
{
	struct shutdown_config c;

	tree_new();
	all_started();
	c = cfg(-1, SHUTSTEPS_UNKNOWN);
	CHECK(shutdown_start(&c) && !shutdown_active());
	sh("rm -rf %s", run);
	advance(20);
	CHECK(!shutdown_active() && fakeloop_watched() == 1);
	sh("mkdir -p %s", run);
	advance(1100);				/* the retry watches it again */
	marker_make("reboot");
	advance(20);
	CHECK(shutdown_active() && shows("Saving state\xE2\x80\xA6", "1 / 6", 0));
	shutdown_stop();
	tree_drop();
}

static void test_start_twice(void)
{
	struct shutdown_config c;

	tree_new();
	c = cfg(-1, SHUTSTEPS_UNKNOWN);
	CHECK(shutdown_start(&c) && !shutdown_start(&c));
	shutdown_stop();
	tree_drop();
}

int main(void)
{
	covertest_app();
	test_frozen();
	test_shown_at_start();
	test_walk();
	test_reasons();
	test_screen_off_start();
	test_input_and_sleep();
	test_marker_removal();
	test_replaces_splash();
	test_missing_run_dir();
	test_run_dir_replaced();
	test_start_twice();
	return check_report("test-shutdown");
}
