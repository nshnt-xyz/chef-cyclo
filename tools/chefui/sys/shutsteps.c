/*
 * shutsteps.c - the shutdown screen's step table (shutsteps.h) on the
 * step tracker (steptrack.h). No LVGL: built into chefui's state sources
 * and the LVGL-free host test.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "shutsteps.h"

/* In lifecycle order, earlier state first: OpenRC links a service's new
 * state before it unlinks the old one (librc rc_service_mark), so a link
 * that is absent when looked for means the next one already exists, and
 * a service on the move is never seen in none of them. */
static const char *const state_names[4] = { "starting", "started", "inactive", "stopping" };

/* The names are the log's: "<name> <verb> at ..." */
static const char *const names[SHUTSTEPS_N - 1] = {
	"state", "Wi-Fi", "Bluetooth", "sensors", "GPS modem",
};
static const char *const verbs[SHUTSTEPS_N - 1] = {
	"saved", "stopped", "stopped", "stopped", "stopped",
};
static const char *const labels[SHUTSTEPS_N - 1] = {
	"Saving state", "Stopping Wi-Fi", "Stopping Bluetooth", "Stopping sensors", "Stopping GPS modem",
};

/* ------------------------------------------------------------ sources */

/* The first line of a small file (no newline), without following a link;
 * false when it cannot be read or is empty. */
static bool first_line(const char *path, char *buf, size_t size)
{
	ssize_t n;
	int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);

	if (fd < 0)
		return false;
	n = read(fd, buf, size - 1);
	close(fd);
	if (n <= 0)
		return false;
	buf[n] = '\0';
	buf[strcspn(buf, "\n")] = '\0';
	return true;
}

/* Not in any of the four state directories. */
static bool stopped(const struct shutsteps *s, const char *svc)
{
	int i;

	for (i = 0; i < 4; i++)
		if (steptrack_exists(s->state_dir[i], svc))
			return false;
	return true;
}

/* <openrc>/softlevel: "shutdown" on its first line. */
static bool softlevel_shutdown(const struct shutsteps *s)
{
	char path[300], line[32];

	if (snprintf(path, sizeof(path), "%s/softlevel", s->openrc_dir) >= (int)sizeof(path))
		return false;
	return first_line(path, line, sizeof(line)) && !strcmp(line, "shutdown");
}

/* What the sources say about step I now (not sticky). */
static enum steptrack_state read_step(void *ctx, int i)
{
	const struct shutsteps *s = ctx;
	bool done = false;

	switch (i) {
	case 0: done = softlevel_shutdown(s); break;
	case 1: done = stopped(s, "networkmanager"); break;
	case 2: done = stopped(s, "bt-up") && stopped(s, "bluetooth"); break;
	case 3: done = stopped(s, "sensord") && stopped(s, "sensors-up"); break;
	case 4: done = stopped(s, "modem-up"); break;
	}
	return done ? STEPTRACK_DONE : STEPTRACK_PENDING;
}

static const struct steptrack_spec spec = {
	.n = SHUTSTEPS_N - 1,		/* step 6 is derived */
	.log_prefix = "shutdown screen",
	.names = names,
	.done_verbs = verbs,
	.read = read_step,
};

/* ---------------------------------------------------------------- API */

void shutsteps_init(struct shutsteps *s, const struct shutsteps_config *cfg)
{
	const char *openrc = cfg->openrc ? cfg->openrc : SHUTSTEPS_OPENRC_DEFAULT;
	struct steptrack_config tcfg = { .now = cfg->now, .boot_ms = cfg->boot_ms, .log = cfg->log };
	struct steptrack_watch watches[5];
	int i;

	snprintf(s->openrc_dir, sizeof(s->openrc_dir), "%s", openrc);
	/* softlevel is written in place or by rename */
	watches[0] = (struct steptrack_watch){ s->openrc_dir, IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE };
	for (i = 0; i < 4; i++) {
		snprintf(s->state_dir[i], sizeof(s->state_dir[i]), "%s/%s", openrc, state_names[i]);
		watches[1 + i] = (struct steptrack_watch){ s->state_dir[i], STEPTRACK_DIR_EVENTS };
	}
	steptrack_init(&s->t, &spec, s, &tcfg, watches, 5);
}

void shutsteps_fini(struct shutsteps *s)
{
	steptrack_fini(&s->t);
}

int shutsteps_fd(const struct shutsteps *s)
{
	return steptrack_fd(&s->t);
}

void shutsteps_handle(struct shutsteps *s)
{
	steptrack_handle(&s->t);
}

void shutsteps_rescan(struct shutsteps *s)
{
	steptrack_rescan(&s->t);
}

int64_t shutsteps_deadline(const struct shutsteps *s)
{
	return steptrack_deadline(&s->t);
}

void shutsteps_tick(struct shutsteps *s)
{
	steptrack_tick(&s->t);
}

int shutsteps_finished(const struct shutsteps *s)
{
	return steptrack_finished(&s->t);
}

int shutsteps_current(const struct shutsteps *s)
{
	int f = shutsteps_finished(s);

	return f == SHUTSTEPS_N ? SHUTSTEPS_N - 1 : f;
}

const char *shutsteps_label(int step, enum shutsteps_reason reason)
{
	if (step >= 0 && step < SHUTSTEPS_N - 1)
		return labels[step];
	if (step != SHUTSTEPS_N - 1)
		return "";
	return reason == SHUTSTEPS_POWEROFF ? "Powering off" : reason == SHUTSTEPS_REBOOT ? "Restarting" : "Shutting down";
}

enum shutsteps_reason shutsteps_read_reason(const char *run, bool *present)
{
	char path[300], line[32];

	if (!run)
		run = SHUTSTEPS_RUN_DEFAULT;
	/* present is that the name exists, whatever can be read of it */
	if (present)
		*present = steptrack_exists(run, SHUTSTEPS_MARKER);
	if (snprintf(path, sizeof(path), "%s/" SHUTSTEPS_MARKER, run) >= (int)sizeof(path) ||
	    !first_line(path, line, sizeof(line)))
		return SHUTSTEPS_UNKNOWN;
	return !strcmp(line, "poweroff") ? SHUTSTEPS_POWEROFF : !strcmp(line, "reboot") ? SHUTSTEPS_REBOOT : SHUTSTEPS_UNKNOWN;
}

const char *shutsteps_reason_name(enum shutsteps_reason r)
{
	return r == SHUTSTEPS_POWEROFF ? "poweroff" : r == SHUTSTEPS_REBOOT ? "reboot" : "unknown";
}
