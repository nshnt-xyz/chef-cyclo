/*
 * blesclient.c - the blesensd client and the view's rules (blesclient.h).
 * No LVGL: built into chefui's state sources (blesens.c is its wrapper)
 * and the LVGL-free host test.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "blesclient.h"
#include "common/json.h"

#define BACKOFF_MIN_MS 1000
#define BACKOFF_MAX_MS 8000

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

/* One blesensd line through the shared reader, with the rules of the
 * flat parser it replaced: a number that is not finite makes the whole
 * line invalid, and strings are read as ASCII (control characters
 * dropped, every other character outside ASCII one '?', cut to fit). */
struct line {
	struct json_obj o;
	char s[16][96];		/* the strings handed out, one per jstr() call (at most 9 a line) */
	int ns;
};

static bool line_parse(struct line *l, const char *text)
{
	double d;

	l->ns = 0;
	if (json_parse(&l->o, text, strlen(text)))
		return false;
	for (int i = 0; i < l->o.n; i++)
		if (l->o.m[i].type == JSON_NUMBER &&
		    (!json_span_double(JSON_NUMBER, l->o.m[i].val, &d) || !isfinite(d)))
			return false;
	return true;
}

/* KEY's string, or NULL when missing, not a string or not readable. */
static const char *jstr(struct line *l, const char *key)
{
	const struct json_member *m = json_find(&l->o, key);
	char raw[512], *out;
	size_t o = 0;

	if (!m || m->type != JSON_STRING || !json_span_string(m->type, m->val, raw, sizeof(raw)))
		return NULL;
	out = l->s[l->ns++ % 16];
	for (const unsigned char *p = (const unsigned char *)raw; *p; p++) {
		if (*p >= 0x80 && *p < 0xc0)
			continue;	/* the rest of a UTF-8 character */
		if (*p < 0x20)
			continue;
		if (o + 1 < sizeof(l->s[0]))
			out[o++] = *p >= 0x80 ? '?' : (char)*p;
	}
	out[o] = '\0';
	return out;
}

static bool jbool(struct line *l, const char *key)
{
	bool b;

	return json_get_bool(&l->o, key, &b) && b;
}

/* A number: true and *out, false for null or missing. */
static bool jnum(struct line *l, const char *key, double *out)
{
	return json_get_double(&l->o, key, out);
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
	struct line l;
	const char *type, *addr;
	struct ble_view old = *v;

	(void)now_ms;
	if (!line_parse(&l, line) || !(type = jstr(&l, "type")))
		return false;
	addr = jstr(&l, "addr");
	if (!strcmp(type, "ok") && jstr(&l, "cmd") && !strcmp(jstr(&l, "cmd"), "watch")) {
		/* a snapshot follows: start from nothing */
		v->known = true;
		v->n = v->n_found = 0;
	} else if (!strcmp(type, "adapter")) {
		v->adapter = jbool(&l, "present");
		v->powered = jbool(&l, "powered");
		v->scanning = jbool(&l, "scanning");
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
		s->kinds = kinds_from(jstr(&l, "kinds"));
		cpy(s->name, sizeof(s->name), jstr(&l, "name") ? jstr(&l, "name") : addr);
		cpy(s->paired, sizeof(s->paired), jstr(&l, "paired") ? jstr(&l, "paired") : "");
		s->st = state_from(jstr(&l, "state"));
		s->battery = jnum(&l, "battery", &d) && d >= 0 && d <= 100 ? (int)d : -1;
		s->has_rssi = jnum(&l, "rssi", &d) && d >= -127 && d <= 20;
		s->rssi = s->has_rssi ? (int)d : 0;
		s->rssi_live = s->has_rssi && jbool(&l, "rssi_live");
		if (s->st != BLE_ST_CONNECTED)
			memset(s->val, 0, sizeof(s->val));	/* nothing live without a link */
		if (found(v, addr))
			remove_found(v, found(v, addr));
	} else if (!strcmp(type, "value") && addr_ok(addr)) {
		struct ble_sensor *s = ble_sensor(v, addr);
		int k = kind_from(jstr(&l, "kind"));
		struct ble_value *x;

		if (!s || k < 0 || s->st != BLE_ST_CONNECTED || !(s->kinds & (1u << k)))
			return false;
		x = &s->val[k];
		x->has = jnum(&l, k == BLE_CSC ? "speed" : "value", &x->v);
		x->has_cadence = k != BLE_HR && jnum(&l, "cadence", &x->cadence);
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
		x->kinds = kinds_from(jstr(&l, "kinds"));
		cpy(x->name, sizeof(x->name), jstr(&l, "name") && *jstr(&l, "name")
			 ? jstr(&l, "name") : addr);
		x->has_rssi = jnum(&l, "rssi", &d) && d >= -127 && d <= 20;
		x->rssi = x->has_rssi ? (int)d : 0;
	} else if (!strcmp(type, "lost") && addr_ok(addr)) {
		if (found(v, addr))
			remove_found(v, found(v, addr));
	} else if (!strcmp(type, "pair") && addr_ok(addr)) {
		const char *st = jstr(&l, "state");

		if (v->pair.st != BLE_PAIR_RUNNING || strcmp(v->pair.addr, addr) || !st)
			return false;	/* not ours (another client's, or one we let go) */
		if (!strcmp(st, "done"))
			pair_end(v, true, NULL);
		else if (!strcmp(st, "failed"))
			pair_end(v, false, jstr(&l, "reason"));
		else
			cpy(v->pair.step, sizeof(v->pair.step), st);
	} else if (!strcmp(type, "error")) {
		const char *cmd = jstr(&l, "cmd");

		if (cmd && !strcmp(cmd, "pair") && v->pair.st == BLE_PAIR_RUNNING && !strcmp(v->pair.step, "asking"))
			pair_end(v, false, jstr(&l, "reason"));
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

/* ------------------------------------------------------------ the client */

static void changed(struct blesclient *bc)
{
	if (bc->cfg.changed)
		bc->cfg.changed(bc, bc->cfg.ctx);
}

/* hold/scan follow what the caller wants */
static void wants_sync(struct blesclient *bc, int64_t now)
{
	if (!bc->watching)
		return;
	if (bc->want_hold != bc->sent_hold &&
	    !lineclient_send(&bc->lc, bc->want_hold ? "hold on\n" : "hold off\n", now))
		bc->sent_hold = bc->want_hold;
	if (bc->watching && bc->want_scan != bc->sent_scan &&
	    !lineclient_send(&bc->lc, bc->want_scan ? "scan on\n" : "scan off\n", now))
		bc->sent_scan = bc->want_scan;
}

static void on_connected(struct lineclient *lc, int64_t now)
{
	lineclient_send(lc, "watch\n", now);
}

static void on_line(struct lineclient *lc, char *text, size_t len, int64_t now)
{
	struct blesclient *bc = lineclient_ctx(lc);
	struct line l;
	const char *type, *cmd;

	(void)len;
	if (line_parse(&l, text) && (type = jstr(&l, "type")) && (cmd = jstr(&l, "cmd")) &&
	    !strcmp(type, "ok") && !strcmp(cmd, "watch")) {
		bc->watching = true;
		lineclient_healthy(lc);
		wants_sync(bc, now);
		if (!lineclient_up(lc))
			return;		/* a send failed: lost */
	}
	if (ble_apply_line(&bc->view, text, now))
		changed(bc);
}

/* blesensd went: nothing of it is current any more. */
static void on_lost(struct lineclient *lc, int64_t now)
{
	struct blesclient *bc = lineclient_ctx(lc);
	struct ble_view *v = &bc->view;
	bool was = v->known || v->n || v->n_found || v->pair.st == BLE_PAIR_RUNNING;

	(void)now;
	bc->watching = bc->sent_hold = bc->sent_scan = false;
	v->known = v->adapter = v->powered = v->scanning = false;
	v->n = v->n_found = 0;
	if (v->pair.st == BLE_PAIR_RUNNING)
		pair_end(v, false, "blesensd stopped");
	if (was)
		changed(bc);
}

void blesclient_init(struct blesclient *bc, const struct blesclient_config *cfg)
{
	struct lineclient_config c = {
		.path = cfg->path ? cfg->path : BLESC_SOCK,
		.backoff_min_ms = BACKOFF_MIN_MS, .backoff_max_ms = BACKOFF_MAX_MS,
		.on_connected = on_connected, .on_line = on_line, .on_lost = on_lost, .ctx = bc,
	};

	memset(bc, 0, sizeof(*bc));
	bc->cfg = *cfg;
	lineclient_init(&bc->lc, &c);
}

void blesclient_start(struct blesclient *bc, int64_t now)
{
	lineclient_start(&bc->lc, now);
}

void blesclient_stop(struct blesclient *bc, int64_t now)
{
	lineclient_stop(&bc->lc, now);
	bc->watching = bc->sent_hold = bc->sent_scan = false;
	memset(&bc->view, 0, sizeof(bc->view));
	changed(bc);
}

void blesclient_want(struct blesclient *bc, bool hold, bool scan, int64_t now)
{
	bc->want_hold = hold;
	bc->want_scan = scan && hold;
	wants_sync(bc, now);
}

int blesclient_pair(struct blesclient *bc, const struct ble_found *f, int64_t now)
{
	struct ble_view *v = &bc->view;
	char b[40];

	if (!bc->watching || v->pair.st == BLE_PAIR_RUNNING)
		return -ENOTCONN;
	snprintf(b, sizeof(b), "pair %s\n", f->addr);
	if (lineclient_send(&bc->lc, b, now))
		return -EIO;
	memset(&v->pair, 0, sizeof(v->pair));
	v->pair.st = BLE_PAIR_RUNNING;
	cpy(v->pair.addr, sizeof(v->pair.addr), f->addr);
	cpy(v->pair.name, sizeof(v->pair.name), f->name);
	v->pair.kinds = f->kinds;
	snprintf(v->pair.step, sizeof(v->pair.step), "asking");
	v->pair.started_ms = now;
	changed(bc);
	return 0;
}

int blesclient_cancel(struct blesclient *bc, int64_t now)
{
	struct ble_view *v = &bc->view;
	char b[40];

	if (v->pair.st != BLE_PAIR_RUNNING)
		return 0;
	snprintf(b, sizeof(b), "cancel %s\n", v->pair.addr);
	lineclient_send(&bc->lc, b, now);
	/* its "failed" event is not waited for: the pairing is let go now */
	memset(&v->pair, 0, sizeof(v->pair));
	changed(bc);
	return 0;
}

void blesclient_pair_clear(struct blesclient *bc)
{
	struct ble_view *v = &bc->view;

	if (v->pair.st == BLE_PAIR_DONE || v->pair.st == BLE_PAIR_FAILED) {
		memset(&v->pair, 0, sizeof(v->pair));
		changed(bc);
	}
}

int blesclient_forget(struct blesclient *bc, const char *addr, int64_t now)
{
	char b[40];

	if (!bc->watching)
		return -ENOTCONN;
	snprintf(b, sizeof(b), "forget %s\n", addr);
	return lineclient_send(&bc->lc, b, now) ? -EIO : 0;
}
