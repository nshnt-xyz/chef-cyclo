/*
 * sys.h - the system UI's state sources (sysstate.h holds the state).
 * Each start function joins the chefui loop and never blocks; a source
 * whose service is missing keeps retrying in the background.
 */
#ifndef CHEF_SYS_H
#define CHEF_SYS_H

#include <stdbool.h>
#include <stddef.h>

#include "sysstate.h"

/* Battery: ps_root NULL = /sys/class/power_supply; uevent_fd -1 opens the
 * kernel uevent netlink socket (tests pass a datagram socket instead). */
int sys_battery_start(const char *ps_root, int uevent_fd);
void sys_battery_refresh(void);
void sys_battery_stop(void);
bool sys_uevent_is_power_supply(const char *msg, size_t len);

/* Clock: local hh:mm under a POSIX TZ, updated at each minute and after
 * a clock step (timerfd with TFD_TIMER_CANCEL_ON_SET). */
int sys_clock_start(const char *tz);
void sys_clock_set_tz(const char *tz);
void sys_clock_refresh(void);
void sys_clock_stop(void);

/* GPS: gps-manager `watch` (never a lease) on sock (NULL = default), and
 * gpsd TPV mode/eph while RUNNING (port 0 = 2947). */
int sys_gps_start(const char *sock, int gpsd_port);
void sys_gps_stop(void);
/* Pure parsers. state line: 0 and *out, or -1. TPV: 0 with mode (and eph
 * or -1), -1 for anything else. */
int sys_gps_parse_state(const char *line, enum sys_gps *out);
int sys_gps_parse_tpv(const char *line, int *mode, double *eph);

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

/* Fake state for host runs, tests and reference captures: every field
 * from a small text file (format in fake.c). 0 or -errno/-EINVAL. */
int sys_fake_load(const char *path);

#endif /* CHEF_SYS_H */
