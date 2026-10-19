/*
 * settings_file.h - the system UI's settings file, without the store: its
 * format, its checks and which file is in use. chef-settings (settings.c)
 * is the only writer; another program (the ride recorder) reads it with
 * this alone (docs/features/system-ui.md "Reading the settings from
 * another program"). No LVGL, no threads.
 *
 * File (same rules as chef-state, docs/features/storage.md):
 *
 *   chef-ui-settings 3
 *   brightness 70
 *   theme dark
 *   sleep 60
 *   tz IST-5:30
 *   alert_volume 60
 *   haptics 1
 *   alert_sounds 1
 *   quiet_paused 0
 *   clock_24h 1
 *   auto_brightness 0
 *   auto_offset 0
 *   sha256 <hex of the lines above, newlines included>
 *
 * The parser accepts v1 (no alert/flag lines), v2 (no auto lines) and v3 exactly;
 * missing fields get the defaults and the writer always writes v3 (one-way:
 * an older image rejects v3 and uses its defaults). auto_offset is percent
 * points added to the ambient-light brightness curve, -95..95.
 *
 * Which file: /data/v1/ui/settings when /data is ours (a mount point
 * whose chef-layout reads chef-cyclo-data-v1, checked again before every
 * /data access), otherwise /run/ui/settings. A file that fails any check
 * (or is over UI_SETTINGS_MAX_BYTES, or not a regular file) gives the
 * defaults.
 */
#ifndef CHEF_SETTINGS_FILE_H
#define CHEF_SETTINGS_FILE_H

#include <stdbool.h>
#include <stddef.h>

enum ui_theme_mode { UI_THEME_DARK, UI_THEME_LIGHT, UI_THEME_AUTO };

#define UI_BRIGHTNESS_MIN 5	/* percent; lower is unreadable outdoors and */
#define UI_BRIGHTNESS_MAX 100	/* easy to lose the screen at */
#define UI_AUTO_OFFSET_MIN -95	/* every slider level 5..100 is curve + offset */
#define UI_AUTO_OFFSET_MAX 95
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
	bool auto_brightness;		/* brightness follows the light sensor */
	int auto_offset;		/* percent points on the curve, -95..95 */
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

/* /data is ours: a mount point and data/chef-layout is the v1 marker. */
bool ui_data_ours(const char *data, bool (*mountpoint)(const char *path));
bool ui_mountpoint(const char *path);

/* The settings file under DATA (on_data) or RUN (NULL: /data, /run):
 * 0, or -ENAMETOOLONG. */
int ui_settings_file_path(char *buf, size_t size, const char *data, const char *run, bool on_data);
/* Read the file in use now: *ours whether /data is ours (MOUNTPOINT NULL:
 * ui_mountpoint), then that file read whole and parsed into *out. 0;
 * -EINVAL with *why (static text) when it fails a check or is too big;
 * -errno when it cannot be read (-ENOENT: none yet). On an error the
 * caller uses ui_settings_defaults(). Never writes, locks or creates. */
int ui_settings_read(const char *data, const char *run, bool (*mountpoint)(const char *path),
		     struct ui_settings *out, bool *ours, const char **why);

#endif /* CHEF_SETTINGS_FILE_H */
