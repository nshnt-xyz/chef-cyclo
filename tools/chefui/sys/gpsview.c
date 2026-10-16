/*
 * gpsview.c - the pure part of the GPS view (gpsview.h): gpsd JSON TPV
 * and SKY reports into struct gps_view, satellite labels and counts. No
 * sockets, no LVGL; unit tested in test-sys.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpsview.h"

struct gps_view gps_view;

/* "key": number within [p, end); false when absent or not a number. */
static bool json_num(const char *p, const char *end, const char *key, double *out)
{
	char pat[32];
	size_t pl = (size_t)snprintf(pat, sizeof(pat), "\"%s\":", key);

	while (p && p < end) {
		const char *k = strstr(p, pat);
		char *e;
		double v;

		if (!k || k >= end)
			return false;
		v = strtod(k + pl, &e);
		if (e != k + pl && e <= end && isfinite(v)) {
			*out = v;
			return true;
		}
		p = k + pl;
	}
	return false;
}

static bool json_true(const char *p, const char *end, const char *key)
{
	char pat[32];
	const char *k;

	snprintf(pat, sizeof(pat), "\"%s\":true", key);
	k = strstr(p, pat);
	return k && k < end;
}

int gps_parse_tpv_view(const char *line, struct gps_view *v)
{
	const char *end = line + strlen(line);
	double d;

	if (line[0] != '{' || !strstr(line, "\"class\":\"TPV\""))
		return -1;
	v->mode = json_num(line, end, "mode", &d) && d >= 0 && d <= 3 ? (int)d : 0;
	v->eph = json_num(line, end, "eph", &d) && d >= 0 ? d : NAN;
	/* gpsd 3.27: altMSL (and altHAE); older: alt */
	if (!json_num(line, end, "altMSL", &v->alt) && !json_num(line, end, "alt", &v->alt))
		v->alt = NAN;
	v->speed = json_num(line, end, "speed", &d) && d >= 0 ? d : NAN;
	v->lat = json_num(line, end, "lat", &d) && d >= -90 && d <= 90 ? d : NAN;
	v->lon = json_num(line, end, "lon", &d) && d >= -180 && d <= 180 ? d : NAN;
	if (v->mode < 2)
		v->lat = v->lon = v->alt = v->eph = v->speed = NAN;	/* no fix: nothing kept */
	return 0;
}

int gps_parse_sky_view(const char *line, struct gps_view *v)
{
	const char *end = line + strlen(line), *p, *arr;
	double d;

	if (line[0] != '{' || !strstr(line, "\"class\":\"SKY\""))
		return -1;
	arr = strstr(line, "\"satellites\":[");
	/* the DOPs come before the array. gpsd sends a DOP-only SKY (no satellites key) between
	 * its full ones: it updates the DOPs and keeps the satellites of the last full SKY; a
	 * SKY with the array, even an empty one, replaces them. */
	v->hdop = json_num(line, arr ? arr : end, "hdop", &d) && d >= 0 ? d : NAN;
	v->pdop = json_num(line, arr ? arr : end, "pdop", &d) && d >= 0 ? d : NAN;
	v->sky = true;
	if (!arr)
		return 0;
	v->n_sats = 0;
	p = arr + 14;
	while (v->n_sats < GPS_SATS_MAX) {
		const char *o = strchr(p, '{'), *c;
		struct gps_sat s = { .az = -1, .el = -1, .ss = -1 };
		double prn = 0, gnss = -1, svid = -1;

		c = o ? strchr(o, '}') : NULL;
		if (!o || !c || (strchr(p, ']') && strchr(p, ']') < o))
			break;
		json_num(o, c, "PRN", &prn);
		json_num(o, c, "gnssid", &gnss);
		json_num(o, c, "svid", &svid);
		if (json_num(o, c, "az", &d) && d >= 0 && d <= 360)
			s.az = d;
		if (json_num(o, c, "el", &d) && d >= -90 && d <= 90)
			s.el = d;
		if (json_num(o, c, "ss", &d) && d >= 0 && d < 100)
			s.ss = d;
		s.used = json_true(o, c, "used");
		s.gnss = gnss >= 0 ? (int)gnss : GNSS_GPS;
		s.svid = svid >= 0 ? (int)svid : (int)prn;
		v->sats[v->n_sats++] = s;
		p = c + 1;
	}
	return 0;
}

void gps_sat_label(const struct gps_sat *s, char *buf, size_t n)
{
	char c;

	switch (s->gnss) {
	case GNSS_GPS: c = 'G'; break;
	case GNSS_GLONASS: c = 'R'; break;
	case GNSS_GALILEO: c = 'E'; break;
	case GNSS_BEIDOU: c = 'C'; break;
	case GNSS_QZSS: c = 'J'; break;
	case GNSS_SBAS: c = 'S'; break;
	case GNSS_NAVIC: c = 'I'; break;
	default: c = '?'; break;
	}
	snprintf(buf, n, "%c%02d", c, s->svid % 1000);
}

int gps_sats_used(const struct gps_view *v)
{
	int n = 0;

	for (int i = 0; i < v->n_sats; i++)
		n += v->sats[i].used;
	return n;
}

void gps_constellations(const struct gps_view *v, int used[4], int seen[4])
{
	static const int order[4] = { GNSS_GPS, GNSS_GLONASS, GNSS_GALILEO, GNSS_BEIDOU };

	for (int k = 0; k < 4; k++)
		used[k] = seen[k] = 0;
	for (int i = 0; i < v->n_sats; i++)
		for (int k = 0; k < 4; k++)
			if (v->sats[i].gnss == order[k]) {
				seen[k]++;
				used[k] += v->sats[i].used;
			}
}
