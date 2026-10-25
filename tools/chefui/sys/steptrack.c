/*
 * steptrack.c - the step tracker behind bootsteps.c and shutsteps.c
 * (steptrack.h). No LVGL: built into chefui's state sources and the
 * LVGL-free host tests.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "steptrack.h"

#define SELF_EVENTS (IN_DELETE_SELF | IN_MOVE_SELF)

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

static int64_t now_ms(const struct steptrack *t)
{
	return t->cfg.now ? t->cfg.now() : mono_ms();
}

static void log_finish(const struct steptrack *t, int step, enum steptrack_state s)
{
	const char *verb = "failed";
	int64_t ms;

	if (!t->cfg.log)
		return;
	if (s == STEPTRACK_DONE)
		verb = t->spec->done_verbs ? t->spec->done_verbs[step] : "ready";
	ms = t->cfg.boot_ms ? t->cfg.boot_ms() : boot_ms_now();
	t->cfg.log("%s: %s %s at %lld.%02d s", t->spec->log_prefix, t->spec->names[step], verb,
		   (long long)(ms / 1000), (int)(ms % 1000 / 10));
}

bool steptrack_exists(const char *dir, const char *name)
{
	char path[512];
	struct stat st;

	if (snprintf(path, sizeof(path), "%s/%s", dir, name) >= (int)sizeof(path))
		return false;
	return lstat(path, &st) == 0;
}

/* ------------------------------------------------------------ watches */

static void watch_all(struct steptrack *t)
{
	int i;

	if (t->fd < 0)
		return;
	for (i = 0; i < t->nwatch; i++)
		if (t->wd[i] < 0)
			t->wd[i] = inotify_add_watch(t->fd, t->watch[i].path, t->watch[i].events | SELF_EVENTS);
}

static bool watches_missing(const struct steptrack *t)
{
	int i;

	if (t->fd < 0)
		return true;
	for (i = 0; i < t->nwatch; i++)
		if (t->wd[i] < 0)
			return true;
	return false;
}

static bool active(const struct steptrack *t)
{
	return !t->capped && !steptrack_all_finished(t);
}

/* Closed once nothing is left to learn. */
static void stop_watching(struct steptrack *t)
{
	int i;

	if (t->fd >= 0)
		close(t->fd);
	t->fd = -1;
	for (i = 0; i < STEPTRACK_MAX; i++)
		t->wd[i] = -1;
}

/* ---------------------------------------------------------------- API */

void steptrack_init(struct steptrack *t, const struct steptrack_spec *spec, void *ctx,
		    const struct steptrack_config *cfg, const struct steptrack_watch *watches, int nwatch)
{
	int i;

	/* a table that does not fit is a programming error */
	if (spec->n > STEPTRACK_MAX || nwatch > STEPTRACK_MAX)
		abort();
	memset(t, 0, sizeof(*t));
	t->spec = spec;
	t->ctx = ctx;
	t->cfg = *cfg;
	t->nwatch = nwatch;
	memcpy(t->watch, watches, (size_t)nwatch * sizeof(*watches));
	t->start_ms = now_ms(t);
	t->retry_at = -1;
	t->fd = -1;		/* the memset left 0: never close that */
	for (i = 0; i < STEPTRACK_MAX; i++)
		t->wd[i] = -1;
	t->fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	/* the watches first, so a change made between them and the scan is
	 * still reported as an event */
	watch_all(t);
	steptrack_rescan(t);
}

void steptrack_fini(struct steptrack *t)
{
	stop_watching(t);
}

int steptrack_fd(const struct steptrack *t)
{
	return t->fd;
}

void steptrack_handle(struct steptrack *t)
{
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	ssize_t n;

	if (t->fd < 0)
		return;
	for (;;) {
		char *p;

		n = read(t->fd, buf, sizeof(buf));
		if (n <= 0)
			break;
		for (p = buf; p < buf + n; p += sizeof(struct inotify_event) + ((struct inotify_event *)p)->len) {
			const struct inotify_event *ev = (const struct inotify_event *)p;
			int i;

			/* a watched directory that went away: watch it again when it is back */
			if (ev->mask & (SELF_EVENTS | IN_IGNORED))
				for (i = 0; i < t->nwatch; i++)
					if (t->wd[i] == ev->wd)
						t->wd[i] = -1;
		}
	}
	steptrack_rescan(t);
}

void steptrack_rescan(struct steptrack *t)
{
	int i;

	if (!active(t))
		return;
	/* no inotify (out of descriptors at init): tried again with the retry */
	if (t->fd < 0)
		t->fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	watch_all(t);
	for (i = 0; i < t->spec->n; i++) {
		enum steptrack_state s;

		if (t->state[i] != STEPTRACK_PENDING)
			continue;
		s = t->spec->read(t->ctx, i);
		if (s == STEPTRACK_PENDING)
			continue;
		t->state[i] = s;
		log_finish(t, i, s);
	}
	if (!active(t)) {
		stop_watching(t);
		t->retry_at = -1;
		return;
	}
	t->retry_at = watches_missing(t) ? now_ms(t) + STEPTRACK_RETRY_MS : -1;
}

int64_t steptrack_deadline(const struct steptrack *t)
{
	int64_t cap;

	if (!active(t))
		return -1;
	if (!t->spec->cap_ms)
		return t->retry_at;
	cap = t->start_ms + t->spec->cap_ms;
	return t->retry_at >= 0 && t->retry_at < cap ? t->retry_at : cap;
}

void steptrack_tick(struct steptrack *t)
{
	int64_t now = now_ms(t);
	int i;

	if (!active(t))
		return;
	if (t->spec->cap_ms && now >= t->start_ms + t->spec->cap_ms) {
		/* a last look, so a change that came without an event is not blamed */
		steptrack_rescan(t);
		if (!active(t))
			return;
		for (i = 0; i < t->spec->n; i++)
			if (t->state[i] == STEPTRACK_PENDING && t->spec->capped)
				t->spec->capped(t->ctx, i);
		t->capped = true;
		stop_watching(t);
		t->retry_at = -1;
		return;
	}
	if (t->retry_at >= 0 && now >= t->retry_at)
		steptrack_rescan(t);
}

int steptrack_finished(const struct steptrack *t)
{
	int i;

	for (i = 0; i < t->spec->n; i++)
		if (t->state[i] == STEPTRACK_PENDING)
			return i;
	return t->spec->n + 1;	/* the screen's own step follows the tracked ones */
}

bool steptrack_all_finished(const struct steptrack *t)
{
	return steptrack_finished(t) == t->spec->n + 1;
}
