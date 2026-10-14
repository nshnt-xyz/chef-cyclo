/*
 * als.h - ambient light logic for Auto brightness and theme Auto. Pure:
 * no I/O, no LVGL, no clock of its own. The caller (alsclient.c) feeds
 * (lux, t_ms) samples from sensord's illuminance stream, calls als_tick()
 * at als_next_deadline_ms() and acts on the returned events. The stream is
 * on-change only (gaps of many seconds at steady light), so a sample is
 * the light until the next one and everything is driven by time.
 *
 * Brightness: lux maps to a percent through a log curve, is smoothed in
 * the log domain (faster brightening than dimming), shifted by the user's
 * offset and applied through a small deadband. Theme: a hysteresis on the
 * raw lux with hold times. Details and numbers: docs/features/system-ui.md.
 */
#ifndef CHEF_ALS_H
#define CHEF_ALS_H

#include <stdbool.h>
#include <stdint.h>

#define ALS_LIGHT_LUX_DEFAULT 2500.0
#define ALS_DARK_LUX_DEFAULT 800.0
#define ALS_LIGHT_HOLD_S_DEFAULT 5
#define ALS_DARK_HOLD_S_DEFAULT 20
#define ALS_HOLD_S_MIN 1
#define ALS_HOLD_S_MAX 600
#define ALS_TAU_UP_MS 1000.0	/* brightening */
#define ALS_TAU_DOWN_MS 5000.0	/* dimming */
#define ALS_PROBE_MS 30000	/* status probe on the claimed connection */
#define ALS_REPLY_MS 5000	/* no status reply within this: sensord hung, reconnect */
#define ALS_TICK_MS 200		/* smoothing step while the level has not settled */
#define ALS_DEADBAND 2		/* percent points */

struct als_params {
	double light_lux, dark_lux;	/* dark_lux < light_lux */
	int light_hold_s, dark_hold_s;	/* ALS_HOLD_S_MIN..MAX */
};

struct als {
	struct als_params p;
	int offset;			/* auto_offset, percent points */
	/* brightness */
	bool xf_known;			/* xf was ever set (survives restarts) */
	bool have_prev;			/* a sample of this stream was seen: cur_lux holds */
	double cur_lux;			/* the last sample: the light until the next one */
	double xf;			/* filtered log10(lux + 1) */
	int64_t t_xf;			/* the time xf is valid for */
	int applied;			/* percent, -1 none yet */
	/* theme */
	bool light;			/* current ALS theme */
	int64_t above_since, below_since;	/* when cur_lux entered the zone, -1 none */
};

struct als_event {
	bool bright_changed;
	int bright;			/* new applied percent */
	bool theme_changed;
	bool light;			/* the new theme */
	double lux;			/* the lux that decided */
	int64_t held_ms;		/* how long the condition held (0: first decision) */
};

void als_params_default(struct als_params *p);
bool als_params_valid(const struct als_params *p);
void als_init(struct als *a, const struct als_params *p);
/* The sensor stream (re)started or ended: the held lux is forgotten, hold
 * timers stop, and the next sample sets the filter directly and decides
 * the theme at once. The theme, the applied level and the curve state are
 * kept. */
void als_stream_restart(struct als *a);
/* Theme Auto was selected from another mode: Dark, then decided at once from
 * the held lux if the stream has one, else by the next sample. */
void als_theme_reset(struct als *a);
/* Auto brightness was (re)enabled or the backlight was set by someone
 * else: forget the applied level (pct < 0) or record it. */
void als_set_applied(struct als *a, int pct);
void als_set_offset(struct als *a, int offset);

/* The light is a held value: a sample's lux stays current until the next
 * sample, however long. als_feed() takes a sample; als_tick() completes
 * what the held lux implies (theme holds, smoothing) at now; both may
 * report an applied level and a theme transition. als_next_deadline_ms()
 * is when als_tick() next has something to do, -1 for nothing (steady
 * light: no timer needed). */
void als_feed(struct als *a, double lux, int64_t t_ms, struct als_event *ev);
void als_tick(struct als *a, int64_t now_ms, struct als_event *ev);
int64_t als_next_deadline_ms(const struct als *a);

/* Curve percent for a lux value, rounded, 5..100. */
int als_curve_percent(double lux);
/* The curve at the current filtered value; -1 if no sample was ever seen. */
int als_curve_now(const struct als *a);
/* The offset a manual level implies on the current curve, clamped to
 * -50..50; 0 and false when the curve is unknown. */
bool als_offset_for(const struct als *a, int slider, int *offset);

#endif /* CHEF_ALS_H */
