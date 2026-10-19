/*
 * sys.h - the system UI's state sources (sysstate.h holds the state).
 * Each start function joins the chefui loop and never blocks; a source
 * whose service is missing keeps retrying in the background.
 */
#ifndef CHEF_SYS_H
#define CHEF_SYS_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include "als.h"
#include "sysstate.h"
#include "timeview.h"

struct ui_store_stat_result;

/* Battery: ps_root NULL = /sys/class/power_supply; uevent_fd -1 opens the
 * kernel uevent netlink socket (tests pass a datagram socket instead). */
int sys_battery_start(const char *ps_root, int uevent_fd);
void sys_battery_refresh(void);
void sys_battery_stop(void);
bool sys_uevent_is_power_supply(const char *msg, size_t len);

/* Clock: local hh:mm under a POSIX TZ, updated at each minute and after
 * a clock step (timerfd with TFD_TIMER_CANCEL_ON_SET). */
int sys_clock_start(const char *tz);
/* Local fields of an epoch under the current TZ into out->clock; and
 * setting the TZ without reading the clock (fake runs). */
void sys_clock_fields(time_t t, struct sys_state *out);
void sys_clock_use_tz(const char *tz);
/* "Thursday 8 October 2026" (English names, no locale); tm must be valid */
void sys_clock_date_text(const struct tm *tm, char *out, size_t size);
void sys_clock_set_tz(const char *tz);
void sys_clock_refresh(void);
void sys_clock_stop(void);

/* Current time source verification, queried only while Time is visible. */
int sys_timeview_init(void);
void sys_timeview_want(bool visible);
void sys_timeview_stop(void);

/* GPS: gps-manager `watch` (never a lease) on sock (NULL = default), and
 * gpsd TPV mode/eph while RUNNING (port 0 = 2947). */
int sys_gps_start(const char *sock, int gpsd_port);
void sys_gps_stop(void);
/* One gps-manager line (JSON, proto 1; docs/features/gps.md "Socket
 * contract"), as far as this client uses it. */
enum sys_gps_mgr_type {
	SYS_GPS_MGR_OTHER,	/* a type this client does not use */
	SYS_GPS_MGR_HELLO,	/* proto */
	SYS_GPS_MGR_OK,		/* cmd */
	SYS_GPS_MGR_ERROR,	/* cmd, reason */
	SYS_GPS_MGR_SWITCH,	/* enabled */
	SYS_GPS_MGR_STATE,	/* state, up_ms, fix_ms */
};
struct sys_gps_mgr_line {
	enum sys_gps_mgr_type type;
	char cmd[16], reason[32];	/* "" when absent */
	long proto;			/* -1 when absent */
	int enabled;
	enum sys_gps state;
	long up_ms, fix_ms;		/* the bring-up's age and first fix, -1 null */
};
/* Pure parsers. Manager line: 0 and *out, or -1 for anything unreadable
 * (not JSON, no type, a switch or state line without its members, an
 * unknown state). TPV: 0 with mode (and eph or -1), -1 for anything else. */
int sys_gps_parse_mgr(const char *line, struct sys_gps_mgr_line *out);
int sys_gps_parse_tpv(const char *line, int *mode, double *eph);

/* Ambient light: an illuminance claim on sensord (default
 * /run/sensord.sock) while wanted, fed through the pure logic in als.h.
 * The callback gets every applied brightness change and theme transition.
 * offset: the user's auto_offset, used for the next sample. */
typedef void (*sys_als_cb)(const struct als_event *ev, void *user);
int sys_als_start(const char *sock, const struct als_params *p);
void sys_als_stop(void);
void sys_als_want(bool want);
bool sys_als_wanted(void);
void sys_als_on_change(sys_als_cb cb, void *user);
void sys_als_set_offset(int offset);
bool sys_als_offset_for(int slider, int *offset);
int sys_als_curve_now(void);
void sys_als_set_applied(int pct);	/* -1: forget (re-apply at the next sample) */
void sys_als_theme_reset(void);
bool sys_als_theme_light(void);
int sys_als_applied(void);
/* Pure: one illuminance sample line -> lux. 0 or -1. */
int sys_als_parse_sample(const char *line, double *lux);
/* the same with the sample's CLOCK_MONOTONIC stamp in ns (-1 when absent) */
int sys_als_parse_sample_t(const char *line, double *lux, int64_t *t_ns);
/* a sample stamped before the claim was sent, less ALS_STALE_SLACK_MS (D11) */
bool sys_als_sample_stale(int64_t t_ns, int64_t claim_ns);

/* NetworkManager and BlueZ over the system bus (sd-bus); -ENOTSUP in a
 * build without it. Reconnects when the bus or a service comes back. */
int sys_netbus_start(void);
void sys_netbus_stop(void);
/* The Bluetooth adapter's Powered (the Bluetooth switch); -ENOTCONN
 * without BlueZ or an adapter. */
int sys_bt_set_powered(bool on);

/* Storage: free bytes of /data when it is ours, read asynchronously on the
 * settings worker (needs ui_store_load() first); call on hub entry. */
void sys_storage_refresh(void);
void sys_storage_usage_init(void);
void sys_storage_usage_want(bool visible);

/* Device facts for About (image-info, serial, addresses, uptime), read on
 * the settings worker while the page is visible. width/height/lvgl come
 * from the display and the compile-time LVGL version. */
void sys_device_info_init(void);
void sys_device_info_want(bool visible, int width, int height, const char *lvgl);
#ifdef CHEFUI_TEST
void sys_storage_test_apply_stat(const struct ui_store_stat_result *stat);
struct sys_usage_result;
void sys_storage_test_apply_usage(const struct sys_usage_result *usage);
#endif

/* Fake state for host runs, tests and reference captures: every field
 * from a small text file (format in fake.c). 0 or -errno/-EINVAL. */
int sys_fake_load(const char *path);

#endif /* CHEF_SYS_H */
