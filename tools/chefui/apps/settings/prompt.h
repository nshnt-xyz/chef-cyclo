/*
 * prompt.h - the guided-test prompt overlay of chef-settings.
 *
 * A live test that needs the user (cover the sensor, hold still, ...) shows
 * its steps on the phone: a script writes <run>/chef-settings/prompt and the
 * app shows a card over the bottom of the screen until the file is removed.
 * Format, one `key value` per line, unknown keys ignored:
 *
 *   title STEP 3/9
 *   text COVER the sensor with your palm
 *   until 123456789          CLOCK_MONOTONIC ms when the step ends (optional)
 *
 * With `until` the card counts down whole seconds ("12 s"); one 1 s timer
 * exists only while such a card is shown. The file is noticed by inotify (a
 * writer renames a temp file over it); same trust rules as the args file:
 * a regular file, owned by root, not group/other writable, not a symlink,
 * at most 1 KiB; anything else is ignored with one kmsg line. Text is
 * ASCII (other bytes show as '?'). The card is never clickable, never counts
 * as activity and never wakes the screen. See docs/features/system-ui.md.
 */
#ifndef CHEF_SETTINGS_PROMPT_H
#define CHEF_SETTINGS_PROMPT_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#include "lvgl.h"

#define PROMPT_MAX_BYTES 1024

/* Watch dir/prompt (inotify on dir) and show it; now_ms is the monotonic
 * clock the `until` value refers to. 0 or -errno. */
int prompt_start(const char *dir, int64_t (*now_ms)(void));
void prompt_stop(void);
/* The uid the prompt file must belong to (default 0; host captures and tests). */
void prompt_set_trusted_uid(uid_t uid);
/* Read the file now and update the card (what an inotify event does). */
void prompt_reload(void);
bool prompt_visible(void);
/* Pure: parse the file body. title/text sizes as in the struct below. */
struct prompt_data {
	char title[64], text[160];
	bool has_until;
	int64_t until;
};
void prompt_parse(const char *buf, size_t len, struct prompt_data *out);
#ifdef CHEFUI_TEST
lv_obj_t *prompt_test_box(void);
lv_obj_t *prompt_test_title(void);
lv_obj_t *prompt_test_text(void);
lv_obj_t *prompt_test_count(void);
#endif

#endif /* CHEF_SETTINGS_PROMPT_H */
