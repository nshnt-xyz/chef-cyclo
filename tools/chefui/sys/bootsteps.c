/*
 * bootsteps.c - the boot splash's step tracker (bootsteps.h). No LVGL:
 * built into chefui's state sources and the LVGL-free host test.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "bootsteps.h"

enum { WD_STARTED, WD_FAILED, WD_RUN };

#define WATCH_MASK (IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF)

/* The names are the log's and the failed labels'. */
static const char *const names[BOOTSTEPS_N] = {
	"storage", "GPS modem", "Sensors", "Bluetooth", "Wi-Fi", "home",
};
static const char *const labels[BOOTSTEPS_N] = {
	"Mounting storage", "Starting GPS modem", "Starting sensors", "Starting Bluetooth",
	"Starting Wi-Fi", "Loading home",
};
static const char *const failed_labels[BOOTSTEPS_N] = {
	"", "GPS modem failed", "Sensors failed", "Bluetooth failed", "Wi-Fi failed", "",
};

static int64_t mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int64_t boot_ms_now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_BOOTTIME, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int64_t now_ms(const struct bootsteps *b)
{
	return b->cfg.now ? b->cfg.now() : mono_ms();
}

static void logf_(const struct bootsteps *b, const char *what, int step)
{
	int64_t t;

	if (!b->cfg.log)
		return;
	t = b->cfg.boot_ms ? b->cfg.boot_ms() : boot_ms_now();
	b->cfg.log("boot splash: %s %s at %lld.%02d s", names[step], what, (long long)(t / 1000),
		   (int)(t % 1000 / 10));
}

/* ------------------------------------------------------------ sources */

static bool exists(const char *dir, const char *name)
{
	char path[512];
	struct stat st;

	if (snprintf(path, sizeof(path), "%s/%s", dir, name) >= (int)sizeof(path))
		return false;
	return lstat(path, &st) == 0;
}

static bool started(const struct bootsteps *b, const char *svc)
{
	return exists(b->started_dir, svc);
}

static bool failed(const struct bootsteps *b, const char *svc)
{
	return exists(b->failed_dir, svc);
}

/* What the sources say about step I now (not sticky). */
static enum bootstep_state read_step(const struct bootsteps *b, int i)
{
	switch (i) {
	case 0:
		return BOOTSTEP_DONE;
	case 1:
		if (failed(b, "modem-up"))
			return BOOTSTEP_FAILED;
		return exists(b->run_dir, "modem-ready") ? BOOTSTEP_DONE : BOOTSTEP_PENDING;
	case 2:
		if (failed(b, "sensord") || failed(b, "sensors-up"))
			return BOOTSTEP_FAILED;
		return started(b, "sensord") && started(b, "sensors-up") ? BOOTSTEP_DONE : BOOTSTEP_PENDING;
	case 3:
		if (failed(b, "bt-up") || failed(b, "bluetooth"))
			return BOOTSTEP_FAILED;
		return started(b, "bt-up") && started(b, "bluetooth") ? BOOTSTEP_DONE : BOOTSTEP_PENDING;
	case 4:
		if (failed(b, "networkmanager"))
			return BOOTSTEP_FAILED;
		return started(b, "networkmanager") ? BOOTSTEP_DONE : BOOTSTEP_PENDING;
	}
	return BOOTSTEP_PENDING;	/* step 6 is derived */
}

static void finish(struct bootsteps *b, int i, enum bootstep_state s)
{
	b->state[i] = s;
	logf_(b, s == BOOTSTEP_DONE ? "ready" : "failed", i);
}

/* ------------------------------------------------------------ watches */

static void watch_dir(struct bootsteps *b, int which, const char *dir)
{
	if (b->fd < 0 || b->wd[which] >= 0)
		return;
	b->wd[which] = inotify_add_watch(b->fd, dir, WATCH_MASK);
}

static bool watches_missing(const struct bootsteps *b)
{
	return b->fd < 0 || b->wd[WD_STARTED] < 0 || b->wd[WD_FAILED] < 0 || b->wd[WD_RUN] < 0;
}

static bool active(const struct bootsteps *b)
{
	return !b->capped && !bootsteps_all_finished(b);
}

/* Closed once nothing is left to learn. */
static void stop_watching(struct bootsteps *b)
{
	if (b->fd >= 0)
		close(b->fd);
	b->fd = -1;
	b->wd[0] = b->wd[1] = b->wd[2] = -1;
}

/* ---------------------------------------------------------------- API */

void bootsteps_init(struct bootsteps *b, const struct bootsteps_config *cfg)
{
	const char *openrc, *run;
	int i;

	memset(b, 0, sizeof(*b));
	b->cfg = *cfg;
	openrc = cfg->openrc ? cfg->openrc : BOOTSTEPS_OPENRC_DEFAULT;
	run = cfg->run ? cfg->run : BOOTSTEPS_RUN_DEFAULT;
	snprintf(b->started_dir, sizeof(b->started_dir), "%s/started", openrc);
	snprintf(b->failed_dir, sizeof(b->failed_dir), "%s/failed", openrc);
	snprintf(b->run_dir, sizeof(b->run_dir), "%s", run);
	b->start_ms = now_ms(b);
	b->retry_at = -1;
	b->fd = -1;
	for (i = 0; i < 3; i++)
		b->wd[i] = -1;
	b->fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	/* the watches first, so a link made between them and the scan is
	 * still reported as an event */
	watch_dir(b, WD_STARTED, b->started_dir);
	watch_dir(b, WD_FAILED, b->failed_dir);
	watch_dir(b, WD_RUN, b->run_dir);
	bootsteps_rescan(b);
}

void bootsteps_fini(struct bootsteps *b)
{
	stop_watching(b);
}

int bootsteps_fd(const struct bootsteps *b)
{
	return b->fd;
}

void bootsteps_handle(struct bootsteps *b)
{
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	ssize_t n;

	if (b->fd < 0)
		return;
	for (;;) {
		char *p;

		n = read(b->fd, buf, sizeof(buf));
		if (n <= 0)
			break;
		for (p = buf; p < buf + n; p += sizeof(struct inotify_event) + ((struct inotify_event *)p)->len) {
			const struct inotify_event *ev = (const struct inotify_event *)p;
			int i;

			/* a watched directory that went away: watch it again when it is back */
			if (ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_IGNORED))
				for (i = 0; i < 3; i++)
					if (b->wd[i] == ev->wd)
						b->wd[i] = -1;
		}
	}
	bootsteps_rescan(b);
}

void bootsteps_rescan(struct bootsteps *b)
{
	int i;

	if (!active(b))
		return;
	/* no inotify (out of descriptors at init): tried again with the retry */
	if (b->fd < 0)
		b->fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	watch_dir(b, WD_STARTED, b->started_dir);
	watch_dir(b, WD_FAILED, b->failed_dir);
	watch_dir(b, WD_RUN, b->run_dir);
	for (i = 0; i < BOOTSTEPS_N - 1; i++) {
		enum bootstep_state s;

		if (b->state[i] != BOOTSTEP_PENDING)
			continue;
		s = read_step(b, i);
		if (s != BOOTSTEP_PENDING)
			finish(b, i, s);
	}
	if (!active(b)) {
		stop_watching(b);
		b->retry_at = -1;
		return;
	}
	b->retry_at = watches_missing(b) ? now_ms(b) + BOOTSTEPS_RETRY_MS : -1;
}

int64_t bootsteps_deadline(const struct bootsteps *b)
{
	int64_t cap = b->start_ms + BOOTSTEPS_CAP_MS;

	if (!active(b))
		return -1;
	return b->retry_at >= 0 && b->retry_at < cap ? b->retry_at : cap;
}

void bootsteps_tick(struct bootsteps *b)
{
	int64_t now = now_ms(b);
	int i;

	if (!active(b))
		return;
	if (now >= b->start_ms + BOOTSTEPS_CAP_MS) {
		/* a last look, so a link that came without an event is not blamed */
		bootsteps_rescan(b);
		if (!active(b))
			return;
		for (i = 0; i < BOOTSTEPS_N - 1; i++)
			if (b->state[i] == BOOTSTEP_PENDING && b->cfg.log)
				b->cfg.log("boot splash: %s not finished after %d s", labels[i],
					   BOOTSTEPS_CAP_MS / 1000);
		b->capped = true;
		stop_watching(b);
		b->retry_at = -1;
		return;
	}
	if (b->retry_at >= 0 && now >= b->retry_at)
		bootsteps_rescan(b);
}

int bootsteps_finished(const struct bootsteps *b)
{
	int i;

	for (i = 0; i < BOOTSTEPS_N - 1; i++)
		if (b->state[i] == BOOTSTEP_PENDING)
			return i;
	return BOOTSTEPS_N;	/* step 6 follows steps 1 to 5 */
}

int bootsteps_current(const struct bootsteps *b)
{
	return bootsteps_finished(b);
}

bool bootsteps_all_finished(const struct bootsteps *b)
{
	return bootsteps_finished(b) == BOOTSTEPS_N;
}

const char *bootsteps_label(int step)
{
	return step >= 0 && step < BOOTSTEPS_N ? labels[step] : "";
}

const char *bootsteps_failed_label(int step)
{
	return step >= 0 && step < BOOTSTEPS_N ? failed_labels[step] : "";
}
