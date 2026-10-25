/*
 * shutsteps.h - the shutdown screen's six steps without LVGL: which of
 * them are finished, read from OpenRC's state links and its softlevel, and
 * the reason in /run/chef/shutting-down. The shutdown table on the step
 * tracker (steptrack.h), driven by the caller's loop and clock like
 * bootsteps.h.
 *
 * The steps, their sources and their labels are the table in
 * docs/next-steps/shutdown-screen-handoff.md. Steps 2 to 5 are finished
 * when their services are stopped: <openrc>/started, /starting, /stopping
 * and /inactive hold no link for any of them (only ever tested for
 * existence with lstat; a link is never followed or read, a service that
 * failed at boot or never started is already stopped). Step 1 is finished
 * when <openrc>/softlevel reads "shutdown". Step 6 never finishes: it is
 * shown once 1 to 5 have. A finished step stays finished, and they finish
 * in any order. There are no failures and no cap.
 *
 * Watching: <openrc> (softlevel) and the four state directories; see
 * steptrack.h. Nothing is watched once steps 1 to 5 have finished.
 */
#ifndef CHEF_SHUTSTEPS_H
#define CHEF_SHUTSTEPS_H

#include <stdbool.h>
#include <stdint.h>

#include "steptrack.h"

#define SHUTSTEPS_N 6
#define SHUTSTEPS_RETRY_MS STEPTRACK_RETRY_MS

#define SHUTSTEPS_OPENRC_DEFAULT "/run/openrc"
#define SHUTSTEPS_RUN_DEFAULT "/run/chef"
#define SHUTSTEPS_MARKER "shutting-down"	/* under the run root */

enum shutsteps_reason { SHUTSTEPS_UNKNOWN, SHUTSTEPS_POWEROFF, SHUTSTEPS_REBOOT };

struct shutsteps_config {
	const char *openrc;		/* NULL: SHUTSTEPS_OPENRC_DEFAULT */
	int64_t (*now)(void);		/* the loop's clock in ms, NULL: CLOCK_MONOTONIC */
	int64_t (*boot_ms)(void);	/* ms since boot for the log, NULL: CLOCK_BOOTTIME */
	void (*log)(const char *fmt, ...) __attribute__((format(printf, 1, 2)));	/* NULL: none */
};

/* Private to shutsteps.c except where noted: the tracker holds the step
 * states (t.state), the watches (t.wd) and the retry (t.retry_at). */
struct shutsteps {
	char openrc_dir[256], state_dir[4][288];
	struct steptrack t;
};

/* Reads the sources once and starts watching. */
void shutsteps_init(struct shutsteps *s, const struct shutsteps_config *cfg);
/* The inotify fd closed. */
void shutsteps_fini(struct shutsteps *s);

/* As steptrack.h: the fd to watch (read it again after every handle and
 * tick), handle, rescan, the retry deadline (no cap here) and tick. */
int shutsteps_fd(const struct shutsteps *s);
void shutsteps_handle(struct shutsteps *s);
void shutsteps_rescan(struct shutsteps *s);
int64_t shutsteps_deadline(const struct shutsteps *s);
void shutsteps_tick(struct shutsteps *s);

/* The bar's fill out of SHUTSTEPS_N: steps finished from the first on
 * without a gap, and SHUTSTEPS_N once step 6 is shown. */
int shutsteps_finished(const struct shutsteps *s);
/* The step shown (0 based): the first one not finished, 5 (step 6) when
 * 1 to 5 are. The screen shows step current + 1 of SHUTSTEPS_N. */
int shutsteps_current(const struct shutsteps *s);

/* "Saving state", "Stopping Wi-Fi", ..., and for step 6 (index 5) by the
 * reason: "Powering off", "Restarting", "Shutting down". Without the
 * ellipsis the screen adds. */
const char *shutsteps_label(int step, enum shutsteps_reason reason);

/* The reason in <run>/shutting-down (NULL run: SHUTSTEPS_RUN_DEFAULT):
 * "poweroff" or "reboot" on the first line, anything else (a missing or
 * unreadable file, garbage) unknown. *present is whether the name exists,
 * readable or not (may be NULL). */
enum shutsteps_reason shutsteps_read_reason(const char *run, bool *present);
/* "poweroff", "reboot", "unknown" */
const char *shutsteps_reason_name(enum shutsteps_reason r);

#endif /* CHEF_SHUTSTEPS_H */
