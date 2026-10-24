/*
 * sensor-proxy's core: which kinds the bus claims, what is sent to sensord
 * for them, and what sensord's lines (proto 2, docs/features/sensors.md
 * "Socket contract") change. Pure: requests go out through ops.send and
 * property changes through ops.changed, so the tests drive it without a
 * socket or a bus. See main.c's header.
 *
 * Claims: the first claimant of a kind sends sensord's claim (accel at
 * 10 Hz, illuminance, proximity and heading at sensord's defaults), the
 * last one gone sends the release; while sensord is unreachable the
 * claims stay counted, and the connect sends them again after `list`.
 * When the last accelerometer claim ends, orientation and tilt go back to
 * undefined (as upstream); the light level, proximity and heading keep
 * their last value. A sample of a kind nobody claims (a late line after
 * the release) is ignored.
 *
 * Presence: `list` after each connect; Has* is a sensor's `present` in
 * the reply (compass: the heading entry). While a reply says SMGR is not
 * ready, `list` again every 5 s until one says it is. While sensord is
 * unreachable every Has* is false.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>

#include "common/json.h"
#include "sensor-proxy.h"

/* sensord's channel per kind, and the claim it gets */
static const char *const channels[SP_KINDS] = { "accel", "illuminance", "proximity", "heading" };
static const char *const claims[SP_KINDS] = { "claim accel 10\n", "claim illuminance\n", "claim proximity\n",
					      "claim heading\n" };
static const char *const kind_names[SP_KINDS] = { "accelerometer", "light", "proximity", "compass" };
static const enum sp_prop has_props[SP_KINDS] = { SP_HAS_ACCEL, SP_HAS_LIGHT, SP_HAS_PROX, SP_HAS_COMPASS };

const char *sp_kind_name(enum sp_kind k)
{
	return kind_names[k];
}

void sp_init(struct proxy *p, const struct sp_ops *ops)
{
	memset(p, 0, sizeof(*p));
	p->ops = *ops;
	p->heading = -1;
	p->relist_at = -1;
}

/* A request; claims and releases are logged, `list` is not. */
static void send_line(struct proxy *p, const char *line)
{
	if (p->ops.send(p->ops.ctx, line) == 0 && strcmp(line, "list\n") != 0)
		p->ops.log("sent %.*s", (int)strlen(line) - 1, line);
}

static void set_has(struct proxy *p, enum sp_kind k, bool v)
{
	if (p->has[k] == v)
		return;
	p->has[k] = v;
	p->ops.changed(p->ops.ctx, has_props[k]);
}

static void set_orientation(struct proxy *p, enum sp_orientation o, enum sp_tilt t)
{
	if (p->orientation != o) {
		p->orientation = o;
		p->ops.changed(p->ops.ctx, SP_ORIENTATION);
	}
	if (p->tilt != t) {
		p->tilt = t;
		p->ops.changed(p->ops.ctx, SP_TILT);
	}
}

void sp_claimants(struct proxy *p, enum sp_kind k, unsigned count)
{
	char line[32];

	if (count && !p->claimed[k]) {
		p->claimed[k] = true;
		if (p->up)
			send_line(p, claims[k]);
	} else if (!count && p->claimed[k]) {
		p->claimed[k] = false;
		if (p->up) {
			snprintf(line, sizeof(line), "release %s\n", channels[k]);
			send_line(p, line);
		}
		if (k == SP_ACCEL)
			set_orientation(p, SP_OR_UNDEFINED, SP_TILT_UNDEFINED);
	}
}

void sp_connected(struct proxy *p, int64_t now)
{
	int k;

	(void)now;
	p->up = true;
	p->ops.log("connected to sensord");
	send_line(p, "list\n");
	for (k = 0; k < SP_KINDS; k++)
		if (p->claimed[k])
			send_line(p, claims[k]);
}

void sp_lost(struct proxy *p)
{
	int k;

	p->up = false;
	p->relist_at = -1;
	p->ops.log("sensord connection lost; every sensor absent until it is back");
	for (k = 0; k < SP_KINDS; k++)
		set_has(p, (enum sp_kind)k, false);
}

static int kind_of(const char *channel)
{
	int k;

	for (k = 0; k < SP_KINDS; k++)
		if (strcmp(channel, channels[k]) == 0)
			return k;
	return -1;
}

/* A list reply: Has* from each entry's present; true when SMGR is ready. */
static bool on_list(struct proxy *p, const struct json_obj *o, int64_t now)
{
	const struct json_member *m = json_find(o, "sensors");
	bool present[SP_KINDS] = { false };
	char smgr[16] = "", name[32];
	struct json_span elem;
	enum json_type type;
	size_t pos = 0;
	bool ready;
	int k;

	json_get_string(o, "smgr", smgr, sizeof(smgr));
	ready = strcmp(smgr, "ready") == 0;
	while (m && m->type == JSON_ARRAY && json_array_next(m->val, &pos, &type, &elem)) {
		struct json_obj e;
		bool v;

		if (type != JSON_OBJECT || json_parse(&e, elem.p, elem.len) ||
		    !json_get_string(&e, "sensor", name, sizeof(name)))
			continue;
		k = kind_of(name);
		if (k >= 0 && json_get_bool(&e, "present", &v))
			present[k] = v;
	}
	for (k = 0; k < SP_KINDS; k++)
		set_has(p, (enum sp_kind)k, present[k]);
	p->relist_at = ready ? -1 : now + SP_RELIST_MS;
	return ready;
}

static void on_sample(struct proxy *p, const struct json_obj *o)
{
	char name[32];
	double x, y, z, v;
	long n;
	int k;

	if (!json_get_string(o, "sensor", name, sizeof(name)) || (k = kind_of(name)) < 0 || !p->claimed[k])
		return;
	switch (k) {
	case SP_ACCEL:
		if (json_get_double(o, "x", &x) && json_get_double(o, "y", &y) && json_get_double(o, "z", &z))
			set_orientation(p, sp_orientation_calc(p->orientation, x, y, z), sp_tilt_calc(x, y, z));
		break;
	case SP_LIGHT:
		if (json_get_double(o, "illuminance", &v) && v != p->level) {
			p->level = v;
			p->ops.changed(p->ops.ctx, SP_LIGHT_LEVEL);
		}
		break;
	case SP_PROX:
		if (json_get_long(o, "near", &n) && (n != 0) != p->near) {
			p->near = n != 0;
			p->ops.changed(p->ops.ctx, SP_PROX_NEAR);
		}
		break;
	case SP_COMPASS:
		if (json_get_double(o, "heading", &v) && v != p->heading) {
			p->heading = v;
			p->ops.changed(p->ops.ctx, SP_HEADING);
		}
		break;
	}
}

bool sp_line(struct proxy *p, const char *line, size_t len, int64_t now)
{
	struct json_obj o;
	char type[16], reason[64] = "", sensor[32] = "", cmd[16] = "";
	long proto;

	if (json_parse(&o, line, len) || !json_get_string(&o, "type", type, sizeof(type)))
		return false;
	if (strcmp(type, "sample") == 0) {
		on_sample(p, &o);
	} else if (strcmp(type, "list") == 0) {
		return on_list(p, &o, now);
	} else if (strcmp(type, "hello") == 0) {
		if ((!json_get_long(&o, "proto", &proto) || proto != SP_SENSORD_PROTO) && !p->proto_warned) {
			p->ops.log("sensord speaks another protocol than %d; carrying on", SP_SENSORD_PROTO);
			p->proto_warned = true;
		}
	} else if (strcmp(type, "error") == 0) {
		/* the property keeps its value */
		json_get_string(&o, "reason", reason, sizeof(reason));
		json_get_string(&o, "sensor", sensor, sizeof(sensor));
		json_get_string(&o, "cmd", cmd, sizeof(cmd));
		p->ops.log("sensord: %s%s%s%s%s", cmd, cmd[0] ? ": " : "", reason, sensor[0] ? " " : "", sensor);
	}
	/* ok, dropped and unknown types: nothing */
	return false;
}

int64_t sp_deadline(const struct proxy *p)
{
	return p->up ? p->relist_at : -1;
}

void sp_tick(struct proxy *p, int64_t now)
{
	if (!p->up || p->relist_at < 0 || now < p->relist_at)
		return;
	p->relist_at = -1;	/* the reply sets the next one */
	send_line(p, "list\n");
}
