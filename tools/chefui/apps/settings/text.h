/*
 * text.h - hub subtitles and status bar mapping from sys_state (pure, so
 * the strings are unit tested). Strings use the design's "·" and "±".
 */
#ifndef CHEF_SETTINGS_TEXT_H
#define CHEF_SETTINGS_TEXT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "settings.h"
#include "sysstate.h"
#include "widgets.h"

void text_bt(const struct sys_state *s, char *buf, size_t n);
void text_gps(const struct sys_state *s, char *buf, size_t n);
void text_wifi(const struct sys_state *s, char *buf, size_t n);
void text_display(const struct ui_settings *st, char *buf, size_t n);
void text_time(const struct sys_state *s, bool clock_24h, char *buf, size_t n);
void text_storage(const struct sys_state *s, char *buf, size_t n);

void text_sound(const struct ui_settings *st, char *buf, size_t n);
/* Time page: the source row, the zone row and the last-sync help line. */
void text_time_source(const struct sys_state *s, char *buf, size_t n);
void text_time_zone(const struct sys_state *s, char *buf, size_t n);
void text_time_sync(const struct sys_state *s, bool clock_24h, char *buf, size_t n);
void text_time_hint(const struct sys_state *s, char *buf, size_t n);
/* Storage page: the headline pieces and category values (bytes in GB). */
void text_gb(uint64_t bytes, bool estimate, char *buf, size_t n);
void text_storage_unit(const struct sys_state *s, char *buf, size_t n);
/* About: uptime "3 h 12 min" from seconds. */
void text_uptime(uint64_t seconds, char *buf, size_t n);

/* Tiles: on = the subsystem is on (dot and accent frame). */
bool tile_bt_on(const struct sys_state *s);
bool tile_gps_on(const struct sys_state *s);
bool tile_wifi_on(const struct sys_state *s);

void status_from_state(const struct sys_state *s, bool clock_24h, struct kit_status *out);
const char *glyph_bt(enum kit_bt b);
const char *glyph_gps(enum kit_gps g);
const char *glyph_wifi(enum kit_wifi w);

#endif /* CHEF_SETTINGS_TEXT_H */
