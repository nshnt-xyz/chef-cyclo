/*
 * steptrack.c - the step tracker behind bootsteps.c and shutsteps.c
 * (steptrack.h). No LVGL: built into chefui's state sources and the
 * LVGL-free host tests.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "steptrack.h"

int64_t steptrack_mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int64_t steptrack_boot_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_BOOTTIME, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void steptrack_stamp(char *out, size_t n, int64_t ms)
{
	snprintf(out, n, "%lld.%02d", (long long)(ms / 1000), (int)(ms % 1000 / 10));
}

static int64_t now_ms(const struct steptrack *t)
{
	return t->cfg.now ? t->cfg.now() : steptrack_mono_ms();
}

static void log_finish(const struct steptrack *t, int step, enum steptrack_state s)
{
	const char *verb = "failed";
	char stamp[24];
	int64_t ms;

	if (!t->cfg.log)
		return;
	if (s == STEPTRACK_DONE)
		verb = t->spec->done_verbs ? t->spec->done_verbs[step] : "ready";
	ms = t->cfg.boot_ms ? t->cfg.boot_ms() : steptrack_boot_ms();
	steptrack_stamp(stamp, sizeof(stamp), ms);
	t->cfg.log("%s: %s %s at %s s", t->spec->log_prefix, t->spec->names[step], verb, stamp);
}

bool steptrack_exists(const char *dir, const char *name)
{
	char path[512];
	struct stat st;

	if (snprintf(path, sizeof(path), "%s/%s", dir, name) >= (int)sizeof(path))
		return false;
	return lstat(path, &st) == 0;
}

static bool active(const struct steptrack *t)
{
	return !t->capped && !steptrack_all_finished(t);
}

/* Closed once nothing is left to learn. */
static void stop_watching(struct steptrack *t)
{
	dirwatch_close(&t->dw);
}

/* ---------------------------------------------------------------- API */

void steptrack_init(struct steptrack *t, const struct steptrack_spec *spec, void *ctx,
		    const struct steptrack_config *cfg, const struct dirwatch_dir *watches, int nwatch)
{
	/* a table that does not fit is a programming error */
	if (spec->n > STEPTRACK_MAX)
		abort();
	memset(t, 0, sizeof(*t));
	t->spec = spec;
	t->ctx = ctx;
	t->cfg = *cfg;
	if (nwatch > DIRWATCH_MAX)
		abort();
	memcpy(t->dir, watches, (size_t)nwatch * sizeof(*watches));
	dirwatch_init(&t->dw, t->dir, nwatch);
	t->start_ms = now_ms(t);
	/* the watches first, so a change made between them and the scan is
	 * still reported as an event */
	dirwatch_arm(&t->dw, now_ms(t));
	steptrack_rescan(t);
}

void steptrack_fini(struct steptrack *t)
{
	stop_watching(t);
}

int steptrack_fd(const struct steptrack *t)
{
	return t->dw.fd;
}

void steptrack_handle(struct steptrack *t)
{
	dirwatch_drain(&t->dw);
	steptrack_rescan(t);
}

void steptrack_rescan(struct steptrack *t)
{
	int i;

	if (!active(t))
		return;
	/* the fd made again if there is none, missing directories watched if
	 * they are there now */
	dirwatch_arm(&t->dw, now_ms(t));
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
	if (!active(t))
		stop_watching(t);
}

int64_t steptrack_deadline(const struct steptrack *t)
{
	int64_t cap;

	if (!active(t))
		return -1;
	if (!t->spec->cap_ms)
		return t->dw.retry_at;
	cap = t->start_ms + t->spec->cap_ms;
	return t->dw.retry_at >= 0 && t->dw.retry_at < cap ? t->dw.retry_at : cap;
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
		return;
	}
	if (t->dw.retry_at >= 0 && now >= t->dw.retry_at)
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
