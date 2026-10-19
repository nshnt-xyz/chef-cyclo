/*
 * gpsview.c - the pure part of the GPS view (gpsview.h): gpsd JSON TPV
 * and SKY reports into struct gps_view through the shared JSON reader
 * (../common/json.h), satellite labels and counts. No sockets, no LVGL;
 * unit tested in test-sys and built into the LVGL-free test-gpsclient.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/json.h"
#include "gpsview.h"

struct gps_view gps_view;

/* A finite number member; false when absent or not a number. */
static bool num(const struct json_obj *o, const char *key, double *out)
{
	double v;

	if (!json_get_double(o, key, &v) || !isfinite(v))
		return false;
	*out = v;
	return true;
}

/* LINE is a gpsd report of class CLS: its members in *O. */
static bool report(struct json_obj *o, const char *line, const char *cls)
{
	char c[8];

	return json_parse(o, line, strlen(line)) == JSON_OK && json_get_string(o, "class", c, sizeof(c)) &&
	       strcmp(c, cls) == 0;
}

int gps_parse_tpv_status(const char *line, int *mode, double *eph)
{
	struct json_obj o;
	long m;
	double e;

	if (!report(&o, line, "TPV") || !json_get_long(&o, "mode", &m) || m < 0 || m > 3)
		return -1;
	*mode = (int)m;
	*eph = num(&o, "eph", &e) && e >= 0 && e < 1e6 ? e : -1;
	return 0;
}

int gps_parse_tpv_view(const char *line, struct gps_view *v)
{
	struct json_obj o;
	double d;

	if (!report(&o, line, "TPV"))
		return -1;
	v->mode = num(&o, "mode", &d) && d >= 0 && d <= 3 ? (int)d : 0;
	v->eph = num(&o, "eph", &d) && d >= 0 ? d : NAN;
	/* gpsd 3.27: altMSL (and altHAE); older: alt */
	if (!num(&o, "altMSL", &v->alt) && !num(&o, "alt", &v->alt))
		v->alt = NAN;
	v->speed = num(&o, "speed", &d) && d >= 0 ? d : NAN;
	v->lat = num(&o, "lat", &d) && d >= -90 && d <= 90 ? d : NAN;
	v->lon = num(&o, "lon", &d) && d >= -180 && d <= 180 ? d : NAN;
	if (v->mode < 2)
		v->lat = v->lon = v->alt = v->eph = v->speed = NAN;	/* no fix: nothing kept */
	return 0;
}

int gps_parse_sky_view(const char *line, struct gps_view *v)
{
	const struct json_member *arr;
	struct json_span e;
	enum json_type t;
	struct json_obj o, so;
	size_t pos = 0;
	double d;

	if (!report(&o, line, "SKY"))
		return -1;
	/* gpsd sends a DOP-only SKY (no satellites member) between its full
	 * ones: it updates the DOPs and keeps the satellites of the last full
	 * SKY; a SKY with the array, even an empty one, replaces them. */
	v->hdop = num(&o, "hdop", &d) && d >= 0 ? d : NAN;
	v->pdop = num(&o, "pdop", &d) && d >= 0 ? d : NAN;
	v->sky = true;
	arr = json_find(&o, "satellites");
	if (!arr || arr->type != JSON_ARRAY)
		return 0;
	v->n_sats = 0;
	while (v->n_sats < GPS_SATS_MAX && json_array_next(arr->val, &pos, &t, &e)) {
		struct gps_sat s = { .az = -1, .el = -1, .ss = -1 };
		double prn = 0, gnss = -1, svid = -1;
		bool used = false;

		if (t != JSON_OBJECT || json_parse(&so, e.p, e.len) != JSON_OK)
			continue;
		num(&so, "PRN", &prn);
		num(&so, "gnssid", &gnss);
		num(&so, "svid", &svid);
		if (num(&so, "az", &d) && d >= 0 && d <= 360)
			s.az = d;
		if (num(&so, "el", &d) && d >= -90 && d <= 90)
			s.el = d;
		if (num(&so, "ss", &d) && d >= 0 && d < 100)
			s.ss = d;
		s.used = json_get_bool(&so, "used", &used) && used;
		s.gnss = gnss >= 0 ? (int)gnss : GNSS_GPS;
		s.svid = svid >= 0 ? (int)svid : (int)prn;
		v->sats[v->n_sats++] = s;
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
