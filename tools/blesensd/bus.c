/*
 * blesensd's side of the bus: the asynchronous calls to BlueZ and their
 * identities, the object cache, the signals, the pairing agent and the
 * bus life (connect, GetManagedObjects, bluetoothd coming and going).
 * See main.c.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blesensd.h"

/* ------------------------------------------------------------ bus calls */

static int on_reply(sd_bus_message *m, void *u, sd_bus_error *e);

static struct pend *pend_new(enum pk kind)
{
	for (int i = 0; i < MAX_PENDS; i++)
		if (!D.pends[i].used) {
			memset(&D.pends[i], 0, sizeof(D.pends[i]));
			D.pends[i].used = true;
			D.pends[i].kind = kind;
			return &D.pends[i];
		}
	return NULL;
}

static void pend_free(struct pend *p)
{
	p->slot = sd_bus_slot_unref(p->slot);	/* cancels the call if still pending */
	p->used = false;
}

/* Cancel the calls of a sensor (sid; its removal only with forget too),
 * of the pairing (pair) or all. */
void bd_pend_cancel(bool all, uint32_t sid, uint32_t pair, bool forget)
{
	for (int i = 0; i < MAX_PENDS; i++) {
		struct pend *p = &D.pends[i];

		if (p->used && (all || (sid && p->sid == sid && (forget || p->kind != PK_FORGET)) ||
				(pair && p->pair == pair)))
			pend_free(p);
	}
}

/*
 * An asynchronous call to BlueZ; the reply goes to on_reply with a copy of
 * *tmpl. types and the arguments follow sd_bus_message_append. 0 or -errno.
 */
static int callv(const struct pend *tmpl, const char *path, const char *iface, const char *member,
		 uint64_t usec, const char *types, va_list ap)
{
	sd_bus_message *m = NULL;
	struct pend *p;
	int r;

	if (!D.bus)
		return -ENOTCONN;
	if (!(p = pend_new(tmpl->kind))) {
		chef_log("too many calls in flight: %s not sent", member);
		return -EBUSY;
	}
	p->sid = tmpl->sid;
	p->epoch = tmpl->epoch;
	p->pair = tmpl->pair;
	p->k = tmpl->k;
	r = sd_bus_message_new_method_call(D.bus, &m, BLUEZ, path, iface, member);
	if (r >= 0)
		r = sd_bus_message_set_auto_start(m, 0);
	if (r >= 0 && types)
		r = sd_bus_message_appendv(m, types, ap);
	if (r >= 0)
		r = sd_bus_call_async(D.bus, &p->slot, m, on_reply, p, usec);
	sd_bus_message_unref(m);
	if (r < 0)
		pend_free(p);
	return r < 0 ? r : 0;
}

int bd_call(const struct pend *tmpl, const char *path, const char *iface, const char *member,
	    uint64_t usec, const char *types, ...)
{
	va_list ap;
	int r;

	va_start(ap, types);
	r = callv(tmpl, path, iface, member, usec, types, ap);
	va_end(ap);
	return r;
}

struct pend bd_plain(enum pk kind)
{
	return (struct pend){ .kind = kind };
}

struct pend bd_for_sensor(enum pk kind, const struct sensor *s, int k)
{
	return (struct pend){ .kind = kind, .sid = s->sid, .epoch = s->epoch, .k = k };
}

struct pend bd_for_pair(enum pk kind)
{
	return (struct pend){ .kind = kind, .pair = D.pr.id };
}

const char *bd_err_name(sd_bus_message *m)
{
	const sd_bus_error *e = sd_bus_message_get_error(m);

	return e && e->name ? e->name : "error";
}

bool bd_err_is(sd_bus_message *m, const char *name)
{
	return sd_bus_message_is_method_error(m, name) > 0;
}

/* ------------------------------------------------------------ object cache */

static struct dev *dev_get(const char *path)
{
	struct dev *d = bd_dev_by_path(path), *free_d = NULL;

	if (d)
		return d;
	for (int i = 0; i < MAX_DEVS && !free_d; i++)
		if (!D.devs[i].used)
			free_d = &D.devs[i];
	if (!free_d) {
		if (!D.devs_full_logged)
			chef_log("more than %d Bluetooth devices: the rest are ignored", MAX_DEVS);
		D.devs_full_logged = true;
		return NULL;
	}
	memset(free_d, 0, sizeof(*free_d));
	free_d->used = true;
	free_d->battery = -1;
	snprintf(free_d->path, sizeof(free_d->path), "%s", path);
	return free_d;
}

/* Read a variant of basic type t; skip it if it is another type.
 * 1 read, 0 skipped, < 0 malformed. */
static int vread(sd_bus_message *m, char t, void *p)
{
	const char *contents = NULL;
	char type, sig[2] = { t, 0 };
	int r = sd_bus_message_peek_type(m, &type, &contents);

	if (r <= 0)
		return r < 0 ? r : -ENXIO;
	if (type != 'v' || !contents || strcmp(contents, sig))
		return sd_bus_message_skip(m, "v") < 0 ? -EBADMSG : 0;
	if ((r = sd_bus_message_enter_container(m, 'v', sig)) <= 0 ||
	    (r = sd_bus_message_read_basic(m, t, p)) <= 0)
		return r < 0 ? r : -ENXIO;
	return sd_bus_message_exit_container(m) < 0 ? -EBADMSG : 1;
}

static int vread_bool(sd_bus_message *m, bool *out)
{
	int v = 0, r = vread(m, 'b', &v);

	if (r > 0)
		*out = v;
	return r;
}

static int vread_str(sd_bus_message *m, char *out, size_t n)
{
	const char *s = NULL;
	int r = vread(m, 's', &s);

	if (r > 0)
		snprintf(out, n, "%s", s ? s : "");
	return r;
}

/* A variant "as": UUIDs (the sensor kinds among them, into *kinds) or
 * characteristic flags (into c). */
static int vread_strs(sd_bus_message *m, unsigned *kinds, struct chr *c)
{
	const char *contents = NULL, *s;
	char type;
	int r = sd_bus_message_peek_type(m, &type, &contents);

	if (r <= 0)
		return r < 0 ? r : -ENXIO;
	if (type != 'v' || !contents || strcmp(contents, "as"))
		return sd_bus_message_skip(m, "v") < 0 ? -EBADMSG : 0;
	if (sd_bus_message_enter_container(m, 'v', "as") <= 0 ||
	    sd_bus_message_enter_container(m, 'a', "s") < 0)
		return -EBADMSG;
	if (kinds)
		*kinds = 0;
	if (c)
		c->can_notify = c->can_read = false;
	while ((r = sd_bus_message_read_basic(m, 's', &s)) > 0) {
		if (kinds) {
			int k = bs_kind_of_service(bs_uuid16(s));

			if (k >= 0)
				*kinds |= 1u << k;
		} else if (c && (!strcmp(s, "notify") || !strcmp(s, "indicate"))) {
			c->can_notify = true;
		} else if (c && !strcmp(s, "read")) {
			c->can_read = true;
		}
	}
	if (r < 0 || sd_bus_message_exit_container(m) < 0 || sd_bus_message_exit_container(m) < 0)
		return -EBADMSG;
	return 1;
}


/* A variant "ay": a characteristic Value. Only a live notification
 * (PropertiesChanged) is handed on; a snapshot's cached value is not. */
static int vread_value(sd_bus_message *m, struct chr *c, bool live)
{
	const char *contents = NULL;
	const void *p = NULL;
	size_t n = 0;
	char type;
	int r = sd_bus_message_peek_type(m, &type, &contents);

	if (r <= 0)
		return r < 0 ? r : -ENXIO;
	if (type != 'v' || !contents || strcmp(contents, "ay"))
		return sd_bus_message_skip(m, "v") < 0 ? -EBADMSG : 0;
	if (sd_bus_message_enter_container(m, 'v', "ay") <= 0 ||
	    sd_bus_message_read_array(m, 'y', &p, &n) < 0 || sd_bus_message_exit_container(m) < 0)
		return -EBADMSG;
	if (c && live)
		bd_value_bytes(c, p, n);
	return 1;
}

/*
 * One interface's a{sv} for the object at path. live: from a
 * PropertiesChanged signal (else a snapshot: GetManagedObjects or
 * InterfacesAdded). Characteristics are kept only when they are one of
 * ours, which a snapshot tells by its UUID; a signal only updates a kept one.
 */
static int read_iface(sd_bus_message *m, const char *path, const char *iface, bool live)
{
	struct dev *d = NULL;
	struct chr *c = NULL, tmp;
	bool is_adapter = !strcmp(iface, ADAPTER_IF), is_chr = !strcmp(iface, CHR_IF);
	int r = sd_bus_message_enter_container(m, 'a', "{sv}");

	if (r <= 0)
		return r < 0 ? r : -ENXIO;
	if (!strcmp(iface, DEVICE_IF) || !strcmp(iface, BATTERY_IF))
		d = live ? bd_dev_by_path(path) : dev_get(path);
	else if (is_chr) {
		c = bd_chr_by_path(path);
		if (!c && !live) {
			memset(&tmp, 0, sizeof(tmp));
			snprintf(tmp.path, sizeof(tmp.path), "%s", path);
			bd_dev_of_path(path, tmp.dev, sizeof(tmp.dev));
			c = &tmp;
		}
	} else if (is_adapter && !D.adapter[0] && !live)
		snprintf(D.adapter, sizeof(D.adapter), "%s", path);
	if (!strcmp(iface, BATTERY_IF) && d && !live)
		d->battery = -1;
	while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
		const char *k = NULL;

		if (sd_bus_message_read_basic(m, 's', &k) <= 0)
			return -EBADMSG;
		if (d && !strcmp(iface, BATTERY_IF) && !strcmp(k, "Percentage")) {
			uint8_t v = 0;

			r = vread(m, 'y', &v);
			if (r > 0)
				d->battery = v <= 100 ? v : -1;
		} else if (d && !strcmp(iface, BATTERY_IF)) {
			r = sd_bus_message_skip(m, "v");
		} else if (d && !strcmp(k, "Address"))
			r = vread_str(m, d->addr, sizeof(d->addr));
		else if (d && !strcmp(k, "Alias")) {
			char raw[64];

			r = vread_str(m, raw, sizeof(raw));
			if (r > 0)
				bd_name_clean(d->name, sizeof(d->name), raw);
		} else if (d && !strcmp(k, "Connected"))
			r = vread_bool(m, &d->connected);
		else if (d && !strcmp(k, "ServicesResolved"))
			r = vread_bool(m, &d->resolved);
		else if (d && !strcmp(k, "Paired"))
			r = vread_bool(m, &d->paired);
		else if (d && !strcmp(k, "Trusted"))
			r = vread_bool(m, &d->trusted);
		else if (d && !strcmp(k, "UUIDs"))
			r = vread_strs(m, &d->kinds, NULL);
		else if (d && !strcmp(k, "RSSI")) {
			int16_t v = 0;

			r = vread(m, 'n', &v);
			if (r > 0) {
				struct sensor *s = bd_sensor_by_addr(d->addr);

				d->rssi = v;
				d->has_rssi = true;
				if (s) {
					s->rssi = v;
					s->has_rssi = true;
				}
			}
		} else if (c && !strcmp(k, "UUID")) {
			char u[40];

			r = vread_str(m, u, sizeof(u));
			if (r > 0 && c == &tmp)
				c->uuid16 = bs_uuid16(u);
		} else if (c && !strcmp(k, "Flags"))
			r = vread_strs(m, NULL, c);
		else if (c && !strcmp(k, "Value"))
			r = vread_value(m, c, live);
		else if (is_adapter && !strcmp(path, D.adapter) && !strcmp(k, "Powered"))
			r = vread_bool(m, &D.powered);
		else if (is_adapter && !strcmp(path, D.adapter) && !strcmp(k, "Discovering"))
			r = vread_bool(m, &D.discovering);
		else
			r = sd_bus_message_skip(m, "v");
		if (r < 0 || sd_bus_message_exit_container(m) < 0)
			return -EBADMSG;
	}
	if (r < 0)
		return r;
	if (sd_bus_message_exit_container(m) < 0)
		return -EBADMSG;
	if (c == &tmp && bd_is_sensor_chr(tmp.uuid16)) {
		for (int i = 0; i < MAX_CHRS; i++)
			if (!D.chrs[i].used) {
				D.chrs[i] = tmp;
				D.chrs[i].used = true;
				break;
			}
	}
	/* the address is the same on every name ours could match: keep the
	 * sensor's last RSSI in step with a device a snapshot brought */
	if (d && d->addr[0] && d->has_rssi) {
		struct sensor *s = bd_sensor_by_addr(d->addr);

		if (s) {
			s->rssi = d->rssi;
			s->has_rssi = true;
		}
	}
	return 1;
}

/* PropertiesChanged's invalidated list. */
static void read_invalidated(sd_bus_message *m, const char *iface, struct dev *d)
{
	const char *k;

	if (sd_bus_message_enter_container(m, 'a', "s") <= 0)
		return;
	while (sd_bus_message_read_basic(m, 's', &k) > 0) {
		if (d && !strcmp(iface, DEVICE_IF) && !strcmp(k, "RSSI"))
			d->has_rssi = false;
		else if (d && !strcmp(iface, BATTERY_IF) && !strcmp(k, "Percentage"))
			d->battery = -1;
	}
	sd_bus_message_exit_container(m);
}


/* ------------------------------------------------------------ signals */

/* After any change in BlueZ's objects: connections lost, found devices,
 * the pairing, the scan, every sensor event. */
static void devices_changed(void)
{
	for (int i = 0; i < MAX_SENSORS; i++) {
		struct sensor *s = &D.sensors[i];
		struct dev *d = s->used ? bd_dev_by_addr(s->addr) : NULL;

		if (!s->used)
			continue;
		/* a connection that went (or lost its services) ends ours */
		if ((s->was_connected && (!d || !d->connected || !D.powered)) ||
		    (s->was_resolved && (!d || !d->resolved)))
			bd_sensor_conn_reset(s);
		if (s->link_up != (d && d->connected && D.powered)) {
			s->link_up = !s->link_up;
			chef_log("%s %s", s->addr, s->link_up ? "connected" : "disconnected");
			if (!s->link_up && D.powered && !s->forgetting)
				bd_retry_window();	/* it may be back in a moment */
		}
		bd_sensor_event(s, false);
	}
	for (int i = 0; i < MAX_DEVS; i++)
		if (D.devs[i].used)
			bd_found_update(&D.devs[i], false);
	bd_adapter_event();
	bd_pair_check();
	bd_scan_sync();
}

static int props_signal(sd_bus_message *m, void *u, sd_bus_error *e)
{
	const char *iface = NULL, *path = sd_bus_message_get_path(m);
	bool was_powered = D.powered;
	int old_rssi = 0;
	bool old_has = false;
	struct dev *d;

	(void)u;
	(void)e;
	if (!path || sd_bus_message_read_basic(m, 's', &iface) <= 0 || !iface)
		return 0;
	if (strcmp(iface, DEVICE_IF) && strcmp(iface, CHR_IF) && strcmp(iface, ADAPTER_IF) &&
	    strcmp(iface, BATTERY_IF))
		return 0;
	d = bd_dev_by_path(path);
	if (d) {
		old_rssi = d->rssi;
		old_has = d->has_rssi;
	}
	if (read_iface(m, path, iface, true) < 0)
		return 0;
	read_invalidated(m, iface, d);
	if (was_powered != D.powered) {
		chef_log("adapter %s", D.powered ? "on" : "off");
		if (D.powered)
			bd_retry_window();
		if (!D.powered) {
			bd_pair_fail("bluetooth off");
			D.scan_on = false;	/* BlueZ ended every discovery */
		}
	}
	if (d && (d->rssi != old_rssi || d->has_rssi != old_has))
		bd_found_update(d, true);
	devices_changed();
	return 0;
}

static int added_signal(sd_bus_message *m, void *u, sd_bus_error *e)
{
	const char *path = NULL;
	int r;

	(void)u;
	(void)e;
	if (sd_bus_message_read_basic(m, 'o', &path) <= 0 || !path ||
	    sd_bus_message_enter_container(m, 'a', "{sa{sv}}") <= 0)
		return 0;
	while ((r = sd_bus_message_enter_container(m, 'e', "sa{sv}")) > 0) {
		const char *iface = NULL;

		if (sd_bus_message_read_basic(m, 's', &iface) <= 0)
			return 0;
		if (iface && (!strcmp(iface, DEVICE_IF) || !strcmp(iface, CHR_IF) || !strcmp(iface, ADAPTER_IF) ||
			      !strcmp(iface, BATTERY_IF)))
			r = read_iface(m, path, iface, false);
		else
			r = sd_bus_message_skip(m, "a{sv}");
		if (r < 0 || sd_bus_message_exit_container(m) < 0)
			return 0;
	}
	devices_changed();
	return 0;
}

static int removed_signal(sd_bus_message *m, void *u, sd_bus_error *e)
{
	const char *path = NULL, *iface;
	struct dev *d;

	(void)u;
	(void)e;
	if (sd_bus_message_read_basic(m, 'o', &path) <= 0 || !path ||
	    sd_bus_message_enter_container(m, 'a', "s") <= 0)
		return 0;
	while (sd_bus_message_read_basic(m, 's', &iface) > 0) {
		if (!strcmp(iface, DEVICE_IF) && (d = bd_dev_by_path(path))) {
			d->connected = d->resolved = false;
			d->has_rssi = false;
			bd_found_update(d, false);
			for (int i = 0; i < MAX_CHRS; i++)
				if (D.chrs[i].used && !strcmp(D.chrs[i].dev, path))
					D.chrs[i].used = false;
			d->used = false;
		} else if (!strcmp(iface, BATTERY_IF) && (d = bd_dev_by_path(path))) {
			d->battery = -1;
		} else if (!strcmp(iface, CHR_IF)) {
			struct chr *c = bd_chr_by_path(path);
			char dev[128];

			if (c) {
				/* our session on it is gone with it */
				snprintf(dev, sizeof(dev), "%s", c->dev);
				c->used = false;
				d = bd_dev_by_path(dev);
				for (int i = 0; d && i < MAX_SENSORS; i++)
					if (D.sensors[i].used && !D.sensors[i].forgetting &&
					    !strcmp(D.sensors[i].addr, d->addr) &&
					    (D.sensors[i].was_connected || D.sensors[i].was_resolved))
						bd_sensor_conn_reset(&D.sensors[i]);
			}
		} else if (!strcmp(iface, ADAPTER_IF) && !strcmp(path, D.adapter)) {
			D.adapter[0] = 0;
			D.powered = false;
			D.scan_on = false;
			bd_pair_fail("adapter gone");
		}
	}
	devices_changed();
	return 0;
}

/* ------------------------------------------------------------ the agent */

/* Only BlueZ, and only about the device being paired (or, for services,
 * a saved sensor); anything else is rejected. */
static int agent_call(sd_bus_message *m, void *u, sd_bus_error *e)
{
	const char *member = sd_bus_message_get_member(m), *sender = sd_bus_message_get_sender(m);
	const char *dev = NULL;
	bool ok = false;

	(void)u;
	if (!sender || !D.bluez_owner[0] || strcmp(sender, D.bluez_owner))
		return sd_bus_error_set(e, BLUEZ ".Error.Rejected", "not from bluetoothd");
	if (!strcmp(member, "Release") || !strcmp(member, "Cancel"))
		return sd_bus_reply_method_return(m, NULL);
	if (sd_bus_message_read_basic(m, 'o', &dev) <= 0 || !dev)
		return sd_bus_error_set(e, BLUEZ ".Error.Rejected", "no device");
	if (!strcmp(member, "RequestConfirmation") || !strcmp(member, "RequestAuthorization") ||
	    !strcmp(member, "DisplayPasskey") || !strcmp(member, "DisplayPinCode"))
		ok = D.pr.step != PS_IDLE && !strcmp(dev, D.pr.path);
	else if (!strcmp(member, "AuthorizeService")) {
		struct dev *d = bd_dev_by_path(dev);
		struct sensor *s = d ? bd_sensor_by_addr(d->addr) : NULL;

		ok = (D.pr.step != PS_IDLE && !strcmp(dev, D.pr.path)) || (s && !s->forgetting);
	}
	chef_log("agent %s for %s: %s", member, dev, ok ? "accepted" : "rejected");
	if (!ok)
		return sd_bus_error_set(e, BLUEZ ".Error.Rejected", "not a sensor being paired");
	return sd_bus_reply_method_return(m, NULL);
}

static int agent_reject(sd_bus_message *m, void *u, sd_bus_error *e)
{
	(void)m;
	(void)u;
	return sd_bus_error_set(e, BLUEZ ".Error.Rejected", "no input or output");
}

static const sd_bus_vtable agent_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("Release", "", "", agent_call, 0),
	SD_BUS_METHOD("RequestPinCode", "o", "s", agent_reject, 0),
	SD_BUS_METHOD("DisplayPinCode", "os", "", agent_call, 0),
	SD_BUS_METHOD("RequestPasskey", "o", "u", agent_reject, 0),
	SD_BUS_METHOD("DisplayPasskey", "ouq", "", agent_call, 0),
	SD_BUS_METHOD("RequestConfirmation", "ou", "", agent_call, 0),
	SD_BUS_METHOD("RequestAuthorization", "o", "", agent_call, 0),
	SD_BUS_METHOD("AuthorizeService", "os", "", agent_call, 0),
	SD_BUS_METHOD("Cancel", "", "", agent_call, 0),
	SD_BUS_VTABLE_END
};

void bd_agent_release_drive(int64_t now)
{
	struct pend t = bd_plain(PK_AGENT_RELEASE);

	if (!D.bluez || !D.agent_releasing || D.agent_release_inflight || now < D.agent_retry_ms)
		return;
	if (bd_call(&t, "/org/bluez", BLUEZ ".AgentManager1", "UnregisterAgent", 0, "o", AGENT_PATH) == 0)
		D.agent_release_inflight = true;
	else
		D.agent_retry_ms = now + RETRY_MS;
}

void bd_agent_register(int64_t now)
{
	struct pend t = bd_plain(PK_AGENT);

	if (D.agent_releasing) {
		bd_agent_release_drive(now);
		return;
	}
	if (!D.bluez || D.agent_registered || D.agent_inflight || now < D.agent_retry_ms)
		return;
	if (bd_call(&t, "/org/bluez", BLUEZ ".AgentManager1", "RegisterAgent", 0, "os", AGENT_PATH,
		 "NoInputNoOutput") == 0)
		D.agent_inflight = true;
}

static void agent_reply(sd_bus_message *m)
{
	D.agent_inflight = false;
	if (!sd_bus_message_is_method_error(m, NULL) || bd_err_is(m, BLUEZ ".Error.AlreadyExists")) {
		D.agent_registered = true;
		vlog("agent registered");
		bd_pair_check();
	} else {
		chef_log("RegisterAgent: %s; retrying", bd_err_name(m));
		D.agent_retry_ms = bd_now_ms() + RETRY_MS;
	}
}

/* ------------------------------------------------------------ bus life */

static void objects_reply(sd_bus_message *m)
{
	const char *sender = sd_bus_message_get_sender(m);
	int r;

	if (sd_bus_message_is_method_error(m, NULL)) {
		vlog("GetManagedObjects: %s", bd_err_name(m));
		return;	/* bluetoothd not there yet: NameOwnerChanged brings it */
	}
	memset(D.devs, 0, sizeof(D.devs));
	memset(D.chrs, 0, sizeof(D.chrs));
	D.adapter[0] = 0;
	D.powered = false;
	snprintf(D.bluez_owner, sizeof(D.bluez_owner), "%s", sender ? sender : "");
	if (sd_bus_message_enter_container(m, 'a', "{oa{sa{sv}}}") <= 0)
		return;
	while ((r = sd_bus_message_enter_container(m, 'e', "oa{sa{sv}}")) > 0) {
		const char *path = NULL;

		if (sd_bus_message_read_basic(m, 'o', &path) <= 0 ||
		    sd_bus_message_enter_container(m, 'a', "{sa{sv}}") <= 0)
			return;
		while ((r = sd_bus_message_enter_container(m, 'e', "sa{sv}")) > 0) {
			const char *iface = NULL;

			if (sd_bus_message_read_basic(m, 's', &iface) <= 0)
				return;
			if (!strcmp(iface, DEVICE_IF) || !strcmp(iface, CHR_IF) || !strcmp(iface, ADAPTER_IF) ||
			    !strcmp(iface, BATTERY_IF))
				r = read_iface(m, path, iface, false);
			else
				r = sd_bus_message_skip(m, "a{sv}");
			if (r < 0 || sd_bus_message_exit_container(m) < 0)
				return;
		}
		if (sd_bus_message_exit_container(m) < 0 || sd_bus_message_exit_container(m) < 0)
			return;
	}
	D.bluez = true;
	chef_log("bluetoothd: adapter %s, %s", D.adapter[0] ? D.adapter : "none", D.powered ? "on" : "off");
	bd_retry_window();		/* also this daemon's start */
	bd_agent_register(bd_now_ms());
	devices_changed();
}

/* bluetoothd went or came back: nothing of the old one is ours any more. */
static void bluez_reset(void)
{
	bd_pair_fail("bluetoothd gone");
	bd_pend_cancel(true, 0, 0, true);
	D.bluez = false;
	D.bluez_owner[0] = 0;
	D.agent_registered = D.agent_inflight = false;
	D.agent_releasing = D.agent_release_inflight = false;
	D.agent_retry_ms = 0;
	D.adapter[0] = 0;
	D.powered = D.discovering = D.scan_on = D.scan_inflight = false;
	D.scan_retry_ms = 0;
	for (int i = 0; i < MAX_DEVS; i++)
		if (D.devs[i].used && D.devs[i].found_sent) {
			D.devs[i].has_rssi = false;
			bd_found_update(&D.devs[i], false);
		}
	memset(D.devs, 0, sizeof(D.devs));
	memset(D.chrs, 0, sizeof(D.chrs));
	for (int i = 0; i < MAX_SENSORS; i++)
		if (D.sensors[i].used) {
			bd_sensor_conn_reset(&D.sensors[i]);
			D.sensors[i].next_try_ms = 0;
			D.sensors[i].backoff_ms = BACKOFF_MIN_MS;
		}
}

static void bluez_refresh(void)
{
	struct pend t = bd_plain(PK_OBJECTS);

	bd_call(&t, "/", OBJMGR, "GetManagedObjects", 0, NULL);
}

static int owner_signal(sd_bus_message *m, void *u, sd_bus_error *e)
{
	const char *name, *old, *nw;

	(void)u;
	(void)e;
	if (sd_bus_message_read(m, "sss", &name, &old, &nw) < 0 || strcmp(name, BLUEZ))
		return 0;
	bluez_reset();
	if (nw && *nw) {
		chef_log("bluetoothd (re)started");
		bluez_refresh();
	} else {
		chef_log("bluetoothd gone");
	}
	bd_adapter_event();
	for (int i = 0; i < MAX_SENSORS; i++)
		if (D.sensors[i].used)
			bd_sensor_event(&D.sensors[i], false);
	return 0;
}

static int on_reply(sd_bus_message *m, void *u, sd_bus_error *e)
{
	struct pend *pp = u, p = *pp;

	(void)e;
	pend_free(pp);
	switch (p.kind) {
	case PK_OBJECTS:
		objects_reply(m);
		break;
	case PK_AGENT:
		agent_reply(m);
		break;
	case PK_AGENT_RELEASE:
		D.agent_release_inflight = false;
		if (!sd_bus_message_is_method_error(m, NULL) || bd_err_is(m, BLUEZ ".Error.DoesNotExist")) {
			D.agent_releasing = false;
			D.agent_retry_ms = 0;
			bd_agent_register(bd_now_ms());
		} else {
			chef_log("pairing agent release: %s; retrying", bd_err_name(m));
			D.agent_retry_ms = bd_now_ms() + RETRY_MS;
		}
		break;
	case PK_FILTER:
	case PK_SCAN:
	case PK_STOPSCAN:
		bd_scan_reply(&p, m);
		break;
	case PK_CONNECT:
	case PK_NOTIFY:
	case PK_BATTERY:
	case PK_BATTERY_NOTIFY:
		bd_sensor_reply(&p, m);
		break;
	case PK_FORGET:
		bd_forget_reply(&p, m);
		break;
	case PK_PAIR_AGENT:
	case PK_PAIR_CONNECT:
	case PK_PAIR_NOTIFY:
	case PK_PAIR_BOND:
	case PK_PAIR_TRUST:
		bd_pair_reply(&p, m);
		break;
	case PK_PLAIN:
		if (sd_bus_message_is_method_error(m, NULL))
			vlog("%s", bd_err_name(m));
		break;
	}
	return 0;
}

void bd_bus_open(void)
{
	static const char *const rules[] = {
		"type='signal',sender='" BLUEZ "',interface='" PROPS "',member='PropertiesChanged'",
		"type='signal',sender='" BLUEZ "',interface='" OBJMGR "',member='InterfacesAdded'",
		"type='signal',sender='" BLUEZ "',interface='" OBJMGR "',member='InterfacesRemoved'",
		"type='signal',sender='org.freedesktop.DBus',path='/org/freedesktop/DBus',"
		"interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='" BLUEZ "'",
	};
	static sd_bus_message_handler_t cbs[] = { props_signal, added_signal, removed_signal, owner_signal };
	int r;

	r = sd_bus_open_system(&D.bus);
	if (r < 0) {
		D.bus = NULL;
		bd_bus_lost("unavailable");
		return;
	}
	D.bus_fd = sd_bus_get_fd(D.bus);
	for (unsigned i = 0; i < 4; i++) {
		r = sd_bus_add_match(D.bus, &D.slots[D.n_slots], rules[i], cbs[i], NULL);
		if (r < 0) {
			chef_log("AddMatch: %s", strerror(-r));
			bd_bus_lost("refused a match");
			return;
		}
		D.n_slots++;
	}
	r = sd_bus_add_object_vtable(D.bus, &D.agent_slot, AGENT_PATH, BLUEZ ".Agent1", agent_vtable, NULL);
	if (r < 0)
		chef_log("agent object: %s", strerror(-r));
	D.bus_backoff_ms = 1000 / bd_scale;
	bluez_refresh();
}

void bd_bus_lost(const char *why)
{
	chef_log("system bus %s: retrying", why);
	bluez_reset();
	for (int i = 0; i < D.n_slots; i++)
		sd_bus_slot_unref(D.slots[i]);
	D.n_slots = 0;
	D.agent_slot = sd_bus_slot_unref(D.agent_slot);
	if (D.bus) {
		sd_bus_close(D.bus);
		sd_bus_unref(D.bus);
	}
	D.bus = NULL;
	D.bus_fd = -1;
	if (D.bus_backoff_ms <= 0)
		D.bus_backoff_ms = 1000 / bd_scale;
	D.bus_retry_ms = bd_now_ms() + D.bus_backoff_ms;
	D.bus_backoff_ms = D.bus_backoff_ms * 2 > BUS_BACKOFF_MAX_MS ? BUS_BACKOFF_MAX_MS : D.bus_backoff_ms * 2;
	bd_adapter_event();
	for (int i = 0; i < MAX_SENSORS; i++)
		if (D.sensors[i].used)
			bd_sensor_event(&D.sensors[i], false);
}
