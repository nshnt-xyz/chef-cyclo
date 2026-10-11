/*
 * text.h - hub subtitles and status bar mapping from sys_state (pure, so
 * the strings are unit tested). Strings use the design's "·" and "±".
 */
#ifndef CHEF_SETTINGS_TEXT_H
#define CHEF_SETTINGS_TEXT_H

#include <stddef.h>

#include "settings.h"
#include "sysstate.h"
#include "widgets.h"

void text_bt(const struct sys_state *s, char *buf, size_t n);
void text_gps(const struct sys_state *s, char *buf, size_t n);
void text_wifi(const struct sys_state *s, char *buf, size_t n);
void text_display(const struct ui_settings *st, char *buf, size_t n);
void text_time(const struct sys_state *s, char *buf, size_t n);
void text_storage(const struct sys_state *s, char *buf, size_t n);

/* Tiles: on = the subsystem is on (dot and accent frame). */
bool tile_bt_on(const struct sys_state *s);
bool tile_gps_on(const struct sys_state *s);
bool tile_wifi_on(const struct sys_state *s);

void status_from_state(const struct sys_state *s, struct kit_status *out);
const char *glyph_bt(enum kit_bt b);
const char *glyph_gps(enum kit_gps g);
const char *glyph_wifi(enum kit_wifi w);

#endif /* CHEF_SETTINGS_TEXT_H */
