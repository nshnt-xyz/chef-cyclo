/*
 * Orientation and tilt from the accelerometer, net.hadess.SensorProxy's
 * AccelerometerOrientation and AccelerometerTilt. Our own implementation
 * of iio-sensor-proxy 3.9's rules (orientation.c there):
 *
 * - Its frame has upright portrait at y = -1 g; sensord's accel is the
 *   Android frame (upright y = +9.8, screen up z = +9.8), so the vector
 *   used is (-x, -y, z): upright portrait is "normal", top edge down
 *   "bottom-up", left edge up "left-up", right edge up "right-up".
 * - Angles in whole degrees (rounded half away from zero, as there): the
 *   portrait angle atan2(x, |(y, z)|) and the landscape angle
 *   atan2(y, |(x, z)|). Both beyond 35 degrees (the shared border): no
 *   change. Portrait beyond 35: left-up when positive, right-up when
 *   negative; else landscape beyond 35: bottom-up when positive, normal
 *   when negative; else no change (the hysteresis band). Upstream also
 *   checks a 5 degree same-axis limit, but only where the angle is
 *   already beyond 35 degrees, so it never changes the result; it is left
 *   out.
 * - Tilt atan2(z, |(x, y)|): within 20 degrees vertical, below 70
 *   tilted-up (positive) or tilted-down, else face-up (screen up) or
 *   face-down. No hysteresis.
 * A sample that is not finite changes nothing (orientation) or gives
 * undefined (tilt).
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdlib.h>

#include "sensor-proxy.h"

#define THRESHOLD_PORTRAIT  35
#define THRESHOLD_LANDSCAPE 35
#define THRESHOLD_TILT      20
#define THRESHOLD_FACE      70

static const char *const orientation_names[] = { "undefined", "normal", "bottom-up", "left-up", "right-up" };
static const char *const tilt_names[] = { "undefined", "vertical", "tilted-up", "tilted-down", "face-up",
					  "face-down" };

const char *sp_orientation_name(enum sp_orientation o)
{
	return orientation_names[o];
}

const char *sp_tilt_name(enum sp_tilt t)
{
	return tilt_names[t];
}

/* atan2(a, b) in whole degrees */
static int degrees(double a, double b)
{
	return (int)lround(atan2(a, b) * 180.0 / M_PI);
}

enum sp_orientation sp_orientation_calc(enum sp_orientation prev, double x, double y, double z)
{
	double ux = -x, uy = -y, uz = z;
	int portrait, landscape;

	if (!isfinite(x) || !isfinite(y) || !isfinite(z))
		return prev;
	portrait = degrees(ux, sqrt(uy * uy + uz * uz));
	landscape = degrees(uy, sqrt(ux * ux + uz * uz));
	if (abs(portrait) > THRESHOLD_PORTRAIT && abs(landscape) > THRESHOLD_LANDSCAPE)
		return prev;
	if (abs(portrait) > THRESHOLD_PORTRAIT)
		return portrait > 0 ? SP_OR_LEFT_UP : SP_OR_RIGHT_UP;
	if (abs(landscape) > THRESHOLD_LANDSCAPE)
		return landscape > 0 ? SP_OR_BOTTOM_UP : SP_OR_NORMAL;
	return prev;
}

enum sp_tilt sp_tilt_calc(double x, double y, double z)
{
	int tilt;

	if (!isfinite(x) || !isfinite(y) || !isfinite(z))
		return SP_TILT_UNDEFINED;
	tilt = degrees(z, sqrt(x * x + y * y));
	if (abs(tilt) <= THRESHOLD_TILT)
		return SP_TILT_VERTICAL;
	if (abs(tilt) < THRESHOLD_FACE)
		return tilt > 0 ? SP_TILT_UP : SP_TILT_DOWN;
	return tilt > 0 ? SP_TILT_FACE_UP : SP_TILT_FACE_DOWN;
}
