/*
 * steptrack.h - the machinery behind the boot splash's and the shutdown
 * screen's step lists, without LVGL: a table of steps whose state a rule
 * reads from the file system, an inotify watch list for the caller's loop,
 * the retry of a directory that is missing, an optional cap, sticky
 * results and the log line of each. bootsteps.c and shutsteps.c are
 * the two tables on it.
 *
 * N steps are tracked; the step after them (step N + 1) is the screen's
 * own and is shown as soon as the tracked ones have finished. A step that
 * has finished (done or failed) stays finished: the bar never goes back
 * and each step is logged once.
 *
 * Watching: one inotify fd on the table's directories (create, delete,
 * move, plus the directory itself going away), for the caller's loop
 * (steptrack_fd()); any event rescans every rule. A directory that is
 * missing is tried again every STEPTRACK_RETRY_MS (steptrack_deadline()).
 * Nothing is watched or rescanned once all steps have finished, or after
 * the cap (spec cap_ms after init, 0 = none) has logged every step still
 * waiting and set `capped`.
 */
#ifndef CHEF_STEPTRACK_H
#define CHEF_STEPTRACK_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/inotify.h>

#define STEPTRACK_MAX 8			/* tracked steps and watches, at most */
#define STEPTRACK_RETRY_MS 1000
/* The event bits a watch usually wants; the directory going away is added. */
#define STEPTRACK_DIR_EVENTS (IN_MOVED_FROM | IN_MOVED_TO | IN_CREATE | IN_DELETE)

enum steptrack_state { STEPTRACK_PENDING, STEPTRACK_DONE, STEPTRACK_FAILED };

struct steptrack_watch {
	const char *path;		/* a directory; must outlive the tracker */
	uint32_t events;		/* inotify bits */
};

struct steptrack_spec {
	int n;				/* tracked steps */
	int64_t cap_ms;			/* 0: no cap */
	/* the log line of a finished step: "<prefix>: <name> <verb> at S.CC s",
	 * verb "failed" or done_verbs[step] (NULL array: "ready") */
	const char *log_prefix;
	const char *const *names;
	const char *const *done_verbs;
	/* what the sources say about a step now (not sticky) */
	enum steptrack_state (*read)(void *ctx, int step);
	/* the cap passed with this step still waiting (NULL: nothing to log) */
	void (*capped)(void *ctx, int step);
};

struct steptrack_config {
	int64_t (*now)(void);		/* the loop's clock in ms, NULL: CLOCK_MONOTONIC */
	int64_t (*boot_ms)(void);	/* ms since boot for the log, NULL: CLOCK_BOOTTIME */
	void (*log)(const char *fmt, ...) __attribute__((format(printf, 1, 2)));	/* NULL: none */
};

/* Private to steptrack.c except where noted. */
struct steptrack {
	const struct steptrack_spec *spec;
	void *ctx;			/* handed to the spec's callbacks */
	struct steptrack_config cfg;
	struct steptrack_watch watch[STEPTRACK_MAX];
	int nwatch;
	int fd;				/* read: inotify, -1 none */
	int wd[STEPTRACK_MAX];		/* read */
	enum steptrack_state state[STEPTRACK_MAX];	/* read */
	bool capped;			/* read: the cap has passed with steps waiting */
	int64_t start_ms, retry_at;
};

/* Watches first, then reads every rule once. A table or watch list over
 * STEPTRACK_MAX aborts. */
void steptrack_init(struct steptrack *t, const struct steptrack_spec *spec, void *ctx,
		    const struct steptrack_config *cfg, const struct steptrack_watch *watches, int nwatch);
/* The inotify fd closed. */
void steptrack_fini(struct steptrack *t);

/* The fd to watch for POLLIN, -1 when nothing is watched. It can appear
 * later (inotify_init1 failed at init and is retried by the rescan), so the
 * caller reads it again after every handle and tick. */
int steptrack_fd(const struct steptrack *t);
/* The fd readable: drained, then a rescan. */
void steptrack_handle(struct steptrack *t);
/* Every rule tested again (and missing directories watched if they are
 * there now). */
void steptrack_rescan(struct steptrack *t);
/* The next time (the loop's clock) steptrack_tick() is due: the retry of a
 * missing directory or the cap; -1 none. */
int64_t steptrack_deadline(const struct steptrack *t);
void steptrack_tick(struct steptrack *t);

/* Steps finished (done or failed) from the first on without a gap: the
 * bar's fill out of n + 1. All n tracked ones give n + 1 (the screen's own
 * step follows them). */
int steptrack_finished(const struct steptrack *t);
bool steptrack_all_finished(const struct steptrack *t);

/* lstat of DIR/NAME: it exists. A link is never followed or read. */
bool steptrack_exists(const char *dir, const char *name);

#endif /* CHEF_STEPTRACK_H */
