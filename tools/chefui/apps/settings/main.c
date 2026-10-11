/*
 * chef-settings - the system UI app (status bar, settings hub, Display
 * page). Started at boot by inittab (respawn) and owns the panel; fblog
 * shows the log screen whenever it is not running. See
 * docs/features/system-ui.md.
 *
 *   chef-settings [--fake-state FILE] [--page hub|display] [--theme dark|light]
 *                 [--brightness PCT] [--snapshot PATH [--snapshot-after MS]]
 *                 [--data DIR] [--run DIR]
 *
 * --fake-state   fixed system state (sys/fake.c), no real sources
 * --page         open this page at start
 * --theme        this theme for this run (not saved)
 * --brightness   this brightness for this run (not saved)
 * --snapshot     write the frame as PPM after MS (default 500) and exit
 * --data/--run   settings roots instead of /data and /run (host runs)
 *
 * Device captures: the running app writes /run/chef-settings/snapshot.ppm
 * (<--run DIR>/chef-settings/snapshot.ppm) on SIGUSR1. A root-owned /run/chef-settings/args (one option per line)
 * is read once at start and deleted, so the next respawn runs normally:
 * that is how a capture with --fake-state/--page/--theme runs under init.
 *
 * The log screen: `mkdir -p /run/chef-settings; touch /run/chef-settings/stop;
 * kill $(pidof chef-settings)` (the app also creates the directory at start).
 * Started with that file present, the app waits (polling once a second)
 * without taking the panel, so fblog shows the log; removing the file
 * lets it start.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
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
#include "chefui.h"
#include "settings.h"
#include "sys.h"

#define RUN_DIR "/run/chef-settings"
#define ARGS_FILE RUN_DIR "/args"
#define STOP_FILE RUN_DIR "/stop"
#define INIT_FAIL_SLEEP_S 5	/* so init's respawn does not spin */

struct opts {
	const char *fake, *page, *theme, *snapshot, *data, *run;
	int snapshot_after;
	int brightness;		/* 0 = from settings */
};

static struct opts O = { .snapshot_after = 500 };

static int64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int usage(void)
{
	fprintf(stderr, "usage: chef-settings [--fake-state FILE] [--page hub|display] "
			"[--theme dark|light] [--brightness PCT] [--snapshot PATH [--snapshot-after MS]] "
			"[--data DIR] [--run DIR]\n");
	return 2;
}

static int parse(int argc, char **argv, struct opts *o)
{
	for (int i = 0; i < argc; i++) {
		const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;

		if (!v)
			return -1;
		if (!strcmp(a, "--fake-state"))
			o->fake = v;
		else if (!strcmp(a, "--page") && (!strcmp(v, "hub") || !strcmp(v, "display")))
			o->page = v;
		else if (!strcmp(a, "--theme") && (!strcmp(v, "dark") || !strcmp(v, "light")))
			o->theme = v;
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
		else
			return -1;
		i++;
	}
	return 0;
}

/* Before chefui_init() (no app name there yet): kmsg and stderr. */
static void early_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void early_log(const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	int fd;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	fprintf(stderr, "chef-settings: %s\n", buf);
	fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
	if (fd >= 0) {
		dprintf(fd, "chef-settings: %s\n", buf);
		close(fd);
	}
}

/* One-shot extra options from root's /run/chef-settings/args. */
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

/* Before chefui_init(): fblog keeps the panel meanwhile. */
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
	(void)c;
	(void)n;
	(void)user;
	app_on_touch();
}

static void on_screen(bool on, void *user)
{
	(void)user;
	app_on_screen(on);
}

static void on_state(unsigned what, void *user)
{
	(void)user;
	app_on_state(what);
}

/* /data became ours late and its settings replace the defaults. */
static void on_replaced(const struct ui_settings *s, void *user)
{
	(void)user;
	if (!app.persist)
		return;		/* captures keep their overrides */
	if (!O.fake)
		sys_clock_set_tz(s->tz);
	app_apply_settings(s);
}

static void snapshot_now(lv_timer_t *t)
{
	int r = chefui_snapshot(O.snapshot);

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
	snprintf(dir, sizeof(dir), "%s/chef-settings", O.run ? O.run : "/run");
	snprintf(path, sizeof(path), "%s/snapshot.ppm", dir);
	mkdir(O.run ? O.run : "/run", 0755);	/* exists on the phone */
	mkdir(dir, 0755);
	r = chefui_snapshot(path);
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
	mkdir(RUN_DIR, 0755);	/* for stop, args and captures; fails harmlessly off the phone */
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
	if (r)
		chefui_log("settings: no persistence worker (%s); defaults, not saved", strerror(-r));
	if (O.theme)
		set.theme = !strcmp(O.theme, "light") ? UI_THEME_LIGHT : UI_THEME_DARK;
	if (O.brightness)
		set.brightness = O.brightness;
	chefui_brightness_set(ui_brightness_level(set.brightness));

	sys_on_change(on_state, NULL);
	app.fake_state = O.fake != NULL;
	if (O.fake) {
		r = sys_fake_load(O.fake);
		if (r) {
			chefui_log("fake state %s: %s", O.fake, strerror(-r));
			return 2;
		}
	} else {
		sys_clock_start(set.tz);
		sys_battery_start(NULL, -1);
		sys_gps_start(NULL, 0);
		sys_netbus_start();
	}
	app_init(&pf, &set, chefui_root(), chefui_content_root());
	/* captures and overrides change nothing on disk */
	app.persist = !O.theme && !O.brightness && !O.snapshot;
	if (O.page && !strcmp(O.page, "display"))
		app_open(APP_PAGE_DISPLAY, "Display");

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
