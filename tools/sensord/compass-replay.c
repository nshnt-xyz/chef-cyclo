/* compass-replay: run the compass filter (compass.c, the code sensord
 * runs) over recorded sensord captures on the host and print heading
 * lines, so the filter can be evaluated and tuned on real data.
 *
 *   compass-replay [-m MOUNT] [-r HZ] [-u] [-d DEG] [-F FACTORY] [-s] FILE...
 *
 * FILEs are sensord JSON-lines captures (sensord watch output); lines with
 * "sensor" accel, anglvel or magn are used, anything else is skipped. The
 * samples of all files are merged in timestamp order (at equal stamps:
 * gyro, then accel, then magn) and fed to the filter as sensord feeds it.
 * Pass one magnetometer capture only (e.g. the *-magn-full one).
 *
 *   -m  mount: portrait (default), landscape-left, landscape-right, flat,
 *       upright
 *   -r  output rate in Hz, decimated like a sensord client (default 10);
 *       0 prints a line after every sample that changed the output
 *   -u  treat the magnetometer as not calibrated (default: calibrated,
 *       as the recorded full-calibration captures after learning are)
 *   -d  declination in degrees: adds true_heading
 *   -F  a magn capture at calibration "factory" recorded with the full
 *       one (the magcal run's *-magn-factory): the calibrated flag then
 *       comes from full minus factory as in sensord (compass_magcal),
 *       starting not calibrated, and lines carry cal_source and mag_bias
 *   -s  print a summary line to stderr at the end (counters)
 *
 * Output: sensord's heading lines ({"sensor":"heading",...}) on stdout.
 * tools/compass-check.py uses this to replay captures that have no
 * heading channel of their own. */
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "compass.h"

enum kind { K_GYRO, K_ACCEL, K_MAGN, K_FACTORY };

struct sample {
	int64_t t;
	int kind;
	double v[3];
};

static struct sample *g_s;
static size_t g_n, g_cap;

/* Where the value of "key" starts in a flat JSON object line (after the
 * colon and any blanks), or NULL. */
static const char *value_of(const char *line, const char *key)
{
	char pat[32];
	const char *p = line;
	size_t n;

	n = (size_t)snprintf(pat, sizeof(pat), "\"%s\"", key);
	while ((p = strstr(p, pat))) {
		const char *v = p + n;

		while (*v == ' ' || *v == '\t')
			v++;
		if (*v == ':') {
			v++;
			while (*v == ' ' || *v == '\t')
				v++;
			return v;
		}
		p += n;
	}
	return NULL;
}

/* Value of "key" as a double. */
static int field(const char *line, const char *key, double *out)
{
	const char *p = value_of(line, key);
	char *end;

	if (!p)
		return -1;
	errno = 0;
	*out = strtod(p, &end);
	if (end == p || errno)
		return -1;
	return 0;
}

static int load(const char *path, bool factory)
{
	FILE *f = fopen(path, "r");
	char line[1024];
	unsigned long used = 0;

	if (!f) {
		fprintf(stderr, "compass-replay: %s: %s\n", path, strerror(errno));
		return -1;
	}
	while (fgets(line, sizeof(line), f)) {
		struct sample s;
		const char *p = value_of(line, "sensor");
		const char *t;
		char *end;

		if (!p || *p != '"')
			continue;
		p++;
		if (strncmp(p, "anglvel\"", 8) == 0)
			s.kind = K_GYRO;
		else if (strncmp(p, "accel\"", 6) == 0)
			s.kind = K_ACCEL;
		else if (strncmp(p, "magn\"", 5) == 0)
			s.kind = factory ? K_FACTORY : K_MAGN;
		else
			continue;
		t = value_of(line, "t");
		if (!t)
			continue;
		errno = 0;
		s.t = strtoll(t, &end, 10);
		if (end == t || errno)
			continue;
		if (field(line, "x", &s.v[0]) || field(line, "y", &s.v[1]) || field(line, "z", &s.v[2]))
			continue;
		if (g_n == g_cap) {
			size_t cap = g_cap ? 2 * g_cap : 65536;
			struct sample *n = realloc(g_s, cap * sizeof(*n));

			if (!n) {
				fclose(f);
				return -1;
			}
			g_s = n;
			g_cap = cap;
		}
		g_s[g_n++] = s;
		used++;
	}
	fclose(f);
	fprintf(stderr, "compass-replay: %s: %lu samples\n", path, used);
	return 0;
}

static int cmp(const void *a, const void *b)
{
	const struct sample *x = a, *y = b;

	if (x->t != y->t)
		return x->t < y->t ? -1 : 1;
	return x->kind - y->kind;
}

int main(int argc, char **argv)
{
	enum compass_mount mount = COMPASS_PORTRAIT;
	double hz = 10, decl = 0;
	bool have_decl = false, calibrated = true, summary = false;
	const char *factory = NULL;
	struct compass_magcal mc;
	struct compass c;
	int64_t next_t = 0;
	unsigned long lines = 0;
	size_t i;
	int opt;

	while ((opt = getopt(argc, argv, "m:r:ud:F:s")) != -1) {
		switch (opt) {
		case 'm': {
			int m = compass_mount_parse(optarg);

			if (m < 0) {
				fprintf(stderr, "compass-replay: unknown mount %s\n", optarg);
				return 64;
			}
			mount = (enum compass_mount)m;
			break;
		}
		case 'r': hz = atof(optarg); break;
		case 'u': calibrated = false; break;
		case 'd': decl = atof(optarg); have_decl = true; break;
		case 'F': factory = optarg; break;
		case 's': summary = true; break;
		default:
			fprintf(stderr, "usage: compass-replay [-m MOUNT] [-r HZ] [-u] [-d DEG] [-F FACTORY] [-s] FILE...\n");
			return 64;
		}
	}
	if (optind >= argc || hz < 0) {
		fprintf(stderr, "usage: compass-replay [-m MOUNT] [-r HZ] [-u] [-d DEG] [-F FACTORY] [-s] FILE...\n");
		return 64;
	}
	for (; optind < argc; optind++)
		if (load(argv[optind], false))
			return 1;
	if (factory && load(factory, true))
		return 1;
	qsort(g_s, g_n, sizeof(*g_s), cmp);

	compass_init(&c, mount);
	compass_magcal_init(&mc);
	compass_set_calibrated(&c, factory ? false : calibrated);
	for (i = 0; i < g_n; i++) {
		const struct sample *s = &g_s[i];
		struct compass_out o;
		char buf[320], extra[96] = "";
		int rc;

		if (s->kind == K_FACTORY || (factory && s->kind == K_MAGN)) {
			/* as sensord: the live calibration state from full - factory */
			int ch = s->kind == K_FACTORY ? compass_magcal_factory(&mc, s->t, s->v)
						      : compass_magcal_full(&mc, s->t, s->v);

			if (ch && mc.known) {
				if (mc.calibrated != c.calibrated)
					compass_set_calibrated(&c, mc.calibrated);
				else if (mc.calibrated)
					compass_field_jump(&c);
				if (summary)
					fprintf(stderr, "compass-replay: t %.3f live calibration: %s, bias %.4f %.4f %.4f G\n",
						s->t / 1e9, mc.calibrated ? "calibrated" : "not calibrated",
						mc.bias[0], mc.bias[1], mc.bias[2]);
			}
			if (s->kind == K_FACTORY)
				continue;
		}
		rc = s->kind == K_GYRO ? compass_gyro(&c, s->t, s->v)
		     : s->kind == K_ACCEL ? compass_accel(&c, s->t, s->v) : compass_magn(&c, s->t, s->v);
		if (rc <= 0 || !compass_output(&c, &o))
			continue;
		if (hz > 0) {
			/* sensord's per-client decimation */
			int64_t period = (int64_t)(1e9 / hz);

			if (next_t && o.t + period / 20 < next_t)
				continue;
			next_t = next_t && o.t - next_t < period ? next_t + period : o.t + period;
		}
		if (factory)
			snprintf(extra, sizeof(extra), ",\"cal_source\":\"%s\",\"mag_bias\":%.3f",
				 mc.known ? "live" : "none", mc.known ? sqrt(mc.bias[0] * mc.bias[0] +
				 mc.bias[1] * mc.bias[1] + mc.bias[2] * mc.bias[2]) : 0.0);
		if (compass_format(buf, sizeof(buf), &o, have_decl, decl, extra) > 0) {
			fputs(buf, stdout);
			lines++;
		}
	}
	if (summary)
		fprintf(stderr, "compass-replay: %zu samples, %lu lines; accel %lu gyro %lu magn %lu, "
			"dropped %lu, gaps %lu, resets %lu, gyro lost %lu, disturbances %lu, reacquired %lu, "
			"gyro bias %.5f %.5f %.5f rad/s; full/factory pairs %lu\n", g_n, lines, c.n_accel,
			c.n_gyro, c.n_magn, c.stale, c.gaps, c.resets, c.gyro_lost, c.disturbances,
			c.reacquired, c.bias[0], c.bias[1], c.bias[2], mc.pairs);
	free(g_s);
	return 0;
}
