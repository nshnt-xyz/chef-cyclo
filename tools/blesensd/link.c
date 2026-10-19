/*
 * blesensd's sensor links: the values from notifications, the
 * connections (hold, the retry window, backoff, StartNotify, battery)
 * and the LE scan. See main.c.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "blesensd.h"

/* ------------------------------------------------------------ values */

void bd_value_bytes(struct chr *c, const uint8_t *b, size_t n)
{
	struct dev *d = bd_dev_by_path(c->dev);
	struct sensor *s = d ? bd_sensor_by_addr(d->addr) : NULL;
	struct bs_sample smp;
	int64_t now = bd_now_ms();
	double rate;
	int k = c->uuid16 == BS_CHR_HR ? BS_HR : c->uuid16 == BS_CHR_POWER ? BS_POWER
		: c->uuid16 == BS_CHR_CSC ? BS_CSC : -1;

	/* ours, connected, and on a session this connection started */
	if (!s || s->forgetting || !d->connected)
		return;
	if (c->uuid16 == BS_CHR_BATTERY) {
		if (s->battery_notify != NS_NONE && n == 1 && b[0] <= 100) {
			s->gatt_battery = b[0];
			bd_sensor_event(s, false);
		}
		return;
	}
	if (k < 0 || !(s->kinds & (1u << k)) || s->notify[k] == NS_NONE)
		return;
	/* A rate needs two samples within the freshness interval. */
	if (!s->last_ms[k] || now - s->last_ms[k] >= STALE_MS) {
		bs_revs_reset(&s->crank[k]);
		if (k == BS_CSC)
			bs_revs_reset(&s->wheel);
	}
	if (k == BS_HR) {
		int v = bs_parse_hr(b, n);

		if (v < 0 || v > BS_HR_MAX)
			return;
		s->hr = v;
	} else if (k == BS_POWER) {
		if (bs_parse_power(b, n, &smp))
			return;
		s->power = smp.power;
		s->has_power = true;
		if (smp.has_crank) {
			rate = bs_revs_feed(&s->crank[k], smp.crank_revs, smp.crank_time, 16, 1024, now);
			s->cadence[k] = rate > BS_CADENCE_MAX ? -1 : rate;
		} else {
			s->cadence[k] = -1;
			bs_revs_reset(&s->crank[k]);
		}
	} else {
		if (bs_parse_csc(b, n, &smp))
			return;
		if (smp.has_wheel) {
			rate = bs_revs_feed(&s->wheel, smp.wheel_revs, smp.wheel_time, 32, 1024, now);
			s->speed = rate < 0 ? -1 : bs_speed_kmh(rate, D.wheel_mm);
			if (s->speed > BS_SPEED_MAX)
				s->speed = -1;
		} else {
			s->speed = -1;
			bs_revs_reset(&s->wheel);
		}
		if (smp.has_crank) {
			rate = bs_revs_feed(&s->crank[k], smp.crank_revs, smp.crank_time, 16, 1024, now);
			s->cadence[k] = rate > BS_CADENCE_MAX ? -1 : rate;
		} else {
			s->cadence[k] = -1;
			bs_revs_reset(&s->crank[k]);
		}
	}
	s->last_ms[k] = now;
	bd_value_event(s, k);
}

/* ------------------------------------------------------------ connections */

void bd_retry_window(void)
{
	int64_t now = bd_now_ms();

	D.retry_until = now + (int64_t)bd_retry_s * 1000 / bd_scale;
	for (int i = 0; i < MAX_SENSORS; i++)
		if (D.sensors[i].used) {
			D.sensors[i].next_try_ms = 0;
			D.sensors[i].backoff_ms = BACKOFF_MIN_MS;
		}
}

/* A hold starts or ends; the window lapses: sensor states follow. */

void bd_links_sync(void)
{
	bool h = bd_held();

	if (h && !D.held) {
		for (int i = 0; i < MAX_SENSORS; i++)
			if (D.sensors[i].used) {
				D.sensors[i].next_try_ms = 0;
				D.sensors[i].backoff_ms = BACKOFF_MIN_MS;
			}
		vlog("held: reconnecting");
	}
	D.held = h;
	for (int i = 0; i < MAX_SENSORS; i++)
		if (D.sensors[i].used) {
			struct sensor *s = &D.sensors[i];
			struct dev *d = bd_dev_by_addr(s->addr);

			/* Cancelling our bus callback alone does not stop BlueZ's
			 * outstanding LE scan/connection procedure. */
			if (!bd_links_wanted() && s->connect_inflight && d && !d->connected) {
				struct pend t;

				bd_sensor_conn_reset(s);
				t = bd_for_sensor(PK_PLAIN, s, 0);
				bd_call(&t, d->path, DEVICE_IF, "Disconnect", 0, NULL);
			}
			bd_sensor_event(s, false);
		}
}

/* The connection is gone (or never was): everything per connection goes,
 * values go stale now, and late replies for it no longer match. */
void bd_sensor_conn_reset(struct sensor *s)
{
	bool had[BS_KINDS];

	bd_pend_cancel(false, s->sid, 0, false);
	s->epoch++;
	s->was_connected = s->was_resolved = false;
	s->connect_inflight = false;
	s->battery_read = false;
	s->battery_notify = NS_NONE;
	s->gatt_battery = -1;
	s->hr = -1;
	s->power = 0;
	s->has_power = false;
	s->speed = -1;
	for (int k = 0; k < BS_KINDS; k++) {
		had[k] = s->last_ms[k] != 0;
		s->notify[k] = NS_NONE;
		s->last_ms[k] = 0;
		s->cadence[k] = -1;
		bs_revs_reset(&s->crank[k]);
	}
	bs_revs_reset(&s->wheel);
	for (int k = 0; k < BS_KINDS; k++)
		if (had[k])
			bd_value_event(s, k);
}

void bd_sensor_init(struct sensor *s, const char *addr, unsigned kinds, const char *date, const char *name)
{
	memset(s, 0, sizeof(*s));
	s->used = true;
	s->sid = ++D.next_sid;
	snprintf(s->addr, sizeof(s->addr), "%s", addr);
	s->kinds = kinds;
	snprintf(s->date, sizeof(s->date), "%s", date);
	snprintf(s->name, sizeof(s->name), "%s", name);
	s->backoff_ms = BACKOFF_MIN_MS;
	s->gatt_battery = -1;
	s->hr = -1;
	s->speed = -1;
	for (int k = 0; k < BS_KINDS; k++) {
		s->cadence[k] = -1;
		bs_revs_reset(&s->crank[k]);
	}
	bs_revs_reset(&s->wheel);
}

/* One sensor's step towards connected with notifications on. */
void bd_sensor_keep(struct sensor *s, int64_t now)
{
	struct dev *d = bd_dev_by_addr(s->addr);
	struct pend t;

	if (s->forgetting || !D.powered || !d || (D.pr.step != PS_IDLE && !strcmp(D.pr.addr, s->addr)))
		return;
	if (!d->connected) {
		if (!s->connect_inflight && now >= s->next_try_ms && bd_links_wanted()) {
			t = bd_for_sensor(PK_CONNECT, s, 0);
			if (bd_call(&t, d->path, DEVICE_IF, "Connect", CONNECT_TIMEOUT_US, NULL) == 0)
				s->connect_inflight = true;
		}
		return;
	}
	if (!s->was_connected) {
		s->was_connected = true;
		s->backoff_ms = BACKOFF_MIN_MS;
	}
	if (!d->resolved)
		return;
	s->was_resolved = true;
	for (int k = 0; k < BS_KINDS; k++) {
		struct chr *c;

		if (!(s->kinds & (1u << k)) || s->notify[k] != NS_NONE)
			continue;
		c = bd_chr_of(d->path, bs_chr_of_kind(k));
		t = bd_for_sensor(PK_NOTIFY, s, k);
		if (c && c->can_notify && bd_call(&t, c->path, CHR_IF, "StartNotify", 0, NULL) == 0)
			s->notify[k] = NS_PENDING;
	}
	if (d->battery < 0) {
		struct chr *c = bd_chr_of(d->path, BS_CHR_BATTERY);

		t = bd_for_sensor(PK_BATTERY, s, 0);
		if (!s->battery_read && c && c->can_read && bd_call(&t, c->path, CHR_IF, "ReadValue", 0, "a{sv}", 0) == 0)
			s->battery_read = true;
		t = bd_for_sensor(PK_BATTERY_NOTIFY, s, 0);
		if (s->battery_notify == NS_NONE && c && c->can_notify &&
		    bd_call(&t, c->path, CHR_IF, "StartNotify", 0, NULL) == 0)
			s->battery_notify = NS_PENDING;
	}
}

void bd_sensor_reply(const struct pend *p, sd_bus_message *m)
{
	struct sensor *s = bd_sensor_by_sid(p->sid);

	if (!s || s->epoch != p->epoch)
		return;		/* forgotten, replaced or another connection */
	if (p->kind == PK_CONNECT) {
		s->connect_inflight = false;
		if (sd_bus_message_is_method_error(m, NULL)) {
			vlog("connect %s: %s; next in %d ms", s->addr, bd_err_name(m), s->backoff_ms);
			s->next_try_ms = bd_now_ms() + s->backoff_ms;
			s->backoff_ms = s->backoff_ms * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : s->backoff_ms * 2;
		}
	} else if (p->kind == PK_NOTIFY) {
		/* InProgress: BlueZ already has our session (registering) */
		if (!sd_bus_message_is_method_error(m, NULL) || bd_err_is(m, BLUEZ ".Error.InProgress")) {
			s->notify[p->k] = NS_OURS;
		} else {
			bd_notef("%s: %s notifications refused: %s", s->addr, bs_kind_name(p->k), bd_err_name(m));
			s->notify[p->k] = NS_NONE;	/* tried again on the next tick */
		}
	} else if (p->kind == PK_BATTERY_NOTIFY) {
		s->battery_notify = !sd_bus_message_is_method_error(m, NULL) ||
			bd_err_is(m, BLUEZ ".Error.InProgress") ? NS_OURS : NS_NONE;
	} else if (p->kind == PK_BATTERY) {
		const void *v = NULL;
		size_t n = 0;

		if (!sd_bus_message_is_method_error(m, NULL) && sd_bus_message_read_array(m, 'y', &v, &n) >= 0 &&
		    n == 1 && ((const uint8_t *)v)[0] <= 100)
			s->gatt_battery = ((const uint8_t *)v)[0];
	}
	bd_sensor_event(s, false);
}

/* ------------------------------------------------------------ scanning */

static bool scan_wanted(void)
{
	if (!D.bluez || !D.powered || !D.adapter[0])
		return false;
	for (int i = 0; i < MAX_CLIENTS; i++)
		if (D.clients[i].fd >= 0 && D.clients[i].scan)
			return true;
	return false;
}

/* Our discovery session follows scan_wanted. BlueZ ends it on its own when
 * the adapter powers off; that is seen through Powered. */
void bd_scan_sync(void)
{
	bool want = scan_wanted();
	struct pend t;

	if (D.scan_inflight || want == D.scan_on || bd_now_ms() < D.scan_retry_ms)
		return;
	if (want) {
		/* LE only, our services: a dual-mode peer known over BR/EDR
		 * would otherwise be connected over BR/EDR */
		t = bd_plain(PK_FILTER);
		if (bd_call(&t, D.adapter, ADAPTER_IF, "SetDiscoveryFilter", 0, "a{sv}", 2, "Transport", "s", "le",
			 "UUIDs", "as", 3, "0000180d-0000-1000-8000-00805f9b34fb",
			 "00001818-0000-1000-8000-00805f9b34fb", "00001816-0000-1000-8000-00805f9b34fb") == 0)
			D.scan_inflight = true;
	} else {
		t = bd_plain(PK_STOPSCAN);
		if (D.powered && bd_call(&t, D.adapter, ADAPTER_IF, "StopDiscovery", 0, NULL) == 0) {
			D.scan_inflight = true;
		} else {
			D.scan_on = false;
			for (int i = 0; i < MAX_DEVS; i++)
				if (D.devs[i].used)
					bd_found_update(&D.devs[i], false);
			bd_adapter_event();
		}
	}
}

void bd_scan_reply(const struct pend *p, sd_bus_message *m)
{
	bool err = sd_bus_message_is_method_error(m, NULL);
	struct pend t;

	D.scan_inflight = false;
	if (p->kind == PK_FILTER) {
		t = bd_plain(PK_SCAN);
		if (err) {
			bd_notef("discovery filter: %s", bd_err_name(m));
			D.scan_retry_ms = bd_now_ms() + RETRY_MS;
		} else if (bd_call(&t, D.adapter, ADAPTER_IF, "StartDiscovery", 0, NULL) == 0) {
			D.scan_inflight = true;
		}
		return;
	}
	if (p->kind == PK_SCAN) {
		if (err) {
			bd_notef("discovery: %s", bd_err_name(m));
			D.scan_retry_ms = bd_now_ms() + RETRY_MS;
		} else {
			D.scan_on = true;
			bd_notef("scanning for sensors");
		}
	} else {
		if (err)
			vlog("StopDiscovery: %s", bd_err_name(m));
		D.scan_on = false;	/* an error means it is not ours to stop any more */
		bd_notef("scan stopped");
	}
	for (int i = 0; i < MAX_DEVS; i++)
		if (D.devs[i].used)
			bd_found_update(&D.devs[i], false);
	bd_adapter_event();
	bd_scan_sync();	/* the want may have changed meanwhile */
}
