/*
 * gpsview.h - what the GPS pages show, from gpsd's TPV and SKY reports,
 * kept only while the GPS page asks for it (sys_gps_watch(true)): then
 * the app holds a "map" lease on gps-manager and reads gpsd while it
 * runs. Cleared when the page lets go. Positions here are shown on the
 * page only: never logged, never written to a file (captures mask them).
 */
#ifndef CHEF_GPSVIEW_H
#define CHEF_GPSVIEW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GPS_SATS_MAX 64

/* gpsd gnssid */
enum gps_gnss { GNSS_GPS = 0, GNSS_SBAS = 1, GNSS_GALILEO = 2, GNSS_BEIDOU = 3, GNSS_QZSS = 5,
		GNSS_GLONASS = 6, GNSS_NAVIC = 7 };

struct gps_sat {
	int gnss, svid;
	double az, el, ss;	/* degrees, degrees, dB-Hz; < 0 unknown */
	bool used;
};

struct gps_view {
	/* TPV */
	int mode;		/* 0 none yet, 1 no fix, 2 2D, 3 3D */
	double eph, alt, speed, lat, lon;	/* NAN when not reported */
	/* SKY */
	bool sky;
	double hdop, pdop;	/* NAN when not reported */
	int n_sats;
	struct gps_sat sats[GPS_SATS_MAX];
	/* this lease (monotonic ms; 0 none) */
	int64_t lease_ms;
	int64_t first_fix_ms;
};

extern struct gps_view gps_view;

/* The GPS page is seen (screen on): lease "map", read gpsd's TPV and SKY
 * while gps-manager runs it. false: release, forget the view. */
void sys_gps_watch(bool watch);
bool sys_gps_watching(void);
/* gps-manager's switch ("enable"/"disable"); -ENOTCONN without it. */
int sys_gps_set_enabled(bool on);

/* Pure parsers (tests). 0 when the line is that report. */
int gps_parse_tpv_view(const char *line, struct gps_view *v);
int gps_parse_sky_view(const char *line, struct gps_view *v);

/* Texts: "G05" style label, counts. */
void gps_sat_label(const struct gps_sat *s, char *buf, size_t n);
int gps_sats_used(const struct gps_view *v);
/* Constellations seen (GPS, GLONASS, Galileo, BeiDou order); counts. */
void gps_constellations(const struct gps_view *v, int used[4], int seen[4]);

#endif /* CHEF_GPSVIEW_H */
