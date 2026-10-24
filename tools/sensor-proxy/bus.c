/*
 * sensor-proxy's side of the system bus: the name net.hadess.SensorProxy,
 * its two objects with exactly iio-sensor-proxy 3.9's interfaces
 * (net.hadess.SensorProxy.xml), one claim track per kind and the
 * PropertiesChanged emits. Introspectable and Peer come from sd-bus.
 * See main.c's header.
 *
 *   /net/hadess/SensorProxy           net.hadess.SensorProxy
 *     HasAccelerometer b, AccelerometerOrientation s, AccelerometerTilt s,
 *     HasAmbientLight b, LightLevelUnit s ("lux"), LightLevel d,
 *     HasProximity b, ProximityNear b;
 *     ClaimAccelerometer, ReleaseAccelerometer, ClaimLight, ReleaseLight,
 *     ClaimProximity, ReleaseProximity (no arguments, empty replies)
 *   /net/hadess/SensorProxy/Compass   net.hadess.SensorProxy.Compass
 *     HasCompass b, CompassHeading d (magnetic degrees 0..360, -1 before
 *     the first sample); ClaimCompass, ReleaseCompass
 *
 * Every property is EMITS_CHANGE and is emitted only when its value
 * changes (proxy.c decides). A claim is per bus name: each kind has a
 * non-recursive sd_bus_track, so a second claim from the same name is a
 * no-op and one release ends it, a release from a name without a claim is
 * a no-op, and a name that leaves the bus (kill -9 included) drops its
 * claims through the track's handler. Claiming works before the sensor
 * exists. No polkit: the bus policy decides who may call.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#if defined(CHEF_BASU)
#include <basu/sd-bus.h>
#else
#include <systemd/sd-bus.h>
#endif

#include "sensor-proxy.h"

#define BUS_NAME     "net.hadess.SensorProxy"
#define PATH         "/net/hadess/SensorProxy"
#define IFACE        "net.hadess.SensorProxy"
#define COMPASS_PATH "/net/hadess/SensorProxy/Compass"
#define COMPASS_IFACE "net.hadess.SensorProxy.Compass"

static struct {
	sd_bus *bus;
	sd_bus_slot *slots[2];
	sd_bus_track *track[SP_KINDS];
	struct proxy *p;
} B;

/* Property name per enum sp_prop, and whether it is on the compass object. */
static const char *const prop_names[SP_PROPS] = {
	"HasAccelerometer", "AccelerometerOrientation", "AccelerometerTilt", "HasAmbientLight",
	"LightLevel", "HasProximity", "ProximityNear", "HasCompass", "CompassHeading",
};

static int get_bool(sd_bus *bus, const char *path, const char *iface, const char *prop,
		    sd_bus_message *reply, void *u, sd_bus_error *e)
{
	struct proxy *p = B.p;
	int v = 0;

	(void)bus, (void)path, (void)iface, (void)u, (void)e;
	if (strcmp(prop, "HasAccelerometer") == 0)
		v = p->has[SP_ACCEL];
	else if (strcmp(prop, "HasAmbientLight") == 0)
		v = p->has[SP_LIGHT];
	else if (strcmp(prop, "HasProximity") == 0)
		v = p->has[SP_PROX];
	else if (strcmp(prop, "ProximityNear") == 0)
		v = p->near;
	else if (strcmp(prop, "HasCompass") == 0)
		v = p->has[SP_COMPASS];
	return sd_bus_message_append(reply, "b", v);
}

static int get_string(sd_bus *bus, const char *path, const char *iface, const char *prop,
		      sd_bus_message *reply, void *u, sd_bus_error *e)
{
	struct proxy *p = B.p;
	const char *v = "lux";

	(void)bus, (void)path, (void)iface, (void)u, (void)e;
	if (strcmp(prop, "AccelerometerOrientation") == 0)
		v = sp_orientation_name(p->orientation);
	else if (strcmp(prop, "AccelerometerTilt") == 0)
		v = sp_tilt_name(p->tilt);
	return sd_bus_message_append(reply, "s", v);
}

static int get_double(sd_bus *bus, const char *path, const char *iface, const char *prop,
		      sd_bus_message *reply, void *u, sd_bus_error *e)
{
	struct proxy *p = B.p;

	(void)bus, (void)path, (void)iface, (void)u, (void)e;
	return sd_bus_message_append(reply, "d", strcmp(prop, "LightLevel") == 0 ? p->level : p->heading);
}

/* Claim* and Release*: the kind from the method's name. */
static int method_claim(sd_bus_message *m, void *u, sd_bus_error *e)
{
	static const char *const names[SP_KINDS] = { "Accelerometer", "Light", "Proximity", "Compass" };
	const char *member = sd_bus_message_get_member(m);
	bool claim = strncmp(member, "Claim", 5) == 0;
	int k;

	(void)u, (void)e;
	for (k = 0; k < SP_KINDS; k++)
		if (strcmp(member + (claim ? 5 : 7), names[k]) == 0)
			break;
	if (k == SP_KINDS)
		return -EINVAL;
	if (claim) {
		int r = sd_bus_track_add_sender(B.track[k], m);

		if (r < 0)
			return r;
	} else {
		sd_bus_track_remove_sender(B.track[k], m);	/* not claimed: a no-op */
	}
	sp_claimants(B.p, (enum sp_kind)k, sd_bus_track_count(B.track[k]));
	return sd_bus_reply_method_return(m, "");
}

#define PROP(name, sig, get) SD_BUS_PROPERTY(name, sig, get, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE)

/* The methods are SD_BUS_VTABLE_UNPRIVILEGED: without it sd-bus lets only
 * our own uid or CAP_SYS_ADMIN call them, whatever the bus policy allows.
 * Property reads are not checked. */
static const sd_bus_vtable vtable[] = {
	SD_BUS_VTABLE_START(0),
	PROP("HasAccelerometer", "b", get_bool),
	PROP("AccelerometerOrientation", "s", get_string),
	PROP("AccelerometerTilt", "s", get_string),
	PROP("HasAmbientLight", "b", get_bool),
	PROP("LightLevelUnit", "s", get_string),
	PROP("LightLevel", "d", get_double),
	PROP("HasProximity", "b", get_bool),
	PROP("ProximityNear", "b", get_bool),
	SD_BUS_METHOD("ClaimAccelerometer", "", "", method_claim, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("ReleaseAccelerometer", "", "", method_claim, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("ClaimLight", "", "", method_claim, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("ReleaseLight", "", "", method_claim, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("ClaimProximity", "", "", method_claim, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("ReleaseProximity", "", "", method_claim, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_VTABLE_END
};

static const sd_bus_vtable compass_vtable[] = {
	SD_BUS_VTABLE_START(0),
	PROP("HasCompass", "b", get_bool),
	PROP("CompassHeading", "d", get_double),
	SD_BUS_METHOD("ClaimCompass", "", "", method_claim, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("ReleaseCompass", "", "", method_claim, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_VTABLE_END
};

/* A track became empty (its last name left the bus, or released). Not 0:
 * sd-bus queues an empty track whose handler returns 0 again, at once. */
static int track_empty(sd_bus_track *t, void *u)
{
	int k = (int)(intptr_t)u;

	sp_claimants(B.p, (enum sp_kind)k, sd_bus_track_count(t));
	return 1;
}

void sp_bus_changed(void *ctx, enum sp_prop prop)
{
	bool compass = prop == SP_HAS_COMPASS || prop == SP_HEADING;
	int r;

	(void)ctx;
	if (!B.bus)
		return;
	r = sd_bus_emit_properties_changed(B.bus, compass ? COMPASS_PATH : PATH, compass ? COMPASS_IFACE : IFACE,
					   prop_names[prop], NULL);
	if (r < 0)
		B.p->ops.log("PropertiesChanged %s: %s", prop_names[prop], strerror(-r));
}

int sp_bus_open(struct proxy *p)
{
	int r, k;

	B.p = p;
	r = sd_bus_open_system(&B.bus);
	if (r < 0) {
		B.bus = NULL;
		return r;
	}
	r = sd_bus_add_object_vtable(B.bus, &B.slots[0], PATH, IFACE, vtable, NULL);
	if (r >= 0)
		r = sd_bus_add_object_vtable(B.bus, &B.slots[1], COMPASS_PATH, COMPASS_IFACE, compass_vtable, NULL);
	if (r >= 0)
		r = sd_bus_request_name(B.bus, BUS_NAME, 0);
	for (k = 0; k < SP_KINDS && r >= 0; k++) {
		r = sd_bus_track_new(B.bus, &B.track[k], track_empty, (void *)(intptr_t)k);
		if (r >= 0)
			r = sd_bus_track_set_recursive(B.track[k], 0);
	}
	if (r < 0)
		sp_bus_close();
	return r < 0 ? r : 0;
}

int sp_bus_pollfd(short *events, int *timeout_ms)
{
	uint64_t usec = UINT64_MAX;
	int ev = sd_bus_get_events(B.bus);

	*events = (short)(ev < 0 ? POLLIN : ev);
	*timeout_ms = -1;
	if (sd_bus_get_timeout(B.bus, &usec) >= 0 && usec != UINT64_MAX) {
		struct timespec ts;
		uint64_t now_us;

		clock_gettime(CLOCK_MONOTONIC, &ts);
		now_us = (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
		*timeout_ms = usec <= now_us ? 0 : (int)((usec - now_us + 999) / 1000);
	}
	return sd_bus_get_fd(B.bus);
}

int sp_bus_process(void)
{
	int i, r;

	for (i = 0; i < 64; i++) {
		r = sd_bus_process(B.bus, NULL);
		if (r <= 0)
			return r;
	}
	return 0;
}

void sp_bus_close(void)
{
	int k;

	for (k = 0; k < 2; k++)
		B.slots[k] = sd_bus_slot_unref(B.slots[k]);
	for (k = 0; k < SP_KINDS; k++)
		B.track[k] = sd_bus_track_unref(B.track[k]);
	if (B.bus) {
		sd_bus_flush(B.bus);
		sd_bus_close(B.bus);
		B.bus = sd_bus_unref(B.bus);
	}
}
