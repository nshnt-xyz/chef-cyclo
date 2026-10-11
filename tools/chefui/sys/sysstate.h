/*
 * sysstate.h - system state for the system UI: battery, clock, GPS,
 * Wi-Fi, Bluetooth and storage. Every source runs on the chefui loop
 * (nonblocking fds and LVGL timers), writes its part of sys_state and
 * calls sys_changed() with its bit; the application subscribes with
 * sys_on_change() and redraws from sys_state.
 *
 * Sources and their contracts: docs/features/system-ui.md.
 */
#ifndef CHEF_SYSSTATE_H
#define CHEF_SYSSTATE_H

#include <stdbool.h>
#include <stdint.h>

enum {
	SYS_BATT = 1 << 0,
	SYS_CLOCK = 1 << 1,
	SYS_GPS = 1 << 2,
	SYS_WIFI = 1 << 3,
	SYS_BT = 1 << 4,
	SYS_STORAGE = 1 << 5,
};

/* gps-manager states (docs/features/gps.md) plus "no manager". */
enum sys_gps {
	SYS_GPS_UNAVAILABLE,	/* socket not reachable */
	SYS_GPS_OFF,		/* OFF, STOPPING */
	SYS_GPS_FAILED,
	SYS_GPS_SEARCHING,	/* STARTING, ACQUIRING */
	SYS_GPS_RUNNING,
};

struct sys_state {
	struct {
		bool known;
		int capacity;		/* 0..100 */
		bool powered;		/* not "unpowered" as powerd defines it */
	} batt;
	struct {
		char hhmm[8];		/* local "14:16" */
		char zone[16];		/* "IST" */
	} clock;
	struct {
		enum sys_gps state;
		int mode;		/* gpsd TPV mode: 0 unknown, 1 none, 2 2D, 3 3D */
		double eph;		/* metres; < 0 unknown */
	} gps;
	struct {
		bool known;		/* NetworkManager answered */
		bool enabled;		/* WirelessEnabled */
		bool device;		/* wlan0 exists */
		uint32_t dev_state;	/* NMDeviceState */
		char ssid[64];		/* active connection Id when activated */
	} wifi;
	struct {
		bool known;		/* BlueZ answered */
		bool adapter;
		bool powered;
		int connected;		/* Device1 with Connected */
	} bt;
	struct {
		bool known;
		bool ours;		/* /data is the chef layout */
		uint64_t free_bytes;
	} storage;
};

extern struct sys_state sys_state;

typedef void (*sys_change_cb)(unsigned what, void *user);
void sys_on_change(sys_change_cb cb, void *user);
void sys_changed(unsigned what);

/* NMDeviceState values the UI distinguishes. */
#define SYS_NM_DEVICE_DISCONNECTED	30
#define SYS_NM_DEVICE_PREPARE		40
#define SYS_NM_DEVICE_ACTIVATED		100

/* Wi-Fi as the UI sees it: off (disabled, no NM, no device), on (enabled,
 * not connected) or connected (wlan0 activated with a connection). */
bool sys_wifi_connected(const struct sys_state *s);
bool sys_wifi_on(const struct sys_state *s);

#endif /* CHEF_SYSSTATE_H */
