/*
 * sensor-proxy's internal header: orientation and tilt (orient.c), the
 * claim bookkeeping and sensord's lines (proxy.c, pure: requests and
 * property changes go through struct sp_ops), the bus side (bus.c), shared
 * by main.c and the host tests. Not installed: clients use the D-Bus
 * interface net.hadess.SensorProxy. The daemon as a whole is described in
 * main.c's header.
 */
#ifndef SENSOR_PROXY_H
#define SENSOR_PROXY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SP_SENSORD_PROTO 2	/* the sensord protocol this client speaks */
#define SP_RELIST_MS     5000	/* list again while SMGR is not ready */
#define SP_BACKOFF_MIN_MS 1000
#define SP_BACKOFF_MAX_MS 30000

/* ------------------------------------------------------------ orient.c */

enum sp_orientation { SP_OR_UNDEFINED, SP_OR_NORMAL, SP_OR_BOTTOM_UP, SP_OR_LEFT_UP, SP_OR_RIGHT_UP };
enum sp_tilt { SP_TILT_UNDEFINED, SP_TILT_VERTICAL, SP_TILT_UP, SP_TILT_DOWN, SP_TILT_FACE_UP,
	       SP_TILT_FACE_DOWN };

const char *sp_orientation_name(enum sp_orientation o);
const char *sp_tilt_name(enum sp_tilt t);
/* From one sensord accel sample (m/s^2, Android device frame). */
enum sp_orientation sp_orientation_calc(enum sp_orientation prev, double x, double y, double z);
enum sp_tilt sp_tilt_calc(double x, double y, double z);

/* ------------------------------------------------------------- proxy.c */

/* The four kinds a bus name can claim, and sensord's channel for each. */
enum sp_kind { SP_ACCEL, SP_LIGHT, SP_PROX, SP_COMPASS, SP_KINDS };

/* The properties that can change, in bus.c's order. */
enum sp_prop {
	SP_HAS_ACCEL, SP_ORIENTATION, SP_TILT, SP_HAS_LIGHT, SP_LIGHT_LEVEL, SP_HAS_PROX,
	SP_PROX_NEAR, SP_HAS_COMPASS, SP_HEADING, SP_PROPS
};

struct sp_ops {
	/* one request line for sensord, its newline included: 0 or -1 */
	int (*send)(void *ctx, const char *line);
	/* a property's value changed: emit PropertiesChanged */
	void (*changed)(void *ctx, enum sp_prop prop);
	void (*log)(const char *fmt, ...);
	void *ctx;
};

struct proxy {
	struct sp_ops ops;
	bool up;			/* connected to sensord */
	bool has[SP_KINDS];		/* present in the last list, and up */
	bool claimed[SP_KINDS];		/* a bus name claims it */
	enum sp_orientation orientation;
	enum sp_tilt tilt;
	double level;			/* lux */
	bool near;
	double heading;			/* degrees, -1 before the first sample */
	int64_t relist_at;		/* the next list while SMGR is not ready; -1 none */
	bool proto_warned;
};

void sp_init(struct proxy *p, const struct sp_ops *ops);
/* The number of bus names claiming kind k changed (or may have). */
void sp_claimants(struct proxy *p, enum sp_kind k, unsigned count);
void sp_connected(struct proxy *p, int64_t now);
void sp_lost(struct proxy *p);
/* One line from sensord. True when it shows sensord works (a list with
 * SMGR ready): the caller resets the reconnect backoff. */
bool sp_line(struct proxy *p, const char *line, size_t len, int64_t now);
/* When sp_tick() has something to do (monotonic ms), -1 none. */
int64_t sp_deadline(const struct proxy *p);
void sp_tick(struct proxy *p, int64_t now);
const char *sp_kind_name(enum sp_kind k);

/* --------------------------------------------------------------- bus.c */

/* The bus side: the two objects, the claim tracks and the emits, for
 * proxy P. 0, or -errno (the name not obtained included). */
int sp_bus_open(struct proxy *p);
/* struct sp_ops's changed hook for the bus. */
void sp_bus_changed(void *ctx, enum sp_prop prop);
/* For main's poll: the fd, its events, the timeout (ms, -1 none). */
int sp_bus_pollfd(short *events, int *timeout_ms);
/* Process everything pending; -errno when the connection is gone. */
int sp_bus_process(void);
void sp_bus_close(void);

#endif /* SENSOR_PROXY_H */
