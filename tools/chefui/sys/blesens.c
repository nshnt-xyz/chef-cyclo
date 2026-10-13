/*
 * blesens.c - the Bluetooth pages' view of blesensd (see blesens.h).
 *
 * One `watch` connection on /run/blesensd.sock, nonblocking (connect
 * included), reconnected with backoff (1 s doubling to 8 s) while blesensd
 * is not there. Every line from blesensd is one flat JSON object; they
 * update ble_view (ble_apply_line, pure). "hold on" and "scan on" are sent
 * while a page wants them and again after a reconnect; closing the
 * connection ends both on blesensd's side, so a crash of the app cannot
 * leave the radio scanning or reconnecting for it.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <ctype.h>
#include <math.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "blesens.h"
#include "chefui.h"
#include "linebuf.h"
#include "sysstate.h"

#define SOCK "/run/blesensd.sock"
#define BACKOFF_MIN_MS 1000
#define BACKOFF_MAX_MS 8000
#define READS_PER_WAKEUP 4

struct ble_view ble_view;

static struct {
	char sock[108];
	bool running;
	int fd;
	bool connecting, watching;
	int backoff_ms;
	lv_timer_t *retry;
	struct linebuf lb;
	bool want_hold, want_scan;	/* what the pages want */
	bool sent_hold, sent_scan;	/* what this connection asked for */
} B = { .fd = -1 };

static int64_t mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Copy a string, cut to fit. */
static void cpy(char *d, size_t n, const char *s)
{
	size_t l = s ? strnlen(s, n - 1) : 0;

	memcpy(d, s ? s : "", l);
	d[l] = 0;
}

/* ------------------------------------------------------------ texts */

const char *ble_kind_title(enum ble_kind k)
{
	return k == BLE_HR ? "Heart rate" : k == BLE_POWER ? "Power" : "Speed / cadence";
}

const char *ble_kind_service(enum ble_kind k)
{
	return k == BLE_HR ? "BLE Heart Rate" : k == BLE_POWER ? "BLE Cycling Power" : "BLE Speed and Cadence";
}

const char *ble_signal_word(int rssi)
{
	return rssi >= -65 ? "strong" : rssi >= -80 ? "fair" : "weak";
}

int ble_first_kind(unsigned kinds)
{
	for (int k = 0; k < BLE_KINDS; k++)
		if (kinds & (1u << k))
			return k;
	return BLE_HR;
}

/* ------------------------------------------------------------ JSON */

/* One field of a flat JSON object. */
struct jf {
	char key[16];
	enum { J_STR, J_NUM, J_BOOL, J_NULL } t;
	char s[96];
	double n;
	bool b;
};

#define JF_MAX 24

static const char *ws(const char *p)
{
	while (*p == ' ' || *p == '\t')
		p++;
	return p;
}

/* A JSON string at p (after the quote) into out; the end after the
 * closing quote, or NULL. Escapes: \" \\ \/ \b \f \n \r \t and \uXXXX
 * (non-ASCII as '?', control characters dropped). */
static const char *jstring(const char *p, char *out, size_t n)
{
	size_t o = 0;

	for (; *p && *p != '"'; p++) {
		char c = *p;

		if (c == '\\') {
			p++;
			switch (*p) {
			case '"': case '\\': case '/': c = *p; break;
			case 'b': case 'f': case 'n': case 'r': case 't': c = 0; break;
			case 'u': {
				char h[5] = { 0 };
				unsigned long u;

				for (int i = 0; i < 4; i++)
					if (!(h[i] = p[1 + i]) || !isxdigit((unsigned char)h[i]))
						return NULL;
				u = strtoul(h, NULL, 16);
				c = u >= 0x20 && u < 0x7f ? (char)u : u < 0x20 ? 0 : '?';
				p += 4;
				break;
			}
			default:
				return NULL;
			}
		}
		if ((unsigned char)c < 0x20 && c)
			return NULL;
		if (c && o + 1 < n)
			out[o++] = c;
	}
	out[o] = 0;
	return *p == '"' ? p + 1 : NULL;
}

/* A flat object into f; the number of fields, -1 if malformed. */
static int jparse(const char *p, struct jf *f, int max)
{
	int n = 0;

	p = ws(p);
	if (*p++ != '{')
		return -1;
	p = ws(p);
	if (*p == '}')
		return *ws(p + 1) ? -1 : 0;
	for (;;) {
		struct jf *x;

		if (n >= max)
			return -1;
		x = &f[n];
		char *end;

		p = ws(p);
		if (*p++ != '"' || !(p = jstring(p, x->key, sizeof(x->key))))
			return -1;
		p = ws(p);
		if (*p++ != ':')
			return -1;
		p = ws(p);
		if (*p == '"') {
			x->t = J_STR;
			if (!(p = jstring(p + 1, x->s, sizeof(x->s))))
				return -1;
		} else if (!strncmp(p, "null", 4)) {
			x->t = J_NULL;
			p += 4;
		} else if (!strncmp(p, "true", 4) || !strncmp(p, "false", 5)) {
			x->t = J_BOOL;
			x->b = *p == 't';
			p += x->b ? 4 : 5;
		} else {
			const char *start = p;

			if (*p == '-')
				p++;
			if (*p == '0')
				p++;
			else if (*p >= '1' && *p <= '9')
				while (isdigit((unsigned char)*p)) p++;
			else
				return -1;
			if (*p == '.') {
				p++;
				if (!isdigit((unsigned char)*p)) return -1;
				while (isdigit((unsigned char)*p)) p++;
			}
			if (*p == 'e' || *p == 'E') {
				p++;
				if (*p == '+' || *p == '-') p++;
				if (!isdigit((unsigned char)*p)) return -1;
				while (isdigit((unsigned char)*p)) p++;
			}
			x->t = J_NUM;
			x->n = strtod(start, &end);
			if (end != p || !isfinite(x->n))
				return -1;
			p = end;
		}
		for (int i = 0; i < n; i++)
			if (!strcmp(f[i].key, x->key)) return -1;
		n++;
		p = ws(p);
		if (*p == '}')
			return *ws(p + 1) ? -1 : n;
		if (*p++ != ',')
			return -1;
	}
}

static const struct jf *jget(const struct jf *f, int n, const char *key)
{
	for (int i = 0; i < n; i++)
		if (!strcmp(f[i].key, key))
			return &f[i];
	return NULL;
}

static const char *jstr(const struct jf *f, int n, const char *key)
{
	const struct jf *x = jget(f, n, key);

	return x && x->t == J_STR ? x->s : NULL;
}

static bool jbool(const struct jf *f, int n, const char *key)
{
	const struct jf *x = jget(f, n, key);

	return x && x->t == J_BOOL && x->b;
}

/* A number: true and *out, false for null or missing. */
static bool jnum(const struct jf *f, int n, const char *key, double *out)
{
	const struct jf *x = jget(f, n, key);

	if (!x || x->t != J_NUM)
		return false;
	*out = x->n;
	return true;
}

static unsigned kinds_from(const char *s)
{
	static const char *const names[BLE_KINDS] = { "hr", "power", "csc" };
	char buf[32], *save = NULL, *t;
	unsigned k = 0;

	cpy(buf, sizeof(buf), s ? s : "");
	for (t = strtok_r(buf, ",", &save); t; t = strtok_r(NULL, ",", &save))
		for (int i = 0; i < BLE_KINDS; i++)
			if (!strcmp(t, names[i]))
				k |= 1u << i;
	return k;
}

static int kind_from(const char *s)
{
	return !s ? -1 : !strcmp(s, "hr") ? BLE_HR : !strcmp(s, "power") ? BLE_POWER
		: !strcmp(s, "csc") ? BLE_CSC : -1;
}

static bool addr_ok(const char *a)
{
	if (!a || strlen(a) != 17)
		return false;
	for (int i = 0; i < 17; i++)
		if (i % 3 == 2 ? a[i] != ':' : !((a[i] >= '0' && a[i] <= '9') || (a[i] >= 'A' && a[i] <= 'F')))
			return false;
	return true;
}

/* ------------------------------------------------------------ the view */


struct ble_sensor *ble_sensor(struct ble_view *v, const char *addr)
{
	for (int i = 0; addr && i < v->n; i++)
		if (!strcmp(v->s[i].addr, addr))
			return &v->s[i];
	return NULL;
}

static struct ble_found *found(struct ble_view *v, const char *addr)
{
	for (int i = 0; i < v->n_found; i++)
		if (!strcmp(v->f[i].addr, addr))
			return &v->f[i];
	return NULL;
}

static void remove_sensor(struct ble_view *v, struct ble_sensor *s)
{
	int i = (int)(s - v->s);

	memmove(&v->s[i], &v->s[i + 1], (size_t)(v->n - i - 1) * sizeof(v->s[0]));
	v->n--;
}

static void remove_found(struct ble_view *v, struct ble_found *f)
{
	int i = (int)(f - v->f);

	memmove(&v->f[i], &v->f[i + 1], (size_t)(v->n_found - i - 1) * sizeof(v->f[0]));
	v->n_found--;
}

static enum ble_state state_from(const char *s)
{
	return !s ? BLE_ST_WAITING : !strcmp(s, "off") ? BLE_ST_OFF : !strcmp(s, "connected") ? BLE_ST_CONNECTED
		: !strcmp(s, "connecting") ? BLE_ST_CONNECTING : !strcmp(s, "forgetting") ? BLE_ST_FORGETTING
		: BLE_ST_WAITING;
}

static void pair_end(struct ble_view *v, bool ok, const char *reason)
{
	v->pair.st = ok ? BLE_PAIR_DONE : BLE_PAIR_FAILED;
	cpy(v->pair.reason, sizeof(v->pair.reason), reason ? reason : "");
}

bool ble_apply_line(struct ble_view *v, const char *line, int64_t now_ms)
{
	struct jf f[JF_MAX];
	int n = jparse(line, f, JF_MAX);
	const char *type, *addr;
	struct ble_view old = *v;

	(void)now_ms;
	if (n < 0 || !(type = jstr(f, n, "type")))
		return false;
	addr = jstr(f, n, "addr");
	if (!strcmp(type, "ok") && jstr(f, n, "cmd") && !strcmp(jstr(f, n, "cmd"), "watch")) {
		/* a snapshot follows: start from nothing */
		v->known = true;
		v->n = v->n_found = 0;
	} else if (!strcmp(type, "adapter")) {
		v->adapter = jbool(f, n, "present");
		v->powered = jbool(f, n, "powered");
		v->scanning = jbool(f, n, "scanning");
	} else if (!strcmp(type, "sensor") && addr_ok(addr)) {
		struct ble_sensor *s = ble_sensor(v, addr);
		double d;

		if (!s) {
			if (v->n >= BLE_SENSORS_MAX)
				return false;
			s = &v->s[v->n++];
			memset(s, 0, sizeof(*s));
			cpy(s->addr, sizeof(s->addr), addr);
		}
		s->kinds = kinds_from(jstr(f, n, "kinds"));
		cpy(s->name, sizeof(s->name), jstr(f, n, "name") ? jstr(f, n, "name") : addr);
		cpy(s->paired, sizeof(s->paired), jstr(f, n, "paired") ? jstr(f, n, "paired") : "");
		s->st = state_from(jstr(f, n, "state"));
		s->battery = jnum(f, n, "battery", &d) && d >= 0 && d <= 100 ? (int)d : -1;
		s->has_rssi = jnum(f, n, "rssi", &d) && d >= -127 && d <= 20;
		s->rssi = s->has_rssi ? (int)d : 0;
		s->rssi_live = s->has_rssi && jbool(f, n, "rssi_live");
		if (s->st != BLE_ST_CONNECTED)
			memset(s->val, 0, sizeof(s->val));	/* nothing live without a link */
		if (found(v, addr))
			remove_found(v, found(v, addr));
	} else if (!strcmp(type, "value") && addr_ok(addr)) {
		struct ble_sensor *s = ble_sensor(v, addr);
		int k = kind_from(jstr(f, n, "kind"));
		struct ble_value *x;

		if (!s || k < 0 || s->st != BLE_ST_CONNECTED || !(s->kinds & (1u << k)))
			return false;
		x = &s->val[k];
		x->has = jnum(f, n, k == BLE_CSC ? "speed" : "value", &x->v);
		x->has_cadence = k != BLE_HR && jnum(f, n, "cadence", &x->cadence);
	} else if (!strcmp(type, "forgotten") && addr_ok(addr)) {
		if (ble_sensor(v, addr))
			remove_sensor(v, ble_sensor(v, addr));
	} else if (!strcmp(type, "found") && addr_ok(addr)) {
		struct ble_found *x = found(v, addr);
		double d;

		if (ble_sensor(v, addr))
			return false;	/* ours already */
		if (!x) {
			if (v->n_found >= BLE_FOUND_MAX)
				return false;
			x = &v->f[v->n_found++];
			memset(x, 0, sizeof(*x));
			cpy(x->addr, sizeof(x->addr), addr);
		}
		x->kinds = kinds_from(jstr(f, n, "kinds"));
		cpy(x->name, sizeof(x->name), jstr(f, n, "name") && *jstr(f, n, "name")
			 ? jstr(f, n, "name") : addr);
		x->has_rssi = jnum(f, n, "rssi", &d) && d >= -127 && d <= 20;
		x->rssi = x->has_rssi ? (int)d : 0;
	} else if (!strcmp(type, "lost") && addr_ok(addr)) {
		if (found(v, addr))
			remove_found(v, found(v, addr));
	} else if (!strcmp(type, "pair") && addr_ok(addr)) {
		const char *st = jstr(f, n, "state");

		if (v->pair.st != BLE_PAIR_RUNNING || strcmp(v->pair.addr, addr) || !st)
			return false;	/* not ours (another client's, or one we let go) */
		if (!strcmp(st, "done"))
			pair_end(v, true, NULL);
		else if (!strcmp(st, "failed"))
			pair_end(v, false, jstr(f, n, "reason"));
		else
			cpy(v->pair.step, sizeof(v->pair.step), st);
	} else if (!strcmp(type, "error")) {
		const char *cmd = jstr(f, n, "cmd");

		if (cmd && !strcmp(cmd, "pair") && v->pair.st == BLE_PAIR_RUNNING && !strcmp(v->pair.step, "asking"))
			pair_end(v, false, jstr(f, n, "reason"));
		else
			return false;
	} else {
		return false;
	}
	return memcmp(&old, v, sizeof(old)) != 0;
}

int ble_rows(const struct ble_view *v, struct ble_row *out, int max)
{
	int n = 0;

	for (int k = 0; k < BLE_KINDS; k++)
		for (int i = 0; i < v->n; i++)
			if ((v->s[i].kinds & (1u << k)) && v->s[i].st != BLE_ST_FORGETTING && n < max)
				out[n++] = (struct ble_row){ .sensor = i, .kind = (enum ble_kind)k };
	return n;
}

int ble_rows_connected(const struct ble_view *v)
{
	struct ble_row r[BLE_SENSORS_MAX * BLE_KINDS];
	int n = ble_rows(v, r, BLE_SENSORS_MAX * BLE_KINDS), c = 0;

	for (int i = 0; i < n; i++)
		c += v->s[r[i].sensor].st == BLE_ST_CONNECTED;
	return c;
}

/* ------------------------------------------------------------ connection */

static void conn_start(void);

static void retry_fired(lv_timer_t *t)
{
	(void)t;
	B.retry = NULL;
	if (B.running)
		conn_start();
}

static void conn_close(void)
{
	if (B.fd >= 0) {
		chefui_unwatch_fd(B.fd);
		close(B.fd);
		B.fd = -1;
	}
	B.connecting = B.watching = false;
	B.sent_hold = B.sent_scan = false;
	linebuf_reset(&B.lb);
}

/* blesensd went (or never came): nothing of it is current any more. */
static void lost(void)
{
	bool changed = ble_view.known || ble_view.n || ble_view.n_found || ble_view.pair.st == BLE_PAIR_RUNNING;

	conn_close();
	ble_view.known = ble_view.adapter = ble_view.powered = ble_view.scanning = false;
	ble_view.n = ble_view.n_found = 0;
	if (ble_view.pair.st == BLE_PAIR_RUNNING)
		pair_end(&ble_view, false, "blesensd stopped");
	if (changed)
		sys_changed(SYS_BLE);
	if (B.retry || !B.running)
		return;
	B.retry = lv_timer_create(retry_fired, (uint32_t)B.backoff_ms, NULL);
	lv_timer_set_repeat_count(B.retry, 1);
	B.backoff_ms = B.backoff_ms * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : B.backoff_ms * 2;
}

static bool send_str(const char *s)
{
	size_t n = strlen(s);

	return B.fd >= 0 && !B.connecting && send(B.fd, s, n, MSG_NOSIGNAL | MSG_DONTWAIT) == (ssize_t)n;
}

/* hold/scan follow what the pages want */
static void wants_sync(void)
{
	if (!B.watching)
		return;
	if (B.want_hold != B.sent_hold && send_str(B.want_hold ? "hold on\n" : "hold off\n"))
		B.sent_hold = B.want_hold;
	if (B.want_scan != B.sent_scan && send_str(B.want_scan ? "scan on\n" : "scan off\n"))
		B.sent_scan = B.want_scan;
}

static void line_in(char *line, void *user)
{
	(void)user;
	struct jf f[JF_MAX];
	int n = jparse(line, f, JF_MAX);
	const char *type = n >= 0 ? jstr(f, n, "type") : NULL;
	const char *cmd = n >= 0 ? jstr(f, n, "cmd") : NULL;

	if (type && cmd && !strcmp(type, "ok") && !strcmp(cmd, "watch")) {
		B.watching = true;
		B.backoff_ms = BACKOFF_MIN_MS;
		wants_sync();
	}
	if (ble_apply_line(&ble_view, line, mono_ms()))
		sys_changed(SYS_BLE);
}

static void io(int fd, short revents, void *user)
{
	char buf[4096];

	(void)user;
	if (B.connecting) {
		int err = 0;
		socklen_t l = sizeof(err);

		if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &l) || err || (revents & (POLLERR | POLLHUP))) {
			lost();
			return;
		}
		B.connecting = false;
		if (!send_str("watch\n")) {
			lost();
			return;
		}
		chefui_watch_fd(fd, POLLIN, io, NULL);
		return;
	}
	for (int i = 0; i < READS_PER_WAKEUP && B.fd >= 0; i++) {
		ssize_t n = recv(B.fd, buf, sizeof(buf), MSG_DONTWAIT);

		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return;
		if (n <= 0) {
			lost();
			return;
		}
		linebuf_feed(&B.lb, buf, (size_t)n, line_in, NULL);
	}
}

static void conn_start(void)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	int fd;

	if (B.fd >= 0)
		return;
	cpy(sa.sun_path, sizeof(sa.sun_path), B.sock);
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		lost();
		return;
	}
	B.fd = fd;
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) && errno != EINPROGRESS) {
		lost();		/* not there yet */
		return;
	}
	B.connecting = true;
	chefui_watch_fd(fd, POLLOUT, io, NULL);
}

int sys_ble_start(const char *sock)
{
	if (B.retry) {
		lv_timer_delete(B.retry);
		B.retry = NULL;
	}
	conn_close();
	memset(&ble_view, 0, sizeof(ble_view));
	cpy(B.sock, sizeof(B.sock), sock ? sock : SOCK);
	B.backoff_ms = BACKOFF_MIN_MS;
	B.running = true;
	conn_start();
	return 0;
}

void sys_ble_stop(void)
{
	B.running = false;
	if (B.retry) {
		lv_timer_delete(B.retry);
		B.retry = NULL;
	}
	conn_close();
	memset(&ble_view, 0, sizeof(ble_view));
	sys_changed(SYS_BLE);
}

void sys_ble_want(bool hold, bool scan)
{
	B.want_hold = hold;
	B.want_scan = scan && hold;
	wants_sync();
}

void sys_ble_wants(bool *hold, bool *scan)
{
	*hold = B.want_hold;
	*scan = B.want_scan;
}

int sys_ble_pair(const struct ble_found *f)
{
	char b[40];

	if (!B.watching || ble_view.pair.st == BLE_PAIR_RUNNING)
		return -ENOTCONN;
	snprintf(b, sizeof(b), "pair %s\n", f->addr);
	if (!send_str(b))
		return -EIO;
	memset(&ble_view.pair, 0, sizeof(ble_view.pair));
	ble_view.pair.st = BLE_PAIR_RUNNING;
	cpy(ble_view.pair.addr, sizeof(ble_view.pair.addr), f->addr);
	cpy(ble_view.pair.name, sizeof(ble_view.pair.name), f->name);
	ble_view.pair.kinds = f->kinds;
	snprintf(ble_view.pair.step, sizeof(ble_view.pair.step), "asking");
	ble_view.pair.started_ms = mono_ms();
	sys_changed(SYS_BLE);
	return 0;
}

int sys_ble_cancel(void)
{
	char b[40];

	if (ble_view.pair.st != BLE_PAIR_RUNNING)
		return 0;
	snprintf(b, sizeof(b), "cancel %s\n", ble_view.pair.addr);
	send_str(b);
	/* its "failed" event is not waited for: the pairing is let go now */
	memset(&ble_view.pair, 0, sizeof(ble_view.pair));
	sys_changed(SYS_BLE);
	return 0;
}

void sys_ble_pair_clear(void)
{
	if (ble_view.pair.st == BLE_PAIR_DONE || ble_view.pair.st == BLE_PAIR_FAILED) {
		memset(&ble_view.pair, 0, sizeof(ble_view.pair));
		sys_changed(SYS_BLE);
	}
}

int sys_ble_forget(const char *addr)
{
	char b[40];

	if (!B.watching)
		return -ENOTCONN;
	snprintf(b, sizeof(b), "forget %s\n", addr);
	return send_str(b) ? 0 : -EIO;
}
