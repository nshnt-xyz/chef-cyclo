/*
 * blesens.h - the Bluetooth pages' view of blesensd (BLE sensors, see
 * docs/features/bluetooth.md "BLE sensors"): saved sensors with their
 * state and live values, devices found while scanning, and the pairing in
 * progress. sys/blesens.c keeps it from a `watch` connection on
 * /run/blesensd.sock and calls sys_changed(SYS_BLE) when it changes.
 *
 * The connection is always held (the hub tile counts the sensors); it asks
 * blesensd to reconnect sensors (`hold on`) and to scan (`scan on`) only
 * while a page wants it (sys_ble_want).
 */
#ifndef CHEF_BLESENS_H
#define CHEF_BLESENS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BLE_SENSORS_MAX 8
#define BLE_FOUND_MAX 16

enum ble_kind { BLE_HR, BLE_POWER, BLE_CSC, BLE_KINDS };

enum ble_state {
	BLE_ST_OFF,		/* Bluetooth off */
	BLE_ST_WAITING,		/* not connected, not trying (nobody holds) */
	BLE_ST_CONNECTING,	/* trying, or connected and setting up */
	BLE_ST_CONNECTED,
	BLE_ST_FORGETTING,	/* removal asked, not confirmed yet */
};

/* One kind's latest value: hr (bpm), power (W, cadence rpm), csc (speed
 * km/h, cadence rpm); has_* false = no value (blesensd's null). */
struct ble_value {
	bool has, has_cadence;
	double v, cadence;
};

struct ble_sensor {
	char addr[18], name[64], paired[16];	/* paired "2026-10-13" or "" */
	unsigned kinds;				/* 1 << enum ble_kind */
	enum ble_state st;
	int battery;				/* percent, -1 unknown */
	bool has_rssi, rssi_live;		/* the last advertisement seen */
	int rssi;
	struct ble_value val[BLE_KINDS];
};

struct ble_found {
	char addr[18], name[64];
	unsigned kinds;
	bool has_rssi;
	int rssi;
};

enum ble_pair_state { BLE_PAIR_NONE, BLE_PAIR_RUNNING, BLE_PAIR_DONE, BLE_PAIR_FAILED };

struct ble_view {
	bool known;			/* blesensd answered (watch) */
	bool adapter, powered, scanning;
	int n;
	struct ble_sensor s[BLE_SENSORS_MAX];
	int n_found;
	struct ble_found f[BLE_FOUND_MAX];
	struct {
		enum ble_pair_state st;
		char addr[18], name[64];
		unsigned kinds;
		char step[16];		/* connecting, resolving, bonding, saving */
		char reason[64];	/* why it failed */
		int64_t started_ms;	/* monotonic, for the 20 s bar */
	} pair;
};

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

/* Pure (tests): apply one blesensd line to v; true if it changed. */
bool ble_apply_line(struct ble_view *v, const char *line, int64_t now_ms);
/* Lookups; NULL if none. */
struct ble_sensor *ble_sensor(struct ble_view *v, const char *addr);
/* Sensor rows: kinds of saved sensors in list order (HR, power, speed /
 * cadence; each sensor's kinds in that order). out gets (sensor, kind)
 * pairs; returns how many. */
struct ble_row {
	int sensor;
	enum ble_kind kind;
};
int ble_rows(const struct ble_view *v, struct ble_row *out, int max);
int ble_rows_connected(const struct ble_view *v);
/* Texts. */
const char *ble_kind_title(enum ble_kind k);	/* "Heart rate", "Power", "Speed / cadence" */
const char *ble_kind_service(enum ble_kind k);	/* "BLE Heart Rate", ... */
const char *ble_signal_word(int rssi);		/* strong, fair, weak */
int ble_first_kind(unsigned kinds);

#endif /* CHEF_BLESENS_H */
