/*
 * sensord's internal header: the constants, the types every part shares
 * (struct server and what it holds) and the functions used across files.
 * Not installed, not for other programs: clients speak the socket
 * protocol in server.c's header. The daemon as a whole is described in
 * main.c's header.
 */
#ifndef SENSORD_H
#define SENSORD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "compass.h"
#include "msmipc.h"
#include "sns_msgs.h"
#include "sns_reg.h"

#define SENSORD_MAP_PATH	"/usr/share/sensord/sns_reg.map"
#define SENSORD_DSPS_DEV	"/dev/sensors"
#define SENSORD_PROTO		2	/* the socket protocol's version (hello) */
#define SENSORD_MAX_CLIENTS	16
#define SENSORD_LINE_MAX	128	/* a request line, its newline included */
#define SENSORD_OUTQ		16384	/* per-client output queue */
#define SENSORD_ECHO_MAX	32	/* bytes of a request word echoed in an error */
#define SENSORD_QMI_BUF		4096
#define SENSORD_LOOKUP_MS	1000	/* while SMGR is absent */
#define SENSORD_CHECK_MS	5000	/* while up: is it still the same port? */
#define SENSORD_REQ_TIMEOUT_MS	3000
/* A SAM ENABLE is not idempotent (each one the DSP handles creates an
 * instance), so it is sent once and given longer to answer. */
#define SENSORD_QMAG_ENABLE_TIMEOUT_MS	10000
#define SENSORD_QMAG_EXIT_WAIT_MS	1500
/* chef-state mag-save after a registry write-back (regserv.c): at most
 * once a minute, as chef-state itself allows, and bounded. */
#define SENSORD_MAG_SAVE_EVERY_MS	60000
#define SENSORD_MAG_SAVE_MAX_MS		10000
#define SENSORD_REQ_TRIES	3
#define SENSORD_GET_TIMEOUT_MS	5000
#define SENSORD_MAX_PENDING	8
#define SENSORD_DSP_HZ		32768
/* What claiming "heading" claims internally (docs/research/
 * sensors.md section 8), and its output limit. */
#define SENSORD_HEADING_IMU_HZ	50
#define SENSORD_HEADING_MAG_HZ	20
#define SENSORD_HEADING_MAX_HZ	50
/* ... and its internal factory-calibration magn report: full minus
 * factory is the bias the ADSP applies (the live calibrated flag). */
#define SENSORD_HEADING_FAC_HZ	5
/* The ADSP rotation vector's sample rate (-V): fixed, the SAM service
 * has no "change rate", only disable and enable. */
#define SENSORD_ROTVEC_HZ	20

/* kernel/include/uapi/linux/msm_dsps.h: _IOR('d', 3, unsigned int *) --
 * the size field is that of a pointer, as in the kernel's own macro. */
#define SENSORD_DSPS_READ_SLOW_TIMER _IOR('d', 3, unsigned int *)

/* ---------------------------------------------------------- channels */

enum ch_kind { CH_VEC3, CH_SCALAR, CH_PROX, CH_VIRT };

/* CH_SMGR_COUNT - 1: the SMGR channels clients see (magn_factory is the
 * one internal one). */

/* SMGR channels first (report IDs base + index + 1), then the virtual
 * ones computed in sensord or by a SAM service: heading (the compass
 * filter, compass.c) and rotvec (the ADSP's rotation vector, -V).
 * magn_factory is internal (not listed, not claimable): the heading's
 * second magn report at calibration factory, for the live calibrated
 * flag. */
enum { CH_ACCEL, CH_ANGLVEL, CH_MAGN, CH_ILLUM, CH_PROX_, CH_MAGF, CH_SMGR_COUNT,
       CH_HEADING = CH_SMGR_COUNT, CH_ROTVEC, CH_COUNT };

struct channel {
	const char *name;
	const char *unit;
	enum ch_kind kind;
	uint8_t smgr_id, dt;
	unsigned default_hz;
	uint8_t cal;		/* BUFFERING item calibration, SNS_SMGR_CAL_* (-c) */
	bool internal;		/* sensord's own: not listed, not claimable */
	/* from SINGLE_SENSOR_INFO */
	bool present;
	char hw[SNS_SMGR_NAME + 1];
	char vendor[SNS_SMGR_VENDOR + 1];
	unsigned max_hz;
	double range, resolution;
	/* report state on the DSP */
	unsigned rate;		/* acknowledged rate, 0 = no report */
	unsigned want_sent;	/* rate of the request in flight */
	bool busy;		/* a request for this report is in flight */
	unsigned failed;	/* rate the DSP refused; not retried until claims change */
	unsigned long samples;
};

/* ------------------------------------- clients, SMGR, SAM, the server */

/* A connection's claims, in the slot linesrv gives it (server.c). */
struct client {
	struct linesrv_client *lc;	/* NULL = free slot */
	unsigned rate[CH_COUNT];	/* claimed rate, 0 = none */
	bool get[CH_COUNT];		/* waiting for one sample */
	bool get_only[CH_COUNT];	/* the claim exists only for that get */
	int64_t get_deadline[CH_COUNT];
	int64_t next_t[CH_COUNT];	/* per-client decimation: next sample due */
	unsigned long dropped_total;	/* samples the queue had no room for */
};

enum smgr_state { SMGR_DOWN, SMGR_INFO, SMGR_READY };

struct pending {
	bool used;
	uint16_t txn;
	uint16_t msg;
	int ch;				/* channel for report requests */
	uint8_t sensor_id;		/* for SINGLE_INFO */
	unsigned rate;			/* for report requests */
	int64_t sent;
	int tries;
	struct qrtr_packet pkt;		/* for resends */
	char buf[512];
};

enum ts_mode { TS_DSPS, TS_CNTVCT, TS_RX };

/* A client of one SAM algorithm service on the ADSP (QMAG_CAL 0x140
 * with -Q, ROTATION_VECTOR 0x112 with -V): one request in flight at a
 * time; the service is looked up while its channel is used and forgotten
 * when a send to it fails or its socket is reset. Both follow the same
 * rules: instance-0 lookup filtered to 0x3201, a single ENABLE with a
 * long timeout (each one creates an instance), a stray instance
 * disabled at once, disable on release, SMGR loss and exit. */
enum sam_kind { SAM_QMAG, SAM_ROTVEC };

struct sam {
	const char *name;		/* log prefix and status key */
	enum sam_kind kind;
	uint32_t svc;
	uint16_t inst;			/* instance << 8 | version */
	int ch;				/* the channel whose use wants it */
	bool opt;			/* -Q / -V */
	int sock;
	uint32_t node, port;		/* port 0: not looked up */
	bool on;			/* enabled, instance valid */
	uint8_t instance;
	bool failed;			/* refused/unanswered: not retried until the channel is idle */
	bool attr_done;			/* 0x24 asked once per daemon */
	int64_t next_lookup;
	bool missing_logged;
	char seen[160];
	/* the request in flight */
	bool busy;
	uint16_t txn, msg;
	int64_t sent;
	int tries;
	struct qrtr_packet pkt;
	char buf[128];
	/* latest report */
	bool have;
	uint32_t ts;
	int32_t bias[3];		/* QMAG: raw Q16, SMGR frame */
	uint32_t accuracy;		/* QMAG: as sent; ROTVEC: the u8 accuracy */
	float q[4];			/* ROTVEC: x y z w */
	uint8_t coord;			/* ROTVEC: coordinate system byte */
	bool norm_logged;
	uint8_t last_error;		/* from the latest error indication */
	bool had_error;
	unsigned long enables, inds, errors;
	int64_t last_note;		/* last report that went to the log */
	unsigned long foreign;		/* reports for an instance not ours */
	int64_t last_foreign;		/* last one logged */
};

/* The compass (heading channel): compass.c fed from the SMGR samples
 * while anyone claims heading. */
struct heading {
	struct compass c;
	enum compass_mount mount;	/* -M */
	bool active;			/* claimed: inputs claimed internally, filter fed */
	bool have_decl;			/* -D or the declination request */
	double decl;
	/* calibration: live (full - factory, compass_magcal) when known,
	 * else the registry (group 2980, e.g. a reused /run copy) */
	struct compass_magcal mc;
	bool bias_known;		/* registry has group 2980's bias items */
	int32_t bias_raw[3];		/* registry items 3903..3905, Q16 gauss, SMGR frame */
	bool reg_calibrated;		/* registry bias nonzero */
	bool calibrated;		/* the flag in use (and magn calibration full) */
	const char *cal_source;		/* "live", "registry", "none" */
	bool cal_logged;
	unsigned long lines, gyro_lost_logged;
};

struct server {
	struct channel ch[CH_COUNT];
	struct linesrv *srv;		/* the socket (sn_server_start) */
	struct client clients[SENSORD_MAX_CLIENTS];
	int delivering;			/* inside a loop over the claimants */
	bool sync_pending;		/* a client went during it: sync after */
	bool closing;			/* exiting: dropped clients are not synced */
	struct sns_reg reg;
	bool serve_reg2, serve_time2, periodic, force;
	unsigned report_cap;
	unsigned report_base;		/* -i: SMGR report IDs are base + channel + 1 */
	enum ts_mode ts_mode;
	int dsps_fd;
	int reg_sock, time_sock, smgr_sock;
	enum smgr_state st;
	uint32_t smgr_node, smgr_port;
	int64_t next_lookup, next_check;
	bool smgr_missing_logged;
	char smgr_seen[160];		/* last logged lookup result */
	uint16_t txn;
	struct pending pend[SENSORD_MAX_PENDING];
	uint8_t info_ids[SNS_SMGR_MAX_SENSORS];
	int ninfo, info_next;
	int64_t skew_logged;
	unsigned long time2_reqs, smgr_resets, unknown_reqs;
	/* Write-back of the RAM registry over the /run copy it came from
	 * (never persist: sns_reg_save refuses anything but tmpfs/ramfs), a
	 * few seconds after the DSP's last write and on exit, so the copy
	 * holds what the DSP wrote this boot (live: the magnetometer
	 * hard-iron bias in group 2980) for an ADSP restart or re-read. */
	const char *reg_path;		/* NULL: no write-back (-R, tests) */
	bool reg_dirty;
	int64_t reg_dirty_at;		/* last unsaved write */
	int64_t reg_dirty_since;	/* first unsaved write */
	unsigned long reg_saves, reg_save_errors;
	/* After a write-back, `mag_cmd mag-save` (chef-state) saves the
	 * magnetometer bias the DSP learned (group 2980) to /data: a child in
	 * its own process group, at most one at a time, killed after
	 * SENSORD_MAG_SAVE_MAX_MS, at most once per SENSORD_MAG_SAVE_EVERY_MS.
	 * The snapshot stays pending until chef-state accepts it (exit 0; 3
	 * is its own rate limit), so a refused or failed save is retried. */
	const char *mag_cmd;		/* NULL: no saves (-k "", tests) */
	const char *mag_env_run;	/* CHEF_STATE_SENSORS_RUN: reg_path's directory */
	const char *mag_env_map;	/* CHEF_STATE_MAP: the registry map */
	bool mag_pending;
	int64_t mag_at;			/* when the last one started; 0: never */
	pid_t mag_pid;			/* the running one (its group), 0: none */
	int64_t mag_until;
	unsigned long mag_saves, mag_save_fails;
	struct sam qmag, rotvec;
	struct heading heading;
	/* injected for the tests; the defaults are the real clock/logs */
	int64_t (*now_ms)(void);
	void (*log)(const char *fmt, ...);	/* chef_log: syslog + stderr */
	void (*elog)(const char *fmt, ...);	/* stderr only */
};

/* -------------------------------- functions used across files, by file */

/* channel.c */
extern bool sn_verbose;
void sn_stderr_note(const char *fmt, ...);
void sn_vlog(const char *fmt, ...);
extern const struct channel sn_channel_defaults[CH_COUNT];
extern const char *const sn_cal_names[];
const char *sn_cal_name(uint8_t cal);
int sn_channel_from_name(const char *name);
void sn_convert_sample(enum ch_kind kind, const int32_t d[3], double out[3]);
int sn_format_sample(char *buf, size_t len, const struct channel *ch, int64_t t,
		     const int32_t d[3]);
int64_t sn_ticks_to_ns(uint32_t ts, uint32_t anchor_ticks, int64_t anchor_ns);
void sn_copy_name(char *dst, size_t dstsz, const uint8_t *src, uint32_t len);
int64_t sn_real_now_ms(void);
bool sn_read_dsp_ticks(struct server *s, uint32_t *ticks);
void sn_time_anchor(struct server *s, uint32_t newest, uint32_t *a_ticks, int64_t *a_ns);

/* server.c */
void sn_server_init(struct server *s);
/* The socket server (not listening yet), its log s->log: set the log
 * first. 0, or -ENOMEM. */
int sn_server_start(struct server *s);
/* Exiting: every client dropped, its claims not synced to the DSP. */
void sn_server_drop_all(struct server *s);
/* The listener closed and its path unlinked, the table freed. */
void sn_server_fini(struct server *s);
int sn_channel_claims(const struct server *s, int c);
unsigned sn_internal_want(const struct server *s, int c);
unsigned sn_channel_want(const struct server *s, int c);
int sn_channel_used(const struct server *s, int c);
/* A connected fd into the table, as if accepted (the tests); NULL when
 * refused. */
struct client *sn_server_add_client(struct server *s, int fd);
void sn_server_list_line(const struct server *s, char *buf, size_t len);
void sn_server_status_line(const struct server *s, char *buf, size_t len);
bool sn_deliver(struct server *s, int chn, int64_t t, const char *line, size_t n);
void sn_server_publish(struct server *s, int chn, int64_t t, const int32_t d[3]);
void sn_server_tick_clients(struct server *s);
int sn_server_deadline_ms(struct server *s);

/* heading.c */
int sn_format_heading_status(const struct server *s, char *buf, size_t len);
bool sn_heading_feed(struct server *s, int chn, int64_t t, const int32_t d[3]);
void sn_mag_cal_check(struct server *s, const char *why);
void sn_cal_apply(struct server *s, const char *why);
void sn_heading_sync(struct server *s);

/* smgr.c */
void sn_smgr_reset(struct server *s, const char *why);
void sn_smgr_report(struct server *s, int c, unsigned rate);
void sn_smgr_sync(struct server *s);
void sn_notify_claimants(struct server *s, int c, const char *err);
void sn_smgr_rx(struct server *s, struct qrtr_packet *pkt, uint32_t node, uint32_t port);
int sn_svc_lookup(struct server *s, int sock, uint32_t svc, uint32_t want, char *seen_prev,
		  size_t seen_sz, struct msm_ipc_server_info *out);
void sn_smgr_tick(struct server *s);

/* sam.c */
int sn_format_qmag_fields(const struct sam *q, char *buf, size_t len);
const char *sn_sam_state_name(const struct sam *q);
void sn_sam_forget(struct server *s, struct sam *q, const char *why);
int sn_sam_send(struct server *s, struct sam *q, uint16_t msg, struct qmi_elem_info *ei,
		const void *req);
bool sn_sam_wanted(const struct server *s, const struct sam *q);
void sn_sam_sync_all(struct server *s);
void sn_sam_rx(struct server *s, struct sam *q, struct qrtr_packet *pkt, uint32_t node,
	       uint32_t port);
void sn_sam_tick(struct server *s, struct sam *q);
int64_t sn_sam_deadline(const struct server *s, const struct sam *q, int64_t next);

/* regserv.c */
void sn_reg_writeback(struct server *s, const char *why);
void sn_mag_save_tick(struct server *s);
void sn_mag_save_reap(struct server *s);
int64_t sn_mag_save_due(const struct server *s, int64_t next);
void sn_mag_save_stop(struct server *s);
int64_t sn_reg_save_due(const struct server *s);
void sn_reg_tick(struct server *s);
void sn_reg2_rx(struct server *s, struct qrtr_packet *pkt, uint32_t node, uint32_t port);
void sn_time2_rx(struct server *s, struct qrtr_packet *pkt, uint32_t node, uint32_t port);
int sn_ipc_recv(int sock, char *buf, size_t bsz, struct qrtr_packet *pkt, uint32_t *node,
		uint32_t *port);
int sn_publish(struct server *s, int *sock, uint32_t svc, uint16_t vers, uint16_t inst);

/* main.c */
void sn_usage(void);

/* cli.c */
int sn_cli_main(const char *sock_path, long count, int argc, char **argv);

#endif /* SENSORD_H */
