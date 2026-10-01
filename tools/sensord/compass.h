/* compass: tilt-compensated heading from accelerometer, gyroscope and
 * magnetometer samples (docs/next-steps/sensors-plan.md section 8). Pure:
 * no I/O, no clock, no allocation; sensord feeds it the SMGR samples it
 * publishes, tools/sensord/compass-replay feeds it recorded captures.
 *
 * Frames: the body frame is the Android/IIO device frame sensord
 * publishes (x right, y towards the top edge, z out of the screen); the
 * world frame is East-North-Up. The attitude q (w, x, y, z) rotates body
 * vectors into the world: v_world = q v_body q*, the same convention as
 * an Android rotation vector.
 *
 * Inputs: accel in m/s^2 (at rest it reads +g along "up"), gyro in rad/s,
 * magn in gauss (SMGR calibration "full"), timestamps in ns on one clock.
 * See compass.c for the filter, the gating rules and the constants. */
#ifndef CHEF_CYCLO_COMPASS_H
#define CHEF_CYCLO_COMPASS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Which body vector is "forward"; the heading is the azimuth of its
 * horizontal part. */
enum compass_mount {
	COMPASS_PORTRAIT,	/* up x (+x): the horizontal part of +y - z (top edge when
				 * flat, back of the phone when upright), roll-invariant */
	COMPASS_LANDSCAPE_LEFT,	/* up x (-y): top edge to the left (+x forward when flat) */
	COMPASS_LANDSCAPE_RIGHT,/* up x (+y): top edge to the right (-x forward when flat) */
	COMPASS_FLAT,		/* the horizontal part of +y */
	COMPASS_UPRIGHT,	/* the horizontal part of -z */
	COMPASS_MOUNT_COUNT
};

enum compass_state { COMPASS_INIT, COMPASS_RUN };

struct compass_out {
	int64_t t;		/* ns, time of the newest sample applied */
	double heading;		/* deg, 0..360 clockwise from magnetic north */
	double pitch;		/* deg, top edge above the horizontal (upright = 90) */
	double roll;		/* deg, asin(-up.x): the right edge's angle below the
				 * horizontal (right edge down positive), -90..90; well
				 * defined flat and upright alike */
	double accuracy;	/* deg, estimated heading error (180: meaningless) */
	bool calibrated;	/* magnetometer hard-iron bias learned (from sensord) */
	bool disturbed;		/* magnetometer gated off: gyro carries the heading */
	bool level_ok;		/* the mount has a forward direction (portrait: x not
				 * near vertical, i.e. not on its edge) */
	double q[4];		/* attitude, body to ENU */
};

struct compass {
	/* configuration */
	enum compass_mount mount;
	bool calibrated;
	/* state */
	enum compass_state st;
	double q[4];
	double bias[3];		/* gyro bias estimate, rad/s */
	int64_t t;		/* newest sample applied */
	int64_t gyro_t;		/* last gyro sample integrated (0: none since init) */
	double gyro_prev[3];
	int64_t acc_t, mag_t;	/* last accel / magn sample used */
	double acc_init[3];	/* INIT: running sum of accel samples */
	int acc_init_n;
	double mag_init[3];
	bool mag_init_ok;
	double g_ref;		/* |a| at rest, m/s^2 */
	/* magnetometer gate */
	bool ref_ok;
	double b_ref, incl_ref;	/* running |B| (gauss) and inclination (deg) */
	int64_t reseed_until;	/* refs follow the field quickly until then */
	bool disturbed;
	int suspect;		/* consecutive samples with a large yaw innovation */
	int64_t calm_since;	/* disturbed: inside the exit band since (0: not) */
	int64_t disturbed_since;
	double sm_b, sm_incl, sm_delta;	/* 0.5 s running values (leaving "disturbed") */
	double cand_b, cand_incl;	/* disturbed: 1 s running values ... */
	double cand_lb, cand_lincl;	/* ... and 4 s ones: steady when they agree */
	double cand_dir[2], cand_ldir[2];	/* the same for the innovation angle (cos, sin) */
	int64_t cand_since;	/* candidate field stable since (0: not) */
	int64_t boost_until;	/* fast correction gains until then */
	/* accuracy */
	double innov_ms;	/* running mean square yaw innovation, rad^2 */
	bool innov_seed;	/* the next innovation replaces the mean */
	double drift;		/* deg of heading uncertainty added without magn */
	double last_heading;
	bool have_heading;
	/* counters */
	unsigned long n_accel, n_gyro, n_magn, stale, gaps, resets, reacquired, disturbances;
	unsigned long gyro_lost;	/* anglvel stopped: re-initialised to run without it */
	int future_n;		/* accel/magn samples in a row more than 2 s past the gyro */
};

void compass_init(struct compass *c, enum compass_mount mount);
/* Back to INIT (next accel + magn re-initialise the attitude); keeps the
 * configuration, the gyro bias estimate and the counters. */
void compass_reset(struct compass *c);
/* The magnetometer calibration changed (sensord: REG2 group 2980): the
 * magn samples jump, so the gate re-learns the field and the yaw is
 * pulled in fast. */
void compass_set_calibrated(struct compass *c, bool calibrated);
/* The magnetometer's calibration changed while staying calibrated (the
 * ADSP refined its bias): the same re-learning as a flag change. */
void compass_field_jump(struct compass *c);

/* Feed one sample. Return 1 when the output changed (attitude advanced),
 * 0 when the sample was stored or used only for a later update, -1 when
 * it was dropped (out of order, stale, not finite). */
int compass_accel(struct compass *c, int64_t t, const double a[3]);
int compass_gyro(struct compass *c, int64_t t, const double w[3]);
int compass_magn(struct compass *c, int64_t t, const double m[3]);

/* false while INIT (no attitude yet). */
bool compass_output(struct compass *c, struct compass_out *o);

/* Live magnetometer calibration state from the data: SMGR's "full"
 * output minus its "factory" output of the same field is the hard-iron
 * bias the ADSP applies right now (factory - full, device frame, gauss).
 * Each factory sample is paired with the full samples around it
 * (interpolated, or the nearest within 30 ms); once the difference has
 * stayed within 0.02 G of its running mean for 3 s the state is "known":
 * calibrated when |bias| > 0.05 G. Why 0.05 G: before learning, full and
 * factory differ by under 3 mG (2026-09-27: factory vs full 1 mG at rest,
 * factory vs raw < 3 mG); the learned bias was 0.53..0.55 G (group 2980
 * writes 2026-09-27 and 2026-10-01, and full minus factory live). */
#define MC_RING		8
#define MC_PENDING	4

struct compass_magcal {
	int64_t ft[MC_RING];		/* full samples, oldest first */
	double fv[MC_RING][3];
	int nf;
	int64_t pt[MC_PENDING];		/* factory samples waiting for a later full one */
	double pv[MC_PENDING][3];
	int np;
	double ema[3];			/* running factory - full */
	bool have_ema;
	int64_t stable_since;
	bool known;			/* a stable difference has been seen */
	bool calibrated;		/* |bias| > 0.05 G */
	double bias[3];			/* factory - full, device frame, gauss */
	unsigned long pairs, changes;
};

void compass_magcal_init(struct compass_magcal *m);
/* Feed one full / factory magn sample (device frame, gauss). 1 when
 * known, calibrated or the bias changed (by more than 0.05 G). */
int compass_magcal_full(struct compass_magcal *m, int64_t t, const double v[3]);
int compass_magcal_factory(struct compass_magcal *m, int64_t t, const double v[3]);

/* Heading (deg, 0..360), pitch and roll of an attitude (body to ENU, w
 * x y z) for a mount. Returns false when the forward vector's horizontal
 * part is too short to give a heading (heading then 0). Shared with the
 * ADSP rotation vector comparison. */
bool compass_angles(const double q[4], enum compass_mount mount, double *heading, double *pitch,
		    double *roll);

/* The heading line sensord sends and compass-replay prints:
 * {"sensor":"heading","t":NS,"heading":H,"pitch":P,"roll":R,
 *  "calibrated":B,"disturbed":B,"accuracy":A} plus
 * ,"true_heading":T,"declination":D when have_decl; extra (NULL or a
 * string of ,"key":value pairs) goes after accuracy. Returns snprintf's
 * value. */
int compass_format(char *buf, size_t len, const struct compass_out *o, bool have_decl, double decl,
		   const char *extra);

const char *compass_mount_name(enum compass_mount m);
int compass_mount_parse(const char *s);	/* -1 if unknown */

/* Normalise degrees to [0, 360). */
double compass_wrap360(double deg);

/* Rounded to 0.01, never -0 (for printing angles with %.2f). */
double compass_round2(double v);

#endif
