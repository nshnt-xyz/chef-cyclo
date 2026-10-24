/* Host tests for tools/sensor-proxy/orient.c: iio-sensor-proxy 3.9's
 * orientation test vectors (test-orientation.c there: the four axes, the
 * threshold sequence, the mount-matrix cases after their matrix), turned
 * into sensord's frame (x, y negated) and m/s^2; the six poses as sensord
 * reports them; the 35 degree thresholds, the band that keeps the previous
 * value and the shared border; tilt at its 20 and 70 degree edges; samples
 * that are not finite. Build/run: see tools/sensor-proxy/Makefile ("make
 * test").
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "sensor-proxy.h"

static int g_failures;
static int g_tests;

#define CHECK(cond) do { \
	g_tests++; \
	if (!(cond)) { \
		g_failures++; \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	} \
} while (0)

#define G 9.81
#define ONEG 256.0	/* upstream's 1 g */

/* An upstream-frame vector in upstream units (ONEG = 1 g, or SCALE m/s^2
 * per unit) as sensord would report it. */
static enum sp_orientation up(enum sp_orientation prev, double x, double y, double z, double scale)
{
	return sp_orientation_calc(prev, -x * scale, -y * scale, z * scale);
}

static void test_upstream_vectors(void)
{
	static const struct { double x, y, z; enum sp_orientation want; } axes[] = {
		{ 0, -ONEG, 0, SP_OR_NORMAL },
		{ -ONEG, 0, 0, SP_OR_RIGHT_UP },
		{ ONEG, 0, 0, SP_OR_LEFT_UP },
		{ 0, ONEG, 0, SP_OR_BOTTOM_UP },
	};
	/* each from the previous one's expected value */
	static const struct { double x, y, z; enum sp_orientation want; } threshold[] = {
		{ 0, -ONEG, 0, SP_OR_NORMAL },
		{ 183, -ONEG, 0, SP_OR_NORMAL },
		{ 176, -ONEG, 0, SP_OR_NORMAL },
		{ 183, -ONEG, 0, SP_OR_NORMAL },
	};
	/* readings after their mount matrix "0, -1, 0; -1, 0, 0; 0, 0, 1"
	 * (x' = -y, y' = -x), with their scale */
	static const struct { double x, y, z, scale; enum sp_orientation want; } mounted[] = {
		{ -13, -523, 5, 0.019163, SP_OR_NORMAL },		/* Onda v975 */
		{ -521, -8, -67, 0.019163, SP_OR_RIGHT_UP },
		{ 0, -24, -21, 0.306457, SP_OR_NORMAL },		/* Winbook TW100 */
		{ 25, -15, -14, 0.306457, SP_OR_LEFT_UP },
	};
	enum sp_orientation prev = SP_OR_UNDEFINED;
	unsigned i;

	for (i = 0; i < sizeof(axes) / sizeof(axes[0]); i++)
		CHECK(up(SP_OR_UNDEFINED, axes[i].x, axes[i].y, axes[i].z, G / ONEG) == axes[i].want);
	for (i = 0; i < sizeof(threshold) / sizeof(threshold[0]); i++) {
		CHECK(up(prev, threshold[i].x, threshold[i].y, threshold[i].z, G / ONEG) == threshold[i].want);
		prev = threshold[i].want;
	}
	for (i = 0; i < sizeof(mounted) / sizeof(mounted[0]); i++)
		CHECK(up(SP_OR_UNDEFINED, mounted[i].x, mounted[i].y, mounted[i].z, mounted[i].scale) ==
		      mounted[i].want);
}

/* The six poses, as sensord reports them (Android frame). */
static void test_poses(void)
{
	/* upright portrait: y up */
	CHECK(sp_orientation_calc(SP_OR_UNDEFINED, 0, G, 0) == SP_OR_NORMAL);
	CHECK(sp_tilt_calc(0, G, 0) == SP_TILT_VERTICAL);
	/* top edge down */
	CHECK(sp_orientation_calc(SP_OR_NORMAL, 0, -G, 0) == SP_OR_BOTTOM_UP);
	CHECK(sp_tilt_calc(0, -G, 0) == SP_TILT_VERTICAL);
	/* left edge up: gravity's reaction along -x */
	CHECK(sp_orientation_calc(SP_OR_NORMAL, -G, 0, 0) == SP_OR_LEFT_UP);
	/* right edge up */
	CHECK(sp_orientation_calc(SP_OR_NORMAL, G, 0, 0) == SP_OR_RIGHT_UP);
	CHECK(sp_tilt_calc(G, 0, 0) == SP_TILT_VERTICAL);
	/* screen up and down: tilt says it, the orientation keeps the last one */
	CHECK(sp_tilt_calc(0, 0, G) == SP_TILT_FACE_UP);
	CHECK(sp_tilt_calc(0, 0, -G) == SP_TILT_FACE_DOWN);
	CHECK(sp_orientation_calc(SP_OR_LEFT_UP, 0, 0, G) == SP_OR_LEFT_UP);
	CHECK(sp_orientation_calc(SP_OR_UNDEFINED, 0, 0, -G) == SP_OR_UNDEFINED);
	/* the live face-up reading of the sensors guide */
	CHECK(sp_tilt_calc(0.17, 0.3, 10.09) == SP_TILT_FACE_UP);
	CHECK(sp_orientation_calc(SP_OR_UNDEFINED, 8.42253, 0.173035, 5.33611) == SP_OR_RIGHT_UP);
}

/* Lying screen up, the top edge raised by DEG degrees (negative: lowered):
 * upstream's landscape angle atan2(-y, |(x, z)|) is then -DEG. */
static enum sp_orientation landscape_deg(enum sp_orientation prev, double deg)
{
	double a = deg * M_PI / 180.0;

	return sp_orientation_calc(prev, 0, G * sin(a), G * cos(a));
}

static void test_thresholds(void)
{
	/* the landscape angle beyond 35 (rounded) decides; at 35 the band keeps */
	CHECK(landscape_deg(SP_OR_UNDEFINED, 35.6) == SP_OR_NORMAL);
	CHECK(landscape_deg(SP_OR_UNDEFINED, 35.4) == SP_OR_UNDEFINED);
	CHECK(landscape_deg(SP_OR_LEFT_UP, 35.4) == SP_OR_LEFT_UP);
	CHECK(landscape_deg(SP_OR_LEFT_UP, 36) == SP_OR_NORMAL);
	CHECK(landscape_deg(SP_OR_NORMAL, -36) == SP_OR_BOTTOM_UP);
	CHECK(landscape_deg(SP_OR_BOTTOM_UP, 20) == SP_OR_BOTTOM_UP);	/* the band */
	CHECK(landscape_deg(SP_OR_BOTTOM_UP, 40) == SP_OR_NORMAL);
	/* the shared border: both angles beyond 35, no change */
	CHECK(sp_orientation_calc(SP_OR_RIGHT_UP, -G * 0.7, G * 0.7, 0) == SP_OR_RIGHT_UP);
	CHECK(sp_orientation_calc(SP_OR_UNDEFINED, -G * 0.7, G * 0.7, 0) == SP_OR_UNDEFINED);
	/* portrait first when only it is beyond 35 */
	CHECK(sp_orientation_calc(SP_OR_NORMAL, -G * 0.7, G * 0.3, G * 0.5) == SP_OR_LEFT_UP);
	/* a zero vector changes nothing */
	CHECK(sp_orientation_calc(SP_OR_NORMAL, 0, 0, 0) == SP_OR_NORMAL);
}

/* Tilt DEG degrees from vertical (positive: screen towards up). */
static enum sp_tilt tilt_deg(double deg)
{
	double a = deg * M_PI / 180.0;

	return sp_tilt_calc(0, G * cos(a), G * sin(a));
}

static void test_tilt(void)
{
	CHECK(tilt_deg(0) == SP_TILT_VERTICAL);
	CHECK(tilt_deg(20.4) == SP_TILT_VERTICAL);
	CHECK(tilt_deg(-20.4) == SP_TILT_VERTICAL);
	CHECK(tilt_deg(20.6) == SP_TILT_UP);
	CHECK(tilt_deg(-20.6) == SP_TILT_DOWN);
	CHECK(tilt_deg(69.4) == SP_TILT_UP);
	CHECK(tilt_deg(-69.4) == SP_TILT_DOWN);
	CHECK(tilt_deg(69.6) == SP_TILT_FACE_UP);
	CHECK(tilt_deg(-69.6) == SP_TILT_FACE_DOWN);
	CHECK(tilt_deg(90) == SP_TILT_FACE_UP);
	CHECK(tilt_deg(-90) == SP_TILT_FACE_DOWN);
}

static void test_names_and_bad_samples(void)
{
	CHECK(strcmp(sp_orientation_name(SP_OR_UNDEFINED), "undefined") == 0);
	CHECK(strcmp(sp_orientation_name(SP_OR_NORMAL), "normal") == 0);
	CHECK(strcmp(sp_orientation_name(SP_OR_BOTTOM_UP), "bottom-up") == 0);
	CHECK(strcmp(sp_orientation_name(SP_OR_LEFT_UP), "left-up") == 0);
	CHECK(strcmp(sp_orientation_name(SP_OR_RIGHT_UP), "right-up") == 0);
	CHECK(strcmp(sp_tilt_name(SP_TILT_UNDEFINED), "undefined") == 0);
	CHECK(strcmp(sp_tilt_name(SP_TILT_VERTICAL), "vertical") == 0);
	CHECK(strcmp(sp_tilt_name(SP_TILT_UP), "tilted-up") == 0);
	CHECK(strcmp(sp_tilt_name(SP_TILT_DOWN), "tilted-down") == 0);
	CHECK(strcmp(sp_tilt_name(SP_TILT_FACE_UP), "face-up") == 0);
	CHECK(strcmp(sp_tilt_name(SP_TILT_FACE_DOWN), "face-down") == 0);
	CHECK(sp_orientation_calc(SP_OR_NORMAL, NAN, G, 0) == SP_OR_NORMAL);
	CHECK(sp_orientation_calc(SP_OR_NORMAL, 0, INFINITY, 0) == SP_OR_NORMAL);
	CHECK(sp_tilt_calc(0, 0, NAN) == SP_TILT_UNDEFINED);
}

int main(void)
{
	test_upstream_vectors();
	test_poses();
	test_thresholds();
	test_tilt();
	test_names_and_bad_samples();
	printf("test-orient: %d/%d checks passed\n", g_tests - g_failures, g_tests);
	return g_failures ? 1 : 0;
}
