/*
 * bootsteps.h - the boot splash's six steps without LVGL: which of them
 * are done or failed, read from OpenRC's state links and the modem
 * marker, driven by the caller's loop and clock like gpsclient.h.
 *
 * The steps, their sources and their labels are the table in
 * docs/archive/boot-splash-handoff.md; a source is only ever tested
 * for existence (lstat, a link is never followed or read). A step that
 * has finished stays finished (done or failed, once): the bar never goes
 * back and each step is logged once. Step 6 (Loading home) is the
 * splash's own and finishes as soon as steps 1 to 5 have. The watching,
 * retry, cap and counting are sys/steptrack.h's; this is the boot table.
 *
 * Watching: an inotify fd on <openrc>/started, <openrc>/failed and
 * <run> (create, delete, move), for the caller's loop (bootsteps_fd());
 * any event rescans every source. A directory that is missing is tried
 * again every BOOTSTEPS_RETRY_MS (bootsteps_deadline()). The cap
 * (BOOTSTEPS_CAP_MS after init) logs every step still waiting and sets
 * `capped`; nothing is watched or rescanned after that, or once all steps
 * have finished.
 */
#ifndef CHEF_BOOTSTEPS_H
#define CHEF_BOOTSTEPS_H

#include <stdbool.h>
#include <stdint.h>

#include "steptrack.h"

#define BOOTSTEPS_N 6
#define BOOTSTEPS_CAP_MS 30000
#define BOOTSTEPS_RETRY_MS STEPTRACK_RETRY_MS

#define BOOTSTEPS_OPENRC_DEFAULT "/run/openrc"
#define BOOTSTEPS_RUN_DEFAULT "/run/chef"

/* The step states are the tracker's. */
#define bootstep_state steptrack_state
#define BOOTSTEP_PENDING STEPTRACK_PENDING
#define BOOTSTEP_DONE STEPTRACK_DONE
#define BOOTSTEP_FAILED STEPTRACK_FAILED

struct bootsteps_config {
	const char *openrc;		/* NULL: BOOTSTEPS_OPENRC_DEFAULT */
	const char *run;		/* NULL: BOOTSTEPS_RUN_DEFAULT */
	int64_t (*now)(void);		/* the loop's clock in ms, NULL: CLOCK_MONOTONIC */
	int64_t (*boot_ms)(void);	/* ms since boot for the log, NULL: CLOCK_BOOTTIME */
	void (*log)(const char *fmt, ...) __attribute__((format(printf, 1, 2)));	/* NULL: none */
};

/* Private to bootsteps.c except where noted: the tracker (steptrack.h)
 * holds the step states (t.state), the watches (t.wd), the retry (t.retry_at)
 * and the cap (t.capped). */
struct bootsteps {
	struct bootsteps_config cfg;
	char started_dir[256], failed_dir[256], run_dir[256];
	struct steptrack t;
};

/* Reads the sources once (step 1 is done at once) and starts watching. */
void bootsteps_init(struct bootsteps *b, const struct bootsteps_config *cfg);
/* The inotify fd closed. */
void bootsteps_fini(struct bootsteps *b);

/* The fd to watch for POLLIN, -1 when nothing is watched. It can appear
 * later (inotify_init1 failed at init and is retried by the rescan every
 * BOOTSTEPS_RETRY_MS), so the caller reads it again after every handle
 * and tick. */
int bootsteps_fd(const struct bootsteps *b);
/* The fd readable: drained, then a rescan. */
void bootsteps_handle(struct bootsteps *b);
/* Every source tested again (and missing directories watched if they are
 * there now). */
void bootsteps_rescan(struct bootsteps *b);
/* The next time (the loop's clock) bootsteps_tick() is due: the retry of a
 * missing directory or the cap; -1 none. */
int64_t bootsteps_deadline(const struct bootsteps *b);
void bootsteps_tick(struct bootsteps *b);

/* Steps finished (done or failed) from the first on without a gap: the
 * bar's fill out of BOOTSTEPS_N. */
int bootsteps_finished(const struct bootsteps *b);
/* The first step neither done nor failed (0 based), BOOTSTEPS_N when
 * there is none. The splash shows step current + 1 of BOOTSTEPS_N. */
int bootsteps_current(const struct bootsteps *b);
bool bootsteps_all_finished(const struct bootsteps *b);

/* "Mounting storage", "Starting GPS modem", ... "Loading home" */
const char *bootsteps_label(int step);
/* "GPS modem failed", ... (steps 2 to 5; "" for the others) */
const char *bootsteps_failed_label(int step);

#endif /* CHEF_BOOTSTEPS_H */
