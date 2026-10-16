/*
 * fake.c - fixed system state from a file, for host runs, tests and the
 * reference captures (no real source is started with it). One field per
 * line, '#' comments:
 *
 *   time 14:16              status bar clock (frozen)
 *   zone IST
 *   batt 78 unpowered       capacity, then powered|unpowered; or "batt unknown"
 *   gps running 3 3         unavailable|off|failed|searching|running|disabled
 *                           [mode [eph m]]
 *   wifi connected HomeNet  unknown|off|on|connecting|connected SSID
 *   bt connected 2          unknown|none|off|on|connected N
 *   storage 18200000000 [32000000000]
 *                           free (and total) bytes on an owned /data; or "storage ram"
 *   storageusage MAPS RIDES ROUTES [estimate]
 *                           category bytes; Other is total - free - the three
 *   clockepoch 1791449160   the clock from this epoch under the current TZ
 *                           (date, 12-hour fields, offset); "time" alone sets
 *                           only HH:MM
 *   timesync gps|network|restored|floor|none|unknown REF_EPOCH_OR_0
 *                           the Time page's source (a model, no subprocess)
 *   als LUX|none            the light sensor: available with this reading, or
 *                           not available (host fakes never connect to sensord)
 *   about KEY VALUE         the About page: name version commit builddate
 *                           dirty lvgl kernel resolution serial bluetooth wifi
 *                           uptime (seconds); VALUE is the rest of the line
 *
 * The Wi-Fi pages' view (wifi.h); the SSID is the rest of the line:
 *
 *   wifinet SEC FREQ STRENGTH known|- SSID      one network in range
 *   wifiactive SEC FREQ STRENGTH IP/PREFIX GW CONN SSID
 *                           the connection: CONN unknown|none|portal|limited|full
 *   wifiscan 1              the scanning indicator
 *
 * SEC: open wep wpa wpa2 wpa3 wpa2/wpa3 owe enterprise.
 *
 * The GPS pages' view (gpsview.h; fake positions only):
 *
 *   gpsfix MODE EPH ALT SPEED LAT LON   TPV (metres, m/s, degrees)
 *   gpsdop HDOP PDOP                    SKY
 *   gpssat G05 AZ EL SNR USED           one satellite (G R E C J S I), USED 0|1
 *   gpslease SECONDS [FIRST_FIX_S]      the page's lease began SECONDS ago
 *
 * The Bluetooth pages' view (blesens.h): blesensd's own lines, and a
 * pairing of a found device that began SECONDS ago (or failed):
 *
 *   ble {"type":"sensor","addr":...}     one blesensd line (watch protocol)
 *   blepair ADDR SECONDS                 pairing it, SECONDS into the 20
 *   blepair ADDR failed REASON           that pairing failed
 *
 * Several files may be given (--fake-state twice): later lines win.
 *
 * The design reference values are tools/chefui/apps/settings/design.state.
 */
#include <errno.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chefui.h"
#include "sys.h"
#include "sysstate.h"
#include "blesens.h"
#include "gpsview.h"
#include "wifi.h"

static int sec_from(const char *s)
{
	static const char *const n[WIFI_SEC_N] = { "open", "wep", "wpa", "wpa2", "wpa3",
		"wpa2/wpa3", "owe", "enterprise" };

	for (int i = 0; s && i < WIFI_SEC_N; i++)
		if (!strcmp(s, n[i]))
			return i;
	return -1;
}

static void ssid_from(const char *text, struct wifi_ssid *ss, char *out, size_t n)
{
	ss->len = (int)strnlen(text, WIFI_SSID_MAX);
	memcpy(ss->b, text, (size_t)ss->len);
	wifi_ssid_text(ss, out, n);
}

/* Wi-Fi lines: tokens then the SSID as the rest of the line. */
static int wifi_field(struct wifi_view *v, const char *key, char *rest)
{
	char *save = NULL, *t[6];
	int ntok = !strcmp(key, "wifinet") ? 4 : 6, sec;

	for (int i = 0; i < ntok; i++) {
		t[i] = strtok_r(i ? NULL : rest, " \t", &save);
		if (!t[i])
			return -1;
	}
	rest = save ? save + strspn(save, " \t") : NULL;
	if (!rest || !*rest || (sec = sec_from(t[0])) < 0)
		return -1;
	if (!strcmp(key, "wifinet")) {
		struct wifi_net *n;

		if (v->n >= WIFI_NETS_MAX)
			return -1;
		n = &v->nets[v->n++];
		memset(n, 0, sizeof(*n));
		n->sec = (enum wifi_sec)sec;
		n->freq = (uint32_t)atoi(t[1]);
		n->strength = atoi(t[2]);
		n->known = !strcmp(t[3], "known");
		snprintf(n->ap, sizeof(n->ap), "/fake/ap/%d", v->n);
		ssid_from(rest, &n->ssid, n->text, sizeof(n->text));
	} else {
		static const char *const conn[] = { "unknown", "none", "portal", "limited", "full" };
		struct wifi_active *c = &v->cur;
		struct wifi_ssid ss;
		char *slash = strchr(t[3], '/');

		memset(c, 0, sizeof(*c));
		c->valid = c->ap_known = true;
		c->dev_state = SYS_NM_DEVICE_ACTIVATED;
		c->sec = (enum wifi_sec)sec;
		c->freq = (uint32_t)atoi(t[1]);
		c->strength = atoi(t[2]);
		if (slash) {
			*slash = 0;
			c->prefix = atoi(slash + 1);
		}
		snprintf(c->ip4, sizeof(c->ip4), "%s", t[3]);
		snprintf(c->gw, sizeof(c->gw), "%s", t[4]);
		for (unsigned i = 0; i < 5; i++)
			if (!strcmp(t[5], conn[i]))
				c->connectivity = i;
		c->connectivity_known = c->connectivity != 0;
		ssid_from(rest, &ss, c->text, sizeof(c->text));
		snprintf(c->id, sizeof(c->id), "%.63s", c->text);
		snprintf(c->conn, sizeof(c->conn), "/fake/settings/1");
		snprintf(c->uuid, sizeof(c->uuid), "00000000-0000-4000-8000-000000000001");
	}
	return 0;
}

static int64_t fake_mono_ms(void);

static int field(struct sys_state *s, char *key, char *a, char *b, char *c, char *d)
{
	if (!strcmp(key, "time") && a && strlen(a) < sizeof(s->clock.hhmm)) {
		int h, m;

		strcpy(s->clock.hhmm, a);
		s->clock.valid = strlen(a) == 5 && sscanf(a, "%2d:%2d", &h, &m) == 2 && h >= 0 &&
			h < 24 && m >= 0 && m < 60;
		s->clock.hour = s->clock.valid ? h : -1;
		s->clock.minute = s->clock.valid ? m : -1;
	} else if (!strcmp(key, "clockepoch") && a) {
		char *end;
		long long e = strtoll(a, &end, 10);

		if (*end || e < 1767225600LL || e >= 4102444800LL)
			return -1;
		sys_clock_fields((time_t)e, s);
		if (!s->clock.valid)
			return -1;
	} else if (!strcmp(key, "timesync") && a && b) {
		static const char *const names[] = { "unknown", "gps", "network", "restored", "floor",
			"none" };
		static const enum sys_time_source src[] = { SYS_TIME_UNKNOWN, SYS_TIME_GPS,
			SYS_TIME_NETWORK, SYS_TIME_RESTORED, SYS_TIME_FLOOR, SYS_TIME_NONE };
		char *end;
		double ref = strtod(b, &end);
		unsigned i;

		for (i = 0; i < 6 && strcmp(a, names[i]); i++)
			;
		if (i == 6 || *end || ref < 0)
			return -1;
		memset(&s->timeview, 0, sizeof(s->timeview));
		if (i == 1 || i == 2) {
			s->timeview.verified = true;
			s->timeview.source = src[i];
			s->timeview.reference_epoch = ref;
		} else {
			s->timeview.boot_source = src[i];
		}
	} else if (!strcmp(key, "storageusage") && a && b && c) {
		uint64_t maps = strtoull(a, NULL, 10), rides = strtoull(b, NULL, 10),
			routes = strtoull(c, NULL, 10), used;

		if (!s->storage.known || !s->storage.ours || !s->storage.total_bytes ||
		    s->storage.free_bytes > s->storage.total_bytes)
			return -1;
		used = s->storage.total_bytes - s->storage.free_bytes;
		s->storage.usage_known = true;
		for (int i = 0; i < 3; i++)
			s->storage.usage_cat[i] = USAGE_CAT_EXACT;
		s->storage.usage_maps = maps;
		s->storage.usage_rides = rides;
		s->storage.usage_routes = routes;
		s->storage.usage_other = maps + rides + routes < used ? used - maps - rides - routes : 0;
		s->storage.usage_total_bytes = s->storage.total_bytes;
		s->storage.usage_free_bytes = s->storage.free_bytes;
		if (d && strcmp(d, "estimate"))
			return -1;
		s->storage.usage_complete = !d;
		s->storage.usage_updated_mono_ms = fake_mono_ms();
	} else if (!strcmp(key, "zone") && a && strlen(a) < sizeof(s->clock.zone)) {
		strcpy(s->clock.zone, a);
	} else if (!strcmp(key, "batt") && a) {
		if (!strcmp(a, "unknown")) {
			s->batt.known = false;
			return 0;
		}
		s->batt.known = true;
		s->batt.capacity = atoi(a);
		s->batt.powered = b && !strcmp(b, "powered");
		if (s->batt.capacity < 0 || s->batt.capacity > 100)
			return -1;
	} else if (!strcmp(key, "gps") && a) {
		static const char *const names[] = { "unavailable", "off", "failed", "searching", "running",
			"disabled" };
		unsigned i;

		for (i = 0; i < 6 && strcmp(a, names[i]); i++)
			;
		if (i == 6)
			return -1;
		s->gps.state = (enum sys_gps)i;
		s->gps.enabled = i == SYS_GPS_UNAVAILABLE ? -1 : i != SYS_GPS_DISABLED;
		s->gps.mode = b ? atoi(b) : 0;
		s->gps.eph = c ? atof(c) : -1;
	} else if (!strcmp(key, "als") && a) {
		char *end;

		if (!strcmp(a, "none")) {
			s->als.available = false;
			s->als.lux = 0;
			return 0;
		}
		s->als.lux = strtod(a, &end);
		if (*end || s->als.lux < 0)
			return -1;
		s->als.available = true;
	} else if (!strcmp(key, "wifi") && a) {
		memset(&s->wifi, 0, sizeof(s->wifi));
		if (!strcmp(a, "unknown"))
			return 0;
		s->wifi.known = true;
		s->wifi.device = true;
		s->wifi.enabled = strcmp(a, "off") != 0;
		s->wifi.dev_state = !strcmp(a, "connected") ? SYS_NM_DEVICE_ACTIVATED
			: !strcmp(a, "connecting") ? SYS_NM_DEVICE_PREPARE : SYS_NM_DEVICE_DISCONNECTED;
		if (!strcmp(a, "connected")) {
			if (!b || strlen(b) >= sizeof(s->wifi.ssid))
				return -1;
			strcpy(s->wifi.ssid, b);
		}
	} else if (!strcmp(key, "bt") && a) {
		memset(&s->bt, 0, sizeof(s->bt));
		if (!strcmp(a, "unknown"))
			return 0;
		s->bt.known = true;
		s->bt.adapter = strcmp(a, "none") != 0;
		s->bt.powered = !strcmp(a, "on") || !strcmp(a, "connected");
		s->bt.connected = !strcmp(a, "connected") && b ? atoi(b) : 0;
	} else if (!strcmp(key, "storage") && a) {
		s->storage.known = true;
		s->storage.ours = strcmp(a, "ram") != 0;
		s->storage.free_bytes = s->storage.ours ? strtoull(a, NULL, 10) : 0;
		s->storage.total_bytes = s->storage.ours && b ? strtoull(b, NULL, 10) : 0;
		s->storage.usage_known = false;
		s->storage.usage_complete = false;
	} else {
		return -1;
	}
	return 0;
}

static int64_t fake_mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* GPS view lines (tokens after the key in t[0..n)). */
static int gps_field(struct gps_view *v, const char *key, char **t, int n)
{
	if (!strcmp(key, "gpsfix") && n == 6) {
		v->mode = atoi(t[0]);
		v->eph = atof(t[1]);
		v->alt = atof(t[2]);
		v->speed = atof(t[3]);
		v->lat = atof(t[4]);
		v->lon = atof(t[5]);
	} else if (!strcmp(key, "gpsdop") && n == 2) {
		v->sky = true;
		v->hdop = atof(t[0]);
		v->pdop = atof(t[1]);
	} else if (!strcmp(key, "gpssat") && n == 5 && strlen(t[0]) >= 2 && v->n_sats < GPS_SATS_MAX) {
		static const char letters[] = "GSECxJRI";	/* by gnssid */
		struct gps_sat *s = &v->sats[v->n_sats];
		const char *l = strchr(letters, t[0][0]);

		if (!l || *l == 'x')
			return -1;
		s->gnss = (int)(l - letters);
		s->svid = atoi(t[0] + 1);
		s->az = atof(t[1]);
		s->el = atof(t[2]);
		s->ss = atof(t[3]);
		s->used = atoi(t[4]) != 0;
		v->sky = true;
		v->n_sats++;
	} else if (!strcmp(key, "gpslease") && (n == 1 || n == 2)) {
		v->lease_ms = v->start_ms = fake_mono_ms() - (int64_t)(atof(t[0]) * 1000);
		v->first_fix_ms = n == 2 ? v->lease_ms + (int64_t)(atof(t[1]) * 1000) : 0;
	} else {
		return -1;
	}
	return 0;
}

/* Bluetooth lines: a blesensd line, or a pairing of a found device. */
static int ble_field(struct ble_view *v, const char *key, char *rest)
{
	char *save = NULL, *addr, *a;
	int i;

	if (!rest)
		return -1;
	if (!strcmp(key, "ble")) {
		ble_apply_line(v, rest + strspn(rest, " \t"), fake_mono_ms());
		return 0;
	}
	addr = strtok_r(rest, " \t", &save);
	a = strtok_r(NULL, " \t", &save);
	for (i = 0; addr && a && i < v->n_found; i++)
		if (!strcmp(v->f[i].addr, addr))
			break;
	if (!addr || !a || i == v->n_found)
		return -1;
	memset(&v->pair, 0, sizeof(v->pair));
	snprintf(v->pair.addr, sizeof(v->pair.addr), "%s", addr);
	snprintf(v->pair.name, sizeof(v->pair.name), "%s", v->f[i].name);
	v->pair.kinds = v->f[i].kinds;
	snprintf(v->pair.step, sizeof(v->pair.step), "connecting");
	if (!strcmp(a, "failed")) {
		v->pair.st = BLE_PAIR_FAILED;
		snprintf(v->pair.reason, sizeof(v->pair.reason), "%s", save && *save ? save : "timed out");
	} else {
		v->pair.st = BLE_PAIR_RUNNING;
		v->pair.started_ms = fake_mono_ms() - (int64_t)(atof(a) * 1000);
	}
	return 0;
}

/* About lines: KEY then the rest of the line as the value. */
static int about_field(struct sys_state *s, char *rest, bool *meta)
{
	struct sys_device_info *d = &s->device;
	char *key, *val;
	char *save = NULL;

	key = strtok_r(rest, " \t", &save);
	val = save ? save + strspn(save, " \t") : NULL;
	if (!key || !val || !*val)
		return -1;
	if (!strcmp(key, "name") || !strcmp(key, "version") || !strcmp(key, "commit") ||
	    !strcmp(key, "builddate") || !strcmp(key, "dirty"))
		*meta = true;
	if (!strcmp(key, "name") && strlen(val) < sizeof(d->image_name))
		strcpy(d->image_name, val);
	else if (!strcmp(key, "version") && strlen(val) < sizeof(d->version))
		strcpy(d->version, val);
	else if (!strcmp(key, "commit") && strlen(val) < sizeof(d->commit))
		strcpy(d->commit, val);
	else if (!strcmp(key, "builddate") && strlen(val) < sizeof(d->build_date))
		strcpy(d->build_date, val);
	else if (!strcmp(key, "dirty") && (!strcmp(val, "0") || !strcmp(val, "1")))
		d->dirty = val[0] == '1';
	else if (!strcmp(key, "lvgl") && strlen(val) < sizeof(d->lvgl))
		strcpy(d->lvgl, val);
	else if (!strcmp(key, "kernel") && strlen(val) < sizeof(d->kernel))
		strcpy(d->kernel, val);
	else if (!strcmp(key, "resolution") && strlen(val) < sizeof(d->resolution))
		strcpy(d->resolution, val);
	else if (!strcmp(key, "serial") && strlen(val) < sizeof(d->serial)) {
		strcpy(d->serial, val);
		d->serial_known = true;
	} else if (!strcmp(key, "bluetooth") && strlen(val) == 17) {
		strcpy(d->bluetooth, val);
		d->bluetooth_known = true;
	} else if (!strcmp(key, "wifi") && strlen(val) == 17) {
		strcpy(d->wifi, val);
		d->wifi_known = true;
	} else if (!strcmp(key, "uptime")) {
		char *end;
		unsigned long long u = strtoull(val, &end, 10);

		if (*end || u > 100ULL * 365 * 86400)
			return -1;
		d->uptime_s = u;
		d->sampled_mono_ms = fake_mono_ms();
	} else
		return -1;
	return 0;
}

int sys_fake_load(const char *path)
{
	struct sys_state s = sys_state;
	struct wifi_view v = wifi_view;
	struct gps_view g = gps_view;
	struct ble_view bv = ble_view;
	char line[512];
	int n = 0;
	bool saw_meta = false;
	FILE *f = fopen(path, "re");

	if (!f)
		return -errno;
	while (fgets(line, sizeof(line), f)) {
		char *save = NULL, *key, *a, *b, *c, *d;

		n++;
		line[strcspn(line, "#\r\n")] = 0;
		key = strtok_r(line, " \t", &save);
		if (!key)
			continue;
		if (!strcmp(key, "wifinet") || !strcmp(key, "wifiactive")) {
			if (wifi_field(&v, key, save)) {
				chefui_log("fake state %s:%d: bad line", path, n);
				fclose(f);
				return -EINVAL;
			}
			continue;
		}
		if (!strncmp(key, "gps", 3) && strcmp(key, "gps")) {
			char *t[8];
			int nt = 0;

			while (nt < 8 && (t[nt] = strtok_r(NULL, " \t", &save)))
				nt++;
			if (gps_field(&g, key, t, nt)) {
				chefui_log("fake state %s:%d: bad line", path, n);
				fclose(f);
				return -EINVAL;
			}
			continue;
		}
		if (!strcmp(key, "ble") || !strcmp(key, "blepair")) {
			if (ble_field(&bv, key, save)) {
				chefui_log("fake state %s:%d: bad line", path, n);
				fclose(f);
				return -EINVAL;
			}
			continue;
		}
		if (!strcmp(key, "about")) {
			if (about_field(&s, save, &saw_meta)) {
				chefui_log("fake state %s:%d: bad line", path, n);
				fclose(f);
				return -EINVAL;
			}
			continue;
		}
		if (!strcmp(key, "wifiscan")) {
			a = strtok_r(NULL, " \t", &save);
			v.scanning = a && atoi(a);
			continue;
		}
		a = strtok_r(NULL, " \t", &save);
		b = strtok_r(NULL, " \t", &save);
		c = strtok_r(NULL, " \t", &save);
		d = strtok_r(NULL, " \t", &save);
		if (field(&s, key, a, b, c, d)) {
			chefui_log("fake state %s:%d: bad line", path, n);
			fclose(f);
			return -EINVAL;
		}
	}
	fclose(f);
	/* image facts come whole or not at all: a title without a build is rejected */
	if (saw_meta) {
		if (!s.device.image_name[0] || !s.device.version[0] || !s.device.commit[0] ||
		    !s.device.build_date[0]) {
			chefui_log("fake state %s: about needs name, version, commit and builddate", path);
			return -EINVAL;
		}
		s.device.metadata_known = true;
	}
	sys_state = s;
	wifi_view = v;
	gps_view = g;
	ble_view = bv;
	sys_changed(SYS_BATT | SYS_CLOCK | SYS_GPS | SYS_WIFI | SYS_BT | SYS_STORAGE | SYS_WIFI_VIEW |
		    SYS_GPS_VIEW | SYS_BLE | SYS_TIME_VIEW | SYS_DEVICE_INFO | SYS_ALS);
	return 0;
}
