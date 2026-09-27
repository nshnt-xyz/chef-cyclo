/*
 * sensord - on-board sensors (accelerometer, gyroscope, magnetometer,
 * ambient light, proximity) for chef-cyclo. See docs/next-steps/
 * sensors-plan.md for the research behind every choice here.
 *
 * On SDM636 the sensor core (Qualcomm "SMGR", its drivers and fusion
 * algorithms) runs inside the ADSP that audio-up boots. The ADSP talks
 * the Sensors1 QMI protocol over the IPC router (AF_MSM_IPC); stock
 * Android runs two apps-side pieces for it, which this one daemon
 * replaces, in one event loop:
 *
 *  1. REG2 server, QMI service 0x10f instance 0x0002 (stock: sensors.qti).
 *     The ADSP reads its configuration from it: driver UUIDs, I2C bus and
 *     address, axis orientation, calibration. Served from a RAM copy of
 *     persist's sns.reg (sensors-up copies it to /run and never leaves
 *     persist mounted), laid out by a map sns-reg-map.py extracted from
 *     the stock sensors.qti at image build time. Writes from the DSP
 *     change the RAM copy and are logged; 3 s after the last one, and on
 *     exit, the RAM copy is written back over the /run file (atomic
 *     rename, tmpfs/ramfs only), so the /run copy holds what the DSP
 *     wrote this boot (the ADSP keeps its own state across a sensord
 *     restart; the copy matters when the ADSP restarts or re-reads the
 *     registry). persist is never written.
 *  2. SMGR client, QMI service 0x100 on the ADSP (stock: libsensor1 +
 *     sensors.ssc.so). Enumerates the sensors (all-sensor-info,
 *     single-sensor-info), then adds a BUFFERING report (0x21, what the
 *     stock HAL sends) per claimed sensor at the highest rate any client
 *     asked for, deletes it when the last claim goes, and decodes the
 *     indications (0x22). -P uses the older periodic REPORT (0x02/0x03)
 *     instead, in case the live run shows buffering refused.
 *  3. Optional (-t) TIME2 server, 0x118 instance 0x3202 (stock:
 *     sensors.qti). Nothing shows the DSP needs it; it is here so the
 *     live run can turn it on if the DSP is seen looking it up.
 *  4. Optional (-Q) QMAG_CAL client, SAM service 0x140 instance 0x3201 on
 *     the ADSP: the DSP's dynamic hard-iron calibration of the
 *     magnetometer, for the calibration check in docs/next-steps/
 *     sensors-plan.md section 7. While magn is claimed it queries the
 *     algorithm attributes (0x24, once per daemon) and enables the
 *     algorithm (0x02); it logs every report (0x05: bias, accuracy) and
 *     error (0x06) indication, adds the latest bias to magn lines and to
 *     status, and disables it (0x03) when the last magn claim goes, when
 *     SMGR is lost and on exit. Off by default: production behaviour is
 *     unchanged without the flag.
 *
 * Data model (IIO channel names and units, so a later kernel IIO driver
 * or an iio-sensor-proxy facade changes the transport, not the data):
 *   accel        m/s^2   x y z
 *   anglvel      rad/s   x y z
 *   magn         gauss   x y z
 *   illuminance  lux     illuminance
 *   proximity    -       near (0/1) and proximity (d[1] as the DSP sends
 *                         it, unscaled: the stock HAL only logs it)
 * SMGR reports Q16 fixed point in its own axis frame; the stock HAL maps
 * it to the Android frame (x right, y up, z out of the screen) as
 * x = d[1], y = d[0], z = -d[2] for accel, gyro and mag, and so does
 * this (disassembly of sensors.ssc.so's processReportInd()s). Magnetic
 * data is Gauss (the HAL multiplies by 100 for uT), light is lux in d[0],
 * proximity is "near" when d[0] / 65536 truncates to non-zero (the HAL's
 * fcvtzs(d[0] * 2^-16) != 0). The HAL also smooths the magnetometer with
 * an 8-sample moving average before Android sees it; sensord publishes
 * the samples unsmoothed, so a compass consumer should filter them.
 *
 * Timestamps: SMGR stamps samples with the DSP's 32768 Hz tick counter
 * (u32). The kernel's /dev/sensors DSPS_IOCTL_READ_SLOW_TIMER returns the
 * same counter (QTimer * 16 / 9375, kernel/drivers/sensors/sensors_ssc.c),
 * so each indication is anchored by reading it back to back with
 * CLOCK_MONOTONIC. -T cntvct reads the QTimer directly instead; -T rx
 * pins the newest sample of each indication to its receive time.
 *
 * Socket API, /run/sensord.sock, one JSON object per line from sensord,
 * plain-text requests from the client:
 *   (on connect)          {"hello":"sensord","proto":1}
 *   list                  {"ok":"list","smgr":"ready","sensors":[...]}
 *   status                {"ok":"status",...}
 *   claim SENSOR [HZ]     {"ok":"claim","sensor":"accel","rate":50}
 *                         then {"sensor":"accel","t":NS,"x":..,"y":..,"z":..}
 *   release SENSOR        {"ok":"release","sensor":"accel"}
 *   get SENSOR [HZ]       one sample line, or {"err":"timeout",...}
 *   errors                {"err":"unknown sensor","sensor":"x"} ...
 * A sensor is powered (report added on the DSP) only while at least one
 * connection claims it; a claim lives on its connection. Each client has
 * a bounded output queue: a slow reader loses samples, never stalls the
 * daemon, and is told how many with {"dropped":N} before the next line
 * that fits. A client that lets a reply overflow is disconnected.
 *
 * Usage: sensord [-r REG] [-m MAP] [-S SOCK] [-T dsps|cntvct|rx] [-b HZ]
 *                [-s CH=ID:DT]... [-c CH=full|factory|raw]... [-i BASE] [-Q]
 *                [-P] [-t] [-R] [-F] [-v]
 *   -r  registry copy to serve (default /run/sensors/sns.reg)
 *   -m  registry map (default /usr/share/sensord/sns_reg.map)
 *   -S  socket path (default /run/sensord.sock)
 *   -T  timestamp source (default dsps, falls back to rx if /dev/sensors
 *       cannot be opened)
 *   -b  cap the SMGR report rate (indications/s) below the sample rate,
 *       so samples arrive batched; default 0 = one report per sample, as
 *       the stock HAL does
 *   -s  map a channel to another SMGR sensor ID/data type (accel=0:0)
 *   -c  calibration SMGR applies to a channel's samples (the BUFFERING
 *       item's calibration field): full (0, default: factory plus any
 *       dynamic calibration), factory (1, what the stock HAL asks for its
 *       "uncalibrated" sensors), raw (2). Repeatable, one per channel.
 *   -i  SMGR report ID base: channel N uses report ID BASE+N+1 (default
 *       0). For a second SMGR client beside the main one (with -R), so
 *       the two never share a report ID even if the DSP keys reports by
 *       ID rather than per client
 *   -Q  run the QMAG_CAL client (see 4. above)
 *   -P  use periodic REPORT (0x02) instead of BUFFERING (0x21)
 *   -t  also serve TIME2 (0x118 inst 0x3202) on the apps node
 *   -R  do not serve REG2 (for a run that tests SMGR without it)
 *   -F  allow claims on channels the sensor info did not list
 *   -v  log every REG2/TIME2 request and SMGR exchange to stderr
 * Client mode, for the telnet shell:
 *   sensord [-S SOCK] [-n COUNT] list | status | get SENSOR [HZ] | watch SENSOR [HZ]
 *
 * Pure parts (channel/rate bookkeeping, sample conversion, client queue
 * and protocol) are covered by tests/test_sensord.c, which includes this
 * file with SENSORD_NO_MAIN; tests/test_e2e.sh runs the daemon against a
 * fake SMGR over a fake transport (tests/fake_ipc.c).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "msmipc.h"
#include "sns_msgs.h"
#include "sns_reg.h"

#define SENSORD_SOCK_PATH	"/run/sensord.sock"
#define SENSORD_REG_PATH	"/run/sensors/sns.reg"
#define SENSORD_MAP_PATH	"/usr/share/sensord/sns_reg.map"
#define SENSORD_DSPS_DEV	"/dev/sensors"
#define SENSORD_PROTO		1
#define SENSORD_MAX_CLIENTS	16
#define SENSORD_LINE_MAX	128
#define SENSORD_OUTQ		16384
#define SENSORD_QMI_BUF		4096
#define SENSORD_LOOKUP_MS	1000	/* while SMGR is absent */
#define SENSORD_CHECK_MS	5000	/* while up: is it still the same port? */
#define SENSORD_REQ_TIMEOUT_MS	3000
/* A SAM ENABLE is not idempotent (each one the DSP handles creates an
 * instance), so it is sent once and given longer to answer. */
#define SENSORD_QMAG_ENABLE_TIMEOUT_MS	10000
#define SENSORD_QMAG_EXIT_WAIT_MS	1500
#define SENSORD_REQ_TRIES	3
#define SENSORD_GET_TIMEOUT_MS	5000
#define SENSORD_MAX_PENDING	8
#define SENSORD_DSP_HZ		32768

/* kernel/include/uapi/linux/msm_dsps.h: _IOR('d', 3, unsigned int *) --
 * the size field is that of a pointer, as in the kernel's own macro. */
#define SENSORD_DSPS_READ_SLOW_TIMER _IOR('d', 3, unsigned int *)

static bool g_verbose;
static volatile sig_atomic_t g_stop;

static void kmsg_note(const char *fmt, ...)
{
	char buf[320];
	va_list ap;
	int fd, n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	fprintf(stderr, "sensord: %s\n", buf);
	fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return;
	dprintf(fd, "sensord: %s\n", buf);
	close(fd);
}

/* Detail worth keeping but too frequent for the kernel log: stderr. */
static void stderr_note(const char *fmt, ...)
{
	va_list ap;

	fputs("sensord: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

/* Debug detail: stderr only, and only with -v. */
static void vlog(const char *fmt, ...)
{
	va_list ap;

	if (!g_verbose)
		return;
	fputs("sensord: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

/* ---------------------------------------------------------- channels */

enum ch_kind { CH_VEC3, CH_SCALAR, CH_PROX };

enum { CH_ACCEL, CH_ANGLVEL, CH_MAGN, CH_ILLUM, CH_PROX_, CH_COUNT };

struct channel {
	const char *name;
	const char *unit;
	enum ch_kind kind;
	uint8_t smgr_id, dt;
	unsigned default_hz;
	uint8_t cal;		/* BUFFERING item calibration, SNS_SMGR_CAL_* (-c) */
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

static const struct channel channel_defaults[CH_COUNT] = {
	[CH_ACCEL]   = { "accel", "m/s^2", CH_VEC3, SNS_SMGR_ID_ACCEL, 0, 10 },
	[CH_ANGLVEL] = { "anglvel", "rad/s", CH_VEC3, SNS_SMGR_ID_GYRO, 0, 10 },
	[CH_MAGN]    = { "magn", "gauss", CH_VEC3, SNS_SMGR_ID_MAG, 0, 10 },
	[CH_ILLUM]   = { "illuminance", "lux", CH_SCALAR, SNS_SMGR_ID_PROX_LIGHT, 1, 5 },
	[CH_PROX_]   = { "proximity", "near", CH_PROX, SNS_SMGR_ID_PROX_LIGHT, 0, 5 },
};

static const char *const cal_names[] = { "full", "factory", "raw" };

static const char *cal_name(uint8_t cal)
{
	return cal <= SNS_SMGR_CAL_RAW ? cal_names[cal] : "?";
}

static int channel_from_name(const char *name)
{
	int i;

	for (i = 0; i < CH_COUNT; i++)
		if (strcmp(channel_defaults[i].name, name) == 0)
			return i;
	return -1;
}

/* SMGR frame -> Android/IIO frame and Q16 -> float, as the stock HAL. */
static void convert_sample(enum ch_kind kind, const int32_t d[3], double out[3])
{
	const double q16 = 1.0 / 65536.0;

	switch (kind) {
	case CH_VEC3:
		out[0] = d[1] * q16;
		out[1] = d[0] * q16;
		out[2] = -(double)d[2] * q16;
		break;
	case CH_SCALAR:
		out[0] = d[0] * q16;
		out[1] = out[2] = 0;
		break;
	case CH_PROX:
		out[0] = d[0] / 65536 != 0;	/* C truncates toward zero, as fcvtzs */
		out[1] = d[1];
		out[2] = 0;
		break;
	}
}

static int format_sample(char *buf, size_t len, const struct channel *ch, int64_t t,
			 const int32_t d[3])
{
	double v[3];

	convert_sample(ch->kind, d, v);
	switch (ch->kind) {
	case CH_VEC3:
		return snprintf(buf, len, "{\"sensor\":\"%s\",\"t\":%" PRId64
				",\"x\":%.6g,\"y\":%.6g,\"z\":%.6g}\n", ch->name, t, v[0], v[1], v[2]);
	case CH_SCALAR:
		return snprintf(buf, len, "{\"sensor\":\"%s\",\"t\":%" PRId64 ",\"%s\":%.6g}\n",
				ch->name, t, ch->name, v[0]);
	case CH_PROX:
		return snprintf(buf, len, "{\"sensor\":\"%s\",\"t\":%" PRId64
				",\"near\":%d,\"proximity\":%.6g}\n", ch->name, t, (int)v[0], v[1]);
	}
	return -1;
}

/* DSP tick stamp -> CLOCK_MONOTONIC ns, given one (ticks, ns) pair read
 * at the same moment. Wrap-safe for stamps within +-18 h of the anchor. */
static int64_t ticks_to_ns(uint32_t ts, uint32_t anchor_ticks, int64_t anchor_ns)
{
	int32_t d = (int32_t)(anchor_ticks - ts);

	return anchor_ns - (int64_t)d * 1000000000LL / SENSORD_DSP_HZ;
}

/* Copy a length-counted byte array from the DSP into a C string. */
static void copy_name(char *dst, size_t dstsz, const uint8_t *src, uint32_t len)
{
	size_t n = len < dstsz - 1 ? len : dstsz - 1;
	size_t i;

	for (i = 0; i < n && src[i]; i++)
		dst[i] = (char)src[i];
	dst[i] = '\0';
}

/* JSON string body (no quotes); drops what JSON cannot carry raw. */
static void json_escape(char *dst, size_t dstsz, const char *src)
{
	size_t o = 0;

	for (; *src && o + 3 < dstsz; src++) {
		unsigned char c = (unsigned char)*src;

		if (c == '"' || c == '\\') {
			dst[o++] = '\\';
			dst[o++] = (char)c;
		} else if (c >= 0x20 && c < 0x7f) {
			dst[o++] = (char)c;
		}
	}
	dst[o] = '\0';
}

/* ----------------------------------------------------------- clients */

struct client {
	int fd;				/* -1 = free slot */
	bool discarding;		/* overlong request: skip to newline */
	unsigned rate[CH_COUNT];	/* claimed rate, 0 = none */
	bool get[CH_COUNT];		/* waiting for one sample */
	bool get_only[CH_COUNT];	/* the claim exists only for that get */
	int64_t get_deadline[CH_COUNT];
	int64_t next_t[CH_COUNT];	/* per-client decimation: next sample due */
	char in[SENSORD_LINE_MAX];
	size_t inlen;
	char out[SENSORD_OUTQ];
	size_t outlen;
	unsigned long dropped;		/* not yet reported */
	unsigned long dropped_total;
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

/* QMAG_CAL client (-Q). One request in flight at a time; the service is
 * looked up while magn is claimed and forgotten when a send to it fails
 * or its socket is reset. */
struct qmag {
	bool opt;			/* -Q */
	int sock;
	uint32_t node, port;		/* port 0: not looked up */
	bool on;			/* enabled, instance valid */
	uint8_t instance;
	bool failed;			/* refused/unanswered: not retried until magn claims drop */
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
	bool have_bias;
	int32_t bias[3];		/* raw Q16, SMGR frame */
	uint32_t accuracy, ts;
	uint8_t last_error;		/* from the latest error indication */
	bool had_error;
	unsigned long enables, inds, errors;
	int64_t last_note;		/* last report that went to kmsg */
};

struct server {
	struct channel ch[CH_COUNT];
	struct client clients[SENSORD_MAX_CLIENTS];
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
	struct qmag qmag;
	/* injected for the tests; the defaults are the real clock/logs */
	int64_t (*now_ms)(void);
	void (*log)(const char *fmt, ...);	/* kmsg + stderr */
	void (*elog)(const char *fmt, ...);	/* stderr only */
};

static int64_t mono_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int64_t real_now_ms(void)
{
	return mono_ns() / 1000000;
}

static void server_init(struct server *s)
{
	int i;

	memset(s, 0, sizeof(*s));
	memcpy(s->ch, channel_defaults, sizeof(s->ch));
	for (i = 0; i < SENSORD_MAX_CLIENTS; i++)
		s->clients[i].fd = -1;
	s->serve_reg2 = true;
	s->dsps_fd = -1;
	s->reg_sock = s->time_sock = s->smgr_sock = -1;
	s->qmag.sock = -1;
	s->st = SMGR_DOWN;
	s->now_ms = real_now_ms;
	s->log = kmsg_note;
	s->elog = stderr_note;
}

/* Highest rate any client wants on a channel, clamped to what the sensor
 * reports it can do; 0 when nobody wants it. */
static unsigned channel_want(const struct server *s, int c)
{
	const struct channel *ch = &s->ch[c];
	unsigned want = 0, max = ch->max_hz ? ch->max_hz : 200;
	int i;

	for (i = 0; i < SENSORD_MAX_CLIENTS; i++)
		if (s->clients[i].fd >= 0 && s->clients[i].rate[c] > want)
			want = s->clients[i].rate[c];
	if (want > max)
		want = max;
	return want;
}

static int channel_claims(const struct server *s, int c)
{
	int i, n = 0;

	for (i = 0; i < SENSORD_MAX_CLIENTS; i++)
		if (s->clients[i].fd >= 0 && s->clients[i].rate[c])
			n++;
	return n;
}

static struct client *server_add_client(struct server *s, int fd)
{
	int i;

	for (i = 0; i < SENSORD_MAX_CLIENTS; i++) {
		struct client *c = &s->clients[i];

		if (c->fd >= 0)
			continue;
		memset(c, 0, sizeof(*c));
		c->fd = fd;
		return c;
	}
	return NULL;
}

static void smgr_sync(struct server *s);
static void qmag_sync(struct server *s);

static void server_drop_client(struct server *s, struct client *c)
{
	int i;

	if (c->fd < 0)
		return;
	for (i = 0; i < CH_COUNT; i++)
		if (c->rate[i] && !c->get_only[i])
			s->log("release %s (client gone)", s->ch[i].name);
	close(c->fd);
	c->fd = -1;
	memset(c->rate, 0, sizeof(c->rate));
	memset(c->get, 0, sizeof(c->get));
	for (i = 0; i < CH_COUNT; i++)
		if (!channel_claims(s, i))
			s->ch[i].failed = 0;
	smgr_sync(s);
}

/* Push queued output; the socket is non-blocking. 0, or -errno (drop). */
static int client_flush(struct client *c)
{
	size_t off = 0;

	while (off < c->outlen) {
		ssize_t n = send(c->fd, c->out + off, c->outlen - off, MSG_NOSIGNAL | MSG_DONTWAIT);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			return -errno;
		}
		off += (size_t)n;
	}
	memmove(c->out, c->out + off, c->outlen - off);
	c->outlen -= off;
	return 0;
}

static bool client_room(const struct client *c, size_t n)
{
	return c->outlen + n <= sizeof(c->out);
}

static void client_append(struct client *c, const char *line, size_t n)
{
	memcpy(c->out + c->outlen, line, n);
	c->outlen += n;
}

/* Reply lines must get through: a client that lets them overflow is not
 * reading at all and is dropped. */
static void client_reply(struct server *s, struct client *c, const char *fmt, ...)
{
	char buf[2048];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n < 0 || (size_t)n >= sizeof(buf))
		n = snprintf(buf, sizeof(buf), "{\"err\":\"internal\"}\n");
	if (!client_room(c, (size_t)n)) {
		s->log("client output full, disconnecting it");
		server_drop_client(s, c);
		return;
	}
	client_append(c, buf, (size_t)n);
	if (client_flush(c))
		server_drop_client(s, c);
}

/* Sample lines are best effort: when one does not fit it is counted, and
 * the count goes out ahead of the next line that does. */
static void client_sample(struct client *c, const char *line, size_t n)
{
	char note[64];
	int k = 0;

	if (c->dropped)
		k = snprintf(note, sizeof(note), "{\"dropped\":%lu}\n", c->dropped);
	if (!client_room(c, (size_t)k + n)) {
		c->dropped++;
		c->dropped_total++;
		return;
	}
	if (k) {
		client_append(c, note, (size_t)k);
		c->dropped = 0;
	}
	client_append(c, line, n);
}

static void server_list_line(const struct server *s, char *buf, size_t len)
{
	static const char *st[] = { "down", "info", "ready" };
	size_t off;
	int i;

	off = (size_t)snprintf(buf, len, "{\"ok\":\"list\",\"smgr\":\"%s\",\"sensors\":[", st[s->st]);
	for (i = 0; i < CH_COUNT && off < len; i++) {
		const struct channel *ch = &s->ch[i];
		char hw[2 * sizeof(ch->hw)], vendor[2 * sizeof(ch->vendor)];

		json_escape(hw, sizeof(hw), ch->hw);
		json_escape(vendor, sizeof(vendor), ch->vendor);
		off += (size_t)snprintf(buf + off, len - off,
			"%s{\"sensor\":\"%s\",\"unit\":\"%s\",\"smgr_id\":%u,\"data_type\":%u,"
			"\"present\":%s,\"name\":\"%s\",\"vendor\":\"%s\",\"max_hz\":%u,"
			"\"range\":%.6g,\"resolution\":%.6g,\"rate\":%u,\"claims\":%d,"
			"\"calibration\":\"%s\"}",
			i ? "," : "", ch->name, ch->unit, ch->smgr_id, ch->dt,
			ch->present ? "true" : "false", hw, vendor, ch->max_hz,
			ch->range, ch->resolution, ch->rate, channel_claims(s, i), cal_name(ch->cal));
	}
	if (off < len)
		snprintf(buf + off, len - off, "]}\n");
}

/* The latest QMAG_CAL bias: device frame gauss (the magn samples' axis
 * map), the raw SMGR-frame Q16 words, accuracy as the DSP sends it. */
static int format_qmag_fields(const struct qmag *q, char *buf, size_t len)
{
	double v[3];

	convert_sample(CH_VEC3, q->bias, v);
	return snprintf(buf, len, "\"bias\":[%.6g,%.6g,%.6g],\"bias_raw\":[%" PRId32 ",%" PRId32
			",%" PRId32 "],\"accuracy\":%" PRIu32, v[0], v[1], v[2], q->bias[0], q->bias[1],
			q->bias[2], q->accuracy);
}

static const char *qmag_state_name(const struct qmag *q)
{
	if (!q->opt)
		return "off";
	if (q->busy)
		return q->msg == SNS_SAM_ENABLE ? "enabling" : q->msg == SNS_SAM_DISABLE ? "disabling"
										: "attributes";
	if (q->on)
		return "on";
	return q->failed ? "failed" : "idle";
}

static void server_status_line(const struct server *s, char *buf, size_t len)
{
	static const char *st[] = { "down", "info", "ready" };
	const struct qmag *q = &s->qmag;
	char qb[192] = "";
	int i, nc = 0;
	size_t off;

	for (i = 0; i < SENSORD_MAX_CLIENTS; i++)
		if (s->clients[i].fd >= 0)
			nc++;
	if (q->have_bias) {
		qb[0] = ',';
		format_qmag_fields(q, qb + 1, sizeof(qb) - 1);
	}
	off = (size_t)snprintf(buf, len,
		 "{\"ok\":\"status\",\"proto\":%d,\"smgr\":\"%s\",\"smgr_node\":%u,\"smgr_port\":%u,"
		 "\"smgr_resets\":%lu,\"api\":\"%s\",\"reg2\":%s,\"reg2_reads\":%lu,"
		 "\"reg2_writes\":%lu,\"reg2_misses\":%lu,\"reg2_saves\":%lu,\"reg2_save_errors\":%lu,"
		 "\"time2\":%s,\"time2_reqs\":%lu,"
		 "\"unknown_reqs\":%lu,\"ts\":\"%s\",\"clients\":%d",
		 SENSORD_PROTO, st[s->st], s->smgr_node, s->smgr_port, s->smgr_resets,
		 s->periodic ? "report" : "buffering",
		 s->serve_reg2 ? "true" : "false", s->reg.reads, s->reg.writes, s->reg.misses,
		 s->reg_saves, s->reg_save_errors,
		 s->serve_time2 ? "true" : "false", s->time2_reqs, s->unknown_reqs,
		 s->ts_mode == TS_DSPS ? "dsps" : s->ts_mode == TS_CNTVCT ? "cntvct" : "rx", nc);
	if (off < len)
		snprintf(buf + off, len - off,
			 ",\"qmag\":\"%s\",\"qmag_instance\":%d,\"qmag_enables\":%lu,"
			 "\"qmag_inds\":%lu,\"qmag_errors\":%lu,\"qmag_last_error\":%d%s}\n",
			 qmag_state_name(q), q->on ? q->instance : -1, q->enables, q->inds, q->errors,
			 q->had_error ? q->last_error : -1, qb);
}

static unsigned parse_rate(const char *arg, unsigned dflt)
{
	char *end;
	unsigned long v;

	if (!arg)
		return dflt;
	v = strtoul(arg, &end, 10);
	if (*end || v == 0 || v > 1000)
		return 0;
	return (unsigned)v;
}

/* One complete request line from a client (no trailing newline). */
static void server_handle_line(struct server *s, struct client *c, char *line)
{
	char *cmd, *name, *rate_s, *save = NULL;
	int ch;

	cmd = strtok_r(line, " \t\r", &save);
	if (!cmd)
		return;
	name = strtok_r(NULL, " \t\r", &save);
	rate_s = strtok_r(NULL, " \t\r", &save);

	if (strcmp(cmd, "list") == 0) {
		char buf[2048];

		server_list_line(s, buf, sizeof(buf));
		client_reply(s, c, "%s", buf);
		return;
	}
	if (strcmp(cmd, "status") == 0) {
		char buf[1536];

		server_status_line(s, buf, sizeof(buf));
		client_reply(s, c, "%s", buf);
		return;
	}
	if (strcmp(cmd, "claim") && strcmp(cmd, "release") && strcmp(cmd, "get")) {
		client_reply(s, c, "{\"err\":\"unknown command\",\"command\":\"%.32s\"}\n", cmd);
		return;
	}
	ch = name ? channel_from_name(name) : -1;
	if (ch < 0) {
		char esc[80];

		json_escape(esc, sizeof(esc), name ? name : "");
		client_reply(s, c, "{\"err\":\"unknown sensor\",\"sensor\":\"%s\"}\n", esc);
		return;
	}
	if (strcmp(cmd, "release") == 0) {
		if (c->rate[ch] && !c->get_only[ch])
			s->log("release %s", s->ch[ch].name);
		c->rate[ch] = 0;
		c->get[ch] = c->get_only[ch] = false;
		if (!channel_claims(s, ch))
			s->ch[ch].failed = 0;
		client_reply(s, c, "{\"ok\":\"release\",\"sensor\":\"%s\"}\n", s->ch[ch].name);
		smgr_sync(s);
		return;
	}
	if (s->st == SMGR_READY && !s->ch[ch].present && !s->force) {
		client_reply(s, c, "{\"err\":\"sensor not present\",\"sensor\":\"%s\"}\n",
			     s->ch[ch].name);
		return;
	}
	{
		unsigned rate = parse_rate(rate_s, s->ch[ch].default_hz);
		unsigned max = s->ch[ch].max_hz ? s->ch[ch].max_hz : 200;

		if (!rate) {
			client_reply(s, c, "{\"err\":\"bad rate\",\"sensor\":\"%s\"}\n", s->ch[ch].name);
			return;
		}
		if (rate > max)
			rate = max;
		if (strcmp(cmd, "claim") == 0) {
			if (!c->rate[ch] || c->get_only[ch])
				s->log("claim %s %u Hz", s->ch[ch].name, rate);
			c->rate[ch] = rate;
			c->get_only[ch] = false;
			c->next_t[ch] = 0;
			client_reply(s, c, "{\"ok\":\"claim\",\"sensor\":\"%s\",\"rate\":%u}\n",
				     s->ch[ch].name, rate);
		} else {
			if (!c->rate[ch]) {
				c->rate[ch] = rate;
				c->get_only[ch] = true;
			}
			c->get[ch] = true;
			c->get_deadline[ch] = s->now_ms() + SENSORD_GET_TIMEOUT_MS;
		}
		smgr_sync(s);
	}
}

/* Buffer bytes from a client fd and hand complete lines over. */
static void server_client_input(struct server *s, struct client *c)
{
	ssize_t n;
	char *nl;

	n = recv(c->fd, c->in + c->inlen, sizeof(c->in) - 1 - c->inlen, MSG_DONTWAIT);
	if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
		server_drop_client(s, c);
		return;
	}
	if (n < 0)
		return;
	c->inlen += (size_t)n;
	c->in[c->inlen] = '\0';
	while ((nl = memchr(c->in, '\n', c->inlen))) {
		size_t used = (size_t)(nl - c->in) + 1;

		*nl = '\0';
		if (c->discarding)
			c->discarding = false;
		else
			server_handle_line(s, c, c->in);
		if (c->fd < 0)
			return;
		memmove(c->in, c->in + used, c->inlen - used);
		c->inlen -= used;
		c->in[c->inlen] = '\0';
	}
	if (c->discarding) {
		c->inlen = 0;
	} else if (c->inlen == sizeof(c->in) - 1) {
		client_reply(s, c, "{\"err\":\"line too long\"}\n");
		c->inlen = 0;
		c->discarding = true;
	}
}

/* One converted sample to every client that wants it. */
static void server_publish(struct server *s, int chn, int64_t t, const int32_t d[3])
{
	struct channel *ch = &s->ch[chn];
	bool released = false;
	char line[384];
	int i, n;

	ch->samples++;
	n = format_sample(line, sizeof(line), ch, t, d);
	if (n <= 0 || (size_t)n >= sizeof(line))
		return;
	/* With -Q: the latest QMAG_CAL bias rides along on magn lines. */
	if (chn == CH_MAGN && s->qmag.have_bias && n >= 2) {
		char ex[192];
		int k;

		ex[0] = ',';
		k = format_qmag_fields(&s->qmag, ex + 1, sizeof(ex) - 1) + 1;
		if (k > 1 && (size_t)k < sizeof(ex) && (size_t)(n - 2 + k + 2) < sizeof(line))
			n = n - 2 + snprintf(line + n - 2, sizeof(line) - (size_t)(n - 2), "%s}\n", ex);
	}
	for (i = 0; i < SENSORD_MAX_CLIENTS; i++) {
		struct client *c = &s->clients[i];

		if (c->fd < 0 || !c->rate[chn])
			continue;
		if (c->get[chn]) {
			client_sample(c, line, (size_t)n);
			c->get[chn] = false;
			if (c->get_only[chn]) {
				c->rate[chn] = 0;
				c->get_only[chn] = false;
				released = true;
			}
			continue;
		}
		if (c->get_only[chn])
			continue;
		/* Deliver at the client's own rate when another client drives
		 * the report faster: one sample per period, 5 % early allowed
		 * for jitter, re-phased after a gap. */
		{
			int64_t period = 1000000000LL / c->rate[chn];

			if (c->next_t[chn] && t + period / 20 < c->next_t[chn])
				continue;
			c->next_t[chn] = c->next_t[chn] && t - c->next_t[chn] < period
					 ? c->next_t[chn] + period : t + period;
		}
		client_sample(c, line, (size_t)n);
	}
	if (released)
		smgr_sync(s);
}

static void server_flush_all(struct server *s)
{
	int i;

	for (i = 0; i < SENSORD_MAX_CLIENTS; i++)
		if (s->clients[i].fd >= 0 && s->clients[i].outlen && client_flush(&s->clients[i]))
			server_drop_client(s, &s->clients[i]);
}

/* ---------------------------------------------------------- timestamps */

static bool read_dsp_ticks(struct server *s, uint32_t *ticks)
{
	if (s->ts_mode == TS_DSPS && s->dsps_fd >= 0) {
		unsigned int v = 0;

		if (ioctl(s->dsps_fd, SENSORD_DSPS_READ_SLOW_TIMER, &v) == 0) {
			*ticks = v;
			return true;
		}
		return false;
	}
#if defined(__aarch64__)
	if (s->ts_mode == TS_CNTVCT) {
		uint64_t v;

		__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
		*ticks = (uint32_t)((v << 4) / 9375);
		return true;
	}
#endif
	return false;
}

/* (ticks, ns) read back to back for one indication. In rx mode, or if
 * the counter cannot be read, the newest stamp of the indication is
 * taken as "now". */
static void time_anchor(struct server *s, uint32_t newest, uint32_t *a_ticks, int64_t *a_ns)
{
	uint32_t t;

	if (read_dsp_ticks(s, &t)) {
		*a_ns = mono_ns();
		*a_ticks = t;
		/* A sample from the future or from long ago means the counter
		 * we read is not the one SMGR stamps with: report, once a
		 * minute, so the live run can see it. */
		{
			int32_t d = (int32_t)(t - newest);
			int64_t now = s->now_ms();

			if ((d < -SENSORD_DSP_HZ || d > 10 * SENSORD_DSP_HZ) &&
			    now - s->skew_logged > 60000) {
				s->skew_logged = now;
				s->log("timestamp skew: counter %u, newest sample %u (%.3f s); check -T",
				       t, newest, d / (double)SENSORD_DSP_HZ);
			}
		}
		return;
	}
	*a_ns = mono_ns();
	*a_ticks = newest;
}

/* ------------------------------------------------------ SMGR client */

static struct pending *pend_find(struct server *s, uint16_t txn)
{
	int i;

	for (i = 0; i < SENSORD_MAX_PENDING; i++)
		if (s->pend[i].used && s->pend[i].txn == txn)
			return &s->pend[i];
	return NULL;
}

static void smgr_reset(struct server *s, const char *why)
{
	int i;

	if (s->st != SMGR_DOWN)
		s->log("smgr %u:%u lost (%s); re-looking up service 0x%x", s->smgr_node,
		       s->smgr_port, why, SNS_SMGR_SVC);
	s->smgr_resets++;
	if (s->smgr_sock >= 0)
		qrtr_close(s->smgr_sock);
	s->smgr_sock = -1;
	s->st = SMGR_DOWN;
	s->smgr_node = s->smgr_port = 0;
	s->smgr_missing_logged = false;
	s->next_lookup = s->now_ms() + SENSORD_LOOKUP_MS;
	for (i = 0; i < SENSORD_MAX_PENDING; i++)
		s->pend[i].used = false;
	for (i = 0; i < CH_COUNT; i++) {
		s->ch[i].rate = 0;
		s->ch[i].busy = false;
	}
	/* QMAG_CAL is a separate service: disable it while SMGR is away
	 * (a no-op without -Q, or if the DSP took it down too). */
	qmag_sync(s);
}

static int smgr_send(struct server *s, uint16_t msg, struct qmi_elem_info *ei, const void *req,
		     int ch, uint8_t sensor_id, unsigned rate)
{
	struct pending *p = NULL;
	ssize_t len;
	int i, rc;

	for (i = 0; i < SENSORD_MAX_PENDING; i++)
		if (!s->pend[i].used) {
			p = &s->pend[i];
			break;
		}
	if (!p) {
		s->log("too many SMGR requests in flight, dropping msg 0x%02x", msg);
		return -EBUSY;
	}
	memset(p, 0, sizeof(*p));
	p->pkt.data = p->buf;
	p->pkt.data_len = sizeof(p->buf);
	s->txn = s->txn == 0xffff ? 1 : s->txn + 1;
	len = qmi_encode_message(&p->pkt, QMI_REQUEST, msg, s->txn, req, ei);
	if (len < 0) {
		s->log("encode SMGR msg 0x%02x failed: %s", msg, strerror((int)-len));
		return (int)len;
	}
	p->used = true;
	p->txn = s->txn;
	p->msg = msg;
	p->ch = ch;
	p->sensor_id = sensor_id;
	p->rate = rate;
	p->sent = s->now_ms();
	p->tries = 1;
	vlog("smgr -> msg 0x%02x txn %u (%zd bytes)", msg, p->txn, len);
	rc = qrtr_sendto(s->smgr_sock, s->smgr_node, s->smgr_port, p->pkt.data,
			 (unsigned)p->pkt.data_len);
	if (rc < 0) {
		smgr_reset(s, strerror(-rc));
		return rc;
	}
	return 0;
}

static void smgr_request_info(struct server *s)
{
	if (s->info_next >= s->ninfo) {
		int i, n = 0;

		for (i = 0; i < CH_COUNT; i++)
			n += s->ch[i].present;
		s->st = SMGR_READY;
		s->log("smgr ready: %d sensor(s), %d of %d channels present", s->ninfo, n, CH_COUNT);
		smgr_sync(s);
		return;
	}
	{
		struct sns_smgr_single_info_req req = { .sensor_id = s->info_ids[s->info_next] };

		smgr_send(s, SNS_SMGR_SINGLE_INFO, sns_smgr_single_info_req_ei, &req, -1,
			  req.sensor_id, 0);
	}
}

static void smgr_found(struct server *s, uint32_t node, uint32_t port)
{
	s->smgr_node = node;
	s->smgr_port = port;
	s->st = SMGR_INFO;
	s->ninfo = s->info_next = 0;
	s->next_check = s->now_ms() + SENSORD_CHECK_MS;
	s->log("smgr service 0x%x found at %u:%u, reading sensor info", SNS_SMGR_SVC, node, port);
	smgr_send(s, SNS_SMGR_ALL_INFO, sns_empty_ei, NULL, -1, 0, 0);
}

/* Add, change or delete the DSP report for one channel. */
static void smgr_report(struct server *s, int c, unsigned rate)
{
	struct channel *ch = &s->ch[c];
	uint8_t report_id = (uint8_t)(s->report_base + c + 1);
	int rc;

	if (s->periodic) {
		struct sns_smgr_rep_req req;

		memset(&req, 0, sizeof(req));
		req.report_id = report_id;
		req.action = rate ? SNS_SMGR_ACTION_ADD : SNS_SMGR_ACTION_DELETE;
		req.report_rate_hz = (uint16_t)rate;
		if (rate) {
			req.n_items = 1;
			req.item[0].sensor_id = ch->smgr_id;
			req.item[0].data_type = ch->dt;
			req.item[0].decimation = SNS_SMGR_DECIMATION_DEFAULT;
		}
		rc = smgr_send(s, SNS_SMGR_REPORT, sns_smgr_rep_req_ei, &req, c, 0, rate);
	} else {
		struct sns_smgr_buf_req req;
		unsigned report_hz = rate;

		if (s->report_cap && report_hz > s->report_cap)
			report_hz = s->report_cap;
		memset(&req, 0, sizeof(req));
		req.report_id = report_id;
		req.action = rate ? SNS_SMGR_ACTION_ADD : SNS_SMGR_ACTION_DELETE;
		req.report_rate_q16 = (uint32_t)report_hz << 16;
		if (rate) {
			req.n_items = 1;
			req.item[0].sensor_id = ch->smgr_id;
			req.item[0].data_type = ch->dt;
			req.item[0].decimation = SNS_SMGR_DECIMATION_DEFAULT;
			req.item[0].calibration = ch->cal;
			req.item[0].sampling_rate_hz = (uint16_t)rate;
			req.item[0].sample_quality = SNS_SMGR_SAMPLE_QUALITY_DEFAULT;
			req.notify_valid = 1;	/* apps processor, no wakeups in suspend */
		}
		rc = smgr_send(s, SNS_SMGR_BUFFERING, sns_smgr_buf_req_ei, &req, c, 0, rate);
	}
	if (rc == 0) {
		ch->busy = true;
		ch->want_sent = rate;
		s->log("%s report %u: %s %u Hz%s%s", rate ? (ch->rate ? "change" : "add") : "delete",
		       report_id, ch->name, rate ? rate : ch->rate,
		       rate && !s->periodic ? ", calibration " : "",
		       rate && !s->periodic ? cal_name(ch->cal) : "");
	}
}

/* Bring every channel's DSP report in line with its claims. */
static void smgr_sync(struct server *s)
{
	int c;

	for (c = 0; c < CH_COUNT && s->st == SMGR_READY; c++) {
		struct channel *ch = &s->ch[c];
		unsigned want = channel_want(s, c);

		if (ch->busy || want == ch->rate)
			continue;
		if (want && want == ch->failed)
			continue;
		if (want && !ch->present && !s->force)
			continue;
		smgr_report(s, c, want);
	}
	qmag_sync(s);
}

static void notify_claimants(struct server *s, int c, const char *err)
{
	int i;

	for (i = 0; i < SENSORD_MAX_CLIENTS; i++)
		if (s->clients[i].fd >= 0 && s->clients[i].rate[c])
			client_reply(s, &s->clients[i], "{\"err\":\"%s\",\"sensor\":\"%s\"}\n", err,
				     s->ch[c].name);
}

static void smgr_report_done(struct server *s, struct pending *p, bool ok, unsigned ack,
			     const struct sns_resp *r, uint32_t nreasons,
			     const struct sns_smgr_reason *reasons)
{
	struct channel *ch = &s->ch[p->ch];
	uint32_t i;

	ch->busy = false;
	for (i = 0; i < nreasons && i < 10; i++)
		s->log("%s report reason: item %u reason %u", ch->name, reasons[i].item,
		       reasons[i].reason);
	if (ok) {
		ch->rate = p->rate;
		ch->failed = 0;
		if (ack)
			s->log("%s report accepted with changes (ack %u)", ch->name, ack);
	} else {
		s->log("%s report %s %u Hz refused: result %u err %u ack %u", ch->name,
		       p->rate ? "add" : "delete", p->rate, r->result, r->err, ack);
		if (p->rate) {
			ch->failed = p->rate;
			notify_claimants(s, p->ch, "smgr refused report");
		} else {
			ch->rate = 0;	/* nothing more we can do about it */
		}
	}
	smgr_sync(s);
}

static void smgr_handle_resp(struct server *s, struct qrtr_packet *pkt, unsigned msg,
			     uint16_t txn)
{
	struct pending *p = pend_find(s, txn);
	unsigned int t;
	int rc;

	if (!p || p->msg != msg) {
		vlog("smgr <- stray response msg 0x%02x txn %u", msg, txn);
		return;
	}
	p->used = false;

	switch (msg) {
	case SNS_SMGR_ALL_INFO: {
		struct sns_smgr_all_info_resp r;
		uint32_t i;

		memset(&r, 0, sizeof(r));
		rc = qmi_decode_message(&r, &t, pkt, QMI_RESPONSE, msg, sns_smgr_all_info_resp_ei);
		if (rc < 0 || r.resp.result != SNS_RESULT_SUCCESS) {
			s->log("all-sensor-info failed (decode %d, result %u err %u)", rc,
			       r.resp.result, r.resp.err);
			if (s->force) {
				s->st = SMGR_READY;
				s->log("smgr ready without sensor info (-F)");
				smgr_sync(s);
			} else {
				smgr_reset(s, "no sensor info");
			}
			return;
		}
		s->ninfo = 0;
		for (i = 0; i < r.n && i < SNS_SMGR_MAX_SENSORS; i++) {
			char name[SNS_SMGR_SHORT_NAME + 1];

			copy_name(name, sizeof(name), r.info[i].name, r.info[i].name_len);
			s->log("smgr sensor id %u '%s'", r.info[i].sensor_id, name);
			s->info_ids[s->ninfo++] = r.info[i].sensor_id;
		}
		s->info_next = 0;
		smgr_request_info(s);
		return;
	}
	case SNS_SMGR_SINGLE_INFO: {
		struct sns_smgr_single_info_resp r;
		uint32_t i;
		int c;

		memset(&r, 0, sizeof(r));
		rc = qmi_decode_message(&r, &t, pkt, QMI_RESPONSE, msg, sns_smgr_single_info_resp_ei);
		if (rc < 0 || r.resp.result != SNS_RESULT_SUCCESS)
			s->log("sensor-info for id %u failed (decode %d, result %u err %u)",
			       p->sensor_id, rc, r.resp.result, r.resp.err);
		for (i = 0; rc >= 0 && i < r.n && i < SNS_SMGR_MAX_DT; i++) {
			const struct sns_smgr_dt_info *d = &r.dt[i];
			char name[SNS_SMGR_NAME + 1], vendor[SNS_SMGR_VENDOR + 1];

			copy_name(name, sizeof(name), d->name, d->name_len);
			copy_name(vendor, sizeof(vendor), d->vendor, d->vendor_len);
			s->log("smgr id %u dt %u '%s' (%s) v%u max %u Hz range %.4g res %.4g",
			       d->sensor_id, d->data_type, name, vendor, d->version, d->max_rate_hz,
			       d->max_range / 65536.0, d->resolution / 65536.0);
			for (c = 0; c < CH_COUNT; c++) {
				struct channel *ch = &s->ch[c];

				if (ch->smgr_id != d->sensor_id || ch->dt != d->data_type)
					continue;
				ch->present = true;
				memcpy(ch->hw, name, sizeof(ch->hw));
				memcpy(ch->vendor, vendor, sizeof(ch->vendor));
				ch->max_hz = d->max_rate_hz;
				ch->range = d->max_range / 65536.0;
				ch->resolution = d->resolution / 65536.0;
			}
		}
		s->info_next++;
		smgr_request_info(s);
		return;
	}
	case SNS_SMGR_BUFFERING: {
		struct sns_smgr_buf_resp r;
		bool ok;

		memset(&r, 0, sizeof(r));
		rc = qmi_decode_message(&r, &t, pkt, QMI_RESPONSE, msg, sns_smgr_buf_resp_ei);
		ok = rc >= 0 && r.resp.result == SNS_RESULT_SUCCESS &&
		     (!r.ack_nak_valid || r.ack_nak <= 1);
		if (rc < 0)
			s->log("buffering response did not decode (%d)", rc);
		smgr_report_done(s, p, ok, r.ack_nak_valid ? r.ack_nak : 0, &r.resp,
				 r.reasons_valid ? r.n_reasons : 0, r.reasons);
		return;
	}
	case SNS_SMGR_REPORT: {
		struct sns_smgr_rep_resp r;
		bool ok;

		memset(&r, 0, sizeof(r));
		rc = qmi_decode_message(&r, &t, pkt, QMI_RESPONSE, msg, sns_smgr_rep_resp_ei);
		ok = rc >= 0 && r.resp.result == SNS_RESULT_SUCCESS && r.ack_nak <= 1;
		if (rc < 0)
			s->log("report response did not decode (%d)", rc);
		smgr_report_done(s, p, ok, r.ack_nak, &r.resp, r.n_reasons, r.reasons);
		return;
	}
	}
}

static int channel_by_report(struct server *s, uint8_t report_id, uint8_t sensor_id, uint8_t dt)
{
	int c = (int)report_id - (int)s->report_base - 1;

	if (c >= 0 && c < CH_COUNT && s->ch[c].smgr_id == sensor_id && s->ch[c].dt == dt)
		return c;
	for (c = 0; c < CH_COUNT; c++)
		if (s->ch[c].smgr_id == sensor_id && s->ch[c].dt == dt)
			return c;
	return -1;
}

static void smgr_handle_buf_ind(struct server *s, struct qrtr_packet *pkt)
{
	static struct sns_smgr_buf_ind ind;	/* 1.7 KB, keep it off the stack */
	uint32_t a_ticks, newest = 0, i, k;
	int64_t a_ns;
	unsigned int t;
	int rc;

	memset(&ind, 0, sizeof(ind));
	rc = qmi_decode_message(&ind, &t, pkt, QMI_INDICATION, SNS_SMGR_BUFFERING_IND,
				sns_smgr_buf_ind_ei);
	if (rc < 0) {
		s->log("buffering indication did not decode (%d)", rc);
		return;
	}
	/* Stamps per index: first_ts plus the running sum of offsets,
	 * including the first sample's own offset (as the stock HAL). */
	for (i = 0; i < ind.n_index && i < SNS_SMGR_BUF_ITEMS; i++) {
		uint32_t ts = ind.index[i].first_ts;

		for (k = 0; k < ind.index[i].count; k++) {
			uint32_t j = ind.index[i].first + k;

			if (j >= ind.n_samples)
				break;
			ts += ind.samples[j].ts_offset;
			if (!newest || (int32_t)(ts - newest) > 0)
				newest = ts;
		}
	}
	time_anchor(s, newest, &a_ticks, &a_ns);
	for (i = 0; i < ind.n_index && i < SNS_SMGR_BUF_ITEMS; i++) {
		const struct sns_smgr_buf_index *x = &ind.index[i];
		int c = channel_by_report(s, ind.report_id, x->sensor_id, x->data_type);
		uint32_t ts = x->first_ts;

		if (c < 0) {
			vlog("smgr <- samples for unknown sensor %u dt %u", x->sensor_id, x->data_type);
			continue;
		}
		for (k = 0; k < x->count; k++) {
			uint32_t j = x->first + k;

			if (j >= ind.n_samples)
				break;
			ts += ind.samples[j].ts_offset;
			server_publish(s, c, ticks_to_ns(ts, a_ticks, a_ns), ind.samples[j].data);
		}
	}
}

static void smgr_handle_rep_ind(struct server *s, struct qrtr_packet *pkt)
{
	struct sns_smgr_rep_ind ind;
	uint32_t a_ticks, newest = 0, i;
	int64_t a_ns;
	unsigned int t;
	int rc;

	memset(&ind, 0, sizeof(ind));
	rc = qmi_decode_message(&ind, &t, pkt, QMI_INDICATION, SNS_SMGR_REPORT_IND,
				sns_smgr_rep_ind_ei);
	if (rc < 0) {
		s->log("report indication did not decode (%d)", rc);
		return;
	}
	for (i = 0; i < ind.n_items && i < SNS_SMGR_REP_ITEMS; i++)
		if (!newest || (int32_t)(ind.item[i].ts - newest) > 0)
			newest = ind.item[i].ts;
	time_anchor(s, newest, &a_ticks, &a_ns);
	for (i = 0; i < ind.n_items && i < SNS_SMGR_REP_ITEMS; i++) {
		int c = channel_by_report(s, ind.report_id, ind.item[i].sensor_id,
					  ind.item[i].data_type);

		if (c >= 0)
			server_publish(s, c, ticks_to_ns(ind.item[i].ts, a_ticks, a_ns),
				       ind.item[i].data);
	}
}

static void smgr_rx(struct server *s, struct qrtr_packet *pkt, uint32_t node, uint32_t port)
{
	const uint8_t *h = pkt->data;
	unsigned int msg;
	uint16_t txn;

	if (pkt->data_len < SNS_QMI_HDR_LEN || qmi_decode_header(pkt, &msg) < 0) {
		s->log("malformed QMI message from %u:%u, dropped", node, port);
		return;
	}
	if (node != s->smgr_node || port != s->smgr_port) {
		vlog("smgr socket: message from unexpected %u:%u, ignored", node, port);
		return;
	}
	txn = (uint16_t)(h[1] | h[2] << 8);
	if (h[0] == QMI_RESPONSE) {
		vlog("smgr <- resp 0x%02x txn %u (%zu bytes)", msg, txn, pkt->data_len);
		smgr_handle_resp(s, pkt, msg, txn);
	} else if (h[0] == QMI_INDICATION && msg == SNS_SMGR_BUFFERING_IND) {
		smgr_handle_buf_ind(s, pkt);
	} else if (h[0] == QMI_INDICATION && msg == SNS_SMGR_REPORT_IND) {
		smgr_handle_rep_ind(s, pkt);
	} else {
		vlog("smgr <- type %u msg 0x%02x ignored", h[0], msg);
	}
}

/* Find a DSP service instance (SMGR 0x100 or QMAG_CAL 0x140, both
 * instance 0x3201). msmipc_lookup() always sends lookup_mask 0, and the
 * router matches a server only when (instance & mask) == requested
 * instance (kernel/net/ipc_router/ipc_router_core.c, the LOOKUP_SERVER
 * handler), so the only instance that works is 0 = "all instances of the
 * service"; pick the wanted one out of that list here. Logs the instance
 * set whenever it changes (other instances would be a surprise worth
 * seeing); seen[] holds the last logged set.
 * Returns 1 with *out filled, 0 if not registered, -1 on error. */
static int svc_lookup(struct server *s, int sock, uint32_t svc, uint32_t want, char *seen_prev,
		      size_t seen_sz, struct msm_ipc_server_info *out)
{
	struct msm_ipc_server_info info[8];
	char seen[160];
	size_t o = 0;
	int n, k, hit = -1;

	n = msmipc_lookup(sock, svc, 0, info, 8);
	if (n < 0)
		return -1;
	seen[0] = '\0';
	for (k = 0; k < n; k++) {
		if (info[k].instance == want && hit < 0)
			hit = k;
		if (o < sizeof(seen))
			o += (size_t)snprintf(seen + o, sizeof(seen) - o, "%s0x%x@%u:%u", k ? " " : "",
					      info[k].instance, info[k].node_id, info[k].port_id);
	}
	if (strcmp(seen, seen_prev)) {
		snprintf(seen_prev, seen_sz, "%s", seen);
		s->log("lookup 0x%x (all instances): %s", svc, n ? seen : "none");
	}
	if (hit < 0)
		return 0;
	*out = info[hit];
	return 1;
}

static int smgr_lookup(struct server *s, struct msm_ipc_server_info *out)
{
	return svc_lookup(s, s->smgr_sock, SNS_SMGR_SVC, (SNS_SMGR_INST << 8) | SNS_SMGR_VERS,
			  s->smgr_seen, sizeof(s->smgr_seen), out);
}

static void smgr_tick(struct server *s)
{
	int64_t now = s->now_ms();
	int i;

	if (s->st == SMGR_DOWN && now >= s->next_lookup) {
		struct msm_ipc_server_info info;
		int n;

		s->next_lookup = now + SENSORD_LOOKUP_MS;
		if (s->smgr_sock < 0) {
			s->smgr_sock = qrtr_open(0);
			if (s->smgr_sock < 0) {
				s->log("smgr socket: %s", strerror(errno));
				return;
			}
		}
		n = smgr_lookup(s, &info);
		if (n > 0) {
			smgr_found(s, info.node_id, info.port_id);
		} else if (!s->smgr_missing_logged) {
			s->smgr_missing_logged = true;
			s->log("smgr service 0x%x not registered yet, polling every %d ms",
			       SNS_SMGR_SVC, SENSORD_LOOKUP_MS);
		}
		return;
	}
	if (s->st != SMGR_DOWN && now >= s->next_check) {
		struct msm_ipc_server_info info;
		int n, same;

		s->next_check = now + SENSORD_CHECK_MS;
		n = smgr_lookup(s, &info);
		same = n > 0 && info.node_id == s->smgr_node && info.port_id == s->smgr_port;
		if (!same) {
			smgr_reset(s, n > 0 ? "service moved" : "service gone");
			return;
		}
	}
	for (i = 0; i < SENSORD_MAX_PENDING; i++) {
		struct pending *p = &s->pend[i];
		int rc;

		if (!p->used || now - p->sent < SENSORD_REQ_TIMEOUT_MS)
			continue;
		if (p->tries >= SENSORD_REQ_TRIES) {
			char why[48];

			snprintf(why, sizeof(why), "no response to msg 0x%02x", p->msg);
			smgr_reset(s, why);
			return;
		}
		p->tries++;
		p->sent = now;
		s->log("smgr msg 0x%02x txn %u: no response, resending (try %d)", p->msg, p->txn,
		       p->tries);
		rc = qrtr_sendto(s->smgr_sock, s->smgr_node, s->smgr_port, p->pkt.data,
				 (unsigned)p->pkt.data_len);
		if (rc < 0) {
			smgr_reset(s, strerror(-rc));
			return;
		}
	}
}

/* -------------------------------------------------- QMAG_CAL client */

/* The service's port is gone (send failed, socket reset, no answer):
 * look it up again next time magn wants it. */
static void qmag_forget(struct server *s, const char *why)
{
	struct qmag *q = &s->qmag;

	if (q->port)
		s->log("qmag %u:%u lost (%s)%s", q->node, q->port, why,
		       q->on ? "; its instance is gone with it" : "");
	if (q->sock >= 0)
		qrtr_close(q->sock);
	q->sock = -1;
	q->node = q->port = 0;
	q->on = q->busy = q->have_bias = false;
	q->seen[0] = '\0';
	q->missing_logged = false;
}

static int qmag_send(struct server *s, uint16_t msg, struct qmi_elem_info *ei, const void *req)
{
	struct qmag *q = &s->qmag;
	ssize_t len;
	int rc;

	q->pkt.data = q->buf;
	q->pkt.data_len = sizeof(q->buf);
	s->txn = s->txn == 0xffff ? 1 : s->txn + 1;
	len = qmi_encode_message(&q->pkt, QMI_REQUEST, msg, s->txn, req, ei);
	if (len < 0) {
		s->log("encode qmag msg 0x%02x failed: %s", msg, strerror((int)-len));
		return (int)len;
	}
	q->busy = true;
	q->txn = s->txn;
	q->msg = msg;
	q->sent = s->now_ms();
	q->tries = 1;
	vlog("qmag -> msg 0x%02x txn %u (%zd bytes)", msg, q->txn, len);
	rc = qrtr_sendto(q->sock, q->node, q->port, q->pkt.data, (unsigned)q->pkt.data_len);
	if (rc < 0) {
		qmag_forget(s, strerror(-rc));
		return rc;
	}
	return 0;
}

/* Rate-limited lookup of 0x140 instance 0x3201. 1 when found. */
static int qmag_lookup(struct server *s)
{
	struct qmag *q = &s->qmag;
	struct msm_ipc_server_info info;
	int64_t now = s->now_ms();
	int n;

	if (now < q->next_lookup)
		return 0;
	q->next_lookup = now + SENSORD_LOOKUP_MS;
	if (q->sock < 0) {
		q->sock = qrtr_open(0);
		if (q->sock < 0) {
			s->log("qmag socket: %s", strerror(errno));
			return -1;
		}
	}
	n = svc_lookup(s, q->sock, SNS_QMAG_SVC, (SNS_QMAG_INST << 8) | SNS_QMAG_VERS, q->seen,
		       sizeof(q->seen), &info);
	if (n > 0) {
		q->node = info.node_id;
		q->port = info.port_id;
		q->missing_logged = false;
		s->log("qmag service 0x%x found at %u:%u", SNS_QMAG_SVC, q->node, q->port);
		return 1;
	}
	if (!q->missing_logged) {
		q->missing_logged = true;
		s->log("qmag service 0x%x inst 0x%x not registered; polling every %d ms while magn is claimed",
		       SNS_QMAG_SVC, (SNS_QMAG_INST << 8) | SNS_QMAG_VERS, SENSORD_LOOKUP_MS);
	}
	return n;
}

static bool qmag_wanted(const struct server *s)
{
	return s->qmag.opt && !s->qmag.failed && s->st == SMGR_READY && channel_claims(s, CH_MAGN);
}

/* Bring QMAG_CAL in line with the magn claims: on while magn is claimed
 * and SMGR is up, off otherwise. One request at a time; called again
 * when it is answered. */
static void qmag_sync(struct server *s)
{
	struct qmag *q = &s->qmag;

	if (!q->opt)
		return;
	if (!channel_claims(s, CH_MAGN))
		q->failed = false;
	if (q->busy)
		return;
	if (q->on) {
		if (!qmag_wanted(s)) {
			struct sns_sam_disable_req r = { .instance = q->instance };
			const char *why = s->st != SMGR_READY ? "smgr not ready" : "no magn claims";

			if (qmag_send(s, SNS_SAM_DISABLE, sns_sam_disable_req_ei, &r) == 0)
				s->log("qmag disable instance %u (%s)", r.instance, why);
		}
		return;
	}
	if (!qmag_wanted(s))
		return;
	if (!q->port && qmag_lookup(s) <= 0)
		return;
	if (!q->attr_done) {
		qmag_send(s, SNS_SAM_GET_ATTR, sns_empty_ei, NULL);
		return;
	}
	{
		/* No TLV 0x10: its IDL entry (max_len 0, a u32 at offset 1 of
		 * a 2-byte struct) says no stock client can send it, and a DSP
		 * decoder built from that table could overrun its buffer. */
		struct sns_sam_enable_req r = { .period_valid = 0 };

		if (qmag_send(s, SNS_SAM_ENABLE, sns_sam_enable_req_ei, &r) == 0)
			s->log("qmag enable (no report period TLV)");
	}
}

static void qmag_report(struct server *s, const struct sns_sam_qmag_ind *ind)
{
	struct qmag *q = &s->qmag;
	void (*note)(const char *fmt, ...) = s->elog;
	int64_t now = s->now_ms();
	double v[3];

	if (!q->on || ind->instance != q->instance) {
		s->log("qmag report for instance %u ignored (%s %u)", ind->instance,
		       q->on ? "ours is" : "not enabled; last", q->instance);
		return;
	}
	q->inds++;
	/* Every report goes to stderr; the kernel log gets the first, any
	 * accuracy change, and at most one every 5 s otherwise. */
	if (q->inds == 1 || !q->have_bias || ind->accuracy != q->accuracy ||
	    now - q->last_note >= 5000) {
		note = s->log;
		q->last_note = now;
	}
	memcpy(q->bias, ind->bias, sizeof(q->bias));
	q->accuracy = ind->accuracy;
	q->ts = ind->timestamp;
	q->have_bias = true;
	convert_sample(CH_VEC3, ind->bias, v);
	note("qmag report %lu: instance %u ts %" PRIu32 " bias raw %" PRId32 " %" PRId32 " %" PRId32
	     " (SMGR frame, Q16 %.5f %.5f %.5f gauss) device x %.5f y %.5f z %.5f gauss, accuracy %" PRIu32,
	     q->inds, ind->instance, ind->timestamp, ind->bias[0], ind->bias[1], ind->bias[2],
	     ind->bias[0] / 65536.0, ind->bias[1] / 65536.0, ind->bias[2] / 65536.0, v[0], v[1], v[2],
	     ind->accuracy);
}

/* An ENABLE response we are not waiting for (after a give-up, or a
 * duplicate) that created an instance which is not ours: disable it at
 * once, fire and forget, so no DSP instance outlives its owner. */
static void qmag_stray_enable(struct server *s, struct qrtr_packet *pkt)
{
	struct qmag *q = &s->qmag;
	struct sns_sam_instance_resp r;
	struct sns_sam_disable_req d;
	DEFINE_QRTR_PACKET(out, 64);
	unsigned int t;
	int rc;

	memset(&r, 0, sizeof(r));
	rc = qmi_decode_message(&r, &t, pkt, QMI_RESPONSE, SNS_SAM_ENABLE, sns_sam_instance_resp_ei);
	if (rc < 0 || r.resp.result != SNS_RESULT_SUCCESS || !r.instance_valid)
		return;
	if (q->on && r.instance == q->instance)
		return;
	d.instance = r.instance;
	s->txn = s->txn == 0xffff ? 1 : s->txn + 1;
	rc = (int)qmi_encode_message(&out, QMI_REQUEST, SNS_SAM_DISABLE, s->txn, &d,
				     sns_sam_disable_req_ei);
	if (rc >= 0)
		rc = qrtr_sendto(q->sock, q->node, q->port, out.data, (unsigned)out.data_len);
	s->log("qmag late enable response created instance %u: disabling it%s", r.instance,
	       rc < 0 ? " failed" : "");
}

static void qmag_handle_resp(struct server *s, struct qrtr_packet *pkt, unsigned msg, uint16_t txn)
{
	struct qmag *q = &s->qmag;
	unsigned int t;
	int rc;

	if (!q->busy || txn != q->txn || msg != q->msg) {
		vlog("qmag <- stray response msg 0x%02x txn %u", msg, txn);
		if (msg == SNS_SAM_ENABLE)
			qmag_stray_enable(s, pkt);
		return;
	}
	q->busy = false;
	switch (msg) {
	case SNS_SAM_GET_ATTR: {
		struct sns_sam_attr_resp r;

		memset(&r, 0, sizeof(r));
		q->attr_done = true;
		rc = qmi_decode_message(&r, &t, pkt, QMI_RESPONSE, msg, sns_sam_attr_resp_ei);
		if (rc < 0 || r.resp.result != SNS_RESULT_SUCCESS) {
			s->log("qmag attributes: failed (decode %d, result %u err %u)", rc, r.resp.result,
			       r.resp.err);
			break;
		}
		s->log("qmag attributes (TLV 0x03..0x0b): %" PRIu32 " %" PRIu32 " 0x%" PRIx32 " %" PRIu32
		       " %" PRIu32 " %" PRIu32 " %" PRIu32 " %" PRIu32 " %" PRIu32 "; suid %s0x%016" PRIx64
		       "; reserved %s%" PRIu32, r.attr[0], r.attr[1], r.attr[2], r.attr[3], r.attr[4],
		       r.attr[5], r.attr[6], r.attr[7], r.attr[8], r.suid_valid ? "" : "(absent) ", r.suid,
		       r.reserved_valid ? "" : "(absent) ", r.reserved);
		s->log("qmag attributes read as: revision %" PRIu32 ", processor %" PRIu32
		       ", report modes 0x%" PRIx32 ", report rate %.4g..%.4g Hz, sample rate %.4g..%.4g Hz"
		       ", max batch %" PRIu32 ", power %.4g mA (rates/power as Q16)",
		       r.attr[0], r.attr[1], r.attr[2], r.attr[3] / 65536.0, r.attr[4] / 65536.0,
		       r.attr[5] / 65536.0, r.attr[6] / 65536.0, r.attr[7], r.attr[8] / 65536.0);
		break;
	}
	case SNS_SAM_ENABLE: {
		struct sns_sam_instance_resp r;

		memset(&r, 0, sizeof(r));
		rc = qmi_decode_message(&r, &t, pkt, QMI_RESPONSE, msg, sns_sam_instance_resp_ei);
		if (rc >= 0 && r.resp.result == SNS_RESULT_SUCCESS && r.instance_valid) {
			q->on = true;
			q->instance = r.instance;
			q->enables++;
			q->have_bias = false;
			s->log("qmag enabled: instance %u", r.instance);
		} else {
			q->failed = true;
			s->log("qmag enable refused (decode %d, result %u err %u, instance %s); not retried until magn is released",
			       rc, r.resp.result, r.resp.err, r.instance_valid ? "given" : "missing");
		}
		break;
	}
	case SNS_SAM_DISABLE: {
		struct sns_sam_instance_resp r;

		memset(&r, 0, sizeof(r));
		rc = qmi_decode_message(&r, &t, pkt, QMI_RESPONSE, msg, sns_sam_instance_resp_ei);
		if (rc < 0)
			s->log("qmag disabled: instance %u (response did not decode: %d)", q->instance, rc);
		else
			s->log("qmag disabled: instance %u (result %u err %u)", q->instance,
			       r.resp.result, r.resp.err);
		q->on = false;
		q->have_bias = false;
		break;
	}
	}
	qmag_sync(s);
}

/* libqrtr does not enforce mandatory TLVs: 1 when TLVs first..last are
 * all present in the message (walked with bounds checks). */
static bool tlvs_present(const struct qrtr_packet *pkt, unsigned first, unsigned last)
{
	const uint8_t *p = (const uint8_t *)pkt->data + SNS_QMI_HDR_LEN;
	size_t left = pkt->data_len - SNS_QMI_HDR_LEN;
	uint32_t seen = 0, want = 0;
	unsigned k;

	for (k = first; k <= last && k < 32; k++)
		want |= 1u << k;
	while (left >= 3) {
		size_t len = (size_t)(p[1] | p[2] << 8);

		if (len > left - 3)
			return false;
		if (p[0] < 32)
			seen |= 1u << p[0];
		p += 3 + len;
		left -= 3 + len;
	}
	return (seen & want) == want;
}

static void qmag_rx(struct server *s, struct qrtr_packet *pkt, uint32_t node, uint32_t port)
{
	struct qmag *q = &s->qmag;
	const uint8_t *h = pkt->data;
	unsigned int msg, t;
	uint16_t txn;
	int rc;

	if (pkt->data_len < SNS_QMI_HDR_LEN || qmi_decode_header(pkt, &msg) < 0) {
		s->log("qmag: malformed QMI message from %u:%u, dropped", node, port);
		return;
	}
	if (!q->port || node != q->node || port != q->port) {
		vlog("qmag socket: message from unexpected %u:%u, ignored", node, port);
		return;
	}
	txn = (uint16_t)(h[1] | h[2] << 8);
	if (h[0] == QMI_RESPONSE) {
		vlog("qmag <- resp 0x%02x txn %u (%zu bytes)", msg, txn, pkt->data_len);
		qmag_handle_resp(s, pkt, msg, txn);
	} else if (h[0] == QMI_INDICATION && msg == SNS_SAM_REPORT_IND) {
		struct sns_sam_qmag_ind ind;

		memset(&ind, 0, sizeof(ind));
		rc = qmi_decode_message(&ind, &t, pkt, QMI_INDICATION, (int)msg, sns_sam_qmag_ind_ei);
		if (rc < 0)
			s->log("qmag report indication did not decode (%d)", rc);
		else if (!tlvs_present(pkt, 0x01, 0x04))
			s->log("qmag report indication without all of TLVs 0x01..0x04, ignored");
		else
			qmag_report(s, &ind);
	} else if (h[0] == QMI_INDICATION && msg == SNS_SAM_ERROR_IND) {
		struct sns_sam_error_ind ind;

		memset(&ind, 0, sizeof(ind));
		rc = qmi_decode_message(&ind, &t, pkt, QMI_INDICATION, (int)msg, sns_sam_error_ind_ei);
		if (rc < 0 || !tlvs_present(pkt, 0x01, 0x02)) {
			s->log("qmag error indication malformed (decode %d), ignored", rc);
		} else if (q->on && ind.instance == q->instance) {
			/* Counted and shown in status; what an error means for
			 * the instance is unknown, so it stays enabled. */
			q->errors++;
			q->last_error = ind.error;
			q->had_error = true;
			s->log("qmag error indication: error %u for our instance %u", ind.error,
			       ind.instance);
		} else {
			s->log("qmag error indication: error %u instance %u (not ours: %s%u)", ind.error,
			       ind.instance, q->on ? "" : "none enabled, last ", q->instance);
		}
	} else {
		vlog("qmag <- type %u msg 0x%02x ignored", h[0], msg);
	}
}

static int64_t qmag_timeout_at(const struct qmag *q)
{
	return q->sent + (q->msg == SNS_SAM_ENABLE ? SENSORD_QMAG_ENABLE_TIMEOUT_MS
						   : SENSORD_REQ_TIMEOUT_MS);
}

/* Resends and give-ups for the request in flight (ENABLE: never
 * resent; forgetting the port closes our socket, which tells the DSP
 * this client is gone); lookup retries. */
static void qmag_tick(struct server *s)
{
	struct qmag *q = &s->qmag;
	int64_t now = s->now_ms();

	if (!q->opt)
		return;
	if (q->busy && now >= qmag_timeout_at(q)) {
		if (q->tries >= SENSORD_REQ_TRIES || q->msg == SNS_SAM_ENABLE) {
			uint16_t msg = q->msg;

			s->log("qmag msg 0x%02x txn %u: no response after %d tries, giving up", msg,
			       q->txn, q->tries);
			q->busy = false;
			if (msg == SNS_SAM_GET_ATTR) {
				q->attr_done = true;
			} else {
				if (msg == SNS_SAM_ENABLE)
					q->failed = true;
				qmag_forget(s, "no response");
			}
		} else {
			int rc;

			q->tries++;
			q->sent = now;
			s->log("qmag msg 0x%02x txn %u: no response, resending (try %d)", q->msg, q->txn,
			       q->tries);
			rc = qrtr_sendto(q->sock, q->node, q->port, q->pkt.data,
					 (unsigned)q->pkt.data_len);
			if (rc < 0)
				qmag_forget(s, strerror(-rc));
		}
	}
	qmag_sync(s);
}

/* When qmag_tick has something to do next, for the poll() timeout. */
static int64_t qmag_deadline(const struct server *s, int64_t next)
{
	const struct qmag *q = &s->qmag;

	if (q->busy && qmag_timeout_at(q) < next)
		next = qmag_timeout_at(q);
	else if (!q->busy && !q->on && !q->port && qmag_wanted(s) && q->next_lookup < next)
		next = q->next_lookup;
	return next;
}

/* ------------------------------------------------ REG2/TIME2 servers */

static void hexdump_log(struct server *s, const char *what, const void *data, size_t len)
{
	const uint8_t *p = data;
	char buf[3 * 48 + 1];
	size_t i, o = 0;

	for (i = 0; i < len && i < 48; i++)
		o += (size_t)snprintf(buf + o, sizeof(buf) - o, "%02x ", p[i]);
	buf[o ? o - 1 : 0] = '\0';
	s->log("%s (%zu bytes): %s%s", what, len, buf, len > 48 ? " ..." : "");
}

static void send_resp(struct server *s, int sock, uint32_t node, uint32_t port, unsigned msg,
		      uint16_t txn, const void *resp, struct qmi_elem_info *ei)
{
	DEFINE_QRTR_PACKET(out, SENSORD_QMI_BUF);
	ssize_t len;
	int rc;

	len = qmi_encode_message(&out, QMI_RESPONSE, (int)msg, txn, resp, ei);
	if (len < 0) {
		s->log("encode response 0x%02x failed: %s", msg, strerror((int)-len));
		return;
	}
	rc = qrtr_sendto(sock, node, port, out.data, (unsigned)out.data_len);
	if (rc < 0)
		s->log("send response 0x%02x to %u:%u failed: %s", msg, node, port, strerror(-rc));
}

static void send_generic(struct server *s, int sock, uint32_t node, uint32_t port, unsigned msg,
			 uint16_t txn, uint8_t result, uint8_t err)
{
	struct sns_generic_resp r = { { result, err } };

	send_resp(s, sock, node, port, msg, txn, &r, sns_generic_resp_ei);
}

static void send_version(struct server *s, int sock, uint32_t node, uint32_t port, uint16_t txn,
			 uint32_t minor, uint16_t max_msg)
{
	struct sns_version_resp r = { { SNS_RESULT_SUCCESS, SNS_ERR_NONE }, minor, max_msg };

	send_resp(s, sock, node, port, SNS_MSG_VERSION, txn, &r, sns_version_resp_ei);
}

/* QMAG_CAL's registry: group 2970 = items 3800..3840 (stock map). */
static bool reg2_is_qmag(bool group, uint16_t id)
{
	return group ? id == SNS_REG2_QMAG_GROUP : id >= 3800 && id <= 3840;
}

#define SENSORD_REG_SAVE_QUIET_MS 3000
#define SENSORD_REG_SAVE_MAX_MS	30000	/* ... but never later than this */

/* Write the RAM registry back over the /run copy. A failure is logged
 * and dropped (the RAM copy still serves the DSP); the next DSP write
 * tries again. */
static void reg_writeback(struct server *s, const char *why)
{
	int rc;

	if (!s->reg_path || !s->reg_dirty)
		return;
	s->reg_dirty = false;
	rc = sns_reg_save(&s->reg, s->reg_path);
	if (rc < 0) {
		s->reg_save_errors++;
		s->log("registry write-back to %s failed (%s)%s; the DSP's writes stay in RAM only",
		       s->reg_path, strerror(-rc), rc == -EXDEV ? ": not a RAM filesystem" : "");
		return;
	}
	s->reg_saves++;
	s->log("registry written back to %s (%s; %lu DSP writes so far; persist untouched)",
	       s->reg_path, why, s->reg.writes);
}

/* When the pending write-back is due: 3 s after the last DSP write,
 * capped at 30 s after the first unsaved one (steady writes cannot
 * postpone it for ever). */
static int64_t reg_save_due(const struct server *s)
{
	int64_t quiet = s->reg_dirty_at + SENSORD_REG_SAVE_QUIET_MS;
	int64_t cap = s->reg_dirty_since + SENSORD_REG_SAVE_MAX_MS;

	return quiet < cap ? quiet : cap;
}

static void reg_tick(struct server *s)
{
	if (s->reg_dirty && s->now_ms() >= reg_save_due(s))
		reg_writeback(s, "after DSP writes");
}

static void reg2_rx(struct server *s, struct qrtr_packet *pkt, uint32_t node, uint32_t port)
{
	const uint8_t *h = pkt->data;
	unsigned int msg, t;
	uint16_t txn;
	int rc;

	if (pkt->data_len < SNS_QMI_HDR_LEN || qmi_decode_header(pkt, &msg) < 0) {
		s->log("reg2: malformed QMI message from %u:%u, dropped", node, port);
		return;
	}
	if (h[0] != QMI_REQUEST)
		return;
	txn = (uint16_t)(h[1] | h[2] << 8);

	switch (msg) {
	case SNS_MSG_CANCEL:
		vlog("reg2: cancel from %u:%u", node, port);
		send_generic(s, s->reg_sock, node, port, msg, txn, SNS_RESULT_SUCCESS, SNS_ERR_NONE);
		return;
	case SNS_MSG_VERSION:
		vlog("reg2: version query from %u:%u", node, port);
		send_version(s, s->reg_sock, node, port, txn, SNS_REG2_IDL_MINOR, SNS_REG2_MAX_MSG_ID);
		return;
	case SNS_REG2_ITEM_READ:
	case SNS_REG2_GROUP_READ: {
		bool group = msg == SNS_REG2_GROUP_READ;
		struct sns_reg2_id_req req = { 0 };
		static struct sns_reg2_group_read_resp gr;
		struct sns_reg2_item_read_resp ir;
		int n;

		rc = qmi_decode_message(&req, &t, pkt, QMI_REQUEST, (int)msg, sns_reg2_id_req_ei);
		memset(&gr, 0, sizeof(gr));
		memset(&ir, 0, sizeof(ir));
		n = rc < 0 ? -1 : group ? sns_reg_read(&s->reg, 1, req.id, gr.data, sizeof(gr.data))
					: sns_reg_read(&s->reg, 0, req.id, ir.data, sizeof(ir.data));
		if (n < 0)
			s->log("reg2: %s read %u from %u:%u: %s", group ? "group" : "item", req.id,
			       node, port, rc < 0 ? "malformed" : "no such id");
		else if (reg2_is_qmag(group, req.id))
			/* Not read at boot live: shows QMAG_CAL starting up. */
			s->log("reg2: %s read %u (QMAG_CAL) from %u:%u -> %d bytes",
			       group ? "group" : "item", req.id, node, port, n);
		else
			vlog("reg2: %s read %u from %u:%u -> %d bytes", group ? "group" : "item",
			     req.id, node, port, n);
		if (group) {
			gr.resp.result = n < 0 ? SNS_RESULT_FAILURE : SNS_RESULT_SUCCESS;
			gr.resp.err = n < 0 ? SNS_ERR_BAD_PARAM : SNS_ERR_NONE;
			gr.id = req.id;
			gr.data_len = n < 0 ? 0 : (uint32_t)n;
			send_resp(s, s->reg_sock, node, port, msg, txn, &gr, sns_reg2_group_read_resp_ei);
		} else {
			ir.resp.result = n < 0 ? SNS_RESULT_FAILURE : SNS_RESULT_SUCCESS;
			ir.resp.err = n < 0 ? SNS_ERR_BAD_PARAM : SNS_ERR_NONE;
			ir.id = req.id;
			ir.data_len = n < 0 ? 0 : (uint32_t)n;
			send_resp(s, s->reg_sock, node, port, msg, txn, &ir, sns_reg2_item_read_resp_ei);
		}
		return;
	}
	case SNS_REG2_ITEM_WRITE:
	case SNS_REG2_GROUP_WRITE: {
		bool group = msg == SNS_REG2_GROUP_WRITE;
		static struct sns_reg2_group_write_req gw;
		struct sns_reg2_item_write_req iw;
		const uint8_t *data;
		uint32_t len;
		uint16_t id;
		char what[96];

		memset(&gw, 0, sizeof(gw));
		memset(&iw, 0, sizeof(iw));
		rc = group ? qmi_decode_message(&gw, &t, pkt, QMI_REQUEST, (int)msg,
						sns_reg2_group_write_req_ei)
			   : qmi_decode_message(&iw, &t, pkt, QMI_REQUEST, (int)msg,
						sns_reg2_item_write_req_ei);
		id = group ? gw.id : iw.id;
		data = group ? gw.data : iw.data;
		len = group ? gw.data_len : iw.data_len;
		/* The decoder stores the wire count before it rejects it: never
		 * log (or write) more than the array actually holds. */
		if (rc < 0)
			len = 0;
		if (len > (group ? sizeof(gw.data) : sizeof(iw.data)))
			len = 0;
		if (rc >= 0)
			rc = sns_reg_write(&s->reg, group, id, data, len);
		if (rc >= 0) {
			if (!s->reg_dirty)
				s->reg_dirty_since = s->now_ms();
			s->reg_dirty = true;
			s->reg_dirty_at = s->now_ms();	/* debounce: wait for quiet */
		}
		snprintf(what, sizeof(what), "reg2: %s write %u%s from %u:%u (RAM, /run copy)%s",
			 group ? "group" : "item", id, reg2_is_qmag(group, id) ? " (QMAG_CAL)" : "",
			 node, port, rc == -ENOENT ? " no such id" : rc < 0 ? " rejected" : "");
		hexdump_log(s, what, data, len);
		send_generic(s, s->reg_sock, node, port, msg, txn,
			     rc < 0 ? SNS_RESULT_FAILURE : SNS_RESULT_SUCCESS,
			     rc < 0 ? SNS_ERR_BAD_PARAM : SNS_ERR_NONE);
		return;
	}
	default:
		s->unknown_reqs++;
		hexdump_log(s, "reg2: unsupported request, nacked", h, pkt->data_len);
		send_generic(s, s->reg_sock, node, port, msg, txn, SNS_RESULT_FAILURE,
			     SNS_ERR_BAD_MSG_ID);
		return;
	}
}

static void time2_rx(struct server *s, struct qrtr_packet *pkt, uint32_t node, uint32_t port)
{
	const uint8_t *h = pkt->data;
	unsigned int msg, t;
	uint16_t txn;

	if (pkt->data_len < SNS_QMI_HDR_LEN || qmi_decode_header(pkt, &msg) < 0 ||
	    h[0] != QMI_REQUEST)
		return;
	txn = (uint16_t)(h[1] | h[2] << 8);
	s->time2_reqs++;
	if (msg == SNS_MSG_VERSION) {
		s->log("time2: version query from %u:%u", node, port);
		send_version(s, s->time_sock, node, port, txn, SNS_TIME2_IDL_MINOR, SNS_TIME2_MAX_MSG_ID);
	} else if (msg == SNS_TIME2_TIMESTAMP) {
		struct sns_time2_req req = { 0 };
		struct sns_time2_resp r;
		struct timespec ts;
		uint32_t ticks;

		qmi_decode_message(&req, &t, pkt, QMI_REQUEST, (int)msg, sns_time2_req_ei);
		memset(&r, 0, sizeof(r));
		if (read_dsp_ticks(s, &ticks)) {
			r.dsps_ticks_valid = 1;
			r.dsps_ticks = ticks;
		}
		clock_gettime(CLOCK_BOOTTIME, &ts);
		r.apps_ns_valid = r.apps_boot_ns_valid = 1;
		r.apps_ns = r.apps_boot_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
		s->log("time2: timestamp request from %u:%u (reg_report %s)", node, port,
		       req.reg_report_valid ? (req.reg_report ? "1" : "0") : "unset");
		send_resp(s, s->time_sock, node, port, msg, txn, &r, sns_time2_resp_ei);
	} else {
		s->unknown_reqs++;
		hexdump_log(s, "time2: unsupported request, nacked", h, pkt->data_len);
		send_generic(s, s->time_sock, node, port, msg, txn, SNS_RESULT_FAILURE,
			     SNS_ERR_BAD_MSG_ID);
	}
}

/* Receive one datagram. Returns 1 with *pkt filled, 0 for nothing, or
 * -ENETRESET when the router reset the socket (DSP restart). */
static int ipc_recv(int sock, char *buf, size_t bsz, struct qrtr_packet *pkt, uint32_t *node,
		    uint32_t *port)
{
	struct sockaddr_qrtr sq;
	int ret;

	ret = qrtr_recvfrom(sock, buf, (unsigned)bsz, node, port);
	if (ret == QRTR_RECV_RESUME_TX || ret == -EAGAIN || ret == -EINTR)
		return 0;
	if (ret == -ENETRESET)
		return ret;
	if (ret < 0)
		return 0;
	sq.sq_family = AF_QIPCRTR;
	sq.sq_node = *node;
	sq.sq_port = *port;
	qrtr_decode(pkt, buf, (size_t)ret, &sq);
	return 1;
}

static int publish(struct server *s, int *sock, uint32_t svc, uint16_t vers, uint16_t inst)
{
	if (*sock >= 0)
		qrtr_close(*sock);
	*sock = qrtr_open(0);
	if (*sock < 0) {
		s->log("socket for service 0x%x: %s", svc, strerror(errno));
		return -1;
	}
	if (qrtr_publish(*sock, svc, vers, inst) < 0) {
		s->log("publish service 0x%x inst 0x%x: %s", svc, (inst << 8) | vers, strerror(errno));
		qrtr_close(*sock);
		*sock = -1;
		return -1;
	}
	s->log("serving service 0x%x inst 0x%04x", svc, (inst << 8) | vers);
	return 0;
}

/* Expire gets nobody answered in time. */
static void server_tick_clients(struct server *s)
{
	int64_t now = s->now_ms();
	int i, c;

	for (i = 0; i < SENSORD_MAX_CLIENTS; i++) {
		struct client *cl = &s->clients[i];

		if (cl->fd < 0)
			continue;
		for (c = 0; c < CH_COUNT; c++) {
			if (!cl->get[c] || now < cl->get_deadline[c])
				continue;
			cl->get[c] = false;
			if (cl->get_only[c]) {
				cl->rate[c] = 0;
				cl->get_only[c] = false;
			}
			client_reply(s, cl, "{\"err\":\"timeout\",\"sensor\":\"%s\"}\n", s->ch[c].name);
			if (cl->fd < 0)
				break;
			smgr_sync(s);
		}
	}
}

/* poll() timeout: until the next lookup/liveness check, request
 * timeout or get deadline, so an idle daemon sleeps for seconds. */
static int server_deadline_ms(struct server *s)
{
	int64_t now = s->now_ms();
	int64_t next = s->st == SMGR_DOWN ? s->next_lookup : s->next_check;
	int i, c;

	next = qmag_deadline(s, next);
	if (s->reg_dirty && s->reg_path && reg_save_due(s) < next)
		next = reg_save_due(s);
	for (i = 0; i < SENSORD_MAX_PENDING; i++)
		if (s->pend[i].used && s->pend[i].sent + SENSORD_REQ_TIMEOUT_MS < next)
			next = s->pend[i].sent + SENSORD_REQ_TIMEOUT_MS;
	for (i = 0; i < SENSORD_MAX_CLIENTS; i++)
		for (c = 0; s->clients[i].fd >= 0 && c < CH_COUNT; c++)
			if (s->clients[i].get[c] && s->clients[i].get_deadline[c] < next)
				next = s->clients[i].get_deadline[c];
	if (next <= now)
		return 0;
	return next - now > 60000 ? 60000 : (int)(next - now);
}

#ifndef SENSORD_NO_MAIN

static int sock_listen(const char *path)
{
	struct sockaddr_un sa;
	int fd;

	if (strlen(path) >= sizeof(sa.sun_path))
		return -ENAMETOOLONG;
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -errno;
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	strcpy(sa.sun_path, path);
	unlink(path);
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(fd, 8) < 0) {
		int err = errno;

		close(fd);
		return -err;
	}
	return fd;
}

static int sock_connect(const char *path)
{
	struct sockaddr_un sa;
	int fd;

	if (strlen(path) >= sizeof(sa.sun_path))
		return -ENAMETOOLONG;
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -errno;
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	strcpy(sa.sun_path, path);
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		int err = errno;

		close(fd);
		return -err;
	}
	return fd;
}

/* Self-pipe: the signal handler writes it so a TERM ends poll() at once. */
static int g_sigpipe[2] = { -1, -1 };

static void on_signal(int sig)
{
	int saved = errno;

	(void)sig;
	g_stop = 1;
	if (g_sigpipe[1] >= 0 && write(g_sigpipe[1], "", 1) < 0) {
		/* full pipe: a wakeup is already pending */
	}
	errno = saved;
}

static void usage(void)
{
	fprintf(stderr,
		"usage: sensord [-r REG] [-m MAP] [-S SOCK] [-T dsps|cntvct|rx] [-b HZ] [-s CH=ID:DT]...\n"
		"               [-c CH=full|factory|raw]... [-i BASE] [-Q] [-P] [-t] [-R] [-F] [-v]\n"
		"       sensord [-S SOCK] [-n COUNT] list | status | get SENSOR [HZ] | watch SENSOR [HZ]\n");
}

/* Client mode: one request, print the reply lines (the hello is not
 * printed). list/status/get stop after the first reply; watch streams
 * sample lines until ^C, or COUNT of them with -n. */
static int client_main(const char *sock_path, long count, int argc, char **argv)
{
	const char *cmd = argv[0];
	bool watch = strcmp(cmd, "watch") == 0;
	char req[128], buf[4096];
	size_t have = 0;
	long samples = 0;
	int fd, rc = 0;

	if (watch || strcmp(cmd, "get") == 0) {
		if (argc < 2 || argc > 3) {
			usage();
			return 64;
		}
		snprintf(req, sizeof(req), "%s %s%s%s\n", watch ? "claim" : "get", argv[1],
			 argc == 3 ? " " : "", argc == 3 ? argv[2] : "");
	} else if ((strcmp(cmd, "list") == 0 || strcmp(cmd, "status") == 0) && argc == 1) {
		snprintf(req, sizeof(req), "%s\n", cmd);
	} else {
		usage();
		return 64;
	}
	fd = sock_connect(sock_path);
	if (fd < 0) {
		fprintf(stderr, "sensord: connect %s: %s\n", sock_path, strerror(-fd));
		return 1;
	}
	if (write(fd, req, strlen(req)) < 0) {
		close(fd);
		return 1;
	}
	for (;;) {
		ssize_t n;
		char *line, *nl;

		if (have >= sizeof(buf) - 1)
			have = 0;	/* no line is this long; resync */
		n = read(fd, buf + have, sizeof(buf) - 1 - have);

		if (n <= 0) {
			if (!watch)
				rc = 1;
			break;
		}
		have += (size_t)n;
		buf[have] = '\0';
		line = buf;
		while ((nl = strchr(line, '\n'))) {
			*nl = '\0';
			if (strncmp(line, "{\"hello\"", 8) != 0 &&
			    !(watch && strncmp(line, "{\"ok\"", 5) == 0)) {
				puts(line);
				fflush(stdout);
				if (strncmp(line, "{\"err\"", 6) == 0)
					rc = 1;
				if (!watch || rc)
					goto out;
				if (strncmp(line, "{\"sensor\"", 9) == 0 && count > 0 &&
				    ++samples >= count)
					goto out;
			}
			line = nl + 1;
		}
		have = strlen(line);
		memmove(buf, line, have);
	}
out:
	close(fd);
	return rc;
}

static int parse_override(struct server *s, const char *arg)
{
	char name[32];
	unsigned id, dt;
	int c;

	if (sscanf(arg, "%31[^=]=%u:%u", name, &id, &dt) != 3 || id > 255 || dt > 255)
		return -1;
	c = channel_from_name(name);
	if (c < 0)
		return -1;
	s->ch[c].smgr_id = (uint8_t)id;
	s->ch[c].dt = (uint8_t)dt;
	return 0;
}

static int parse_cal(struct server *s, const char *arg)
{
	char name[32], cal[16];
	int c, k;

	if (sscanf(arg, "%31[^=]=%15s", name, cal) != 2)
		return -1;
	c = channel_from_name(name);
	if (c < 0)
		return -1;
	for (k = 0; k <= SNS_SMGR_CAL_RAW; k++)
		if (strcmp(cal, cal_names[k]) == 0 || (cal[0] == '0' + k && !cal[1])) {
			s->ch[c].cal = (uint8_t)k;
			return 0;
		}
	return -1;
}

static int parse_u32(const char *arg, uint32_t *out)
{
	char *end;
	unsigned long long v;

	errno = 0;
	v = strtoull(arg, &end, 0);
	if (errno || end == arg || *end || v > UINT32_MAX)
		return -1;
	*out = (uint32_t)v;
	return 0;
}

int main(int argc, char **argv)
{
	static struct server s;
	const char *sock_path = SENSORD_SOCK_PATH, *reg_path = SENSORD_REG_PATH;
	const char *map_path = SENSORD_MAP_PATH, *ts = "dsps";
	long count = 0;
	int lfd, opt, i, line = 0, rc;
	size_t got = 0;

	server_init(&s);
	while ((opt = getopt(argc, argv, "r:m:S:T:b:s:c:i:Qn:PtRFv")) != -1) {
		switch (opt) {
		case 'r': reg_path = optarg; break;
		case 'm': map_path = optarg; break;
		case 'S': sock_path = optarg; break;
		case 'T': ts = optarg; break;
		case 'b': s.report_cap = (unsigned)atoi(optarg); break;
		case 's':
			if (parse_override(&s, optarg)) {
				usage();
				return 64;
			}
			break;
		case 'c':
			if (parse_cal(&s, optarg)) {
				usage();
				return 64;
			}
			break;
		case 'i': {
			uint32_t v;

			if (parse_u32(optarg, &v) || v > 255 - CH_COUNT) {
				usage();
				return 64;
			}
			s.report_base = v;
			break;
		}
		case 'Q': s.qmag.opt = true; break;
		case 'n': count = atol(optarg); break;
		case 'P': s.periodic = true; break;
		case 't': s.serve_time2 = true; break;
		case 'R': s.serve_reg2 = false; break;
		case 'F': s.force = true; break;
		case 'v': g_verbose = true; break;
		default: usage(); return 64;
		}
	}
	if (optind < argc)
		return client_main(sock_path, count, argc - optind, argv + optind);

	/* One daemon per socket: a second one would steal the socket and
	 * publish REG2 twice. A socket nobody answers on is stale and is
	 * replaced by sock_listen(). */
	{
		int fd = sock_connect(sock_path);

		if (fd >= 0) {
			close(fd);
			kmsg_note("another sensord is serving %s; refusing to start", sock_path);
			return 1;
		}
	}

	if (pipe2(g_sigpipe, O_CLOEXEC | O_NONBLOCK) < 0) {
		kmsg_note("pipe: %s", strerror(errno));
		return 1;
	}
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGHUP, on_signal);	/* a closed telnet session: exit cleanly too */
	signal(SIGPIPE, SIG_IGN);

	if (strcmp(ts, "dsps") == 0) {
		s.ts_mode = TS_DSPS;
		s.dsps_fd = open(SENSORD_DSPS_DEV, O_RDONLY | O_CLOEXEC);
		if (s.dsps_fd < 0) {
			kmsg_note("%s: %s; timestamps fall back to receive time (-T rx)",
				  SENSORD_DSPS_DEV, strerror(errno));
			s.ts_mode = TS_RX;
		}
	} else if (strcmp(ts, "cntvct") == 0) {
#if defined(__aarch64__)
		s.ts_mode = TS_CNTVCT;
#else
		kmsg_note("-T cntvct needs aarch64; using rx");
		s.ts_mode = TS_RX;
#endif
	} else if (strcmp(ts, "rx") == 0) {
		s.ts_mode = TS_RX;
	} else {
		usage();
		return 64;
	}

	if (s.serve_reg2) {
		rc = sns_reg_load_map(&s.reg, map_path, &line);
		if (rc) {
			kmsg_note("registry map %s: %s%s; exiting", map_path, strerror(-rc),
				  rc == -EINVAL ? " (bad line)" : "");
			return 1;
		}
		rc = sns_reg_load_data(&s.reg, reg_path, 0, &got);
		if (rc) {
			kmsg_note("registry %s: %s (%zu bytes, map wants %zu); exiting", reg_path,
				  strerror(-rc), got, s.reg.size);
			return 1;
		}
		kmsg_note("registry %s loaded: %zu bytes, %zu groups, %zu items (RAM copy; DSP writes are written back to this file, never to persist)",
			  reg_path, s.reg.size, s.reg.ngroups, s.reg.nitems);
		s.reg_path = reg_path;
		if (publish(&s, &s.reg_sock, SNS_REG2_SVC, SNS_REG2_VERS, SNS_REG2_INST))
			return 1;
	} else {
		kmsg_note("not serving REG2 (-R)");
	}
	if (s.serve_time2 &&
	    publish(&s, &s.time_sock, SNS_TIME2_SVC, SNS_TIME2_VERS, SNS_TIME2_APPS_INST))
		return 1;

	lfd = sock_listen(sock_path);
	if (lfd < 0) {
		kmsg_note("listen %s: %s; exiting", sock_path, strerror(-lfd));
		return 1;
	}
	kmsg_note("ready: %s, %s API, timestamps %s%s%s%s", sock_path,
		  s.periodic ? "periodic REPORT" : "BUFFERING",
		  s.ts_mode == TS_DSPS ? "dsps" : s.ts_mode == TS_CNTVCT ? "cntvct" : "rx",
		  s.report_cap ? ", report rate capped" : "", s.force ? ", -F" : "",
		  s.qmag.opt ? ", QMAG_CAL client (-Q)" : "");
	if (s.report_base)
		kmsg_note("SMGR report IDs %u..%u (-i)", s.report_base + 1, s.report_base + CH_COUNT);
	for (i = 0; i < CH_COUNT; i++)
		if (s.ch[i].cal != SNS_SMGR_CAL_FULL)
			kmsg_note("%s: calibration %s (-c)%s", s.ch[i].name, cal_name(s.ch[i].cal),
				  s.periodic ? " has no effect with -P: REPORT has no calibration field" : "");

	while (!g_stop) {
		struct pollfd pfd[6 + SENSORD_MAX_CLIENTS];
		struct client *cmap[SENSORD_MAX_CLIENTS];
		int n = 0, nclients = 0, ireg = -1, itime = -1, ismgr = -1, iqmag = -1, ilisten;

		pfd[n++] = (struct pollfd){ .fd = g_sigpipe[0], .events = POLLIN };
		if (s.reg_sock >= 0) {
			ireg = n;
			pfd[n++] = (struct pollfd){ .fd = s.reg_sock, .events = POLLIN };
		}
		if (s.time_sock >= 0) {
			itime = n;
			pfd[n++] = (struct pollfd){ .fd = s.time_sock, .events = POLLIN };
		}
		if (s.smgr_sock >= 0 && s.st != SMGR_DOWN) {
			ismgr = n;
			pfd[n++] = (struct pollfd){ .fd = s.smgr_sock, .events = POLLIN };
		}
		if (s.qmag.sock >= 0) {
			iqmag = n;
			pfd[n++] = (struct pollfd){ .fd = s.qmag.sock, .events = POLLIN };
		}
		ilisten = n;
		pfd[n++] = (struct pollfd){ .fd = lfd, .events = POLLIN };
		for (i = 0; i < SENSORD_MAX_CLIENTS; i++) {
			if (s.clients[i].fd < 0)
				continue;
			cmap[nclients++] = &s.clients[i];
			pfd[n++] = (struct pollfd){ .fd = s.clients[i].fd,
				.events = (short)(POLLIN | (s.clients[i].outlen ? POLLOUT : 0)) };
		}

		rc = poll(pfd, (nfds_t)n, server_deadline_ms(&s));
		if (rc < 0 && errno != EINTR)
			break;
		if (g_stop)
			break;

		if (ireg >= 0 && (pfd[ireg].revents & (POLLIN | POLLERR))) {
			static char buf[SENSORD_QMI_BUF];
			struct qrtr_packet pkt;
			uint32_t node = 0, port = 0;

			rc = ipc_recv(s.reg_sock, buf, sizeof(buf), &pkt, &node, &port);
			if (rc == -ENETRESET) {
				kmsg_note("reg2 socket reset by the IPC router; republishing");
				publish(&s, &s.reg_sock, SNS_REG2_SVC, SNS_REG2_VERS, SNS_REG2_INST);
			} else if (rc > 0) {
				reg2_rx(&s, &pkt, node, port);
			}
		}
		if (itime >= 0 && (pfd[itime].revents & (POLLIN | POLLERR))) {
			static char buf[SENSORD_QMI_BUF];
			struct qrtr_packet pkt;
			uint32_t node = 0, port = 0;

			rc = ipc_recv(s.time_sock, buf, sizeof(buf), &pkt, &node, &port);
			if (rc == -ENETRESET)
				publish(&s, &s.time_sock, SNS_TIME2_SVC, SNS_TIME2_VERS, SNS_TIME2_APPS_INST);
			else if (rc > 0)
				time2_rx(&s, &pkt, node, port);
		}
		if (ismgr >= 0 && (pfd[ismgr].revents & (POLLIN | POLLERR))) {
			static char buf[SENSORD_QMI_BUF];
			struct qrtr_packet pkt;
			uint32_t node = 0, port = 0;

			rc = ipc_recv(s.smgr_sock, buf, sizeof(buf), &pkt, &node, &port);
			if (rc == -ENETRESET)
				smgr_reset(&s, "IPC router reset");
			else if (rc > 0)
				smgr_rx(&s, &pkt, node, port);
		}
		if (iqmag >= 0 && s.qmag.sock == pfd[iqmag].fd &&
		    (pfd[iqmag].revents & (POLLIN | POLLERR))) {
			static char buf[SENSORD_QMI_BUF];
			struct qrtr_packet pkt;
			uint32_t node = 0, port = 0;

			rc = ipc_recv(s.qmag.sock, buf, sizeof(buf), &pkt, &node, &port);
			if (rc == -ENETRESET)
				qmag_forget(&s, "IPC router reset");
			else if (rc > 0)
				qmag_rx(&s, &pkt, node, port);
		}
		if (pfd[ilisten].revents & POLLIN) {
			int cfd = accept4(lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);

			if (cfd >= 0) {
				struct client *c = server_add_client(&s, cfd);
				int sndbuf = SENSORD_OUTQ;

				/* Keep what a stalled client can have in flight
				 * bounded in the kernel too, not just in outq. */
				setsockopt(cfd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

				if (!c) {
					kmsg_note("client limit (%d) reached, refusing", SENSORD_MAX_CLIENTS);
					close(cfd);
				} else {
					client_reply(&s, c, "{\"hello\":\"sensord\",\"proto\":%d}\n",
						     SENSORD_PROTO);
				}
			}
		}
		for (i = 0; i < nclients; i++) {
			short re = pfd[ilisten + 1 + i].revents;

			if (cmap[i]->fd < 0)
				continue;
			if (re & (POLLIN | POLLERR | POLLHUP))
				server_client_input(&s, cmap[i]);
		}
		smgr_tick(&s);
		qmag_tick(&s);
		reg_tick(&s);
		server_tick_clients(&s);
		server_flush_all(&s);
	}

	kmsg_note("exiting");
	for (i = 0; i < SENSORD_MAX_CLIENTS; i++)
		if (s.clients[i].fd >= 0) {
			close(s.clients[i].fd);
			s.clients[i].fd = -1;
		}
	/* Best effort: tell the DSP to stop the reports we own (the IPC
	 * router also tells it when our port closes). Not waited for. */
	if (s.st == SMGR_READY)
		for (i = 0; i < CH_COUNT; i++)
			if (s.ch[i].rate && !s.ch[i].busy)
				smgr_report(&s, i, 0);
	/* QMAG_CAL: the clients are gone, so nothing wants it. Disable an
	 * enabled instance, and give a request in flight (an ENABLE above
	 * all, whose instance would otherwise be left behind) a short while
	 * to be answered, disabling what it created. */
	{
		struct qmag *q = &s.qmag;
		int64_t end = real_now_ms() + SENSORD_QMAG_EXIT_WAIT_MS;
		bool sent = false;

		while (q->sock >= 0) {
			struct pollfd pfd = { .fd = q->sock, .events = POLLIN };
			int64_t left = end - real_now_ms();

			if (q->on && !q->busy && !sent) {
				struct sns_sam_disable_req r = { .instance = q->instance };

				sent = true;
				if (qmag_send(&s, SNS_SAM_DISABLE, sns_sam_disable_req_ei, &r) == 0)
					kmsg_note("qmag disable instance %u (exiting)", r.instance);
				continue;
			}
			if (!q->busy)
				break;
			if (left <= 0) {
				kmsg_note("qmag msg 0x%02x unanswered at exit%s", q->msg,
					  q->msg == SNS_SAM_ENABLE ?
					  "; its instance, if any, lasts until the DSP drops our port" : "");
				break;
			}
			if (poll(&pfd, 1, left > 100 ? 100 : (int)left) > 0) {
				static char buf[SENSORD_QMI_BUF];
				struct qrtr_packet pkt;
				uint32_t node = 0, port = 0;

				rc = ipc_recv(q->sock, buf, sizeof(buf), &pkt, &node, &port);
				if (rc == -ENETRESET)
					qmag_forget(&s, "IPC router reset");
				else if (rc > 0)
					qmag_rx(&s, &pkt, node, port);
			}
		}
	}
	if (s.qmag.sock >= 0)
		qrtr_close(s.qmag.sock);
	close(lfd);
	unlink(sock_path);
	if (s.smgr_sock >= 0)
		qrtr_close(s.smgr_sock);
	if (s.reg_sock >= 0)
		qrtr_close(s.reg_sock);
	if (s.time_sock >= 0)
		qrtr_close(s.time_sock);
	reg_writeback(&s, "on exit");
	sns_reg_free(&s.reg);
	return 0;
}

#endif /* SENSORD_NO_MAIN */
