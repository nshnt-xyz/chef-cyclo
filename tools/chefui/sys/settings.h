/*
 * settings.h - the system UI's persistent settings: brightness, theme
 * mode, auto-sleep and time zone.
 *
 * File (same rules as chef-state, docs/features/storage.md):
 *
 *   chef-ui-settings 2
 *   brightness 70
 *   theme dark
 *   sleep 60
 *   tz IST-5:30
 *   alert_volume 60
 *   haptics 1
 *   alert_sounds 1
 *   quiet_paused 0
 *   clock_24h 1
 *   sha256 <hex of the lines above, newlines included>
 *
 * /data/v1/ui/settings when /data is ours (a mount point whose chef-layout
 * reads chef-cyclo-data-v1, checked again before every /data access),
 * otherwise /run/ui/settings (lost at reboot, one kmsg line). Writes are
 * debounced (UI_SETTINGS_DEBOUNCE_MS after the last change) and flushed at
 * exit; a write makes the ui directory (0700, its parent fsynced), writes
 * a 0600 temp file in it, fsyncs, renames and fsyncs the directory, under
 * a lock on /run. /data/v1 itself is never created (chef-storage owns the
 * layout). A file that fails any check is ignored with a kmsg line and the
 * defaults are used; it is replaced at the next save. No descriptor on
 * /data is kept open between writes.
 */
#ifndef CHEF_SETTINGS_H
#define CHEF_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "deviceinfo.h"
#include "storage_usage.h"
#include "timeview.h"

enum ui_theme_mode { UI_THEME_DARK, UI_THEME_LIGHT, UI_THEME_AUTO };

#define UI_BRIGHTNESS_MIN 5	/* percent; lower is unreadable outdoors and */
#define UI_BRIGHTNESS_MAX 100	/* easy to lose the screen at */
#define UI_TZ_MAX 63
#define UI_SETTINGS_DEBOUNCE_MS 2000
#define UI_SETTINGS_MAX_BYTES 4096

struct ui_settings {
	int brightness;			/* percent */
	enum ui_theme_mode theme;
	int sleep_s;			/* 30, 60, 300; 0 = never */
	char tz[UI_TZ_MAX + 1];		/* POSIX TZ, no tzdata */
	int alert_volume;			/* alert preference, 0..100 */
	bool haptics, alert_sounds, quiet_paused, clock_24h;
};

void ui_settings_defaults(struct ui_settings *s);
bool ui_settings_valid(const struct ui_settings *s);
bool ui_settings_equal(const struct ui_settings *a, const struct ui_settings *b);
bool ui_tz_valid(const char *tz);
int ui_brightness_level(int percent);	/* percent -> backlight 1..255 */

/* Pure parse/format. parse: 0 or -1 with *why set (static text).
 * format: bytes written (excluding NUL) or -1 if it does not fit. */
int ui_settings_parse(const char *buf, size_t len, struct ui_settings *out, const char **why);
int ui_settings_format(const struct ui_settings *s, char *buf, size_t size);

/* Where and how long (0 = defaults); set before ui_store_load(). */
#define UI_SETTINGS_LOAD_WAIT_MS 1000	/* first read at start */
#define UI_SETTINGS_EXIT_WAIT_MS 2000	/* exit flush */
#define UI_SETTINGS_PROBE_MS 30000	/* looking for a late /data */
struct ui_store_paths {
	const char *data, *run;			/* NULL: /data, /run */
	bool (*mountpoint)(const char *path);	/* NULL: st_dev against the parent */
	int load_wait_ms, exit_wait_ms, probe_ms;
};
void ui_store_set_paths(const struct ui_store_paths *p);

/*
 * All storage I/O runs on one persistence worker thread (the only thread
 * besides the UI thread): it gets immutable jobs and returns results over
 * an eventfd on the chefui loop. LVGL and the store's state stay on the
 * UI thread.
 *
 * Load: starts the worker, reads the settings (waiting at most
 * load_wait_ms; defaults meanwhile) and decides /data or /run. While on
 * /run it probes every probe_ms for /data becoming ours, then: if the
 * settings were changed this boot they win and are written to /data;
 * otherwise a valid /data file replaces them (ui_store_on_replaced() is
 * called with it). 0 or -errno (no worker).
 */
int ui_store_load(struct ui_settings *out);
typedef void (*ui_store_replace_cb)(const struct ui_settings *s, void *user);
void ui_store_on_replaced(ui_store_replace_cb cb, void *user);
/* A setting changed: saved UI_SETTINGS_DEBOUNCE_MS after the last change.
 * A failed save is logged once per kind of error and retried at the next
 * change or exit. */
void ui_store_changed(const struct ui_settings *s);
/* Exit: wait for the worker and write a pending change, at most
 * exit_wait_ms each. 0 saved, 1 nothing to do, -ETIMEDOUT (storage hung;
 * not saved) or the save's -errno. */
int ui_store_flush(void);
/* Free space of /data, read on the worker (never on the UI thread): the
 * callback gets known (statvfs worked), ours and the free bytes. A request
 * while a job is out runs after it. Nothing before ui_store_load(). */
struct ui_store_stat_result {
	bool known, ours;
	uint64_t total_bytes, free_bytes, root_dev, root_ino;
};
typedef void (*ui_store_stat_cb)(const struct ui_store_stat_result *stat, void *user);
void ui_store_on_stat(ui_store_stat_cb cb, void *user);
void ui_store_stat(void);
typedef void (*ui_store_usage_cb)(const struct sys_usage_result *usage, void *user);
void ui_store_on_usage(ui_store_usage_cb cb, void *user);
void ui_store_usage(void);
#ifdef CHEFUI_TEST
unsigned ui_store_test_usage_requests(void);	/* calls, started or not */
#endif
typedef void (*ui_store_info_cb)(const struct sys_device_info *info, void *user);
void ui_store_on_info(ui_store_info_cb cb, void *user);
void ui_store_info(const struct sys_device_info_request *request);
typedef void (*ui_store_time_boot_cb)(enum sys_time_source source, void *user);
void ui_store_on_time_boot(ui_store_time_boot_cb cb, void *user);
void ui_store_time_boot(const char *path);
/* A page was left: drop its queued optional job (one already started may
 * finish). The free-space read is not cancelled. */
void ui_store_usage_cancel(void);
void ui_store_info_cancel(void);
void ui_store_time_boot_cancel(void);
/* The file in use, and state for logs and tests. */
const char *ui_store_path(void);
bool ui_store_on_data(void);
bool ui_store_pending(void);
bool ui_store_busy(void);
/* Generations: each change bumps the wanted one; the persisted one is the
 * newest whose save completed (file renamed and directory fsynced, on the
 * file in use); failed is the newest whose save failed. Saved means
 * persisted >= wanted; ui_store_pending() is the opposite. */
unsigned long ui_store_wanted_gen(void);
unsigned long ui_store_persisted_gen(void);
unsigned long ui_store_failed_gen(void);
int ui_store_wait_clock(void);	/* the clock bounded waits use (tests) */

/* /data is ours: a mount point and data/chef-layout is the v1 marker. */
bool ui_data_ours(const char *data, bool (*mountpoint)(const char *path));
bool ui_mountpoint(const char *path);

#endif /* CHEF_SETTINGS_H */
