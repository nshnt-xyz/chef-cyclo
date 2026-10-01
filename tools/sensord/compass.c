/* compass: tilt-compensated heading, see compass.h and
 * docs/next-steps/sensors-plan.md section 8.
 *
 * Filter: Mahony-style complementary attitude filter on a quaternion.
 *  - The gyro propagates the attitude between samples (midpoint rate,
 *    exact rotation per step), minus a slowly learned bias.
 *  - Each accelerometer sample corrects tilt: the rotation that takes
 *    the predicted "up" onto the measured one, scaled by KP_ACC * dt
 *    (dt: that sensor's own sample interval, so the correction strength
 *    does not depend on the rates or on how the streams interleave). The
 *    correction is perpendicular to up, so it never moves the heading.
 *  - Each magnetometer sample corrects yaw only: the angle about the
 *    predicted up between the measured horizontal field and the
 *    predicted north, scaled by KP_MAG * dt. It never tilts the attitude.
 *  - A small integral term (KI) learns the gyro bias from both
 *    corrections, only while the phone turns slowly (scale errors would
 *    leak in otherwise), outside the fast-convergence windows and only
 *    from errors under 10 deg (a calibration jump is not gyro bias).
 * Corrections use the attitude at the sample's own time (the attitude at
 * the last gyro sample turned by the last gyro rate, back or forward), so
 * a magnetometer sample 50 ms away from the newest gyro sample does not
 * drag the heading during a turn.
 *
 * Why this and not a Kalman filter: three gains and a gate are easy to
 * reason about, host-test and replay; the DSP's own 9-axis rotation
 * vector is available as a comparison (sensord -V).
 *
 * Robustness:
 *  - Accelerometer weight 1 while | |a| - g | <= 5 % of g, 0 beyond 15 %,
 *    linear in between (bumps, braking); g is learned slowly at rest.
 *  - Magnetometer gate: a sample is bad when |B| is more than 25 % off
 *    its running value, the inclination more than 12 deg off, or the yaw
 *    innovation (measured against predicted north) over 25 deg (a magnet
 *    can turn the field without changing |B| or the dip). Bad samples are
 *    skipped; 3 in a row mark the field "disturbed" (single outliers, one
 *    in a few hundred live, never do). Back to normal only after 1 s with
 *    the 0.5 s-smoothed values inside 12 % / 6 deg / 10 deg (hysteresis).
 *    The running values follow the field slowly (30 s) and only while not
 *    disturbed. While disturbed the gyro alone carries the heading and
 *    the accuracy estimate grows. If the field stays disturbed for 10 s
 *    but is itself steady (its 1 s and 4 s running |B|, inclination and
 *    direction agree within 3 % / 3 deg / 5 deg for the last 5 s), it is
 *    taken as the new reference (a changed environment, e.g. the phone
 *    mounted on the bike). Why 25 %: live, indoors, |B| of the calibrated
 *    field varied by +-18 % with orientation during a flat turn while its
 *    direction stayed within 2..6 deg of the gyro's (the 2026-09-27
 *    magcal capture a, replayed with compass-replay); 15 % gated a third
 *    of that turn.
 *  - After init, a calibration change or a reacquired field, the
 *    reference follows the field fast for 3 s and the gains are 5x for
 *    2 s, so the heading converges quickly.
 *  - Timestamps: gyro samples must be strictly newer than the last one
 *    (else dropped); accel/magn samples more than 200 ms older or more
 *    than 2 s newer than the last gyro sample, or not newer than their own previous sample, are
 *    dropped. A gyro gap over 250 ms is not integrated (unknown motion;
 *    the gains are boosted instead); over 2 s the attitude is
 *    re-initialised from accel + magn.
 *  - anglvel stopping is not fatal: 10 accel/magn samples in a row more
 *    than 2 s past the last gyro sample re-initialise the filter, which
 *    then runs on accel + magn alone with 4x gains until the gyro is back.
 *  - Startup: the first attitude comes from the mean of 5 accelerometer
 *    samples and the latest magnetometer sample (TRIAD: up, east =
 *    B x up, north = up x east); nothing is output before that.
 *
 * Heading: the azimuth of the mount's forward vector (compass_angles):
 * portrait = up x (body x), which is the horizontal part of (+y - z) for
 * any pitch but, unlike it, does not move when the phone rolls about the
 * forward axis (see mount_rules).
 *
 * Pitch = asin(up.y) (top edge above the horizontal), roll = asin(-up.x)
 * (right edge below it): both well defined flat and in the upright
 * handlebar pose (Android's atan2 roll reads about +-90 there).
 *
 * Accuracy (deg): sqrt(3^2 + rms yaw innovation^2) plus the drift
 * accumulated without magnetometer correction (0.1 deg/s plus 3 % of the
 * angle turned), which decays again once corrections resume; the first
 * innovation after init or a calibration change seeds the mean, so a
 * large jump reads as a large accuracy at once; 180 when
 * the magnetometer is not calibrated or the mount has no forward
 * direction (portrait: the phone on its edge). 3 deg is the floor: indoors the calibrated field still gave
 * up to 9..14 deg heading error against the gyro over a flat turn
 * (logs/magcal-live-test-2026-09-27-check.txt). */
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "compass.h"

#define NS		1000000000LL
#define G0		9.80665
#define DEG		(180.0 / M_PI)
#define RAD		(M_PI / 180.0)

#define INIT_ACC_N	5
#define INIT_ACC_GAP	(NS / 2)	/* restart the INIT accel mean after this */
#define KP_ACC		1.0		/* rad/s per rad of tilt error */
#define KP_MAG		0.5		/* rad/s per rad of yaw error */
#define KI		0.02		/* rad/s^2 per rad: gyro bias learning */
#define KI_MAX_RATE	0.35		/* rad/s: no bias learning while turning faster */
#define BIAS_MAX	0.05		/* rad/s */
#define BIAS_ERR_MAX	10.0		/* deg: larger errors do not teach the bias */
#define BOOST		5.0
#define NO_GYRO_GAIN	4.0
#define BOOST_NS	(2 * NS)
#define RESEED_NS	(3 * NS)
#define ACC_FULL	0.05
#define ACC_ZERO	0.15
#define G_TAU_S		30.0
#define ACC_DT_MAX	0.1		/* s: longest interval one accel sample stands for */
#define MAG_DT_MAX	0.25
#define STALE_NS	(NS / 5)
#define GYRO_GAP_NS	(NS / 4)
#define GYRO_RESET_NS	(2 * NS)
#define GYRO_LOST_N	10		/* accel/magn samples past it in a row: gyro gone */
#define MAG_H_MIN	0.2		/* horizontal part of B, fraction of |B| */
#define GATE_B		0.25
#define GATE_INCL	12.0
#define CALM_B		0.12
#define CALM_INCL	6.0
#define CALM_NS		NS
#define REF_TAU_S	30.0
#define RESEED_TAU_S	0.3
#define INNOV_GATE	25.0		/* deg: yaw innovation that marks a sample suspect */
#define INNOV_CALM	10.0
#define SUSPECT_N	3		/* consecutive suspect samples: disturbed */
#define SMOOTH_TAU_S	0.5
#define CAND_TAU_S	1.0
#define CAND_LTAU_S	4.0
#define CAND_B		0.03
#define CAND_INCL	3.0
#define CAND_DIR	5.0		/* deg */
#define REACQ_AFTER_NS	(10 * NS)
#define REACQ_STEADY_NS	(5 * NS)
#define INNOV_TAU_S	2.0
#define ACC_FLOOR	3.0
#define DRIFT_RATE	0.1		/* deg/s */
#define DRIFT_SCALE	0.03		/* of the angle turned */
#define NO_MAG_NS	NS		/* no magn sample for this long: drifting */
#define LEVEL_MIN	0.25		/* forward vector horizontal fraction */

/* ------------------------------------------------------------ vectors */

static double dot3(const double a[3], const double b[3])
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void cross3(const double a[3], const double b[3], double o[3])
{
	double r[3] = { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
			a[0] * b[1] - a[1] * b[0] };

	memcpy(o, r, sizeof(r));
}

static double norm3(const double a[3])
{
	return sqrt(dot3(a, a));
}

static bool finite3(const double a[3])
{
	return isfinite(a[0]) && isfinite(a[1]) && isfinite(a[2]);
}

static double clampd(double v, double lo, double hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

/* --------------------------------------------------------- quaternions */

static void q_mul(const double a[4], const double b[4], double o[4])
{
	double r[4] = {
		a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3],
		a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2],
		a[0] * b[2] - a[1] * b[3] + a[2] * b[0] + a[3] * b[1],
		a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0],
	};

	memcpy(o, r, sizeof(r));
}

static void q_normalise(double q[4])
{
	double n = sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
	int i;

	if (n < 1e-12 || !isfinite(n)) {
		q[0] = 1;
		q[1] = q[2] = q[3] = 0;
		return;
	}
	if (q[0] < 0)
		n = -n;		/* keep w >= 0: one representation */
	for (i = 0; i < 4; i++)
		q[i] /= n;
}

/* body -> world: R(q) v */
static void q_rotate(const double q[4], const double v[3], double o[3])
{
	double p[4] = { 0, v[0], v[1], v[2] }, qc[4] = { q[0], -q[1], -q[2], -q[3] }, t[4];

	q_mul(q, p, t);
	q_mul(t, qc, t);
	o[0] = t[1];
	o[1] = t[2];
	o[2] = t[3];
}

/* world -> body: R(q)^T v */
static void q_unrotate(const double q[4], const double v[3], double o[3])
{
	double qc[4] = { q[0], -q[1], -q[2], -q[3] };

	q_rotate(qc, v, o);
}

/* q <- q (x) exp(theta / 2): a body-frame rotation by the vector theta. */
static void q_turn(double q[4], const double th[3])
{
	double a = norm3(th), d[4], s;

	if (a < 1e-15)
		return;
	s = sin(a / 2) / a;
	d[0] = cos(a / 2);
	d[1] = th[0] * s;
	d[2] = th[1] * s;
	d[3] = th[2] * s;
	q_mul(q, d, q);
	q_normalise(q);
}

/* Rotation matrix rows (east, north, up in body coordinates) -> q. */
static void q_from_rows(const double e[3], const double n[3], const double u[3], double q[4])
{
	double m[3][3] = { { e[0], e[1], e[2] }, { n[0], n[1], n[2] }, { u[0], u[1], u[2] } };
	double tr = m[0][0] + m[1][1] + m[2][2], s;

	if (tr > 0) {
		s = sqrt(tr + 1.0) * 2;
		q[0] = 0.25 * s;
		q[1] = (m[2][1] - m[1][2]) / s;
		q[2] = (m[0][2] - m[2][0]) / s;
		q[3] = (m[1][0] - m[0][1]) / s;
	} else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
		s = sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) * 2;
		q[0] = (m[2][1] - m[1][2]) / s;
		q[1] = 0.25 * s;
		q[2] = (m[0][1] + m[1][0]) / s;
		q[3] = (m[0][2] + m[2][0]) / s;
	} else if (m[1][1] > m[2][2]) {
		s = sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]) * 2;
		q[0] = (m[0][2] - m[2][0]) / s;
		q[1] = (m[0][1] + m[1][0]) / s;
		q[2] = 0.25 * s;
		q[3] = (m[1][2] + m[2][1]) / s;
	} else {
		s = sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]) * 2;
		q[0] = (m[1][0] - m[0][1]) / s;
		q[1] = (m[0][2] + m[2][0]) / s;
		q[2] = (m[1][2] + m[2][1]) / s;
		q[3] = 0.25 * s;
	}
	q_normalise(q);
}

/* ------------------------------------------------------------- public */

static const char *const mount_names[COMPASS_MOUNT_COUNT] = {
	"portrait", "landscape-left", "landscape-right", "flat", "upright",
};

/* Portrait and landscape: forward = up x R (horizontal, perpendicular to
 * R), R the body vector pointing to the viewer's right (portrait +x;
 * landscape with the top edge left -y, top edge right +y). For any pitch
 * about R that is exactly the horizontal part of (+y - z) (portrait):
 * the top edge when flat, the back of the phone when upright. Unlike
 * (+y - z) it does not move when the phone rolls about the forward axis
 * (upright and rolled 30 deg, +y - z is 27 deg off), as long as the roll
 * stays under 90 deg. flat and upright: the horizontal part of +y or -z. */
struct mount_rule {
	bool cross;		/* forward = up x v; else the horizontal part of v */
	double v[3];
};

static const struct mount_rule mount_rules[COMPASS_MOUNT_COUNT] = {
	{ true, { 1, 0, 0 } }, { true, { 0, -1, 0 } }, { true, { 0, 1, 0 } },
	{ false, { 0, 1, 0 } }, { false, { 0, 0, -1 } },
};

const char *compass_mount_name(enum compass_mount m)
{
	return (unsigned)m < COMPASS_MOUNT_COUNT ? mount_names[m] : "?";
}

int compass_mount_parse(const char *s)
{
	int i;

	for (i = 0; i < COMPASS_MOUNT_COUNT; i++)
		if (strcmp(s, mount_names[i]) == 0)
			return i;
	return -1;
}

double compass_wrap360(double deg)
{
	double r = fmod(deg, 360.0);

	if (r < 0)
		r += 360.0;
	if (r >= 360.0)
		r = 0;
	return r;
}

bool compass_angles(const double q[4], enum compass_mount mount, double *heading, double *pitch,
		    double *roll)
{
	static const double up_w[3] = { 0, 0, 1 };
	const struct mount_rule *m = &mount_rules[(unsigned)mount < COMPASS_MOUNT_COUNT ? mount : 0];
	double vw[3], fw[3], u[3], h;

	q_rotate(q, m->v, vw);
	if (m->cross)
		cross3(up_w, vw, fw);
	else
		memcpy(fw, vw, sizeof(fw));
	q_unrotate(q, up_w, u);
	h = hypot(fw[0], fw[1]);
	*pitch = asin(clampd(u[1], -1, 1)) * DEG;
	*roll = asin(clampd(-u[0], -1, 1)) * DEG;
	if (h < LEVEL_MIN) {
		*heading = 0;
		return false;
	}
	*heading = compass_wrap360(atan2(fw[0], fw[1]) * DEG);
	return true;
}

static void clear_state(struct compass *c)
{
	c->st = COMPASS_INIT;
	c->q[0] = 1;
	c->q[1] = c->q[2] = c->q[3] = 0;
	c->gyro_t = c->acc_t = c->mag_t = 0;
	memset(c->gyro_prev, 0, sizeof(c->gyro_prev));
	memset(c->acc_init, 0, sizeof(c->acc_init));
	c->acc_init_n = 0;
	c->mag_init_ok = false;
	c->ref_ok = false;
	c->disturbed = false;
	c->suspect = 0;
	c->calm_since = c->disturbed_since = c->cand_since = 0;
	c->reseed_until = c->boost_until = 0;
	c->innov_ms = 0;
	c->innov_seed = true;
	c->drift = 0;
	c->have_heading = false;
	c->future_n = 0;
}

void compass_init(struct compass *c, enum compass_mount mount)
{
	memset(c, 0, sizeof(*c));
	c->mount = (unsigned)mount < COMPASS_MOUNT_COUNT ? mount : COMPASS_PORTRAIT;
	c->g_ref = G0;
	clear_state(c);
}

void compass_reset(struct compass *c)
{
	if (c->st != COMPASS_INIT)
		c->resets++;
	clear_state(c);
}

void compass_set_calibrated(struct compass *c, bool calibrated)
{
	if (c->calibrated == calibrated)
		return;
	c->calibrated = calibrated;
	compass_field_jump(c);
}

void compass_field_jump(struct compass *c)
{
	/* the field jumps by the bias: relearn it and pull the yaw in */
	c->ref_ok = false;
	c->disturbed = false;
	c->suspect = 0;
	c->innov_ms = 0;
	c->innov_seed = true;
	c->calm_since = c->cand_since = 0;
	if (c->st == COMPASS_RUN) {
		c->reseed_until = c->t + RESEED_NS;
		c->boost_until = c->t + BOOST_NS;
	}
}

/* Correction gain: 5x in the fast-convergence windows; 4x with no gyro
 * (anglvel absent or stopped), when accel and magn alone carry the
 * attitude and the normal 1..2 s time constants would lag every turn. */
static double gain(const struct compass *c, double kp, int64_t t)
{
	if (t < c->boost_until)
		return kp * BOOST;
	return c->gyro_t ? kp : kp * NO_GYRO_GAIN;
}

/* The attitude at time t: c->q is the attitude at the last integrated
 * gyro sample (accel and magn samples do not advance it), so it is turned
 * by the last gyro rate over t - gyro_t, back or forward, at most
 * STALE_NS either way. */
static void attitude_at(const struct compass *c, int64_t t, double q[4])
{
	memcpy(q, c->q, sizeof(c->q));
	if (c->gyro_t && t != c->gyro_t) {
		double dt = clampd((double)(t - c->gyro_t) / NS, -(double)STALE_NS / NS,
				   (double)STALE_NS / NS), th[3];
		int i;

		for (i = 0; i < 3; i++)
			th[i] = (c->gyro_prev[i] - c->bias[i]) * dt;
		q_turn(q, th);
	}
}

/* The integral term. Not while the phone turns fast (scale errors would
 * leak in), not in the fast-convergence windows after init, a gap, a
 * reacquired field or a calibration change, and not on a large error
 * (err_deg): those are jumps of the reference, not gyro bias, and
 * integrating one (a 100 deg calibration jump) learned about 1 deg/s of
 * false bias that then drifted the heading whenever magn was missing. */
static void learn_bias(struct compass *c, int64_t t, const double e[3], double dt, double err_deg)
{
	int i;

	if (!c->gyro_t || norm3(c->gyro_prev) > KI_MAX_RATE || t < c->boost_until ||
	    t < c->reseed_until || fabs(err_deg) > BIAS_ERR_MAX)
		return;
	for (i = 0; i < 3; i++)
		c->bias[i] = clampd(c->bias[i] - KI * e[i] * dt, -BIAS_MAX, BIAS_MAX);
}

static void try_init(struct compass *c, int64_t t)
{
	double a[3], up[3], east[3], north[3], an, en;
	int i;

	if (c->acc_init_n < INIT_ACC_N || !c->mag_init_ok)
		return;
	for (i = 0; i < 3; i++)
		a[i] = c->acc_init[i] / c->acc_init_n;
	an = norm3(a);
	if (an < 0.5 * G0 || an > 1.5 * G0)
		return;
	for (i = 0; i < 3; i++)
		up[i] = a[i] / an;
	cross3(c->mag_init, up, east);
	en = norm3(east);
	if (en < MAG_H_MIN * norm3(c->mag_init) || en < 1e-6)
		return;		/* field (nearly) vertical: no north */
	for (i = 0; i < 3; i++)
		east[i] /= en;
	cross3(up, east, north);
	q_from_rows(east, north, up, c->q);
	c->st = COMPASS_RUN;
	if (t > c->t)
		c->t = t;
	c->gyro_t = 0;
	c->ref_ok = false;
	c->reseed_until = c->t + RESEED_NS;
	c->boost_until = c->t + BOOST_NS;
	c->drift = 0;
	c->innov_ms = 0;
	c->innov_seed = true;
}

/* Accepts a sample time for accel/magn: not older than its predecessor,
 * not stale against the newest sample. */
/* accel/magn at t: anglvel stopped? SAMPLES_GYRO_LOST accel/magn samples
 * in a row more than 2 s past the last gyro sample mean it did (a single
 * sample stamped in the future does not count): back to INIT, which
 * re-initialises from accel + magn and runs without gyro until it
 * returns, instead of dropping accel and magn for ever. */
static void gyro_watch(struct compass *c, int64_t t)
{
	if (c->st != COMPASS_RUN || !c->gyro_t)
		return;
	if (t <= c->gyro_t + GYRO_RESET_NS) {
		c->future_n = 0;
		return;
	}
	if (++c->future_n < GYRO_LOST_N)
		return;
	compass_reset(c);
	c->gyro_lost++;
}

static bool fresh(const struct compass *c, int64_t t, int64_t prev)
{
	if (prev && t <= prev)
		return false;
	/* against the gyro's time when there is one: a single stamp far in
	 * the future on accel or magn must not make the others stale, and is
	 * itself dropped (more than 2 s past the last gyro sample: the gyro
	 * would have re-initialised by then) */
	if (c->st == COMPASS_RUN && t < (c->gyro_t ? c->gyro_t : c->t) - STALE_NS)
		return false;
	if (c->st == COMPASS_RUN && c->gyro_t && t > c->gyro_t + GYRO_RESET_NS)
		return false;
	return true;
}

int compass_accel(struct compass *c, int64_t t, const double a[3])
{
	static const double up_w[3] = { 0, 0, 1 };
	double an, dt, w, dev, qa[4], u[3], v[3], ax[3], s, ang, th[3], k;
	int i;

	if (!finite3(a) || (an = norm3(a)) < 1e-3) {
		c->stale++;
		return -1;
	}
	gyro_watch(c, t);
	if (c->st == COMPASS_INIT) {
		if (c->acc_t && (t <= c->acc_t || t - c->acc_t > INIT_ACC_GAP)) {
			memset(c->acc_init, 0, sizeof(c->acc_init));
			c->acc_init_n = 0;
		}
		for (i = 0; i < 3; i++)
			c->acc_init[i] += a[i];
		c->acc_init_n++;
		c->acc_t = t;
		c->n_accel++;
		try_init(c, t);
		return c->st == COMPASS_RUN ? 1 : 0;
	}
	if (!fresh(c, t, c->acc_t)) {
		c->stale++;
		return -1;
	}
	c->n_accel++;
	dt = c->acc_t ? clampd((double)(t - c->acc_t) / NS, 0, ACC_DT_MAX) : 0.02;
	c->acc_t = t;
	dev = fabs(an - c->g_ref) / c->g_ref;
	w = dev <= ACC_FULL ? 1.0 : dev >= ACC_ZERO ? 0.0 : (ACC_ZERO - dev) / (ACC_ZERO - ACC_FULL);
	/* learn g at rest: full weight and turning slowly */
	if (w == 1.0 && norm3(c->gyro_prev) < 0.05)
		c->g_ref += (an - c->g_ref) * clampd(dt / G_TAU_S, 0, 1);
	if (t > c->t)
		c->t = t;
	if (w <= 0)
		return 0;
	attitude_at(c, t, qa);
	q_unrotate(qa, up_w, u);	/* predicted up, body */
	for (i = 0; i < 3; i++)
		v[i] = a[i] / an;	/* measured up */
	cross3(v, u, ax);		/* Mahony: e = v_meas x v_hat */
	s = norm3(ax);
	if (s < 1e-12)
		return 1;
	ang = atan2(s, dot3(v, u));
	k = clampd(gain(c, KP_ACC, t) * w * dt, 0, 1);
	for (i = 0; i < 3; i++) {
		th[i] = ax[i] / s * ang * k;
		ax[i] = ax[i] / s * ang;
	}
	q_turn(c->q, th);
	if (w == 1.0)
		learn_bias(c, t, ax, dt, ang * DEG);
	return 1;
}

/* The magnetometer gate, on |B| and the inclination against their
 * running values and on the yaw innovation (the angle between the
 * measured and the predicted north: a field turned by a magnet can keep
 * |B| and the inclination). di, delta in degrees; have_delta false when
 * the field is nearly vertical. A bad sample is skipped; SUSPECT_N in a
 * row mark the field disturbed (single outliers never do: live, one
 * sample in a few hundred reads 15..20 % off). Leaving uses values
 * smoothed over 0.5 s (per-sample |B| noise is about 3 %). Returns true
 * when the sample may correct the yaw. */
static bool gate(struct compass *c, int64_t t, double dt, double bn, double incl, bool have_delta,
		 double delta)
{
	double k;
	bool innov_on = have_delta && t >= c->boost_until, bad;

	if (!c->ref_ok) {
		c->b_ref = c->sm_b = bn;
		c->incl_ref = c->sm_incl = incl;
		c->sm_delta = 0;
		c->ref_ok = true;
		if (!c->reseed_until || c->reseed_until < t)
			c->reseed_until = t + RESEED_NS;
	}
	k = clampd(dt / SMOOTH_TAU_S, 0, 1);
	c->sm_b += (bn - c->sm_b) * k;
	c->sm_incl += (incl - c->sm_incl) * k;
	c->sm_delta += ((have_delta ? delta : 0) - c->sm_delta) * k;
	if (t < c->reseed_until) {
		k = clampd(dt / RESEED_TAU_S, 0, 1);
		c->b_ref += (bn - c->b_ref) * k;
		c->incl_ref += (incl - c->incl_ref) * k;
		c->disturbed = false;
		c->suspect = 0;
		return true;
	}
	bad = fabs(bn - c->b_ref) > GATE_B * c->b_ref || fabs(incl - c->incl_ref) > GATE_INCL ||
	      (innov_on && fabs(delta) > INNOV_GATE);
	if (!c->disturbed) {
		if (!bad) {
			c->suspect = 0;
			k = clampd(dt / REF_TAU_S, 0, 1);
			c->b_ref += (bn - c->b_ref) * k;
			c->incl_ref += (incl - c->incl_ref) * k;
			return true;
		}
		if (++c->suspect < SUSPECT_N)
			return false;	/* skipped, not (yet) disturbed */
		c->disturbed = true;
		c->disturbances++;
		c->disturbed_since = t;
		c->calm_since = 0;
		c->suspect = 0;
		c->cand_b = c->cand_lb = bn;
		c->cand_incl = c->cand_lincl = incl;
		c->cand_dir[0] = c->cand_ldir[0] = cos(delta * RAD);
		c->cand_dir[1] = c->cand_ldir[1] = sin(delta * RAD);
		c->cand_since = t;
		return false;
	}
	/* disturbed: back when the smoothed field is calm for CALM_NS */
	if (fabs(c->sm_b - c->b_ref) < CALM_B * c->b_ref && fabs(c->sm_incl - c->incl_ref) < CALM_INCL &&
	    (!have_delta || fabs(c->sm_delta) < INNOV_CALM)) {
		if (!c->calm_since)
			c->calm_since = t;
		if (t - c->calm_since >= CALM_NS) {
			c->disturbed = false;
			c->calm_since = 0;
			return !bad;
		}
	} else {
		c->calm_since = 0;
	}
	/* ... or when the new field has been steady long enough: its 1 s
	 * and 4 s running means agree in |B|, inclination and direction (the
	 * yaw innovation: the gyro holds the attitude meanwhile, so a steady
	 * field keeps it constant however the phone turns) */
	k = clampd(dt / CAND_TAU_S, 0, 1);
	c->cand_b += (bn - c->cand_b) * k;
	c->cand_incl += (incl - c->cand_incl) * k;
	c->cand_dir[0] += (cos(delta * RAD) - c->cand_dir[0]) * k;
	c->cand_dir[1] += (sin(delta * RAD) - c->cand_dir[1]) * k;
	k = clampd(dt / CAND_LTAU_S, 0, 1);
	c->cand_lb += (bn - c->cand_lb) * k;
	c->cand_lincl += (incl - c->cand_lincl) * k;
	c->cand_ldir[0] += (cos(delta * RAD) - c->cand_ldir[0]) * k;
	c->cand_ldir[1] += (sin(delta * RAD) - c->cand_ldir[1]) * k;
	if (fabs(c->cand_b - c->cand_lb) > CAND_B * c->cand_lb ||
	    fabs(c->cand_incl - c->cand_lincl) > CAND_INCL ||
	    (have_delta && fabs(atan2(c->cand_dir[0] * c->cand_ldir[1] - c->cand_dir[1] * c->cand_ldir[0],
				      c->cand_dir[0] * c->cand_ldir[0] + c->cand_dir[1] * c->cand_ldir[1])) >
				CAND_DIR * RAD))
		c->cand_since = t;
	if (t - c->disturbed_since >= REACQ_AFTER_NS && t - c->cand_since >= REACQ_STEADY_NS) {
		c->b_ref = c->sm_b = c->cand_lb;
		c->incl_ref = c->sm_incl = c->cand_lincl;
		c->disturbed = false;
		c->calm_since = 0;
		c->reacquired++;
		c->boost_until = t + BOOST_NS;
		return true;
	}
	return false;
}

int compass_magn(struct compass *c, int64_t t, const double m[3])
{
	static const double up_w[3] = { 0, 0, 1 }, north_w[3] = { 0, 1, 0 };
	double bn, dt, qa[4], u[3], n[3], mh[3], mv, hn, ax[3], incl, delta, k, th[3];
	int i;

	if (!finite3(m) || (bn = norm3(m)) < 1e-4) {
		c->stale++;
		return -1;
	}
	gyro_watch(c, t);
	if (c->st == COMPASS_INIT) {
		memcpy(c->mag_init, m, sizeof(c->mag_init));
		c->mag_init_ok = true;
		c->mag_t = t;
		c->n_magn++;
		try_init(c, t);
		return c->st == COMPASS_RUN ? 1 : 0;
	}
	if (!fresh(c, t, c->mag_t)) {
		c->stale++;
		return -1;
	}
	c->n_magn++;
	dt = c->mag_t ? clampd((double)(t - c->mag_t) / NS, 0, MAG_DT_MAX) : 0.05;
	c->mag_t = t;
	if (t > c->t)
		c->t = t;
	attitude_at(c, t, qa);
	q_unrotate(qa, up_w, u);
	q_unrotate(qa, north_w, n);
	mv = dot3(m, u);
	incl = asin(clampd(mv / bn, -1, 1)) * DEG;
	for (i = 0; i < 3; i++)
		mh[i] = m[i] - mv * u[i];
	hn = norm3(mh);
	delta = 0;
	if (hn >= MAG_H_MIN * bn) {
		for (i = 0; i < 3; i++)
			mh[i] /= hn;
		/* signed yaw error about up, from the measured north to the predicted */
		cross3(mh, n, ax);
		delta = atan2(dot3(ax, u), dot3(mh, n));
	}
	if (!gate(c, t, dt, bn, incl, hn >= MAG_H_MIN * bn, delta * DEG))
		return 0;
	if (hn < MAG_H_MIN * bn)
		return 0;	/* field nearly vertical here: no heading information */
	/* after init or a calibration change the first innovation seeds the
	 * mean, so a large jump shows in the accuracy at once */
	if (c->innov_seed)
		c->innov_ms = delta * delta;
	else
		c->innov_ms += (delta * delta - c->innov_ms) * clampd(dt / INNOV_TAU_S, 0, 1);
	c->innov_seed = false;
	k = clampd(gain(c, KP_MAG, t) * dt, 0, 1);
	for (i = 0; i < 3; i++) {
		th[i] = u[i] * delta * k;
		ax[i] = u[i] * delta;
	}
	q_turn(c->q, th);
	learn_bias(c, t, ax, dt, delta * DEG);
	c->drift *= exp(-k);
	return 1;
}

int compass_gyro(struct compass *c, int64_t t, const double w[3])
{
	double dt, rate[3], th[3], dyaw, u[3];
	static const double up_w[3] = { 0, 0, 1 };
	int i;

	if (!finite3(w)) {
		c->stale++;
		return -1;
	}
	if (c->st == COMPASS_INIT) {
		memcpy(c->gyro_prev, w, sizeof(c->gyro_prev));
		c->n_gyro++;
		return 0;
	}
	if (c->gyro_t && t <= c->gyro_t) {
		c->stale++;
		return -1;
	}
	c->n_gyro++;
	if (!c->gyro_t) {
		/* first after init: start here (the init attitude is "now") */
		c->gyro_t = t;
		memcpy(c->gyro_prev, w, sizeof(c->gyro_prev));
		if (t > c->t)
			c->t = t;
		return 1;
	}
	if (t - c->gyro_t > GYRO_RESET_NS) {
		c->gaps++;
		compass_reset(c);
		memcpy(c->gyro_prev, w, sizeof(c->gyro_prev));
		return 0;
	}
	if (t - c->gyro_t > GYRO_GAP_NS) {
		/* unknown motion in the gap: skip it, converge fast after */
		c->gaps++;
		c->gyro_t = t;
		memcpy(c->gyro_prev, w, sizeof(c->gyro_prev));
		c->boost_until = t + BOOST_NS;
		c->drift += 5.0;
		if (t > c->t)
			c->t = t;
		return 1;
	}
	dt = (double)(t - c->gyro_t) / NS;
	for (i = 0; i < 3; i++) {
		rate[i] = 0.5 * (c->gyro_prev[i] + w[i]) - c->bias[i];
		th[i] = rate[i] * dt;
	}
	q_turn(c->q, th);
	c->gyro_t = t;
	memcpy(c->gyro_prev, w, sizeof(c->gyro_prev));
	if (t > c->t)
		c->t = t;
	/* heading uncertainty grows while the magnetometer is not correcting */
	if (c->disturbed || !c->mag_t || t - c->mag_t > NO_MAG_NS) {
		q_unrotate(c->q, up_w, u);
		dyaw = fabs(dot3(th, u)) * DEG;
		c->drift += DRIFT_RATE * dt + DRIFT_SCALE * dyaw;
	}
	return 1;
}

bool compass_output(struct compass *c, struct compass_out *o)
{
	double h, p, r, acc;
	bool level;

	if (c->st != COMPASS_RUN)
		return false;
	level = compass_angles(c->q, c->mount, &h, &p, &r);
	if (level) {
		c->last_heading = h;
		c->have_heading = true;
	} else if (c->have_heading) {
		h = c->last_heading;
	}
	acc = sqrt(ACC_FLOOR * ACC_FLOOR + c->innov_ms * DEG * DEG) + c->drift;
	if (!c->calibrated || !level || acc > 180)
		acc = 180;
	o->t = c->t;
	o->heading = h;
	o->pitch = p;
	o->roll = r;
	o->accuracy = acc;
	o->calibrated = c->calibrated;
	o->disturbed = c->disturbed;
	o->level_ok = level;
	memcpy(o->q, c->q, sizeof(o->q));
	return true;
}

double compass_round2(double v)
{
	v = round(v * 100) / 100;
	return v == 0 ? 0 : v;
}

/* 359.996 must not print as 360.00 */
static double round_heading(double h)
{
	return compass_wrap360(compass_round2(h));
}

int compass_format(char *buf, size_t len, const struct compass_out *o, bool have_decl, double decl,
		   const char *extra)
{
	int n = snprintf(buf, len, "{\"sensor\":\"heading\",\"t\":%" PRId64 ",\"heading\":%.2f,"
			 "\"pitch\":%.2f,\"roll\":%.2f,\"calibrated\":%s,\"disturbed\":%s,"
			 "\"accuracy\":%.1f%s",
			 o->t, round_heading(o->heading), compass_round2(o->pitch), compass_round2(o->roll),
			 o->calibrated ? "true" : "false", o->disturbed ? "true" : "false", o->accuracy,
			 extra ? extra : "");

	if (n < 0 || (size_t)n >= len)
		return n;
	if (have_decl)
		n += snprintf(buf + n, len - (size_t)n, ",\"true_heading\":%.2f,\"declination\":%.2f}\n",
			      round_heading(o->heading + decl), decl);
	else
		n += snprintf(buf + n, len - (size_t)n, "}\n");
	return n;
}

/* ------------------------------------------- live calibration state */

#define MC_BRACKET_NS	(NS * 12 / 100)	/* full samples around a factory one */
#define MC_NEAR_NS	(NS * 3 / 100)	/* or one full sample this close */
#define MC_PENDING_NS	(NS / 2)	/* a factory sample waits this long for a full one */
#define MC_STABLE_G	0.02		/* per-pair spread around the running mean */
#define MC_STABLE_NS	(3 * NS)
#define MC_THRESH_G	0.05		/* |factory - full| above this: bias applied */
#define MC_CHANGE_G	0.05		/* a new bias: tell the filter */

void compass_magcal_init(struct compass_magcal *m)
{
	memset(m, 0, sizeof(*m));
}

static int magcal_update(struct compass_magcal *m, int64_t t, const double d[3])
{
	double e[3], b[3];
	int i;

	m->pairs++;
	for (i = 0; i < 3; i++)
		e[i] = d[i] - m->ema[i];
	if (!m->have_ema || norm3(e) > MC_STABLE_G) {
		memcpy(m->ema, d, sizeof(m->ema));
		m->have_ema = true;
		m->stable_since = t;
	} else {
		for (i = 0; i < 3; i++)
			m->ema[i] += 0.2 * e[i];
	}
	if (t - m->stable_since < MC_STABLE_NS)
		return 0;
	for (i = 0; i < 3; i++)
		b[i] = m->ema[i] - m->bias[i];
	if (m->known && (norm3(m->ema) > MC_THRESH_G) == m->calibrated && norm3(b) <= MC_CHANGE_G) {
		memcpy(m->bias, m->ema, sizeof(m->bias));	/* follows small drifts quietly */
		return 0;
	}
	m->known = true;
	m->calibrated = norm3(m->ema) > MC_THRESH_G;
	memcpy(m->bias, m->ema, sizeof(m->bias));
	m->changes++;
	return 1;
}

/* Pair factory sample (t, f) with the full samples: interpolated between
 * the two around it, or the nearest within 30 ms. 1: paired (*rc set),
 * 0: wait for a newer full sample, -1: give up. */
static int magcal_pair(struct compass_magcal *m, int64_t t, const double f[3], int *rc)
{
	int i, best = -1;
	int64_t bd = 0;

	if (!m->nf)
		return 0;
	for (i = 0; i + 1 < m->nf; i++)
		if (m->ft[i] <= t && t <= m->ft[i + 1] && m->ft[i + 1] - m->ft[i] <= MC_BRACKET_NS) {
			double a = (double)(t - m->ft[i]) / (double)(m->ft[i + 1] - m->ft[i] ? m->ft[i + 1] - m->ft[i] : 1);
			double d[3];
			int k;

			for (k = 0; k < 3; k++)
				d[k] = f[k] - (m->fv[i][k] + a * (m->fv[i + 1][k] - m->fv[i][k]));
			*rc = magcal_update(m, t, d);
			return 1;
		}
	for (i = 0; i < m->nf; i++) {
		int64_t dd = m->ft[i] > t ? m->ft[i] - t : t - m->ft[i];

		if (best < 0 || dd < bd) {
			best = i;
			bd = dd;
		}
	}
	if (bd <= MC_NEAR_NS && (m->ft[m->nf - 1] >= t || bd == 0)) {
		double d[3];
		int k;

		for (k = 0; k < 3; k++)
			d[k] = f[k] - m->fv[best][k];
		*rc = magcal_update(m, t, d);
		return 1;
	}
	if (m->ft[m->nf - 1] < t)
		return 0;	/* a newer full sample may still come */
	return -1;
}

int compass_magcal_full(struct compass_magcal *m, int64_t t, const double v[3])
{
	int i, k, rc = 0, r;

	if (!finite3(v) || (m->nf && t <= m->ft[m->nf - 1]))
		return 0;
	if (m->nf == MC_RING) {
		memmove(m->ft, m->ft + 1, (MC_RING - 1) * sizeof(m->ft[0]));
		memmove(m->fv, m->fv + 1, (MC_RING - 1) * sizeof(m->fv[0]));
		m->nf--;
	}
	m->ft[m->nf] = t;
	memcpy(m->fv[m->nf], v, sizeof(m->fv[0]));
	m->nf++;
	for (i = 0, k = 0; i < m->np; i++) {
		int got = 0;

		r = magcal_pair(m, m->pt[i], m->pv[i], &got);
		rc |= got;
		if (r == 0 && t - m->pt[i] <= MC_PENDING_NS) {
			m->pt[k] = m->pt[i];
			memcpy(m->pv[k], m->pv[i], sizeof(m->pv[0]));
			k++;
		}
	}
	m->np = k;
	return rc;
}

int compass_magcal_factory(struct compass_magcal *m, int64_t t, const double v[3])
{
	int rc = 0;

	if (!finite3(v))
		return 0;
	if (magcal_pair(m, t, v, &rc) != 0)
		return rc;
	if (m->np == MC_PENDING) {
		memmove(m->pt, m->pt + 1, (MC_PENDING - 1) * sizeof(m->pt[0]));
		memmove(m->pv, m->pv + 1, (MC_PENDING - 1) * sizeof(m->pv[0]));
		m->np--;
	}
	m->pt[m->np] = t;
	memcpy(m->pv[m->np], v, sizeof(m->pv[0]));
	m->np++;
	return 0;
}
