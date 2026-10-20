/*
 * blesensd's pairing (the one attempt: connect, resolve, bond if needed,
 * trust, save) and forget (bond and record removed, durably). See
 * main.c.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "blesensd.h"

/* ------------------------------------------------------------ pairing */

/* BlueZ may initiate security during GATT resolution, before our own
 * StartNotify. Be its default agent only for the active pairing attempt.
 * Unregister removes us from BlueZ's default-agent queue, restoring the
 * previous agent; re-registration is ordinary and does not make us default. */
static void pair_agent_release(void)
{
	if (!D.pr.agent_requested)
		return;
	D.pr.agent_requested = false;
	D.agent_registered = false;
	D.agent_releasing = true;
	D.agent_retry_ms = 0;
	bd_agent_release_drive(bd_now_ms());
}


static void pair_done(void)
{
	struct sensor *s = NULL;
	struct dev *d = bd_dev_by_addr(D.pr.addr);
	time_t t = time(NULL);
	struct tm tm;
	char date[16] = "";

	for (int i = 0; i < MAX_SENSORS && !s; i++)
		if (!D.sensors[i].used)
			s = &D.sensors[i];
	if (!s || !d) {
		bd_pair_fail(!s ? "too many sensors" : "device gone");
		return;
	}
	/* a date only from a set clock (the boot clock starts at 1970) */
	if (gmtime_r(&t, &tm) && tm.tm_year + 1900 >= 2026)
		strftime(date, sizeof(date), "%Y-%m-%d", &tm);
	bd_sensor_init(s, D.pr.addr, D.pr.kinds, date, D.pr.name);
	if (d->has_rssi) {
		s->rssi = d->rssi;
		s->has_rssi = true;
	}
	bd_pend_cancel(false, 0, D.pr.id, false);
	D.pr.step = PS_IDLE;
	pair_agent_release();
	bd_retry_window();
	bd_pair_event("done", "");
	chef_log("paired %s", s->addr);
	bd_save_records();
	bd_found_update(d, false);
	bd_sensor_event(s, true);
	bd_sensor_keep(s, bd_now_ms());
}

void bd_pair_fail(const char *reason)
{
	struct dev *d = bd_dev_by_addr(D.pr.addr);
	struct pend t = bd_plain(PK_PLAIN);
	enum pair_step step = D.pr.step;

	if (step == PS_IDLE)
		return;
	chef_log("pairing %s failed: %s", D.pr.addr, reason);
	bd_pend_cancel(false, 0, D.pr.id, false);
	D.pr.step = PS_IDLE;
	pair_agent_release();
	if (D.bus && D.bluez && d && !strcmp(d->path, D.pr.path)) {
		if (step == PS_BOND)
			bd_call(&t, d->path, DEVICE_IF, "CancelPairing", 0, NULL);
		if (D.pr.bonded || step == PS_BOND)
			bd_call(&t, D.adapter, ADAPTER_IF, "RemoveDevice", 0, "o", d->path);
		else if (D.pr.connected_by_us)
			bd_call(&t, d->path, DEVICE_IF, "Disconnect", 0, NULL);
	}
	bd_pair_event("failed", reason);
}

static void pair_trust(void)
{
	struct pend t = bd_for_pair(PK_PAIR_TRUST);

	D.pr.step = PS_TRUST;
	bd_pair_event("saving", "");
	if (bd_call(&t, D.pr.path, PROPS, "Set", 0, "ssv", DEVICE_IF, "Trusted", "b", 1))
		bd_pair_fail("trust");
}

static bool auth_error(sd_bus_message *m)
{
	const sd_bus_error *e = sd_bus_message_get_error(m);
	const char *msg = e && e->message ? e->message : "";

	return bd_err_is(m, BLUEZ ".Error.NotPermitted") || bd_err_is(m, BLUEZ ".Error.NotAuthorized") ||
	       bd_err_is(m, BLUEZ ".Error.AuthenticationFailed") || strstr(msg, "uthenticat") ||
	       strstr(msg, "ncrypt");
}

/* Authentication can be required by a GATT profile during Connect,
 * before ServicesResolved, as well as by our measurement subscription. */
static void pair_bond(void)
{
	struct pend t;

	if (!D.agent_registered) {
		bd_pair_fail("no pairing agent");
		return;
	}
	D.pr.step = PS_BOND;
	bd_pair_event("bonding", "");
	t = bd_for_pair(PK_PAIR_BOND);
	if (bd_call(&t, D.pr.path, DEVICE_IF, "Pair", (uint64_t)PAIR_TIMEOUT_MS * 1000, NULL))
		bd_pair_fail("pair");
}

/* A new attempt can arrive while the previous one re-registers. */
static void pair_agent_request(void)
{
	struct pend t;

	if (!D.agent_registered || D.pr.agent_requested)
		return;
	t = bd_for_pair(PK_PAIR_AGENT);
	if (bd_call(&t, "/org/bluez", BLUEZ ".AgentManager1", "RequestDefaultAgent", 0, "o", AGENT_PATH))
		bd_pair_fail("pairing agent");
	else
		D.pr.agent_requested = true;
}

/* Prove the measurement works: StartNotify on the first kind's value. */
static void pair_notify(void)
{
	struct dev *d = bd_dev_by_addr(D.pr.addr);
	struct chr *c = NULL;
	struct pend t = bd_for_pair(PK_PAIR_NOTIFY);

	for (int k = 0; k < BS_KINDS && !c && d; k++)
		if (D.pr.kinds & (1u << k))
			c = bd_chr_of(d->path, bs_chr_of_kind(k));
	if (!c || !c->can_notify) {
		bd_pair_fail("no sensor measurement found");
		return;
	}
	D.pr.step = PS_NOTIFY;
	if (bd_call(&t, c->path, CHR_IF, "StartNotify", 0, NULL))
		bd_pair_fail("notify");
}

/* The pairing goes on when the device's services are resolved. */
void bd_pair_check(void)
{
	struct dev *d = bd_dev_by_addr(D.pr.addr);

	if (D.pr.step == PS_IDLE)
		return;
	if (!d || strcmp(d->path, D.pr.path)) {
		bd_pair_fail("device gone");
		return;
	}
	if (bd_now_ms() >= D.pr.deadline) {
		bd_pair_fail("timed out");
		return;
	}
	if (D.pr.step == PS_CONNECT && !D.pr.agent_requested && !D.pr.connected_by_us)
		pair_agent_request();
	if (D.pr.step == PS_RESOLVE && d->connected && d->resolved) {
		unsigned k = d->kinds;

		if (k)
			D.pr.kinds = k;	/* the full list once resolved */
		pair_notify();
		return;
	}
	if (bd_now_ms() >= D.pr.deadline)
		bd_pair_fail("timed out");
}

void bd_pair_reply(const struct pend *p, sd_bus_message *m)
{
	bool err = sd_bus_message_is_method_error(m, NULL);
	struct pend t;

	if (D.pr.step == PS_IDLE || p->pair != D.pr.id)
		return;		/* a cancelled or earlier attempt */
	switch (p->kind) {
	case PK_PAIR_AGENT: {
		struct dev *d = bd_dev_by_addr(D.pr.addr);

		if (D.pr.step != PS_CONNECT)
			return;
		if (err || !d) {
			bd_pair_fail(err ? bd_err_name(m) : "device gone");
			return;
		}
		if (d->connected) {
			D.pr.step = PS_RESOLVE;
			bd_pair_event("resolving", "");
			bd_pair_check();
		} else {
			t = bd_for_pair(PK_PAIR_CONNECT);
			D.pr.connected_by_us = true;
			if (bd_call(&t, d->path, DEVICE_IF, "Connect", CONNECT_TIMEOUT_US, NULL))
				bd_pair_fail("connect");
		}
		break;
	}
	case PK_PAIR_CONNECT:
		if (D.pr.step != PS_CONNECT)
			return;
		if (err) {
			if (auth_error(m) && !D.pr.bonded)
				pair_bond();
			else
				bd_pair_fail(bd_err_name(m));
			return;
		}
		D.pr.step = PS_RESOLVE;
		bd_pair_event("resolving", "");
		bd_pair_check();
		break;
	case PK_PAIR_NOTIFY:
		if (D.pr.step != PS_NOTIFY)
			return;
		if (!err) {
			pair_trust();
		} else if (auth_error(m) && !D.pr.bonded) {
			pair_bond();
		} else {
			bd_pair_fail(bd_err_name(m));
		}
		break;
	case PK_PAIR_BOND:
		if (D.pr.step != PS_BOND)
			return;
		if (err && !bd_err_is(m, BLUEZ ".Error.AlreadyExists")) {
			bd_pair_fail(bd_err_name(m));
			return;
		}
		D.pr.bonded = true;
		D.pr.step = PS_RESOLVE;
		bd_pair_check();
		break;
	case PK_PAIR_TRUST:
		if (D.pr.step != PS_TRUST)
			return;
		if (err)
			bd_pair_fail(bd_err_name(m));
		else
			pair_done();
		break;
	default:
		break;
	}
}

/* Why ADDR cannot be paired now, or NULL. */
const char *bd_pair_refusal(const char *addr)
{
	struct dev *d = bd_dev_by_addr(addr);
	int n = 0;

	if (!D.loaded)
		return "loading";
	if (!D.bluez || !D.powered)
		return "bluetooth off";
	if (D.pr.step != PS_IDLE)
		return "busy";
	if (bd_sensor_by_addr(addr))
		return "already paired";

	if (!d || !d->kinds)
		return "not a sensor in range";
	for (int i = 0; i < MAX_SENSORS; i++)
		n += D.sensors[i].used;
	return n >= MAX_SENSORS ? "too many sensors" : NULL;
}

void bd_pair_start(const char *addr)
{
	struct dev *d = bd_dev_by_addr(addr);

	memset(&D.pr, 0, sizeof(D.pr));
	{
		static uint32_t ids;

		D.pr.id = ++ids;
	}
	snprintf(D.pr.addr, sizeof(D.pr.addr), "%s", addr);
	snprintf(D.pr.path, sizeof(D.pr.path), "%s", d->path);
	/* the advertised name the user picked; a connection can replace the
	 * alias with the device's GAP name */
	snprintf(D.pr.name, sizeof(D.pr.name), "%s", d->name[0] ? d->name : addr);
	D.pr.kinds = d->kinds;
	D.pr.deadline = bd_now_ms() + PAIR_TIMEOUT_MS;
	D.pr.step = PS_CONNECT;
	chef_log("pairing %s", addr);
	bd_pair_event("connecting", "");
	pair_agent_request();
}

/* ------------------------------------------------------------ forget */

static void forget_finish(struct sensor *s)
{
	if (!s->removal_confirmed) {
		s->removal_confirmed = true;
		bd_save_records();
		s->removal_revision = D.record_revision;
	}
}

/* Report completion only once the record deletion is saved. */
static void forget_durable(struct sensor *s)
{
	char b[OUT_MAX], addr[18];

	snprintf(addr, sizeof(addr), "%s", s->addr);
	bd_pend_cancel(false, s->sid, 0, true);
	memset(s, 0, sizeof(*s));
	chef_log("forgot %s", addr);
	snprintf(b, sizeof(b), "{\"type\":\"forgotten\",\"addr\":\"%s\"}\n", addr);
	bd_broadcast(b);
	/* back to an ordinary found device if it is still advertising */
	if (bd_dev_by_addr(addr))
		bd_found_update(bd_dev_by_addr(addr), false);
}

/* Drive a pending removal: RemoveDevice by the device's current path. */
void bd_forget_step(struct sensor *s, int64_t now)
{
	struct dev *d;
	struct pend t;

	if (s->removal_confirmed) {
		if (D.saved_revision >= s->removal_revision)
			forget_durable(s);
		return;
	}
	if (!s->forgetting || D.saved_revision < s->forget_revision || s->forget_inflight ||
	    now < s->forget_next_ms || !D.bluez || !D.adapter[0])
		return;
	d = bd_dev_by_addr(s->addr);
	if (!d) {
		forget_finish(s);	/* BlueZ does not know it (any more) */
		return;
	}
	t = bd_for_sensor(PK_FORGET, s, 0);
	if (bd_call(&t, D.adapter, ADAPTER_IF, "RemoveDevice", 0, "o", d->path) == 0)
		s->forget_inflight = true;
	else
		s->forget_next_ms = now + RETRY_MS;
}

void bd_forget_reply(const struct pend *p, sd_bus_message *m)
{
	struct sensor *s = bd_sensor_by_sid(p->sid);

	if (!s || !s->forgetting)
		return;
	s->forget_inflight = false;
	if (!sd_bus_message_is_method_error(m, NULL) || bd_err_is(m, BLUEZ ".Error.DoesNotExist") ||
	    bd_err_is(m, "org.freedesktop.DBus.Error.UnknownObject")) {
		forget_finish(s);
	} else {
		chef_log("%s: removal failed (%s), retrying", s->addr, bd_err_name(m));
		s->forget_next_ms = bd_now_ms() + RETRY_MS;
	}
}

void bd_forget(const char *addr)
{
	struct sensor *s = bd_sensor_by_addr(addr);

	if (!s || s->forgetting)
		return;
	bd_sensor_conn_reset(s);
	s->forgetting = true;
	s->forget_next_ms = 0;
	chef_log("forgetting %s", addr);
	bd_save_records();		/* save the flag before removing from BlueZ */
	s->forget_revision = D.record_revision;
	bd_sensor_event(s, false);
	bd_forget_step(s, bd_now_ms());
}
