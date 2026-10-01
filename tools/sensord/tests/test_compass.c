/* Unit tests for compass.c on synthetic motion with a known attitude:
 * a simulated phone (true attitude from heading psi, pitch theta and a
 * roll phi about the horizontal forward axis) produces accel, gyro and
 * magn samples at 50/50/20 Hz with noise, biases, disturbances and
 * timing faults, and the filter's output is checked against the truth.
 * compass.c is included so the quaternion helpers are shared. */
#include "../compass.c"

#include <stdio.h>
#include <stdlib.h>

static int failures;

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
		failures++; \
	} \
} while (0)

#define CHECKF(cond, fmt, ...) do { \
	if (!(cond)) { \
		fprintf(stderr, "%s:%d: CHECK(%s) failed: " fmt "\n", __FILE__, __LINE__, #cond, \
			__VA_ARGS__); \
		failures++; \
	} \
} while (0)

/* ------------------------------------------------------------ the sim */

struct sim {
	struct compass c;
	double bw[3];		/* world field, gauss (E, N, U) */
	double dist[3];		/* extra body-frame field (a magnet, steel) */
	double lin[3];		/* linear acceleration, world, m/s^2 */
	double gbias[3];	/* gyro bias, rad/s */
	double gscale;		/* gyro scale error (0 = none) */
	double na, ng, nm;	/* noise std: accel m/s^2, gyro rad/s, magn gauss */
	double shake;		/* random linear accel std, m/s^2 (vibration) */
	double osc;		/* deg: true attitude oscillation (vibration, 5..11 Hz) */
	/* the current segment: angles move linearly from a to a + d */
	int64_t seg_t0, seg_t1;
	double seg_a[3], seg_d[3];
	int64_t t;
	int64_t next_a, next_g, next_m;
	int64_t mag_delay;	/* magn samples delivered this late */
	bool no_gyro, no_mag;
	double psi, theta, phi;	/* deg: current true angles */
	uint64_t rng;
	/* pending late magn samples */
	int64_t late_t[16];
	double late_m[16][3];
	int nlate;
	/* sampled output */
	struct compass_out o;
	bool have_out;
	unsigned long outs;
};

static double urand(struct sim *s)
{
	s->rng = s->rng * 6364136223846793005ULL + 1442695040888963407ULL;
	return ((s->rng >> 11) + 0.5) / 9007199254740992.0;
}

static double gauss(struct sim *s)
{
	return sqrt(-2 * log(urand(s))) * cos(2 * M_PI * urand(s));
}

static void axis_angle_q(const double ax[3], double deg, double q[4])
{
	double a = deg * RAD, n = norm3(ax);

	q[0] = cos(a / 2);
	q[1] = ax[0] / n * sin(a / 2);
	q[2] = ax[1] / n * sin(a / 2);
	q[3] = ax[2] / n * sin(a / 2);
}

/* True attitude: heading psi (clockwise from north, of the portrait
 * forward vector), pitch theta (top edge up from flat), then roll phi
 * about the body axis that is horizontal-forward at that pitch. */
static void true_q(double psi, double theta, double phi, double q[4])
{
	double qz[4], qx[4], qr[4];
	double z[3] = { 0, 0, 1 }, x[3] = { 1, 0, 0 };
	double f[3] = { 0, cos(theta * RAD), -sin(theta * RAD) };

	axis_angle_q(z, -psi, qz);
	axis_angle_q(x, theta, qx);
	axis_angle_q(f, phi, qr);
	q_mul(qz, qx, q);
	q_mul(q, qr, q);
	q_normalise(q);
}

static void sim_init(struct sim *s, enum compass_mount mount, uint64_t seed)
{
	memset(s, 0, sizeof(*s));
	compass_init(&s->c, mount);
	compass_set_calibrated(&s->c, true);
	/* a field like the lab's: 0.32 G horizontal, 0.2 G down */
	s->bw[0] = 0;
	s->bw[1] = 0.32;
	s->bw[2] = -0.20;
	s->na = 0.03;
	s->ng = 0.002;
	s->nm = 0.002;
	s->t = 1000 * NS;
	s->next_a = s->t;
	s->next_g = s->t + NS / 200;
	s->next_m = s->t + NS / 100;
	s->rng = seed;
}

static void sample_out(struct sim *s)
{
	if (compass_output(&s->c, &s->o)) {
		s->have_out = true;
		s->outs++;
	}
}

static void deliver_late(struct sim *s, int64_t now)
{
	int i, k = 0;

	for (i = 0; i < s->nlate; i++) {
		if (s->late_t[i] + s->mag_delay <= now) {
			compass_magn(&s->c, s->late_t[i], s->late_m[i]);
		} else {
			s->late_t[k] = s->late_t[i];
			memcpy(s->late_m[k], s->late_m[i], sizeof(s->late_m[k]));
			k++;
		}
	}
	s->nlate = k;
}

/* True attitude at time t (ns, may be fractional) in the current segment,
 * with the vibration oscillation on top (a body-frame rotation). */
static void sim_att(const struct sim *s, double t, double q[4])
{
	double f = s->seg_t1 > s->seg_t0 ? (t - s->seg_t0) / (double)(s->seg_t1 - s->seg_t0) : 1;

	true_q(s->seg_a[0] + s->seg_d[0] * f, s->seg_a[1] + s->seg_d[1] * f,
	       s->seg_a[2] + s->seg_d[2] * f, q);
	if (s->osc) {
		double ts = t / NS, th[3];

		th[0] = s->osc * RAD * sin(2 * M_PI * 7 * ts);
		th[1] = s->osc * RAD * sin(2 * M_PI * 11 * ts + 1);
		th[2] = s->osc * RAD * sin(2 * M_PI * 5 * ts + 2);
		q_turn(q, th);
	}
}

/* Move the angles linearly by (dpsi, dtheta, dphi) over dur seconds. */
static void sim_move(struct sim *s, double dur, double dpsi, double dtheta, double dphi)
{
	int64_t t1 = s->t + (int64_t)(dur * NS);

	s->seg_t0 = s->t;
	s->seg_t1 = t1;
	s->seg_a[0] = s->psi;
	s->seg_a[1] = s->theta;
	s->seg_a[2] = s->phi;
	s->seg_d[0] = dpsi;
	s->seg_d[1] = dtheta;
	s->seg_d[2] = dphi;
	for (;;) {
		int64_t t = s->next_g;
		int which = 0;
		double q[4], v[3], g[3] = { 0, 0, G0 }, b[3];
		int i;

		if (s->next_a < t) {
			t = s->next_a;
			which = 1;
		}
		if (s->next_m < t) {
			t = s->next_m;
			which = 2;
		}
		if (t > t1)
			break;
		sim_att(s, (double)t, q);
		s->t = t;
		deliver_late(s, t);
		if (which == 0) {
			/* body rate by central difference of the true attitude */
			double h = 0.0005, qa[4], qb[4], qi[4], d[4], a, sn;

			sim_att(s, t - h * NS, qa);
			sim_att(s, t + h * NS, qb);
			qi[0] = qa[0];
			qi[1] = -qa[1];
			qi[2] = -qa[2];
			qi[3] = -qa[3];
			q_mul(qi, qb, d);
			q_normalise(d);
			sn = sqrt(d[1] * d[1] + d[2] * d[2] + d[3] * d[3]);
			a = 2 * atan2(sn, d[0]);
			for (i = 0; i < 3; i++) {
				v[i] = sn > 1e-12 ? d[i + 1] / sn * a / (2 * h) : 0;
				v[i] = v[i] * (1 + s->gscale) + s->gbias[i] + s->ng * gauss(s);
			}
			if (!s->no_gyro)
				compass_gyro(&s->c, t, v);
			s->next_g += NS / 50;
		} else if (which == 1) {
			double lw[3];

			for (i = 0; i < 3; i++)
				lw[i] = g[i] + s->lin[i] + s->shake * gauss(s);
			q_unrotate(q, lw, v);
			for (i = 0; i < 3; i++)
				v[i] += s->na * gauss(s);
			compass_accel(&s->c, t, v);
			s->next_a += NS / 50;
		} else {
			q_unrotate(q, s->bw, b);
			for (i = 0; i < 3; i++)
				v[i] = b[i] + s->dist[i] + s->nm * gauss(s);
			if (!s->no_mag) {
				if (s->mag_delay && s->nlate < 16) {
					s->late_t[s->nlate] = t;
					memcpy(s->late_m[s->nlate], v, sizeof(v));
					s->nlate++;
				} else if (!s->mag_delay) {
					compass_magn(&s->c, t, v);
				}
			}
			s->next_m += NS / 20;
		}
		sample_out(s);
	}
	s->t = t1;
	s->psi += dpsi;
	s->theta += dtheta;
	s->phi += dphi;
}

static double hdiff(double a, double b)
{
	double d = fmod(a - b + 540.0, 360.0) - 180.0;

	return d;
}

/* Heading statistics over a hold: mean error against psi, max error, std. */
struct hstat {
	double mean, max, std;
	int n;
	bool disturbed_any;
	double acc_max;
};

static struct hstat sim_hold(struct sim *s, double dur, double psi_true)
{
	struct hstat h = { 0 };
	int64_t end = s->t + (int64_t)(dur * NS);
	double sum = 0, sum2 = 0;

	while (s->t < end) {
		double e;

		sim_move(s, 0.1, 0, 0, 0);
		if (!s->have_out)
			continue;
		e = hdiff(s->o.heading, psi_true);
		sum += e;
		sum2 += e * e;
		if (fabs(e) > h.max)
			h.max = fabs(e);
		if (s->o.disturbed)
			h.disturbed_any = true;
		if (s->o.accuracy > h.acc_max)
			h.acc_max = s->o.accuracy;
		h.n++;
	}
	if (h.n) {
		h.mean = sum / h.n;
		h.std = sqrt(fabs(sum2 / h.n - h.mean * h.mean));
	}
	return h;
}

/* ------------------------------------------------------------- tests */

static void test_angles_helper(void)
{
	double q[4], h, p, r;
	int m;

	/* flat, top edge north-east */
	true_q(45, 0, 0, q);
	CHECK(compass_angles(q, COMPASS_PORTRAIT, &h, &p, &r));
	CHECKF(fabs(hdiff(h, 45)) < 1e-6 && fabs(p) < 1e-6 && fabs(r) < 1e-6, "%f %f %f", h, p, r);
	CHECK(compass_angles(q, COMPASS_FLAT, &h, &p, &r) && fabs(hdiff(h, 45)) < 1e-6);
	/* flat: upright-only forward (-z) is vertical, no heading */
	CHECK(!compass_angles(q, COMPASS_UPRIGHT, &h, &p, &r));
	/* landscape-left: +x forward when flat: 90 deg clockwise of the top edge */
	CHECK(compass_angles(q, COMPASS_LANDSCAPE_LEFT, &h, &p, &r) && fabs(hdiff(h, 135)) < 1e-6);
	CHECK(compass_angles(q, COMPASS_LANDSCAPE_RIGHT, &h, &p, &r) && fabs(hdiff(h, -45)) < 1e-6);
	/* upright, back of the phone facing 200 deg */
	true_q(200, 90, 0, q);
	CHECK(compass_angles(q, COMPASS_PORTRAIT, &h, &p, &r));
	CHECKF(fabs(hdiff(h, 200)) < 1e-6 && fabs(p - 90) < 1e-6, "%f %f", h, p);
	CHECK(compass_angles(q, COMPASS_UPRIGHT, &h, &p, &r) && fabs(hdiff(h, 200)) < 1e-6);
	CHECK(!compass_angles(q, COMPASS_FLAT, &h, &p, &r));
	/* portrait at any pitch 0..90 and roll about forward: heading = psi */
	for (m = 0; m <= 90; m += 15) {
		true_q(300, m, m / 3.0, q);
		CHECK(compass_angles(q, COMPASS_PORTRAIT, &h, &p, &r));
		CHECKF(fabs(hdiff(h, 300)) < 1e-6, "pitch %d: %f", m, h);
	}
	/* roll sign: right edge down is positive */
	true_q(0, 0, 20, q);
	compass_angles(q, COMPASS_PORTRAIT, &h, &p, &r);
	CHECKF(fabs(r - 20) < 1e-6, "roll %f", r);
	/* the roll-invariant rule: upright and rolled 30 deg, still psi (the
	 * horizontal part of +y - z would be 27 deg off) */
	true_q(80, 90, 30, q);
	CHECK(compass_angles(q, COMPASS_PORTRAIT, &h, &p, &r) && fabs(hdiff(h, 80)) < 1e-6);
	/* on its edge (x vertical): no portrait heading */
	true_q(0, 0, 90, q);
	CHECK(!compass_angles(q, COMPASS_PORTRAIT, &h, &p, &r));
	true_q(0, 0, 70, q);
	CHECK(compass_angles(q, COMPASS_PORTRAIT, &h, &p, &r) && fabs(hdiff(h, 0)) < 1e-6);
	CHECK(compass_mount_parse("landscape-left") == COMPASS_LANDSCAPE_LEFT);
	CHECK(compass_mount_parse("sideways") == -1);
	CHECK(strcmp(compass_mount_name(COMPASS_UPRIGHT), "upright") == 0);
	CHECK(compass_wrap360(-0.0) == 0 && compass_wrap360(-90) == 270 && compass_wrap360(720.5) == 0.5);
}

static void test_startup(void)
{
	static const double headings[] = { 0, 37, 90, 180, 271, 359 };
	struct sim s;
	unsigned i;

	for (i = 0; i < sizeof(headings) / sizeof(headings[0]); i++) {
		struct hstat h;

		sim_init(&s, COMPASS_PORTRAIT, 1 + i);
		s.psi = headings[i];
		/* no output before accel and magn */
		sim_move(&s, 0.04, 0, 0, 0);
		CHECK(!s.have_out);
		sim_move(&s, 0.2, 0, 0, 0);
		CHECK(s.have_out);
		CHECKF(fabs(hdiff(s.o.heading, headings[i])) < 2, "start %f: %f", headings[i],
		       s.o.heading);
		h = sim_hold(&s, 5, headings[i]);
		CHECKF(h.max < 1.0 && !h.disturbed_any, "psi %f: max %f", headings[i], h.max);
		CHECKF(s.o.accuracy < 5, "accuracy %f", s.o.accuracy);
		CHECK(s.o.calibrated && s.o.level_ok);
	}
	/* upright start (handlebar pose), tilted 70 deg */
	sim_init(&s, COMPASS_PORTRAIT, 9);
	s.psi = 123;
	s.theta = 70;
	{
		struct hstat h = sim_hold(&s, 3, 123);

		CHECKF(h.max < 2 && fabs(s.o.pitch - 70) < 1, "upright start: max %f pitch %f", h.max,
		       s.o.pitch);
	}
	/* no gyro at all: accel + magn alone still give the heading */
	sim_init(&s, COMPASS_PORTRAIT, 10);
	s.no_gyro = true;
	s.psi = 250;
	{
		struct hstat h = sim_hold(&s, 3, 250);

		CHECKF(h.max < 2, "no gyro: %f", h.max);
	}
	/* no magn: never initialised, never output */
	sim_init(&s, COMPASS_PORTRAIT, 11);
	s.no_mag = true;
	sim_move(&s, 2, 30, 0, 0);
	CHECK(!s.have_out && s.c.st == COMPASS_INIT);
}

/* Four flat 90 deg turns (acceptance 1) and a quick 90 deg turn that must
 * settle within 1 s (acceptance 4). */
static void test_turns(void)
{
	struct sim s;
	double prev;
	int k;

	sim_init(&s, COMPASS_PORTRAIT, 21);
	s.psi = 10;
	sim_hold(&s, 3, 10);
	prev = s.o.heading;
	for (k = 0; k < 4; k++) {
		double want = 10 + 90 * (k + 1);
		struct hstat h;

		sim_move(&s, 3, 90, 0, 0);		/* 30 deg/s */
		CHECKF(fabs(hdiff(s.o.heading, want)) < 2, "turn %d end: %f", k, s.o.heading);
		h = sim_hold(&s, 3, want);
		CHECKF(fabs(hdiff(hdiff(s.o.heading, prev), 90)) < 2, "turn %d step %f", k,
		       hdiff(s.o.heading, prev));
		CHECKF(h.max < 2 && !h.disturbed_any, "turn %d hold max %f", k, h.max);
		prev = s.o.heading;
	}
	/* quick turn: 90 deg in 0.4 s (225 deg/s), magn 40 ms late */
	s.mag_delay = 40 * NS / 1000;
	sim_move(&s, 0.4, -90, 0, 0);
	sim_move(&s, 1.0, 0, 0, 0);
	CHECKF(fabs(hdiff(s.o.heading, s.psi)) < 2, "quick turn +1 s: %f vs %f", s.o.heading, s.psi);
	{
		struct hstat h = sim_hold(&s, 2, s.psi);

		CHECKF(h.max < 2, "after quick turn %f", h.max);
	}
	/* a full slow circle with the gyro 2 % off in scale: the magnetometer
	 * keeps it honest */
	s.gscale = 0.02;
	s.mag_delay = 0;
	for (k = 0; k < 12; k++) {
		sim_move(&s, 1, 30, 0, 0);
		CHECKF(fabs(hdiff(s.o.heading, s.psi)) < 4, "scaled gyro circle %d: %f vs %f", k,
		       s.o.heading, s.psi);
	}
}

/* Tilt invariance (acceptance 2): flat -> upright and back, roll +-30,
 * pointing the same way. */
static void test_tilt(void)
{
	struct sim s;
	int k;

	sim_init(&s, COMPASS_PORTRAIT, 31);
	s.psi = 75;
	sim_hold(&s, 3, 75);
	for (k = 0; k < 6; k++) {
		sim_move(&s, 0.5, 0, 15, 0);
		CHECKF(fabs(hdiff(s.o.heading, 75)) < 2, "pitch %f: %f", s.theta, s.o.heading);
	}
	CHECKF(fabs(s.o.pitch - 90) < 2, "pitch at upright %f", s.o.pitch);
	sim_move(&s, 1, 0, 0, 30);
	CHECKF(fabs(hdiff(s.o.heading, 75)) < 2, "roll +30 upright: %f", s.o.heading);
	sim_move(&s, 2, 0, 0, -60);
	CHECKF(fabs(hdiff(s.o.heading, 75)) < 2, "roll -30 upright: %f", s.o.heading);
	sim_move(&s, 1, 0, 0, 30);
	sim_move(&s, 3, 0, -90, 0);
	CHECKF(fabs(hdiff(s.o.heading, 75)) < 2 && fabs(s.o.pitch) < 2, "back flat: %f pitch %f",
	       s.o.heading, s.o.pitch);
	sim_move(&s, 1, 0, 0, 30);
	CHECKF(fabs(hdiff(s.o.heading, 75)) < 2, "roll +30 flat: %f", s.o.heading);
	/* handlebar pose 60 deg, rolled: roll reads back */
	sim_move(&s, 1, 0, 60, -30);
	CHECKF(fabs(hdiff(s.o.heading, 75)) < 2, "60 deg pitch: %f", s.o.heading);
}

/* Vibration (acceptance 3): shaking while pointing the same way. */
static void test_vibration(void)
{
	struct sim s;
	struct hstat h;

	sim_init(&s, COMPASS_PORTRAIT, 41);
	s.psi = 300;
	s.theta = 60;
	sim_hold(&s, 3, 300);
	s.shake = 4.0;		/* m/s^2 random per sample: |a| often off by > 15 % */
	s.osc = 1.0;		/* +-1 deg true shaking about every axis */
	h = sim_hold(&s, 10, 300);
	CHECKF(h.std < 1.5 && h.max < 5, "shake std %f max %f", h.std, h.max);
	/* a sustained 0.5 g braking push must not tilt the heading away */
	s.shake = s.osc = 0;
	s.lin[1] = -4.9;
	h = sim_hold(&s, 2, 300);
	CHECKF(h.max < 3, "braking max %f", h.max);
	s.lin[1] = 0;
}

/* Magnetic disturbance: gated, gyro holds, recovers with hysteresis. */
static void test_disturbance(void)
{
	struct sim s;
	struct hstat h;
	double acc0;

	sim_init(&s, COMPASS_PORTRAIT, 51);
	s.psi = 30;
	s.gbias[2] = 0.003;	/* 0.17 deg/s, learned before the disturbance */
	sim_hold(&s, 20, 30);
	CHECK(!s.o.disturbed);
	acc0 = s.o.accuracy;
	/* a magnet next to the phone for 5 s: 0.25 G sideways (|B| and the
	 * inclination barely change: caught by the yaw innovation) */
	s.dist[0] = 0.25;
	h = sim_hold(&s, 5, 30);
	CHECK(h.disturbed_any && s.o.disturbed);
	CHECKF(h.max < 2, "heading held under disturbance: max %f", h.max);
	CHECKF(s.o.accuracy > acc0, "accuracy grows: %f -> %f", acc0, s.o.accuracy);
	/* turn while disturbed: the gyro carries it */
	sim_move(&s, 2, 60, 0, 0);
	CHECKF(fabs(hdiff(s.o.heading, 90)) < 3, "turn while disturbed: %f", s.o.heading);
	CHECK(s.o.disturbed);
	/* removed: not back before 1 s calm (on 0.5 s smoothed values), back after */
	s.dist[0] = 0;
	sim_move(&s, 0.8, 0, 0, 0);
	CHECK(s.o.disturbed);
	sim_move(&s, 1.5, 0, 0, 0);
	CHECK(!s.o.disturbed);
	h = sim_hold(&s, 5, 90);
	CHECKF(h.max < 3, "after disturbance %f", h.max);
	/* hysteresis: a field 10 % stronger (inside the 25 % gate) never trips it */
	sim_init(&s, COMPASS_PORTRAIT, 52);
	s.psi = 200;
	sim_hold(&s, 5, 200);
	s.dist[0] = 0;
	{
		double q[4], b[3];
		int i;

		true_q(200, 0, 0, q);
		q_unrotate(q, s.bw, b);
		for (i = 0; i < 3; i++)
			s.dist[i] = 0.10 * b[i];
	}
	h = sim_hold(&s, 5, 200);
	CHECK(!h.disturbed_any);
	/* inclination change alone (steel below the phone): gated too */
	sim_init(&s, COMPASS_PORTRAIT, 53);
	s.psi = 0;
	sim_hold(&s, 5, 0);
	s.dist[2] = -0.15;	/* body z = world down here */
	s.dist[1] = -0.05;
	h = sim_hold(&s, 3, 0);
	CHECK(h.disturbed_any);
	CHECKF(h.max < 2, "inclination gate: %f", h.max);
	CHECK(s.c.disturbances >= 1);
}

/* A changed environment: steady new field for > 10 s is taken as the
 * reference; heading follows the new field after that. */
static void test_reacquire(void)
{
	struct sim s;
	struct hstat h;

	sim_init(&s, COMPASS_PORTRAIT, 61);
	s.psi = 100;
	sim_hold(&s, 5, 100);
	s.bw[1] = 0.45;		/* 40 % stronger horizontal field, same direction */
	sim_hold(&s, 2, 100);
	CHECK(s.o.disturbed);
	sim_hold(&s, 12, 100);
	CHECK(!s.o.disturbed && s.c.reacquired == 1);
	h = sim_hold(&s, 3, 100);
	CHECKF(h.max < 2, "after reacquire %f", h.max);
	/* a noisy, unsteady disturbance is never taken as the reference */
	sim_init(&s, COMPASS_PORTRAIT, 62);
	s.psi = 100;
	sim_hold(&s, 5, 100);
	{
		int k;

		for (k = 0; k < 10; k++) {
			s.dist[0] = (k % 2) ? 0.45 : 0.15;
			sim_move(&s, 2, 0, 0, 0);
		}
	}
	CHECK(s.o.disturbed && s.c.reacquired == 0);
}

/* Gyro bias: learned while the magnetometer corrects; limits the drift
 * while disturbed. */
static void test_bias(void)
{
	struct sim s;
	struct hstat h;
	double acc0;

	sim_init(&s, COMPASS_PORTRAIT, 71);
	s.gbias[0] = 0.01;
	s.gbias[1] = -0.008;
	s.gbias[2] = 0.012;	/* 0.7 deg/s about up */
	s.psi = 45;
	sim_hold(&s, 60, 45);
	CHECKF(fabs(s.c.bias[2] - 0.012) < 0.002 && fabs(s.c.bias[0] - 0.01) < 0.004 &&
	       fabs(s.c.bias[1] + 0.008) < 0.004,
	       "bias %f %f %f", s.c.bias[0], s.c.bias[1], s.c.bias[2]);
	h = sim_hold(&s, 5, 45);
	CHECKF(h.max < 1, "with bias %f", h.max);
	acc0 = s.o.accuracy;
	/* 9 s without magnetometer help (a steady disturbance is taken as the
	 * new field after 10 s): drift stays small */
	s.dist[0] = 0.4;
	h = sim_hold(&s, 9, 45);
	CHECKF(h.max < 2 && s.o.disturbed, "9 s disturbed with learned bias: %f", h.max);
	CHECKF(s.o.accuracy > acc0 + 0.8, "accuracy while disturbed %f (before %f)", s.o.accuracy,
	       acc0);
}

/* Timing faults: out-of-order, duplicate, stale, gaps. */
static void test_timing(void)
{
	struct sim s;
	double w[3] = { 0, 0, 1.0 }, a[3] = { 0, 0, G0 }, m[3] = { 0, 0.32, -0.2 };
	unsigned long stale;
	int64_t t;
	double h0;

	sim_init(&s, COMPASS_PORTRAIT, 81);
	s.psi = 0;
	sim_hold(&s, 3, 0);
	t = s.c.gyro_t;
	stale = s.c.stale;
	/* an old gyro sample spinning fast: dropped, no effect */
	h0 = s.o.heading;
	CHECK(compass_gyro(&s.c, t - NS / 50, w) == -1);
	CHECK(compass_gyro(&s.c, t, w) == -1);		/* duplicate */
	CHECK(s.c.stale == stale + 2);
	sample_out(&s);
	CHECK(fabs(hdiff(s.o.heading, h0)) < 1e-9);
	/* stale accel / magn (older than 200 ms) */
	CHECK(compass_accel(&s.c, s.c.t - NS / 2, a) == -1);
	CHECK(compass_magn(&s.c, s.c.t - NS / 2, m) == -1);
	CHECK(compass_accel(&s.c, s.c.acc_t, a) == -1);
	/* one accel sample stamped 10 s in the future: dropped or not, the
	 * following samples are judged against the gyro's time and kept */
	{
		double a2[3] = { 0, 0, G0 };
		unsigned long st;

		compass_accel(&s.c, s.c.t + 10 * NS, a2);
		st = s.c.stale;
		sim_move(&s, 1, 0, 0, 0);
		CHECKF(s.c.stale - st < 3, "future stamp made %lu samples stale", s.c.stale - st);
	}
	/* not finite */
	a[0] = NAN;
	CHECK(compass_accel(&s.c, s.c.t + 1, a) == -1);
	a[0] = 0;
	/* a 0.5 s gap in everything while the phone turns 45 deg: not
	 * integrated (unknown), the magnetometer pulls it in quickly */
	s.no_gyro = true;
	s.no_mag = true;
	sim_move(&s, 0.5, 45, 0, 0);
	s.next_a = s.t;
	s.no_gyro = false;
	s.no_mag = false;
	{
		unsigned long gaps = s.c.gaps;

		sim_move(&s, 0.05, 0, 0, 0);
		CHECK(s.c.gaps == gaps + 1);
	}
	sim_move(&s, 1.5, 0, 0, 0);
	CHECKF(fabs(hdiff(s.o.heading, 45)) < 3, "after 0.5 s gap: %f", s.o.heading);
	/* a 3 s gap in gyro and magn (accel goes on): after 2 s plus 10
	 * accel samples the gyro counts as lost and the filter re-initialises
	 * (once), then from accel + magn when they are back */
	{
		unsigned long resets = s.c.resets, lost = s.c.gyro_lost;

		s.no_gyro = s.no_mag = true;
		sim_move(&s, 3, 90, 0, 0);
		s.no_gyro = s.no_mag = false;
		sim_move(&s, 0.5, 0, 0, 0);
		CHECK(s.c.resets == resets + 1 && s.c.gyro_lost == lost + 1);
	}
	sim_move(&s, 0.5, 0, 0, 0);
	CHECKF(fabs(hdiff(s.o.heading, 135)) < 2, "after re-init: %f", s.o.heading);
	/* magn samples arriving 80 ms late (after newer gyro samples) during a
	 * steady turn: the attitude at the sample time is used */
	s.mag_delay = 80 * NS / 1000;
	sim_move(&s, 3, 180, 0, 0);
	CHECKF(fabs(hdiff(s.o.heading, s.psi)) < 2, "late magn in a turn: %f vs %f", s.o.heading,
	       s.psi);
}

/* The calibration flag: accuracy 180 until calibrated; the jump when the
 * bias is learned does not leave the gate stuck "disturbed". */
static void test_calibration(void)
{
	struct sim s;
	struct hstat h;
	double q[4], b[3];
	int i;

	sim_init(&s, COMPASS_PORTRAIT, 91);
	compass_set_calibrated(&s.c, false);
	s.psi = 160;
	/* an uncalibrated magnetometer: a big body-frame offset */
	s.dist[0] = 0.4;
	s.dist[1] = 0.23;
	s.dist[2] = 0.25;
	sim_hold(&s, 5, 0);
	CHECK(!s.o.calibrated && s.o.accuracy == 180);
	/* the DSP learns the bias: samples jump, the flag changes (either order) */
	memset(s.dist, 0, sizeof(s.dist));
	sim_move(&s, 0.3, 0, 0, 0);
	compass_set_calibrated(&s.c, true);
	h = sim_hold(&s, 5, 160);
	CHECK(!s.o.disturbed && s.o.calibrated);
	CHECKF(fabs(hdiff(s.o.heading, 160)) < 2, "after calibration: %f", s.o.heading);
	/* the jump seeded the accuracy (here about 160 deg); it comes down as
	 * the innovations stay small */
	sim_hold(&s, 5, 160);
	CHECKF(s.o.accuracy < 10, "accuracy 10 s after calibration %f", s.o.accuracy);
	/* flag first, then the samples 1 s later */
	sim_init(&s, COMPASS_PORTRAIT, 92);
	compass_set_calibrated(&s.c, false);
	s.psi = 20;
	true_q(20, 0, 0, q);
	q_unrotate(q, s.bw, b);
	for (i = 0; i < 3; i++)
		s.dist[i] = 0.5 * b[i] + 0.1;
	sim_hold(&s, 5, 0);
	compass_set_calibrated(&s.c, true);
	sim_move(&s, 1, 0, 0, 0);
	memset(s.dist, 0, sizeof(s.dist));
	sim_hold(&s, 13, 20);
	h = sim_hold(&s, 3, 20);
	CHECKF(!h.disturbed_any && h.max < 2, "flag before samples: max %f disturbed %d", h.max,
	       h.disturbed_any);
}

/* Forward vector near vertical: heading held, accuracy 180. */
static void test_level(void)
{
	struct sim s;

	sim_init(&s, COMPASS_PORTRAIT, 101);
	s.psi = 250;
	sim_hold(&s, 3, 250);
	sim_move(&s, 2, 0, 0, 90);	/* on its right edge: no heading */
	CHECK(!s.o.level_ok && s.o.accuracy == 180);
	CHECKF(fabs(hdiff(s.o.heading, 250)) < 2, "held heading %f", s.o.heading);
	sim_move(&s, 2, 0, 0, -90);
	sim_hold(&s, 1, 250);
	CHECK(s.o.level_ok && s.o.accuracy < 10);
	/* landscape mount: heading of the right edge */
	sim_init(&s, COMPASS_LANDSCAPE_LEFT, 102);
	s.psi = 10;
	{
		struct hstat h = sim_hold(&s, 3, 100);

		CHECKF(h.max < 1, "landscape-left %f", h.max);
	}
	/* a reset keeps the bias and counts */
	s.c.bias[2] = 0.01;
	compass_reset(&s.c);
	CHECK(s.c.st == COMPASS_INIT && s.c.resets == 1 && s.c.bias[2] == 0.01);
	compass_reset(&s.c);
	CHECK(s.c.resets == 1);
}

/* The review's case: the ADSP learns its bias while the phone lies
 * still, so the field jumps by 100..135 deg of heading and the flag
 * changes. The bias integrator must not learn the jump (it used to learn
 * about 1 deg/s), so 30 s without magn afterwards stays on course, the
 * first line after the change shows the error in its accuracy, and the
 * clean field afterwards is not marked disturbed. */
static void test_calibration_jump(void)
{
	static const double offs[][3] = { { 0.3, -0.4, 0 }, { -0.2, -0.5, 0.1 } };
	unsigned k;

	for (k = 0; k < 2; k++) {
		struct sim s;
		struct hstat h;
		double before, jump;

		sim_init(&s, COMPASS_PORTRAIT, 111 + k);
		compass_set_calibrated(&s.c, false);
		s.psi = 0;
		memcpy(s.dist, offs[k], sizeof(s.dist));
		sim_hold(&s, 20, 0);
		before = s.o.heading;
		jump = fabs(hdiff(before, 0));
		CHECKF(jump > 95, "offset %u: jump only %f deg", k, jump);
		memset(s.dist, 0, sizeof(s.dist));
		compass_set_calibrated(&s.c, true);
		s.have_out = false;
		sim_move(&s, 0.06, 0, 0, 0);
		CHECKF(s.have_out && s.o.accuracy > 45, "offset %u: first accuracy after the jump %f (heading %f)",
		       k, s.o.accuracy, s.o.heading);
		h = sim_hold(&s, 10, 0);
		CHECKF(fabs(hdiff(s.o.heading, 0)) < 2, "offset %u: after 10 s %f", k, s.o.heading);
		CHECKF(fabs(s.c.bias[0]) < 0.002 && fabs(s.c.bias[1]) < 0.002 && fabs(s.c.bias[2]) < 0.002,
		       "offset %u: false bias %f %f %f rad/s", k, s.c.bias[0], s.c.bias[1], s.c.bias[2]);
		/* 30 s with no magnetometer: gyro only */
		s.no_mag = true;
		h = sim_hold(&s, 30, 0);
		CHECKF(h.max < 3, "offset %u: 30 s without magn drifted %f deg", k, h.max);
		s.no_mag = false;
		h = sim_hold(&s, 5, 0);
		CHECKF(!h.disturbed_any && h.max < 3, "offset %u: magn back: disturbed %d max %f", k,
		       h.disturbed_any, h.max);
	}
}

/* Roll reads the right edge's angle below the horizontal, in the
 * handlebar pose too (Android's atan2 roll reads about 90 there). */
static void test_roll(void)
{
	static const double pitches[] = { 0, 30, 60, 85, 90 };
	struct sim s;
	unsigned i;

	for (i = 0; i < 5; i++) {
		double q[4], h, p, r;

		true_q(210, pitches[i], 30, q);
		compass_angles(q, COMPASS_PORTRAIT, &h, &p, &r);
		CHECKF(fabs(r - 30) < 1e-6 && fabs(hdiff(h, 210)) < 1e-6, "pitch %f: roll %f heading %f",
		       pitches[i], r, h);
		true_q(210, pitches[i], -20, q);
		compass_angles(q, COMPASS_PORTRAIT, &h, &p, &r);
		CHECKF(fabs(r + 20) < 1e-6, "pitch %f: roll %f (want -20)", pitches[i], r);
	}
	/* just past upright: still a small roll, not +-180 */
	{
		double q[4], h, p, r;

		true_q(0, 91, 0, q);
		compass_angles(q, COMPASS_PORTRAIT, &h, &p, &r);
		CHECKF(fabs(r) < 1e-6, "pitch 91: roll %f", r);
	}
	/* through the filter: handlebar pose, rolled 30 */
	sim_init(&s, COMPASS_PORTRAIT, 121);
	s.psi = 270;
	s.theta = 60;
	s.phi = 30;
	sim_hold(&s, 3, 270);
	CHECKF(fabs(s.o.roll - 30) < 1 && fabs(s.o.pitch - asin(sin(60 * RAD) * cos(30 * RAD)) * DEG) < 1,
	       "filter handlebar: roll %f pitch %f", s.o.roll, s.o.pitch);
}

/* anglvel stops for good: not silent. The filter re-initialises once
 * and goes on from accel + magn; when the gyro comes back it is used
 * again. */
static void test_gyro_lost(void)
{
	struct sim s;
	unsigned long outs;

	sim_init(&s, COMPASS_PORTRAIT, 131);
	s.psi = 40;
	sim_hold(&s, 3, 40);
	s.no_gyro = true;
	sim_move(&s, 2.5, 0, 0, 0);
	CHECK(s.c.gyro_lost == 1 && s.c.resets == 1);
	outs = s.outs;
	sim_move(&s, 5, 60, 0, 0);	/* a slow turn on accel + magn alone */
	CHECKF(s.outs > outs + 50, "silent without gyro: %lu lines", s.outs - outs);
	sim_hold(&s, 3, 100);
	CHECKF(fabs(hdiff(s.o.heading, 100)) < 2, "without gyro: %f", s.o.heading);
	CHECK(s.c.gyro_lost == 1);
	s.no_gyro = false;
	sim_move(&s, 2, 90, 0, 0);
	CHECKF(fabs(hdiff(s.o.heading, 190)) < 2 && s.c.gyro_t != 0, "gyro back: %f", s.o.heading);
	CHECK(s.c.gyro_lost == 1);
}

/* The live calibration state from full - factory pairs. */
static void test_magcal(void)
{
	struct compass_magcal m;
	int64_t t = 1000 * NS, tf;
	double full[3], fac[3], off[3] = { 0, 0, 0 };
	int k, changes = 0;

	compass_magcal_init(&m);
	/* full at 20 Hz, factory at 5 Hz 7 ms later, the field turning
	 * (a slow flat spin): interpolation keeps the difference clean */
	for (k = 0; k < 20 * 10; k++, t += NS / 20) {
		double a = 0.5 * (double)(t / 1000000) / 1000.0;

		full[0] = 0.3 * sin(a);
		full[1] = 0.3 * cos(a);
		full[2] = -0.2;
		changes += compass_magcal_full(&m, t, full);
		if (k % 4 == 0) {
			double af = a + 0.5 * 0.007;

			tf = t + 7000000;
			fac[0] = 0.3 * sin(af) + off[0];
			fac[1] = 0.3 * cos(af) + off[1];
			fac[2] = -0.2 + off[2];
			changes += compass_magcal_factory(&m, tf, fac);
		}
		if (k == 100) {
			/* the ADSP applies its learned bias: factory - full = 0.53 G */
			CHECK(m.known && !m.calibrated && norm3(m.bias) < 0.003);
			CHECK(changes == 1);
			off[0] = 0.401;
			off[1] = 0.229;
			off[2] = 0.254;
		}
	}
	CHECKF(m.known && m.calibrated && fabs(m.bias[0] - 0.401) < 0.003 && fabs(m.bias[2] - 0.254) < 0.003,
	       "bias %f %f %f known %d cal %d", m.bias[0], m.bias[1], m.bias[2], m.known, m.calibrated);
	CHECK(changes == 2 && m.changes == 2);
	CHECKF(m.pairs >= 45, "pairs %lu", m.pairs);
	/* not before 3 s of stability: a 2 s glimpse does not count */
	compass_magcal_init(&m);
	for (k = 0, t = 0; k < 40; k++, t += NS / 20) {
		double z[3] = { 0.1, 0.2, 0.3 }, f2[3] = { 0.5, 0.2, 0.3 };

		compass_magcal_full(&m, t, z);
		compass_magcal_factory(&m, t + 1000000, f2);
	}
	CHECK(!m.known);
	/* a factory sample far from any full one is not paired */
	compass_magcal_init(&m);
	full[0] = full[1] = full[2] = 0.1;
	compass_magcal_full(&m, 0, full);
	compass_magcal_full(&m, NS, full);
	compass_magcal_factory(&m, NS / 2, full);
	CHECK(m.pairs == 0 && m.np == 0);
	/* out-of-order full samples are dropped; NaN ignored */
	compass_magcal_full(&m, NS / 2, full);
	CHECK(m.nf == 2);
	full[0] = NAN;
	CHECK(compass_magcal_full(&m, 2 * NS, full) == 0 && m.nf == 2);
}

int main(void)
{
	test_angles_helper();
	test_startup();
	test_turns();
	test_tilt();
	test_vibration();
	test_disturbance();
	test_reacquire();
	test_bias();
	test_timing();
	test_calibration();
	test_level();
	test_calibration_jump();
	test_roll();
	test_gyro_lost();
	test_magcal();
	if (failures) {
		fprintf(stderr, "test_compass: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_compass: all passed\n");
	return 0;
}
