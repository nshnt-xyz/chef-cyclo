/*
 * gps-manager's internal header: the core's types and operations
 * (core.c), the lease server (server.c), the modem owner check and the
 * state-file record (owner.c) and the command-line client (cli.c), shared
 * by main.c and the host tests. Not installed, not for other programs:
 * clients speak the socket protocol in server.c's header. The daemon as a
 * whole is described in main.c's header.
 */
#ifndef GPS_MANAGER_H
#define GPS_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GM_SOCK_PATH   "/run/gps-manager.sock"
#define GM_DIR         "/run/gps-manager"
#define GM_QMUX_SOCK   "/run/qmux_socket"
#define GM_OWNER_LOCK  "/run/gps-up.lock"
#define GM_NMEA_TYPES  "gga|rmc|gsv|gsa|vtg"
#define GM_GPSD_SOURCE "udp://127.0.0.1:20175"

#define GM_GRACE_MS        30000
#define GM_STEP_TIMEOUT_MS 25000
#define GM_OWNER_WAIT_MS   240000
#define GM_OWNER_POLL_MS   2000
#define GM_SILENCE_MS      60000
#define GM_HEALTHY_MS      120000
#define GM_MAX_FAST        5
#define GM_FIX_HOLD_MS     10000
#define GM_INT_TO_TERM_MS  5000
#define GM_TERM_TO_KILL_MS 3000
#define GM_GPSD_KILL_MS    5000
#define GM_TICK_MS         1000
#define GM_LOAD_TIMEOUT_MS 5000	/* chef-state gps-load; then: enabled */
#define GM_SAVE_TIMEOUT_MS 10000	/* chef-state gps-save; then killed */
#define GM_SAVE_DRAIN_MS   3000	/* at exit: the last switch change */

#define GM_MAX_CLIENTS 16
#define GM_LINE_MAX    128	/* a request line, its newline included */
#define GM_OUTQ        4096	/* per-client output queue */
#define GM_PROTO       1	/* the socket protocol's version (hello) */
#define GM_ECHO_MAX    32	/* bytes of a request word echoed in an error */
#define GM_DETAIL_MAX  112

/* ------------------------------------------------------------------ core */

enum gm_state { ST_OFF, ST_STARTING, ST_ACQUIRING, ST_RUNNING, ST_STOPPING, ST_FAILED, ST_DISABLED };
extern const char *const gm_state_names[];

/* Owned processes; CH_STEP is the one qmicli setup/teardown step in flight. */
enum gm_child { CH_FOLLOW, CH_BROKER, CH_GPSD, CH_STEP, CH_COUNT };
extern const char *const gm_child_names[];

enum gm_phase {
	PH_IDLE,	/* nothing owned: OFF, or FAILED waiting/parked/backing off */
	PH_STALE_STOP, PH_STALE_RELEASE,	/* a previous instance's CID */
	PH_OWNER,	/* STARTING: waiting for the modem owner */
	PH_ALLOC, PH_NMEA, PH_START,	/* STARTING: a qmicli step in flight */
	PH_UP,		/* ACQUIRING/RUNNING: gpsd + pipeline */
	PH_STOP_PIPE,	/* STOPPING: follower/broker exiting */
	PH_LOCSTOP, PH_RELEASE,	/* STOPPING: a qmicli step in flight */
	PH_STOP_GPSD,	/* STOPPING: gpsd exiting */
};
extern const char *const gm_phase_names[];

struct gm_owner {
	int pid;
	unsigned long long start;
};

struct gm;

struct gm_ops {
	void *ctx;
	void (*log)(void *ctx, const char *msg);
	/* current modem owner identity, false if there is none ready */
	bool (*owner)(void *ctx, struct gm_owner *o);
	bool (*bridge)(void *ctx);	/* QMUX socket present */
	/* start one qmicli step; its result arrives through gm_step_done() */
	int (*run_step)(void *ctx, char *const argv[]);
	int (*spawn_gpsd)(void *ctx);
	int (*spawn_pipeline)(void *ctx, int cid);
	void (*signal)(void *ctx, enum gm_child c, int sig);
	void (*save)(void *ctx, const struct gm *m);	/* CID/owner/stale changed */
	void (*rotate)(void *ctx);	/* a bring-up starts: keep the previous logs */
	void (*changed)(void *ctx, const struct gm *m);	/* state/detail changed */
	void (*persist)(void *ctx, bool enabled);	/* the switch was changed */
};

struct gm {
	struct gm_ops ops;
	const char *qmicli, *qmux_sock;
	int grace_ms;
	enum gm_state state;
	char detail[GM_DETAIL_MAX];
	bool fix_logged;	/* first RUNNING of this bring-up went to the log */
	enum gm_phase phase;
	int64_t phase_at;	/* start of the current phase (timeouts, escalation) */
	int escalation;		/* signals sent so far in a stop phase */
	bool step_killed;
	int leases;		/* total over all clients and kinds (0 while disabled) */
	bool enabled;		/* the GPS switch */
	bool loaded;		/* the saved switch has been read (or set since) */
	int cid;		/* -1: none allocated */
	struct gm_owner cid_owner;	/* the owner that served cid */
	int stale_cid;		/* a previous instance's CID, -1: none */
	struct gm_owner stale_owner;
	bool session;		/* LOC session 1 (maybe) started on cid */
	bool alive[CH_COUNT];
	bool tearing;		/* teardown requested while a bring-up step ran */
	bool failing;		/* the current/last teardown was caused by a failure */
	bool fail_fast;
	bool owner_lost;	/* FAILED waits for an owner; not counted as fast */
	bool parked;		/* FAILED after GM_MAX_FAST fast failures */
	bool stale_from_owner;	/* stale cleanup interrupted a bring-up in PH_OWNER */
	bool healthy;		/* this run has had GM_HEALTHY_MS of NMEA */
	bool owner_ok;		/* cached owner check */
	bool owner_fresh;	/* owner_ok/owner_seen/owner_at are valid */
	struct gm_owner owner_seen;
	int64_t owner_at;	/* when owner_ok was taken */
	char fail_reason[64];
	int streak;		/* consecutive fast failures */
	int64_t grace_at, retry_at, up_at, last_nmea, last_fix;
	int64_t start_at, first_fix_at;	/* this bring-up's start and first fix (0: none): up_ms/fix_ms */
	unsigned long bringups, failures, nmea_lines, leaked;
	char argv_buf[8][96];
	char *argv[9];
};

void gm_log(struct gm *m, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void gm_set(struct gm *m, enum gm_state st, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void gm_init(struct gm *m, const struct gm_ops *ops);
/* The owner, checked at most every GM_OWNER_POLL_MS; *o gets its identity. */
bool gm_owner(struct gm *m, int64_t now, struct gm_owner *o);
/* An owned part failed: one full teardown, then retry/park/wait. */
void gm_fail(struct gm *m, int64_t now, bool owner_lost, const char *fmt, ...)
	__attribute__((format(printf, 4, 5)));
void gm_teardown(struct gm *m, int64_t now);
/* "CID: '3'" in qmicli's "Client ID not released" block. */
int gm_parse_cid(const char *out);
void gm_step_done(struct gm *m, int64_t now, int rc, const char *out);
void gm_child_exited(struct gm *m, int64_t now, enum gm_child c, int status);
/* One line of the broker's tee. Only counted and time-stamped. */
void gm_nmea_line(struct gm *m, int64_t now, const char *line);
void gm_set_leases(struct gm *m, int64_t now, int n);
int gm_retry(struct gm *m, int64_t now);
void gm_tick(struct gm *m, int64_t now);

/* ---------------------------------------------------------- lease server */

enum gm_kind { LK_MAP, LK_RIDE, LK_COUNT };
extern const char *const gm_kind_names[];

struct linesrv;

/* gps-manager's own state per connection, indexed by the linesrv slot. */
struct gm_client {
	bool watch;
	int leases[LK_COUNT];
};

struct gm_server {
	struct gm *m;
	struct linesrv *srv;	/* the socket: linesrv_listen(), _pollfds(), _handle() */
	struct gm_client clients[GM_MAX_CLIENTS];
	int64_t (*now)(void);
	int broadcasting;	/* inside a loop over the watchers */
	bool sync_pending;	/* a client went during it: sync the count after */
	bool closing;		/* gm_server_fini(): no syncing on the way out */
};

/* The server for M (not listening yet). LOG NULL = chef_log; the socket
 * library logs through it too. 0, or -ENOMEM. */
int gm_server_init(struct gm_server *s, struct gm *m, int64_t (*now)(void),
		   void (*log)(const char *fmt, ...));
/* Every client closed and the socket path unlinked. Their leases are not
 * synced into the core: the caller tears down itself. */
void gm_server_fini(struct gm_server *s);
/* State change: one "state" line to every watcher. */
void gm_server_broadcast(struct gm_server *s);
void gm_server_set_enabled(struct gm_server *s, bool on, bool persist);

/* ------------------------------------------------- owner and state file */

struct gm_paths {
	const char *proc;	/* "/proc" */
	const char *owner_lock;	/* "/run/gps-up.lock" */
	const char *qmux_sock;	/* "/run/qmux_socket" */
};

/* DIR/state: "cid N", "owner PID START", "<child> PID START" lines. */
struct gm_record {
	int cid;
	struct gm_owner owner;
	int pid[CH_STEP];
	unsigned long long start[CH_STEP];
};

/* /proc/PID/stat: start time (field 22), zombie state and comm. */
int gm_proc_start(const char *proc, int pid, unsigned long long *start, bool *zombie,
		  char *comm, size_t commlen);
bool gm_bridge_present(const struct gm_paths *p);
/* modem-owner.sh's modem_owner_pid() in C. */
bool gm_owner_check(const struct gm_paths *p, struct gm_owner *o);
void gm_record_parse(struct gm_record *r, const char *text);
int gm_record_format(const struct gm_record *r, char *buf, size_t len);
/* Is the recorded child still that same process (pid, start time, name)? */
bool gm_record_match(const char *proc, const struct gm_record *r, int c);

/* ------------------------------------------------------------------- cli */

void gm_usage(void);
/* gps-manager [-S SOCK] status | watch | retry | enable | disable | hold KIND... */
int gm_cli_main(const char *sock_path, int argc, char **argv);

#endif /* GPS_MANAGER_H */
