/*
 * powerd's internal header: the defaults, the sample and the daemon state
 * with its hooks, shared by sample.c (sysfs reads, powered(), the uevent
 * filter), policy.c (confirmations, alerts, throttle, policy_step, poll
 * cadence, one whole sample), log.c (state file, CSV format and rotation,
 * the batched -l log and its flush), main.c and the host tests. Not
 * installed, not for other programs: they read the state file (`powerd
 * status`). The daemon as a whole is described in main.c's header.
 */
#ifndef POWERD_H
#define POWERD_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define POWERD_SYSFS        "/sys/class/power_supply"
#define POWERD_VIB_PATH     "/sys/class/timed_output/vibrator/enable"
#define POWERD_WARN_PCT     15
#define POWERD_CRIT_PCT     5
#define POWERD_REARM_PCT    20
#define POWERD_EMPTY_PCT    0
#define POWERD_EMPTY_MV     3300
#define POWERD_OVERTEMP_DC  680	/* deci-degrees C, like the psy temp */
#define POWERD_THR_SET_DC   440
#define POWERD_THR_CLR_DC   420
#define POWERD_CONFIRM_N    3
#define POWERD_CONFIRM_GAP_MS 5000
#define POWERD_STEP_MS      5000	/* SS-BATT-BATT sampling */
#define POWERD_POLL_MS      60000
#define POWERD_POLL_LOW_MS  10000
#define POWERD_ROW_IDLE_MS  30000	/* unchanged throttle ticks: a row every 30 s */
#define POWERD_LOG_MAX      (1024 * 1024)
#define POWERD_FLUSH_MS     600000	/* -l: pending rows reach LOGDIR at least this often */
#define POWERD_PENDING_MAX  (256 * 1024)
#define POWERD_CRIT_BUZZ_MS 600
#define POWERD_NA           INT_MIN		/* unreadable, -22, not a number */
#define POWERD_ABSENT       (INT_MIN + 1)	/* online of a psy that does not exist */

struct sample {
	/* battery psy */
	char status[24];
	char health[24];
	char charge_type[24];
	int present;
	int capacity;		/* % */
	int temp;		/* deci-degrees C */
	int voltage_uv;
	int current_ua;		/* raw: + = discharging, - = charging (Q1) */
	int profile_fcc_ua;	/* constant_charge_current_max: the battery-profile
				 * FCC vote only, NOT the effective FCC; the
				 * throttle's THERMAL_DAEMON_VOTER vote never shows
				 * here (only in the mmi heartbeat's EFFECTIVE FCC) */
	int stl;		/* system_temp_level */
	int nstl;		/* num_system_temp_levels */
	/* bms psy (fuel gauge) */
	int ocv_uv;
	int charge_full_uah;
	int charge_counter_uah;	/* coulomb counter: rises while charging */
	int cycle_count;
	int soc_ready;
	/* usb / dc psy */
	int usb_online;
	char usb_type[24];
	int usb_current_max_ua;
	int usb_input_ua;
	int usb_voltage_uv;
	int input_settled_ua;	/* main/input_current_settled: post-AICL ICL */
	char typec_mode[48];
	int pc_online;		/* pc_port/online: the SDP / host-data-role input */
	int dc_online;
};

struct config {
	int warn_pct;
	int crit_pct;
	int rearm_pct;
	int empty_pct;
	int empty_mv;
	int overtemp_dc;
	int thr_set_dc;
	int thr_clr_dc;
	int confirm_n;
	int confirm_gap_ms;
	int step_ms;
	int poll_ms;
	int poll_low_ms;
	bool act_when_online;	/* --shutdown-when-online: test only */
	bool throttle;		/* false: never touch system_temp_level */
};

struct confirm {
	int count;
	int64_t last;
};

struct powerd {
	struct config cfg;
	const char *sysfs;
	const char *dir;
	const char *logdir;		/* log.csv(.1); NULL = dir */
	char boot_id[40];		/* for the start row */
	bool log_failed;		/* an append (or -l: a flush) failed; logged once */
	int flush_ms;			/* -l: batch interval */
	long pending_max;		/* -l: bound of dir/log.pending */
	int64_t flush_due;		/* -l: when pending rows must be flushed; -1 none */
	bool pending_checked;		/* -l: a torn tail from an earlier powerd removed */
	bool pending_failed;		/* -l: appending to log.pending failed; logged once */
	long pending_dropped;		/* -l: rows dropped from a full log.pending */
	const char *vib;		/* NULL/"" = no buzz */
	char overrides[256];		/* "" in production */
	long log_max;

	bool have_prev;
	struct sample prev;
	bool warned;
	bool crit;
	struct confirm empty;
	struct confirm hot;
	bool shutdown_fired;
	bool shutdown_now;
	char shutdown_reason[160];
	int level;			/* the level we last wrote */
	bool level_broken;		/* a write failed: throttle off */
	bool level_evaluated;
	int64_t level_last;
	int64_t last_row;
	bool have_row;

	void (*log)(const char *fmt, ...);
	int (*write_level)(struct powerd *p, int level);
	void (*shutdown)(struct powerd *p);
	void (*buzz)(struct powerd *p, int ms);
	int (*sync_fd)(int fd);		/* fsync; the tests count the calls */
};

/* sample.c */
void sample_read(const char *root, struct sample *s);
bool supply_online(const struct sample *s);
bool powered(const struct sample *s);
const char *source_name(const struct sample *s);
bool uevent_is_power_supply(const char *buf, size_t len);

/* policy.c */
void powerd_init(struct powerd *p);
const char *alert_name(const struct powerd *p);
const char *fmt_temp(char *buf, size_t len, int dc);
int next_poll_ms(const struct powerd *p, const struct sample *s);
int powerd_sample(struct powerd *p, const char *reason, int64_t now, double uptime);

/* log.c */
extern const char csv_header[];
void write_file_atomic(const char *dir, const char *name, const char *data);
size_t state_format(const struct powerd *p, const struct sample *s, double uptime,
		    char *buf, size_t len);
size_t csv_format(const struct powerd *p, const struct sample *s, double uptime,
		  const char *utc, const char *reason, char *buf, size_t len);
void log_row(struct powerd *p, const char *row, bool urgent, bool durable, int64_t now);
int log_flush(struct powerd *p, bool durable);
void log_flush_now(struct powerd *p, bool durable, int64_t now);
void log_flush_if_due(struct powerd *p, int64_t now);
int flush_main(struct powerd *p);

#endif
