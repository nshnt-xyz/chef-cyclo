/*
 * chef-settings - the system UI app (status bar, settings hub, Display
 * page). The supervised OpenRC service chef-settings, started at boot right
 * after syslog; owns the panel. See docs/features/system-ui.md.
 *
 *   chef-settings [--fake-state FILE]... [--page PAGE] [--theme dark|light|auto]
 *                 [--brightness PCT] [--auto-brightness on|off] [--sleep 30|60|300|0]
 *                 [--snapshot PATH [--snapshot-after MS]]
 *                 [--data DIR] [--run DIR] [--keys TEXT]
 *                 [--prompt-uid UID] [--sensord-sock PATH] [--als-light LUX] [--als-dark LUX]
 *                 [--als-light-hold S] [--als-dark-hold S]
 *
 * --fake-state   fixed system state (sys/fake.c), no real sources; up to
 *                four files, read in order
 * --page         open this page at start: hub, display, wifi, wifi-password
 *                (for the first network listed), wifi-hidden, wifi-details
 *                (on real state these two open once NetworkManager has
 *                given the network or the connection), gps, gps-sats, bt,
 *                bt-sensor (the first sensor row, once blesensd has listed it),
 *                sound, time, storage, about
 * --keys         with --fake-state only: type TEXT on the page's keyboard
 * --theme        this theme (dark, light or auto) for this run (not saved)
 * --auto-brightness  Auto brightness on or off for this run (not saved)
 * --brightness   this brightness for this run (not saved)
 * --sleep        this auto-sleep (seconds, 0 = never) for this run (not saved)
 * --clock        12 or 24 hour time for this run (not saved)
 * --snapshot     write the frame as PPM after MS (default 500) and exit
 * --data/--run   settings roots instead of /data and /run/chef (host runs)
 * --prompt-uid   host captures: trust a prompt file owned by this uid (default root)
 * --sensord-sock the light sensor's socket (default /run/chef/sensord.sock)
 * --als-light/--als-dark   theme Auto thresholds in lux (defaults 2500 and
 *                800; dark must be below light)
 * --als-light-hold/--als-dark-hold   seconds the light/dark condition must
 *                hold (1..600; defaults 5 and 20)
 *
 * Device captures: the running app writes
 * /run/chef/chef-settings/snapshot.ppm (<--run DIR>/chef-settings/snapshot.ppm)
 * on SIGUSR1. A root-owned /run/chef/chef-settings/args (one option per line)
 * is read once at start and deleted, so the next respawn runs normally:
 * that is how a capture with --fake-state/--page/--theme runs under its
 * supervisor.
 *
 * Holding the UI off the panel: `rc-service chef-settings stop`, or
 * `mkdir -p /run/chef/chef-settings; touch /run/chef/chef-settings/stop;
 * kill $(pidof chef-settings)` (the app also creates the directory at
 * start; its supervisor restarts it after 1 s). Started with that file
 * present, the app waits (polling once a second) without taking the
 * panel; removing the file lets it start.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app.h"
#include "blesens.h"
#include "chefui.h"
#include "common/chef_log.h"
#include "common/chef_paths.h"
#include "pages.h"
#include "prompt.h"
#include "settings.h"
#include "feedback.h"
#include "sys.h"
#include "wifi.h"

#define ARGS_FILE CHEF_SETTINGS_APP_DIR "/args"
#define STOP_FILE CHEF_SETTINGS_APP_DIR "/stop"
#define INIT_FAIL_SLEEP_S 5	/* so the supervisor's respawn does not spin */

#define FAKE_MAX 4

struct opts {
	const char *fake[FAKE_MAX], *page, *theme, *snapshot, *data, *run, *keys, *sensord_sock;
	struct als_params als;
	int n_fake;
	int snapshot_after;
	int brightness;		/* 0 = from settings */
	int sleep_s;		/* -1 = from settings */
	int clock;		/* 12, 24; 0 = from settings */
	int auto_brightness;	/* -1 = from settings, 0, 1 */
	int prompt_uid;		/* -1 = root (production); host captures pass their uid */
};

static struct opts O = { .snapshot_after = 500, .sleep_s = -1, .auto_brightness = -1, .prompt_uid = -1 };
static bool als_set;		/* an --als-* option was given */

static int64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int usage(void)
{
	fprintf(stderr, "usage: chef-settings [--fake-state FILE]... [--page hub|display|wifi|"
			"wifi-password|wifi-hidden|wifi-details|gps|gps-sats|bt|bt-sensor|sound|time|storage|about] [--theme dark|light|auto] [--brightness PCT] [--auto-brightness on|off] "
			"[--sleep 30|60|300|0] [--clock 12|24] "
			"[--snapshot PATH [--snapshot-after MS]] [--data DIR] [--run DIR] "
			"[--keys TEXT] [--sensord-sock PATH] [--als-light LUX] [--als-dark LUX] "
			"[--als-light-hold S] [--als-dark-hold S]\n");
	return 2;
}

static int pending_page;
static void open_pending(void);

static int page_kind(const char *v)
{
	static const struct { const char *name; enum app_page kind; } pages[] = {
		{ "hub", APP_PAGE_HUB }, { "display", APP_PAGE_DISPLAY }, { "wifi", APP_PAGE_WIFI },
		{ "wifi-password", APP_PAGE_WIFI_PASSWORD }, { "wifi-hidden", APP_PAGE_WIFI_HIDDEN },
		{ "wifi-details", APP_PAGE_WIFI_DETAILS }, { "gps", APP_PAGE_GPS },
		{ "gps-sats", APP_PAGE_GPS_SATS }, { "bt", APP_PAGE_BT }, { "bt-sensor", APP_PAGE_BT_SENSOR },
		{ "sound", APP_PAGE_SOUND }, { "time", APP_PAGE_TIME }, { "storage", APP_PAGE_STORAGE },
		{ "about", APP_PAGE_ABOUT },
	};

	for (unsigned i = 0; i < sizeof(pages) / sizeof(pages[0]); i++)
		if (!strcmp(v, pages[i].name))
			return (int)pages[i].kind;
	return -1;
}

/* --page: the pages a person would pass on the way there */
static void open_start_page(const char *v)
{
	switch (page_kind(v)) {
	case APP_PAGE_DISPLAY:
		app_open(APP_PAGE_DISPLAY, "Display");
		break;
	case APP_PAGE_SOUND:
		app_open(APP_PAGE_SOUND, "Sound & haptics");
		break;
	case APP_PAGE_TIME:
		app_open(APP_PAGE_TIME, "Time & date");
		break;
	case APP_PAGE_STORAGE:
		app_open(APP_PAGE_STORAGE, "Storage");
		break;
	case APP_PAGE_ABOUT:
		app_open(APP_PAGE_ABOUT, "About");
		break;
	case APP_PAGE_WIFI_PASSWORD:
	case APP_PAGE_WIFI_DETAILS:
		app_open(APP_PAGE_WIFI, "Wi-Fi");
		pending_page = page_kind(v);
		open_pending();		/* now with fake state, later with real */
		break;
	case APP_PAGE_WIFI_HIDDEN:
		app_open(APP_PAGE_WIFI, "Wi-Fi");
		app_open(APP_PAGE_WIFI_HIDDEN, "Hidden network");
		break;
	case APP_PAGE_WIFI:
		app_open(APP_PAGE_WIFI, "Wi-Fi");
		break;
	case APP_PAGE_GPS_SATS:
		app_open(APP_PAGE_GPS, "GPS");
		app_open(APP_PAGE_GPS_SATS, "Satellites");
		break;
	case APP_PAGE_GPS:
		app_open(APP_PAGE_GPS, "GPS");
		break;
	case APP_PAGE_BT_SENSOR:
		app_open(APP_PAGE_BT, "Bluetooth");
		pending_page = APP_PAGE_BT_SENSOR;
		open_pending();
		break;
	case APP_PAGE_BT:
		app_open(APP_PAGE_BT, "Bluetooth");
		break;
	default:
		break;
	}
}

/* --als-*: a number the value must be entirely; applied over the defaults. */
static bool als_option(const char *a, const char *v, struct opts *o)
{
	char *end;
	double x;

	if (strncmp(a, "--als-", 6))
		return false;
	x = strtod(v, &end);
	if (end == v || *end || !isfinite(x) || x < 0 || x > 1e7)
		return false;
	if (!als_set) {
		als_params_default(&o->als);
		als_set = true;
	}
	if (!strcmp(a, "--als-light"))
		o->als.light_lux = x;
	else if (!strcmp(a, "--als-dark"))
		o->als.dark_lux = x;
	else if (!strcmp(a, "--als-light-hold") && x <= ALS_HOLD_S_MAX && x == (int)x)
		o->als.light_hold_s = (int)x;
	else if (!strcmp(a, "--als-dark-hold") && x <= ALS_HOLD_S_MAX && x == (int)x)
		o->als.dark_hold_s = (int)x;
	else
		return false;
	return true;		/* validated as a whole after parsing */
}

static int parse(int argc, char **argv, struct opts *o)
{
	for (int i = 0; i < argc; i++) {
		const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;

		if (!v)
			return -1;
		if (!strcmp(a, "--fake-state") && o->n_fake < FAKE_MAX)
			o->fake[o->n_fake++] = v;
		else if (!strcmp(a, "--page") && page_kind(v) >= 0)
			o->page = v;
		else if (!strcmp(a, "--keys"))
			o->keys = v;
		else if (!strcmp(a, "--theme") && (!strcmp(v, "dark") || !strcmp(v, "light") ||
						    !strcmp(v, "auto")))
			o->theme = v;
		else if (!strcmp(a, "--auto-brightness") && (!strcmp(v, "on") || !strcmp(v, "off")))
			o->auto_brightness = !strcmp(v, "on");
		else if (!strcmp(a, "--sleep") && (!strcmp(v, "30") || !strcmp(v, "60") ||
						    !strcmp(v, "300") || !strcmp(v, "0")))
			o->sleep_s = atoi(v);
		else if (!strcmp(a, "--clock") && (!strcmp(v, "12") || !strcmp(v, "24")))
			o->clock = atoi(v);
		else if (!strcmp(a, "--brightness") && atoi(v) >= UI_BRIGHTNESS_MIN &&
			 atoi(v) <= UI_BRIGHTNESS_MAX)
			o->brightness = atoi(v);
		else if (!strcmp(a, "--snapshot"))
			o->snapshot = v;
		else if (!strcmp(a, "--snapshot-after"))
			o->snapshot_after = atoi(v);
		else if (!strcmp(a, "--data"))
			o->data = v;
		else if (!strcmp(a, "--run"))
			o->run = v;
		else if (!strcmp(a, "--prompt-uid") && atoi(v) >= 0)
			o->prompt_uid = atoi(v);
		else if (!strcmp(a, "--sensord-sock"))
			o->sensord_sock = v;
		else if (!als_option(a, v, o))
			return -1;
		i++;
	}
	if (als_set && !als_params_valid(&o->als))
		return -1;	/* dark must be below light; holds 1..600 s */
	return 0;
}

/* Before chefui_init() (chefui_log() has no backend yet): syslog
 * through chef_log, tag chef-settings (main() sets it first), and
 * stderr; stderr only on a PC, as chefui_log() does. */
static void early_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void early_log(const char *fmt, ...)
{
	char buf[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
#if CHEFUI_SDL
	fprintf(stderr, "chef-settings: %s\n", buf);
#else
	chef_log("%s", buf);
#endif
}

/* One-shot extra options from root's /run/chef/chef-settings/args. */
static void args_file(struct opts *o)
{
	static char buf[1024];
	static char *argv[32];
	struct stat st;
	int fd, argc = 0;
	ssize_t n;
	char *save = NULL, *t;

	fd = open(ARGS_FILE, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != 0) {
		close(fd);
		early_log("ignoring %s: not a root-owned file", ARGS_FILE);
		return;
	}
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	unlink(ARGS_FILE);
	if (n <= 0)
		return;
	buf[n] = 0;
	for (t = strtok_r(buf, " \t\n", &save); t && argc < 32; t = strtok_r(NULL, " \t\n", &save))
		argv[argc++] = t;
	if (parse(argc, argv, o))
		early_log("ignoring %s: bad options", ARGS_FILE);
	else
		early_log("one-shot options from %s", ARGS_FILE);
}

/* Before chefui_init(): the panel is still as the bootloader or the previous owner left it. */
static void wait_while_stopped(void)
{
	struct stat st;
	bool said = false;

	while (stat(STOP_FILE, &st) == 0) {
		if (!said) {
			early_log("%s present; waiting for its removal", STOP_FILE);
			said = true;
		}
		sleep(1);
	}
}

static bool on_button(const char *gesture, void *user)
{
	(void)user;
	return app_on_button(gesture);
}

static void on_touch(const struct chefui_contact *c, int n, void *user)
{
	(void)user;
	app_on_touch_frame(c, n);
}

static void on_screen(bool on, void *user)
{
	(void)user;
	app_on_screen(on);
}

/* --page wifi-details / wifi-password on real state: opened once the Wi-Fi
 * view has what they show (a connection, a network in range). */
static int pending_page = -1;

static void open_pending(void)
{
	struct ble_row r;

	if (pending_page == APP_PAGE_BT_SENSOR && app_top() == APP_PAGE_BT) {
		struct page_bt_sensor_arg a;

		if (ble_rows(&ble_view, &r, 1) < 1)
			return;
		snprintf(a.addr, sizeof(a.addr), "%s", ble_view.s[r.sensor].addr);
		a.kind = r.kind;
		app_open_arg(APP_PAGE_BT_SENSOR, ble_kind_title(r.kind), &a);
		pending_page = -1;
		return;
	}
	if (pending_page < 0 || app_top() != APP_PAGE_WIFI)
		return;
	if (pending_page == APP_PAGE_WIFI_DETAILS && wifi_view.cur.valid)
		app_open(APP_PAGE_WIFI_DETAILS, wifi_view.cur.text);
	else if (pending_page == APP_PAGE_WIFI_PASSWORD && wifi_view.n > 0)
		app_open_arg(APP_PAGE_WIFI_PASSWORD, wifi_view.nets[0].text, &wifi_view.nets[0]);
	else
		return;
	pending_page = -1;
}

static void on_state(unsigned what, void *user)
{
	(void)user;
	app_on_state(what);
	if (what & (SYS_WIFI_VIEW | SYS_BLE))
		open_pending();
}

/* /data became ours late and its settings replace the defaults. */
static void on_replaced(const struct ui_settings *s, void *user)
{
	(void)user;
	if (!app.persist)
		return;		/* captures keep their overrides */
	if (!O.n_fake)
		sys_clock_set_tz(s->tz);
	app_apply_settings(s);
}

/* A capture never holds a typed passphrase: the field shows bullets. */
static int snapshot_guarded(const char *path)
{
	int r;

	app_snapshot_guard(true);
	r = chefui_snapshot(path);
	app_snapshot_guard(false);
	return r;
}

static void snapshot_now(lv_timer_t *t)
{
	int r = O.n_fake ? chefui_snapshot(O.snapshot) : snapshot_guarded(O.snapshot);

	(void)t;
	if (r)
		chefui_log("snapshot %s: %s", O.snapshot, strerror(-r));
	chefui_quit(r ? 1 : 0);
}

static void sigusr1(int fd, short revents, void *user)
{
	struct signalfd_siginfo si;
	int r;

	(void)revents;
	(void)user;
	char dir[300], path[320];

	while (read(fd, &si, sizeof(si)) == sizeof(si))
		;
	snprintf(dir, sizeof(dir), "%s/chef-settings", O.run ? O.run : CHEF_RUN_DIR);
	snprintf(path, sizeof(path), "%s/snapshot.ppm", dir);
	mkdir(O.run ? O.run : CHEF_RUN_DIR, 0755);	/* exists on the phone */
	mkdir(dir, 0755);
	r = O.n_fake ? chefui_snapshot(path) : snapshot_guarded(path);
	chefui_log("snapshot %s: %s", path, r ? strerror(-r) : "written");
}

int main(int argc, char **argv)
{
	struct chefui_config cfg = { .app_name = "chef-settings" };
	struct ui_store_paths paths = { 0 };
	struct app_platform pf = {
		.now_ms = now_ms,
		.screen_is_on = chefui_screen_is_on,
		.screen_set = chefui_screen_set,
		.brightness_set = chefui_brightness_set,
	};
	struct ui_settings set;
	sigset_t mask;
	int r, sfd;

	/* SIGUSR1 (capture) goes to the signalfd below: blocked before any
	 * thread exists, so no thread can take its default (terminating)
	 * action */
	sigemptyset(&mask);
	sigaddset(&mask, SIGUSR1);
	sigprocmask(SIG_BLOCK, &mask, NULL);

	if (parse(argc - 1, argv + 1, &O))
		return usage();
	chef_log_init(cfg.app_name, true);	/* early_log(); chefui_init() sets the same */
	mkdir(CHEF_SETTINGS_APP_DIR, 0755);	/* for stop, args and captures; fails harmlessly off the phone */
	args_file(&O);
	wait_while_stopped();

	if (!chefui_init(&cfg)) {
		/* already logged; do not let respawn spin */
		sleep(INIT_FAIL_SLEEP_S);
		return 1;
	}

	paths.data = O.data;
	paths.run = O.run;
	ui_store_set_paths(&paths);
	ui_store_on_replaced(on_replaced, NULL);
	r = ui_store_load(&set);
	sys_storage_usage_init();
	sys_device_info_init();
	if (r)
		chefui_log("settings: no persistence worker (%s); defaults, not saved", strerror(-r));
	if (O.theme)
		set.theme = !strcmp(O.theme, "light") ? UI_THEME_LIGHT
			: !strcmp(O.theme, "auto") ? UI_THEME_AUTO : UI_THEME_DARK;
	if (O.auto_brightness >= 0)
		set.auto_brightness = O.auto_brightness;
	if (O.brightness)
		set.brightness = O.brightness;
	if (O.sleep_s >= 0)
		set.sleep_s = O.sleep_s;
	if (O.clock)
		set.clock_24h = O.clock == 24;
	chefui_brightness_set(ui_brightness_level(set.brightness));

	sys_on_change(on_state, NULL);
	app.fake_state = O.n_fake > 0;
	/* fake clock fields (clockepoch) use the saved zone, no clock source */
	if (O.n_fake)
		sys_clock_use_tz(set.tz);
	for (int i = 0; i < O.n_fake; i++) {
		r = sys_fake_load(O.fake[i]);
		if (r) {
			chefui_log("fake state %s: %s", O.fake[i], strerror(-r));
			return 2;
		}
	}
	if (!O.n_fake) {
		sys_clock_start(set.tz);
		sys_battery_start(NULL, -1);
		sys_gps_start(NULL, 0);
		sys_als_start(O.sensord_sock, als_set ? &O.als : NULL);
		sys_ble_start(NULL);
		sys_netbus_start();
		sys_wifi_init();
	}
	/* UI haptics (taps, Back, wake): the phone only, never a fake run */
	if (!O.n_fake && chefui_backend_is_fbdev() &&
	    (r = sys_feedback_start(NULL, set.haptics)))
		chefui_log("haptics: no feedback worker (%s)", strerror(-r));
	app_init(&pf, &set, chefui_root(), chefui_content_root());
	/* captures and overrides change nothing on disk */
	app.persist = !O.theme && !O.brightness && O.sleep_s < 0 && !O.clock && !O.snapshot &&
		      O.auto_brightness < 0;
	if (O.page)
		open_start_page(O.page);
	{	/* the guided-test prompt card: <run>/chef-settings/prompt (prompt.h) */
		char pdir[300];

		snprintf(pdir, sizeof(pdir), "%s/chef-settings", O.run ? O.run : CHEF_RUN_DIR);
		mkdir(O.run ? O.run : CHEF_RUN_DIR, 0755);
		mkdir(pdir, 0755);
		prompt_set_trusted_uid(O.prompt_uid >= 0 ? (uid_t)O.prompt_uid : 0);
		if ((r = prompt_start(pdir, now_ms)))
			chefui_log("prompt: not watching %s (%s)", pdir, strerror(-r));
	}
	/* typed text only into fake state: a real passphrase never comes
	 * from the command line or the args file */
	if (O.keys && (!O.n_fake || !page_wifi_type(O.keys)))
		chefui_log("--keys ignored (needs --fake-state and an entry page)");

	chefui_on_button(on_button, NULL);
	chefui_on_touch(on_touch, NULL);
	chefui_on_screen(on_screen, NULL);

	sfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
	if (sfd >= 0)
		chefui_watch_fd(sfd, POLLIN, sigusr1, NULL);

	if (O.snapshot) {
		lv_timer_t *t = lv_timer_create(snapshot_now, (uint32_t)O.snapshot_after, NULL);

		lv_timer_set_repeat_count(t, 1);
	}

	r = chefui_run();
	if (app.persist)
		ui_store_flush();
	return r;
}
