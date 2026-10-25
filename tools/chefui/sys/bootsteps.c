/*
 * bootsteps.c - the boot splash's step table (bootsteps.h) on the step
 * tracker (steptrack.h). No LVGL: built into chefui's state sources and
 * the LVGL-free host test.
 */
#include <stdio.h>

#include "bootsteps.h"

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

/* ------------------------------------------------------------ sources */

static bool started(const struct bootsteps *b, const char *svc)
{
	return steptrack_exists(b->started_dir, svc);
}

static bool failed(const struct bootsteps *b, const char *svc)
{
	return steptrack_exists(b->failed_dir, svc);
}

/* What the sources say about step I now (not sticky). */
static enum steptrack_state read_step(void *ctx, int i)
{
	const struct bootsteps *b = ctx;

	switch (i) {
	case 0:
		return STEPTRACK_DONE;
	case 1:
		if (failed(b, "modem-up"))
			return STEPTRACK_FAILED;
		return steptrack_exists(b->run_dir, "modem-ready") ? STEPTRACK_DONE : STEPTRACK_PENDING;
	case 2:
		if (failed(b, "sensord") || failed(b, "sensors-up"))
			return STEPTRACK_FAILED;
		return started(b, "sensord") && started(b, "sensors-up") ? STEPTRACK_DONE : STEPTRACK_PENDING;
	case 3:
		if (failed(b, "bt-up") || failed(b, "bluetooth"))
			return STEPTRACK_FAILED;
		return started(b, "bt-up") && started(b, "bluetooth") ? STEPTRACK_DONE : STEPTRACK_PENDING;
	case 4:
		if (failed(b, "networkmanager"))
			return STEPTRACK_FAILED;
		return started(b, "networkmanager") ? STEPTRACK_DONE : STEPTRACK_PENDING;
	}
	return STEPTRACK_PENDING;
}

static void cap_log(void *ctx, int i)
{
	const struct bootsteps *b = ctx;

	if (b->cfg.log)
		b->cfg.log("boot splash: %s not finished after %d s", labels[i], BOOTSTEPS_CAP_MS / 1000);
}

static const struct steptrack_spec spec = {
	.n = BOOTSTEPS_N - 1,		/* step 6 is derived */
	.cap_ms = BOOTSTEPS_CAP_MS,
	.log_prefix = "boot splash",
	.names = names,
	.read = read_step,
	.capped = cap_log,
};

/* ---------------------------------------------------------------- API */

void bootsteps_init(struct bootsteps *b, const struct bootsteps_config *cfg)
{
	const char *openrc = cfg->openrc ? cfg->openrc : BOOTSTEPS_OPENRC_DEFAULT;
	const char *run = cfg->run ? cfg->run : BOOTSTEPS_RUN_DEFAULT;
	struct steptrack_config tcfg = { .now = cfg->now, .boot_ms = cfg->boot_ms, .log = cfg->log };
	struct steptrack_watch watches[3];

	b->cfg = *cfg;
	snprintf(b->started_dir, sizeof(b->started_dir), "%s/started", openrc);
	snprintf(b->failed_dir, sizeof(b->failed_dir), "%s/failed", openrc);
	snprintf(b->run_dir, sizeof(b->run_dir), "%s", run);
	watches[0] = (struct steptrack_watch){ b->started_dir, STEPTRACK_DIR_EVENTS };
	watches[1] = (struct steptrack_watch){ b->failed_dir, STEPTRACK_DIR_EVENTS };
	watches[2] = (struct steptrack_watch){ b->run_dir, STEPTRACK_DIR_EVENTS };
	steptrack_init(&b->t, &spec, b, &tcfg, watches, 3);
}

void bootsteps_fini(struct bootsteps *b)
{
	steptrack_fini(&b->t);
}

int bootsteps_fd(const struct bootsteps *b)
{
	return steptrack_fd(&b->t);
}

void bootsteps_handle(struct bootsteps *b)
{
	steptrack_handle(&b->t);
}

void bootsteps_rescan(struct bootsteps *b)
{
	steptrack_rescan(&b->t);
}

int64_t bootsteps_deadline(const struct bootsteps *b)
{
	return steptrack_deadline(&b->t);
}

void bootsteps_tick(struct bootsteps *b)
{
	steptrack_tick(&b->t);
}

int bootsteps_finished(const struct bootsteps *b)
{
	return steptrack_finished(&b->t);
}

int bootsteps_current(const struct bootsteps *b)
{
	return bootsteps_finished(b);
}

bool bootsteps_all_finished(const struct bootsteps *b)
{
	return steptrack_all_finished(&b->t);
}

const char *bootsteps_label(int step)
{
	return step >= 0 && step < BOOTSTEPS_N ? labels[step] : "";
}

const char *bootsteps_failed_label(int step)
{
	return step >= 0 && step < BOOTSTEPS_N ? failed_labels[step] : "";
}
