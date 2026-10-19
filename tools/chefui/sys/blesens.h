/*
 * blesens.h - the Bluetooth pages' view of blesensd (BLE sensors, see
 * docs/features/bluetooth.md "BLE sensors"): saved sensors with their
 * state and live values, devices found while scanning, and the pairing in
 * progress. sys/blesens.c is the chefui wrapper of the LVGL-free client
 * (blesclient.h, which has the view's types and rules): ble_view is its
 * view, and sys_changed(SYS_BLE) is called when it changes.
 *
 * The connection is always held (the hub tile counts the sensors); it asks
 * blesensd to reconnect sensors (`hold on`) and to scan (`scan on`) only
 * while a page wants it (sys_ble_want).
 */
#ifndef CHEF_BLESENS_H
#define CHEF_BLESENS_H

#include <stdbool.h>

#include "blesclient.h"

extern struct ble_view ble_view;

/* The connection (sock NULL = /run/blesensd.sock), reconnected with
 * backoff (1 s doubling to 8 s) while blesensd is away. */
int sys_ble_start(const char *sock);
void sys_ble_stop(void);
/* What the pages want: sensors kept connected (a Bluetooth page is seen),
 * a scan (the list page is seen with the switch on). */
void sys_ble_want(bool hold, bool scan);
void sys_ble_wants(bool *hold, bool *scan);	/* what was asked (tests) */
/* Requests; -ENOTCONN without blesensd. pair starts ble_view.pair. */
int sys_ble_pair(const struct ble_found *f);
int sys_ble_cancel(void);
int sys_ble_forget(const char *addr);
/* The pairing's outcome was shown: back to none. */
void sys_ble_pair_clear(void);

#endif /* CHEF_BLESENS_H */
