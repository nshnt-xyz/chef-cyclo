/*
 * test_splash.c - the boot splash (apps/settings/splash.c) over the app on
 * a headless LVGL display with test-controlled time: the frozen captures'
 * texts and fills, the live walk over temporary OpenRC and run trees (done
 * and failed steps, the failed and Ready holds, the cap), once per boot,
 * the version line, the marker, a screen-off start, touch, Power and
 * auto-sleep during the splash, and every fd and timer released at the end.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app.h"
#include "bootsteps.h"
#include "check.h"
#include "chefui.h"
#include "fakeloop.h"
#include "splash.h"
#include "sys.h"
#include "sysstate.h"
#include "theme.h"
#include "widgets.h"

static int64_t t_now = 100000;
static bool screen_on = true;
static int screen_sets, ended_calls;
static char root[64], openrc[96], run[96], done_dir[128], info[128];
static char logs[32][160];
static int nlogs;

static int64_t pf_now(void) { return t_now; }
static bool pf_on(void) { return screen_on; }

static int pf_set(bool on)
{
	screen_sets++;
	if (on != screen_on) {
		screen_on = on;
		app_on_screen(on);
	}
	return 0;
}

static int pf_bright(int level)
{
	(void)level;
	return 0;
}

static void advance(int ms)
{
	while (ms > 0) {
		int step = ms > 10 ? 10 : ms;

		t_now += step;
		fakeloop_advance((uint32_t)step);
		ms -= step;
	}
}

static void log_fn(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	if (nlogs < 32)
		vsnprintf(logs[nlogs++], sizeof(logs[0]), fmt, ap);
	va_end(ap);
}

static bool logged(const char *sub)
{
	for (int i = 0; i < nlogs; i++)
		if (strstr(logs[i], sub))
			return true;
	return false;
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

static int64_t ended_at;

static void ended(void)
{
	ended_calls++;
	ended_at = t_now;
	app_splash_ended();
}

static void tree_new(void)
{
	snprintf(root, sizeof(root), "/tmp/splash-test-XXXXXX");
	if (!mkdtemp(root))
		abort();
	snprintf(openrc, sizeof(openrc), "%s/openrc", root);
	snprintf(run, sizeof(run), "%s/chef", root);
	snprintf(done_dir, sizeof(done_dir), "%s/chef/chef-settings", root);
	snprintf(info, sizeof(info), "%s/image-info", root);
	sh("mkdir -p %s/started %s/failed %s", openrc, openrc, run);
	nlogs = 0;
	ended_calls = 0;
}

static void tree_drop(void)
{
	sh("rm -rf %s", root);
}

static void svc(const char *dir, const char *name)
{
	sh("ln -s /nonexistent %s/%s/%s", openrc, dir, name);
}

static void all_started(void)
{
	sh("touch %s/modem-ready", run);
	svc("started", "sensord");
	svc("started", "sensors-up");
	svc("started", "bt-up");
	svc("started", "bluetooth");
	svc("started", "networkmanager");
}

static struct splash_config cfg(int frozen)
{
	struct splash_config c = { .parent = lv_screen_active(), .openrc = openrc, .run = run,
				   .done_dir = done_dir, .image_info = info, .frozen = frozen,
				   .now = pf_now, .boot_ms = pf_now, .log = log_fn, .ended = ended };

	return c;
}

static lv_obj_t *cover(void)
{
	lv_obj_t *scr = lv_screen_active();

	return lv_obj_get_child(scr, lv_obj_get_child_count(scr) - 1);
}

/* the labels by their text: the step text and the count are the last two kit_text boxes */
static const char *text_of(int i)
{
	lv_obj_t *c = cover();
	/* children: title, version, step text, count, track */
	return lv_label_get_text(lv_obj_get_child(lv_obj_get_child(c, i), 0));
}

static int fill_w(void)
{
	lv_obj_t *track, *fill;

	lv_obj_update_layout(cover());
	track = lv_obj_get_child(cover(), 4);
	fill = lv_obj_get_child(track, 0);
	return lv_obj_is_hidden(fill) ? 0 : lv_obj_get_width(fill);
}

static bool shows(const char *step, const char *count, int fill)
{
	return !strcmp(text_of(2), step) && !strcmp(text_of(3), count) && fill_w() == fill;
}

static bool marker(void)
{
	char p[200];
	struct stat st;

	snprintf(p, sizeof(p), "%s/splash-done", done_dir);
	return stat(p, &st) == 0;
}

static void write_info(const char *commit, int dirty)
{
	FILE *f = fopen(info, "w");

	fprintf(f, "chef-image-info 1\nname chef-cyclo\nversion dev\ncommit %s\ndirty %d\nepoch 1791449160\n",
		commit, dirty);
	fclose(f);
}

/* --------------------------------------------------------------- tests */

static void test_frozen(void)
{
	static const struct { int k; const char *step, *count; int fill; } rows[] = {
		{ 0, "Mounting storage\xE2\x80\xA6", "1 / 6", 0 },
		{ 1, "Starting GPS modem\xE2\x80\xA6", "2 / 6", 140 },
		{ 5, "Loading home\xE2\x80\xA6", "6 / 6", 700 },
		{ 6, "Ready", "6 / 6", 840 },
	};
	int timers_before = 0;

	for (lv_timer_t *t = lv_timer_get_next(NULL); t; t = lv_timer_get_next(t))
		timers_before++;
	tree_new();
	sys_state.fake_boot = 0;
	sys_state.device.metadata_known = true;
	strcpy(sys_state.device.version, "v0.9");
	strcpy(sys_state.device.commit, "96e8c87f00000000000000000000000000000000");
	for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
		struct splash_config c = cfg(rows[i].k);

		CHECK(splash_start(&c) && splash_active());
		CHECK(shows(rows[i].step, rows[i].count, rows[i].fill));
		CHECK(!strcmp(text_of(1), "v0.9 \xC2\xB7 96e8c87f"));
		CHECK(fakeloop_watched() == 0 && !marker() && nlogs == 0);
		advance(60000);		/* nothing ends it */
		CHECK(splash_active() && ended_calls == 0 && !marker());
		splash_stop();
		CHECK(!splash_active());
	}
	sys_state.device.dirty = true;
	{
		struct splash_config c = cfg(0);

		splash_start(&c);
		CHECK(!strcmp(text_of(1), "v0.9 \xC2\xB7 96e8c87f+dirty"));
		splash_stop();
	}
	sys_state.device.metadata_known = false;
	{
		struct splash_config c = cfg(0);

		splash_start(&c);
		CHECK(!strcmp(text_of(1), ""));
		splash_stop();
	}
	{
		int n = 0;

		for (lv_timer_t *t = lv_timer_get_next(NULL); t; t = lv_timer_get_next(t))
			n++;
		CHECK(n == timers_before);
	}
	tree_drop();
}

static void test_walk(void)
{
	struct splash_config c;

	tree_new();
	write_info("7a13e594aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 0);
	c = cfg(-1);
	CHECK(splash_start(&c));
	CHECK(!strcmp(text_of(1), "dev \xC2\xB7 7a13e594"));
	CHECK(shows("Starting GPS modem\xE2\x80\xA6", "2 / 6", 140));
	CHECK(fakeloop_watched() == 1);
	sh("touch %s/modem-ready", run);
	advance(20);
	CHECK(shows("Starting sensors\xE2\x80\xA6", "3 / 6", 280));
	CHECK(logged("boot splash: GPS modem ready at"));
	/* networkmanager first: out of order, skipped over once it is the turn */
	svc("started", "networkmanager");
	advance(20);
	CHECK(shows("Starting sensors\xE2\x80\xA6", "3 / 6", 280));
	svc("started", "sensord");
	svc("started", "sensors-up");
	advance(20);
	CHECK(shows("Starting Bluetooth\xE2\x80\xA6", "4 / 6", 420));
	svc("started", "bt-up");
	svc("started", "bluetooth");
	advance(20);
	CHECK(shows("Ready", "6 / 6", 840) && splash_active());
	advance(SPLASH_READY_HOLD_MS - 60);
	CHECK(splash_active() && ended_calls == 0 && !marker());
	advance(80);
	CHECK(!splash_active() && ended_calls == 1 && marker());
	CHECK(logged("boot splash: ended at") && !logged("(cap)"));
	CHECK(fakeloop_watched() == 0);
	{
		char p[200];
		struct stat st;

		snprintf(p, sizeof(p), "%s/splash-done", done_dir);
		CHECK(stat(p, &st) == 0 && (st.st_mode & 0777) == 0644 && st.st_size == 0);
	}
	/* once per boot: the marker keeps it away */
	c = cfg(-1);
	CHECK(!splash_start(&c) && !splash_active() && fakeloop_watched() == 0);
	tree_drop();
}

static void test_failed_holds(void)
{
	struct splash_config c;

	tree_new();
	c = cfg(-1);
	svc("failed", "modem-up");
	svc("failed", "sensors-up");
	CHECK(splash_start(&c));
	/* step 2 failed at once: its label for 1.5 s, the bar at 2/6 */
	CHECK(shows("GPS modem failed", "2 / 6", 280));
	advance(SPLASH_FAIL_HOLD_MS - 20);
	CHECK(shows("GPS modem failed", "2 / 6", 280));
	advance(40);
	CHECK(shows("Sensors failed", "3 / 6", 420));
	advance(SPLASH_FAIL_HOLD_MS + 20);
	CHECK(shows("Starting Bluetooth\xE2\x80\xA6", "4 / 6", 420));
	svc("started", "bt-up");
	svc("started", "bluetooth");
	svc("failed", "networkmanager");
	advance(20);
	CHECK(shows("Wi-Fi failed", "5 / 6", 700));
	advance(SPLASH_FAIL_HOLD_MS + 20);
	CHECK(shows("Ready", "6 / 6", 840));
	advance(SPLASH_READY_HOLD_MS + 20);
	CHECK(!splash_active() && marker() && logged("GPS modem failed at") && logged("Sensors failed at") &&
	      logged("Wi-Fi failed at"));
	tree_drop();
}

static void test_cap(void)
{
	struct splash_config c;

	tree_new();
	sh("touch %s/modem-ready", run);
	c = cfg(-1);
	CHECK(splash_start(&c));
	advance(BOOTSTEPS_CAP_MS - 100);
	CHECK(splash_active() && shows("Starting sensors\xE2\x80\xA6", "3 / 6", 280));
	advance(200);
	CHECK(!splash_active() && ended_calls == 1 && marker());
	CHECK(logged("boot splash: Starting sensors not finished after 30 s") &&
	      logged("boot splash: Starting Wi-Fi not finished after 30 s") && logged("(cap)"));
	CHECK(fakeloop_watched() == 0);
	tree_drop();
}

static void test_missing_dirs(void)
{
	struct splash_config c;

	tree_new();
	sh("rm -rf %s %s", openrc, run);
	c = cfg(-1);
	CHECK(splash_start(&c));
	CHECK(fakeloop_watched() == 1);	/* inotify exists, with no watch yet */
	sh("mkdir -p %s/started %s/failed %s", openrc, openrc, run);
	all_started();
	advance(900);		/* no event: nothing noticed before the 1 s retry */
	CHECK(shows("Starting GPS modem\xE2\x80\xA6", "2 / 6", 140));
	advance(200);
	CHECK(shows("Ready", "6 / 6", 840));
	advance(SPLASH_READY_HOLD_MS + 20);
	CHECK(!splash_active() && marker());
	tree_drop();
}

static void test_input_and_sleep(void)
{
	struct splash_config c;
	int64_t last;

	tree_new();
	app_set_sleep(30);
	app_open(APP_PAGE_DISPLAY, "Display");	/* depth 1: Power would be Back */
	advance(10000);		/* auto-sleep would be due 20 s into the splash */
	c = cfg(-1);
	CHECK(splash_start(&c));
	CHECK(lv_obj_is_clickable(cover()));
	last = app.sleep.last_ms;
	app_on_touch_frame(&(struct chefui_contact){ .id = 0, .x = 5, .y = 5 }, 1);
	CHECK(app.sleep.last_ms == last);	/* swallowed: no activity */
	CHECK(!app_on_button("power.short"));	/* the default: screen off */
	{
		int sets = screen_sets;

		advance(25000);			/* past the due time, before the 30 s cap */
		CHECK(splash_active() && screen_on && screen_sets == sets);
	}
	all_started();
	advance(100);
	advance(SPLASH_READY_HOLD_MS + 20);
	CHECK(!splash_active());
	CHECK(app.sleep.last_ms == ended_at);	/* counts from the end */
	CHECK(app_on_button("power.short"));	/* Back, as on any page */
	advance(25000);
	CHECK(screen_on);
	advance(8000);
	CHECK(!screen_on);			/* auto-sleep runs again */
	pf_set(true);
	app_back();
	tree_drop();
}

static void test_screen_off_start(void)
{
	struct splash_config c;
	int sets;

	tree_new();
	pf_set(false);
	sets = screen_sets;
	c = cfg(-1);
	CHECK(splash_start(&c));
	sh("touch %s/modem-ready", run);
	advance(20);
	CHECK(shows("Starting sensors\xE2\x80\xA6", "3 / 6", 280));	/* runs unseen */
	pf_set(true);					/* a wake shows its current state */
	CHECK(splash_active() && shows("Starting sensors\xE2\x80\xA6", "3 / 6", 280));
	CHECK(screen_sets == sets + 1);			/* only our own wake */
	all_started();
	advance(100);
	advance(SPLASH_READY_HOLD_MS + 20);
	CHECK(!splash_active() && marker());
	tree_drop();
}

int main(void)
{
	lv_obj_t *scr, *content;
	struct ui_settings set;
	struct app_platform pf = { .now_ms = pf_now, .screen_is_on = pf_on, .screen_set = pf_set,
				   .brightness_set = pf_bright };

	signal(SIGPIPE, SIG_IGN);
	fakeloop_init(true);
	fakeloop_display(1080, 2246);
	scr = lv_screen_active();
	kit_theme_init(KIT_THEME_DARK);
	ui_settings_defaults(&set);
	set.sleep_s = 30;
	content = kit_box(scr);
	lv_obj_set_size(content, 1080, 2246 - 96);
	lv_obj_set_y(content, 96);
	memset(&sys_state, 0, sizeof(sys_state));
	app.fake_state = true;
	app_init(&pf, &set, scr, content);
	test_frozen();
	test_walk();
	test_failed_holds();
	test_cap();
	test_missing_dirs();
	test_input_and_sleep();
	test_screen_off_start();
	return check_report("test-splash");
}
