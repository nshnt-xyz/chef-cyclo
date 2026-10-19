/*
 * blesensd's internal header: the constants, the state D and its types,
 * and the functions used across files. Not installed, not for other
 * programs: clients speak the socket protocol in server.c's header. The
 * daemon as a whole is described in main.c's header.
 */
#ifndef BLESENSD_H
#define BLESENSD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#if defined(CHEF_BASU)
#include <basu/sd-bus.h>
#else
#include <systemd/sd-bus.h>
#endif

#include "blesens.h"

#define SOCK_PATH "/run/blesensd.sock"
#define BLUEZ "org.bluez"
#define ADAPTER_IF BLUEZ ".Adapter1"
#define DEVICE_IF BLUEZ ".Device1"
#define CHR_IF BLUEZ ".GattCharacteristic1"
#define BATTERY_IF BLUEZ ".Battery1"
#define AGENT_PATH "/chef/blesensd/agent"
#define PROPS "org.freedesktop.DBus.Properties"
#define OBJMGR "org.freedesktop.DBus.ObjectManager"

#define MAX_CLIENTS 16
#define MAX_SENSORS 8
#define MAX_DEVS 128
#define MAX_CHRS 128
#define MAX_PENDS 64
#define LINE_MAX_IN 256
#define OUT_MAX 512

/* timers in ms, divided by bd_scale (10 under -T) */
#define TICK_MS (1000 / bd_scale)
#define STALE_MS (5000 / bd_scale)
#define BACKOFF_MIN_MS (5000 / bd_scale)
#define BACKOFF_MAX_MS (30000 / bd_scale)
#define CONNECT_TIMEOUT_US (25000000ull / bd_scale)
#define PAIR_TIMEOUT_MS (20000 / bd_scale)
#define CHILD_LOAD_MS (5000 / bd_scale)
#define CHILD_SAVE_MS (10000 / bd_scale)
#define EXIT_SAVE_MS (3000 / bd_scale)
#define RETRY_MS (5000 / bd_scale)
#define BUS_BACKOFF_MAX_MS (10000 / bd_scale)

#define vlog(...) do { if (bd_verbose) bd_notef(__VA_ARGS__); } while (0)

struct dev {
	bool used;
	char path[128], addr[18], name[64];
	bool connected, resolved, paired, trusted;
	bool has_rssi;
	int rssi;
	int battery;		/* Battery1 Percentage, -1 none */
	unsigned kinds;		/* sensor services among its UUIDs */
	bool found_sent;	/* announced as found to watchers */
};

struct chr {
	bool used;
	char path[192], dev[128];
	int uuid16;
	bool can_notify, can_read;
};

enum nstate { NS_NONE, NS_PENDING, NS_OURS };

struct sensor {
	bool used;
	uint32_t sid;		/* identity: never reused */
	char addr[18], name[64], date[16];
	unsigned kinds;
	bool forgetting;	/* removal asked, not yet durably finished */
	bool removal_confirmed;
	uint64_t forget_revision, removal_revision;
	int64_t forget_next_ms;
	bool forget_inflight;
	/* the last advertisement RSSI seen (kept across connections) */
	bool has_rssi;
	int rssi;
	/* per connection; epoch bumps when the connection is lost */
	uint32_t epoch;
	bool was_connected, was_resolved;
	bool link_up;		/* for the log: the device's own connection */
	bool connect_inflight;
	int backoff_ms;
	int64_t next_try_ms;
	enum nstate notify[BS_KINDS];
	bool battery_read;
	enum nstate battery_notify;
	int gatt_battery;	/* from a 0x2A19 read, -1 none */
	int64_t last_ms[BS_KINDS];
	bool has_power;
	int hr, power;
	double cadence[BS_KINDS], speed;
	struct bs_revs crank[BS_KINDS], wheel;
	char shown[OUT_MAX];	/* the last sensor event sent */
};

enum pair_step { PS_IDLE, PS_CONNECT, PS_RESOLVE, PS_NOTIFY, PS_BOND, PS_TRUST };

struct client {
	int fd;
	bool watch, scan, hold;
	char in[LINE_MAX_IN];
	size_t len;
	bool discarding;
};

/* What an asynchronous call was for, and for whom. */
enum pk {
	PK_OBJECTS, PK_AGENT, PK_AGENT_RELEASE, PK_FILTER, PK_SCAN, PK_STOPSCAN, PK_CONNECT, PK_NOTIFY, PK_BATTERY, PK_BATTERY_NOTIFY,
	PK_FORGET, PK_PAIR_AGENT, PK_PAIR_CONNECT, PK_PAIR_NOTIFY, PK_PAIR_BOND, PK_PAIR_TRUST, PK_PLAIN,
};

struct pend {
	bool used;
	sd_bus_slot *slot;
	enum pk kind;
	uint32_t sid, epoch, pair;
	int k;
};

struct child {
	pid_t pid;		/* the leader, 0 once reaped */
	pid_t pgid;		/* its group, kept until the child is finished */
	int64_t until;
};

/* The daemon's state: one instance, D (state.c). */
struct bd_state {
	const char *sock_path, *state_cmd;
	int wheel_mm;
	sd_bus *bus;
	int bus_fd;
	int bus_backoff_ms;
	int64_t bus_retry_ms;
	bool bluez;		/* GetManagedObjects answered */
	char bluez_owner[64];	/* its unique name: agent calls must come from it */
	sd_bus_slot *slots[8];
	int n_slots;
	sd_bus_slot *agent_slot;
	bool agent_registered, agent_inflight;
	bool agent_releasing, agent_release_inflight;
	int64_t agent_retry_ms;
	char adapter[64];
	bool powered, discovering;
	bool scan_on, scan_inflight;	/* our discovery session */
	int64_t scan_retry_ms;
	struct dev devs[MAX_DEVS];
	bool devs_full_logged;
	struct chr chrs[MAX_CHRS];
	struct sensor sensors[MAX_SENSORS];
	uint32_t next_sid;
	struct client clients[MAX_CLIENTS];
	int64_t retry_until;	/* reconnect without a hold until then */
	bool held;		/* a client holds (as last seen) */
	struct pend pends[MAX_PENDS];
	/* records */
	bool loaded;
	struct child load, save;
	int load_fd;
	char load_buf[2048];
	size_t load_len;
	bool save_pending, save_write_ok;
	uint64_t record_revision, save_revision, saved_revision;
	int64_t save_retry_ms;
	/* the one pairing */
	struct {
		enum pair_step step;
		uint32_t id;	/* identity of this attempt */
		char addr[18], path[128], name[64];
		int64_t deadline;
		bool bonded;	/* Pair() succeeded: undone on failure */
		bool connected_by_us, agent_requested;
		unsigned kinds;
	} pr;
};

extern struct bd_state D;

/* -------------------------------- functions used across files, by file */

/* state.c */
extern bool bd_verbose;
extern int bd_scale;
extern int bd_retry_s;
extern pid_t bd_self;
int64_t bd_now_ms(void);
void bd_notef(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
bool bd_addr_ok(const char *a);
void bd_json_str(char *out, size_t n, const char *s);
void bd_kinds_str(unsigned kinds, char *out, size_t n);
unsigned bd_kinds_parse(const char *s);
void bd_name_clean(char *out, size_t n, const char *s);
struct dev *bd_dev_by_path(const char *p);
struct dev *bd_dev_by_addr(const char *a);
struct sensor *bd_sensor_by_addr(const char *a);
struct sensor *bd_sensor_by_sid(uint32_t sid);
struct chr *bd_chr_by_path(const char *p);
struct chr *bd_chr_of(const char *dev, int uuid16);
void bd_dev_of_path(const char *p, char *out, size_t n);
bool bd_is_sensor_chr(int u);
int bd_sensor_battery(const struct sensor *s);
bool bd_sensor_connected(const struct sensor *s);
bool bd_held(void);
bool bd_links_wanted(void);
const char *bd_sensor_state(const struct sensor *s);

/* server.c */
void bd_broadcast(const char *line);
void bd_adapter_event(void);
void bd_sensor_event(struct sensor *s, bool force);
void bd_found_update(struct dev *d, bool changed);
void bd_value_event(struct sensor *s, int k);
void bd_pair_event(const char *state, const char *reason);
void bd_client_input(struct client *c);

/* bus.c */
void bd_pend_cancel(bool all, uint32_t sid, uint32_t pair, bool forget);
int bd_call(const struct pend *tmpl, const char *path, const char *iface, const char *member,
	    uint64_t usec, const char *types, ...);
struct pend bd_plain(enum pk kind);
struct pend bd_for_sensor(enum pk kind, const struct sensor *s, int k);
struct pend bd_for_pair(enum pk kind);
const char *bd_err_name(sd_bus_message *m);
bool bd_err_is(sd_bus_message *m, const char *name);
void bd_agent_release_drive(int64_t now);
void bd_agent_register(int64_t now);
void bd_bus_open(void);
void bd_bus_lost(const char *why);

/* link.c */
void bd_value_bytes(struct chr *c, const uint8_t *b, size_t n);
/* A trigger (start, adapter on, bluetoothd, pairing, a link lost): try
 * again now and for -r seconds. */
void bd_retry_window(void);
void bd_links_sync(void);
void bd_sensor_conn_reset(struct sensor *s);
void bd_sensor_init(struct sensor *s, const char *addr, unsigned kinds, const char *date, const char *name);
void bd_sensor_keep(struct sensor *s, int64_t now);
void bd_sensor_reply(const struct pend *p, sd_bus_message *m);
void bd_scan_sync(void);
void bd_scan_reply(const struct pend *p, sd_bus_message *m);

/* pair.c */
void bd_pair_fail(const char *reason);
void bd_pair_check(void);
void bd_pair_reply(const struct pend *p, sd_bus_message *m);
const char *bd_pair_refusal(const char *addr);
void bd_pair_start(const char *addr);
void bd_forget_step(struct sensor *s, int64_t now);
void bd_forget_reply(const struct pend *p, sd_bus_message *m);
void bd_forget(const char *addr);

/* records.c */
void bd_child_kill(struct child *c);
void bd_save_records(void);
void bd_save_start(void);
void bd_load_start(void);
void bd_load_read(void);
void bd_reap(void);
void bd_children_tick(int64_t now);

/* main.c */

/* cli.c */
int bd_cli_main(const char *sock, int argc, char **argv);

#endif /* BLESENSD_H */
