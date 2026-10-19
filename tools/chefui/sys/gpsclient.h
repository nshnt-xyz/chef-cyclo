/*
 * gpsclient.h - the gps-manager and gpsd clients without LVGL: what the
 * system UI's GPS sources (gps.c) and the ride recorder need, driven by
 * the caller's loop and monotonic clock (ms) like ../common/lineclient.h.
 *
 * gps-manager (docs/features/gps.md "Socket contract", proto 1): one
 * `watch` connection on its socket, reconnected with backoff (1 s
 * doubling to 8 s, back to 1 s on `ok watch`). Its lines are read with
 * the JSON reader: the hello's proto (an unknown one is logged once), the
 * watch reply, lease replies, `switch` and `state` (unknown types are
 * ignored). The state, the switch, the detail and the bring-up's start
 * and first fix (from up_ms/fix_ms, in the caller's clock) are kept.
 *
 * Leases: wanted per kind (map for the GPS page, ride for the recorder),
 * on the same connection. A wanted lease is sent once `ok watch` and a
 * switch that is not off have been seen, again after a reconnect or when
 * the switch comes back on; the "disabled" error means none is held (it
 * is asked again when the switch is on). One that is no longer wanted is
 * released. A lease lives on its connection, so a crash of the caller
 * releases it too.
 *
 * gpsd (127.0.0.1:port): `?WATCH` JSON while it is wanted: while the
 * manager is RUNNING (the status: TPV mode and eph rounded to a metre),
 * and while the view is wanted also in STARTING and ACQUIRING (the view:
 * TPV and SKY into a struct gps_view, gpsview.h). Reconnected with backoff
 * (1 s doubling to 4 s, back to 1 s on a TPV). Without a full SKY for
 * GPS_SATS_STALE_MS the satellites are cleared (a deadline). The view is
 * forgotten whenever it stops being fed (gpsd or the manager gone, GNSS
 * down, the view no longer wanted); positions never leave it.
 *
 * Changes are reported through cfg.changed with GPSC_CH_STATE (state,
 * switch, detail, status mode and eph) and GPSC_CH_VIEW (the view, the
 * bring-up times, lease times).
 */
#ifndef CHEF_GPSCLIENT_H
#define CHEF_GPSCLIENT_H

#include <poll.h>
#include <stdbool.h>
#include <stdint.h>

#include "common/lineclient.h"
#include "gpsview.h"

#define GPSC_MGR_SOCK "/run/gps-manager.sock"
#define GPSC_GPSD_PORT 2947
#define GPSC_PROTO 1
#define GPSC_DETAIL_MAX 112

/* gps-manager's states; NONE: no connection to it. */
enum gpsc_state {
	GPSC_NONE, GPSC_OFF, GPSC_STARTING, GPSC_ACQUIRING, GPSC_RUNNING, GPSC_STOPPING,
	GPSC_FAILED, GPSC_DISABLED,
};

enum gpsc_lease { GPSC_LEASE_MAP, GPSC_LEASE_RIDE, GPSC_NLEASE };

enum { GPSC_CH_STATE = 1 << 0, GPSC_CH_VIEW = 1 << 1 };

struct gpsclient;

struct gpsclient_config {
	const char *mgr_path;	/* NULL: GPSC_MGR_SOCK */
	int gpsd_port;		/* 0: GPSC_GPSD_PORT */
	void (*changed)(struct gpsclient *gc, unsigned what, void *ctx);
	void (*log)(const char *fmt, ...) __attribute__((format(printf, 1, 2)));	/* NULL: none */
	void *ctx;
};

/* Private to gpsclient.c except where noted; read the public state with
 * the fields marked "read". */
struct gpsclient {
	struct gpsclient_config cfg;
	struct lineclient mgr, gpsd;	/* read: for the caller's poll loop or fd watch */
	enum gpsc_state state;		/* read */
	char detail[GPSC_DETAIL_MAX];	/* read: the manager's detail, "" none */
	int enabled;			/* read: the switch, -1 unknown */
	int mode;			/* read: status TPV mode, 0 none (RUNNING only) */
	double eph;			/* read: status eph in m, rounded, -1 none */
	int64_t start_ms, first_fix_ms;	/* read: the bring-up and its first fix, 0 none */
	int64_t lease_ms[GPSC_NLEASE];	/* read: when the wanted lease was asked, 0 none */
	struct gps_view view;		/* read: only fed while the view is wanted */
	bool want[GPSC_NLEASE], want_view, watching, proto_warned, fifo_warned;
	bool held[GPSC_NLEASE];		/* asked (or held) on this connection */
	int fifo[2 * GPSC_NLEASE], nfifo;	/* kinds of lease requests not answered yet */
	int64_t sats_at;		/* the last full SKY, 0 none */
};

void gpsclient_init(struct gpsclient *gc, const struct gpsclient_config *cfg);
/* Connect to the manager (gpsd follows its state). */
void gpsclient_start(struct gpsclient *gc, int64_t now);
/* Both connections closed (the leases go with them), state NONE. */
void gpsclient_stop(struct gpsclient *gc, int64_t now);

void gpsclient_want_lease(struct gpsclient *gc, enum gpsc_lease kind, bool want, int64_t now);
/* Feed the view from gpsd (also while searching); false: forget it. */
void gpsclient_want_view(struct gpsclient *gc, bool want, int64_t now);
/* The manager's switch ("enable"/"disable"): 0, -ENOTCONN without a
 * watching connection, -EIO when the send failed. */
int gpsclient_set_enabled(struct gpsclient *gc, bool on, int64_t now);

/* For a caller with its own poll(): both connections' pollfds (fd -1 when
 * there is none), then their revents, the next deadline (-1 none) and
 * tick. chefui uses lcwatch on gc->mgr and gc->gpsd instead and a timer
 * on gpsclient_view_deadline(). */
void gpsclient_pollfds(const struct gpsclient *gc, struct pollfd pfd[2]);
void gpsclient_handle(struct gpsclient *gc, const struct pollfd pfd[2], int64_t now);
int64_t gpsclient_deadline(const struct gpsclient *gc);
/* The view's own deadline (the satellites going stale), -1 none. */
int64_t gpsclient_view_deadline(const struct gpsclient *gc);
void gpsclient_tick(struct gpsclient *gc, int64_t now);

/* One gps-manager line, as far as this client uses it (tests). */
enum gpsc_mgr_type {
	GPSC_MGR_OTHER,		/* a type this client does not use */
	GPSC_MGR_HELLO,		/* proto */
	GPSC_MGR_OK,		/* cmd, kind */
	GPSC_MGR_ERROR,		/* cmd, reason, kind */
	GPSC_MGR_SWITCH,	/* enabled */
	GPSC_MGR_STATE,		/* state, detail, up_ms, fix_ms */
};
struct gpsc_mgr_line {
	enum gpsc_mgr_type type;
	char cmd[16], reason[32], kind[8];	/* "" when absent */
	long proto;				/* -1 when absent */
	int enabled;
	enum gpsc_state state;
	char detail[GPSC_DETAIL_MAX];
	long up_ms, fix_ms;			/* the bring-up's age and first fix, -1 null */
};
/* 0 and *out, or -1 for anything unreadable (not JSON, no type, a switch
 * or state line without its members, an unknown state). */
int gpsclient_parse_mgr(const char *line, struct gpsc_mgr_line *out);

#endif /* CHEF_GPSCLIENT_H */
