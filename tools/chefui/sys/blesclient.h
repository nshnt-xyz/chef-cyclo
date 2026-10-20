/*
 * blesclient.h - the blesensd client without LVGL (BLE sensors, see
 * docs/features/bluetooth.md "BLE sensors"): the view of saved sensors
 * with their state and live values, devices found while scanning and the
 * pairing in progress, kept from a `watch` connection on
 * /run/chef/blesensd.sock (a ../common/lineclient, reconnected with backoff 1 s
 * doubling to 8 s, back to 1 s on `ok watch`). blesensd's lines are read
 * with the shared JSON reader; a hello with a proto other than
 * BLESC_PROTO is logged once and the client carries on (the compatibility
 * rule of docs/features/local-sockets.md). "hold on" and "scan on" are
 * sent while wanted, after `ok watch`, again after a reconnect; closing the
 * connection ends both on blesensd's side, so a crash of the caller
 * cannot leave the radio scanning or reconnecting for it. Driven by the
 * caller's loop and monotonic clock (ms): chefui's sys/blesens.c through
 * lcwatch, the recorder with its own poll().
 */
#ifndef CHEF_BLESCLIENT_H
#define CHEF_BLESCLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common/lineclient.h"

#define BLESC_PROTO 1	/* the protocol this client speaks */

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

/* ------------------------------------------------------------ the client */

struct blesclient;

struct blesclient_config {
	const char *path;	/* NULL: CHEF_BLESENSD_SOCK */
	void (*changed)(struct blesclient *bc, void *ctx);	/* the view changed */
	void *ctx;
	void (*log)(const char *fmt, ...) __attribute__((format(printf, 1, 2)));	/* NULL: none */
};

/* Private to blesclient.c except the fields marked "read". */
struct blesclient {
	struct blesclient_config cfg;
	struct lineclient lc;	/* read: for the caller's poll loop or fd watch */
	struct ble_view view;	/* read */
	bool watching;		/* `ok watch` on this connection */
	bool want_hold, want_scan;	/* what the caller wants */
	bool sent_hold, sent_scan;	/* what this connection asked for */
	bool proto_warned;	/* a hello with another proto was logged */
};

void blesclient_init(struct blesclient *bc, const struct blesclient_config *cfg);
void blesclient_start(struct blesclient *bc, int64_t now);
/* Closed (hold and scan end with it), the view emptied. */
void blesclient_stop(struct blesclient *bc, int64_t now);
/* Sensors kept connected, a scan (scan implies hold). */
void blesclient_want(struct blesclient *bc, bool hold, bool scan, int64_t now);
/* Requests: 0, -ENOTCONN without a watching connection (pair: or with a
 * pairing running), -EIO when the send failed. pair starts view.pair;
 * cancel lets it go at once; pair_clear ends a shown outcome. */
int blesclient_pair(struct blesclient *bc, const struct ble_found *f, int64_t now);
int blesclient_cancel(struct blesclient *bc, int64_t now);
int blesclient_forget(struct blesclient *bc, const char *addr, int64_t now);
void blesclient_pair_clear(struct blesclient *bc);

#endif /* CHEF_BLESCLIENT_H */
