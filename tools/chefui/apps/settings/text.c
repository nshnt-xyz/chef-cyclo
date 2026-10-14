/*
 * text.c - hub subtitles and the status bar mapping (see text.h).
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "blesens.h"
#include "icons.h"
#include "text.h"
#include "sys.h"
#include "timeview.h"

#define DOT " \xC2\xB7 "	/* " · " */
#define PLUSMINUS "\xC2\xB1"	/* "±" */
#define ELLIPSIS "\xE2\x80\xA6"	/* "…" */

void text_bt(const struct sys_state *s, char *buf, size_t n)
{
	if (!s->bt.known)
		snprintf(buf, n, "Unavailable");
	else if (!s->bt.adapter)
		snprintf(buf, n, "No adapter");
	else if (!s->bt.powered)
		snprintf(buf, n, "Off");
	else if (ble_view.known && ble_view.n > 0) {
		struct ble_row r[BLE_SENSORS_MAX * BLE_KINDS];
		int rows = ble_rows(&ble_view, r, BLE_SENSORS_MAX * BLE_KINDS);

		/* the sensors (blesensd), as P08 counts them */
		snprintf(buf, n, "%d of %d connected", ble_rows_connected(&ble_view), rows);
	} else if (s->bt.connected > 0)
		snprintf(buf, n, "%d connected", s->bt.connected);
	else
		snprintf(buf, n, "On");
}

void text_gps(const struct sys_state *s, char *buf, size_t n)
{
	const char *fix;

	switch (s->gps.state) {
	case SYS_GPS_UNAVAILABLE: snprintf(buf, n, "Unavailable"); return;
	case SYS_GPS_DISABLED: snprintf(buf, n, "Off"); return;
	case SYS_GPS_OFF: snprintf(buf, n, "On"); return;
	case SYS_GPS_FAILED: snprintf(buf, n, "Failed"); return;
	case SYS_GPS_SEARCHING: snprintf(buf, n, "Searching" ELLIPSIS); return;
	case SYS_GPS_RUNNING: break;
	}
	fix = s->gps.mode == 3 ? "3D fix" : s->gps.mode == 2 ? "2D fix" : "Fix";
	if (s->gps.mode >= 2 && s->gps.eph >= 0)
		snprintf(buf, n, "%s" DOT PLUSMINUS "%.0f m", fix, s->gps.eph);
	else
		snprintf(buf, n, "%s", fix);
}

void text_wifi(const struct sys_state *s, char *buf, size_t n)
{
	if (!s->wifi.known)
		snprintf(buf, n, "Unavailable");
	else if (!s->wifi.enabled)
		snprintf(buf, n, "Off");
	else if (!s->wifi.device)
		snprintf(buf, n, "No device");
	else if (sys_wifi_connected(s))
		snprintf(buf, n, "%s", s->wifi.ssid);
	else if (s->wifi.dev_state >= SYS_NM_DEVICE_PREPARE &&
		 s->wifi.dev_state < SYS_NM_DEVICE_ACTIVATED)
		snprintf(buf, n, "Connecting" ELLIPSIS);
	else
		snprintf(buf, n, "Not connected");
}

void text_display(const struct ui_settings *st, int applied, char *buf, size_t n)
{
	const char *t = st->theme == UI_THEME_LIGHT ? "Light" : st->theme == UI_THEME_AUTO ? "Auto"
		: "Dark";

	if (st->auto_brightness)
		snprintf(buf, n, "Auto" DOT "%d%%", applied >= 0 ? applied : st->brightness);
	else
		snprintf(buf, n, "%s" DOT "%d%%", t, st->brightness);
}

/* hhmm stays the canonical 24-hour string (old fixtures set only it);
 * hour and minute carry the 12-hour presentation. */
static void time_parts(const struct sys_state *s, bool clock_24h, char *digits, size_t dn,
		       char meridiem[3])
{
	int h = -1, m = -1;

	if (s->clock.valid) {
		h = s->clock.hour;
		m = s->clock.minute;
	} else if (strlen(s->clock.hhmm) != 5 || sscanf(s->clock.hhmm, "%2d:%2d", &h, &m) != 2) {
		h = m = -1;
	}
	sys_time_format(h, m, clock_24h, digits, dn, meridiem);	/* "--:--" when invalid */
}

void text_time(const struct sys_state *s, bool clock_24h, char *buf, size_t n)
{
	char digits[8], mer[3];

	time_parts(s, clock_24h, digits, sizeof(digits), mer);
	if (mer[0] && s->clock.zone[0])
		snprintf(buf, n, "%s %s" DOT "%s", digits, mer, s->clock.zone);
	else if (mer[0])
		snprintf(buf, n, "%s %s", digits, mer);
	else if (s->clock.zone[0])
		snprintf(buf, n, "%s" DOT "%s", digits, s->clock.zone);
	else
		snprintf(buf, n, "%s", digits);
}

void text_storage(const struct sys_state *s, char *buf, size_t n)
{
	if (!s->storage.known)
		snprintf(buf, n, "Unknown");
	else if (!s->storage.ours)
		snprintf(buf, n, "RAM only");
	else
		snprintf(buf, n, "%.1f GB free", (double)s->storage.free_bytes / 1e9);
}

bool tile_bt_on(const struct sys_state *s)
{
	return s->bt.known && s->bt.adapter && s->bt.powered;
}

/* On = the switch is on and the manager is there (idle, searching,
 * failed or running). */
bool tile_gps_on(const struct sys_state *s)
{
	return s->gps.state != SYS_GPS_UNAVAILABLE && s->gps.state != SYS_GPS_DISABLED;
}

bool tile_wifi_on(const struct sys_state *s)
{
	return sys_wifi_on(s);
}

void status_from_state(const struct sys_state *s, bool clock_24h, struct kit_status *out)
{
	memset(out, 0, sizeof(*out));
	/* locate-off only when switched off (or no manager); on, idle or
	 * searching: locate, muted; a fix: locate-fixed */
	out->gps = s->gps.state == SYS_GPS_RUNNING ? KIT_GPS_FIX : tile_gps_on(s) ? KIT_GPS_SEARCH
		: KIT_GPS_OFF;
	out->bt = !tile_bt_on(s) ? KIT_BT_OFF : s->bt.connected > 0 ? KIT_BT_CONNECTED : KIT_BT_ON;
	out->wifi = sys_wifi_connected(s) ? KIT_WIFI_CONNECTED : sys_wifi_on(s) ? KIT_WIFI_ON
		: KIT_WIFI_OFF;
	out->batt = s->batt.known ? s->batt.capacity : -1;
	out->powered = s->batt.known && s->batt.powered;
	time_parts(s, clock_24h, out->time, sizeof(out->time), out->meridiem);
	if (!out->time[0])
		snprintf(out->time, sizeof(out->time), "--:--");
}

const char *glyph_bt(enum kit_bt b)
{
	return b == KIT_BT_CONNECTED ? KIT_IC_BLUETOOTH_CONNECTED : b == KIT_BT_ON ? KIT_IC_BLUETOOTH
		: KIT_IC_BLUETOOTH_OFF;
}

const char *glyph_gps(enum kit_gps g)
{
	return g == KIT_GPS_FIX ? KIT_IC_LOCATE_FIXED : g == KIT_GPS_SEARCH ? KIT_IC_LOCATE
		: KIT_IC_LOCATE_OFF;
}

const char *glyph_wifi(enum kit_wifi w)
{
	return w == KIT_WIFI_OFF ? KIT_IC_WIFI_OFF : KIT_IC_WIFI;
}

void text_sound(const struct ui_settings *st, char *buf, size_t n)
{
	snprintf(buf, n, "%s", st->haptics ? "Haptics on" : "Haptics off");
}

void text_time_source(const struct sys_state *s, char *buf, size_t n)
{
	const struct sys_state *t = s;

	if (t->timeview.verified && t->timeview.source == SYS_TIME_GPS)
		snprintf(buf, n, "Set by GPS");
	else if (t->timeview.verified && t->timeview.source == SYS_TIME_NETWORK)
		snprintf(buf, n, "Set by network");
	else if (t->timeview.boot_source == SYS_TIME_RESTORED)
		snprintf(buf, n, "Restored at boot, not yet verified");
	else if (t->timeview.boot_source == SYS_TIME_FLOOR)
		snprintf(buf, n, "Restored boot-time floor, not yet verified");
	else if (t->timeview.boot_source == SYS_TIME_NONE)
		snprintf(buf, n, "Time not yet verified");
	else
		snprintf(buf, n, "Time source unavailable");
}

/* "IST . UTC+05:30" for the fixed zone; the offset is the current local one */
void text_time_zone(const struct sys_state *s, char *buf, size_t n)
{
	int off = s->clock.utc_offset_min, a = off < 0 ? -off : off;

	if (!s->clock.valid)
		snprintf(buf, n, "Unknown");
	else if (s->clock.zone[0])
		snprintf(buf, n, "%s" DOT "UTC%c%02d:%02d", s->clock.zone, off < 0 ? '-' : '+',
			 a / 60, a % 60);
	else
		snprintf(buf, n, "UTC%c%02d:%02d", off < 0 ? '-' : '+', a / 60, a % 60);
}

void text_time_hint(const struct sys_state *s, char *buf, size_t n)
{
	char a[16], b[16];

	if (!s->clock.valid) {
		snprintf(buf, n, "Show 24-hour time");
		return;
	}
	sys_time_full(s->clock.hour, s->clock.minute, true, a, sizeof(a));
	sys_time_full(s->clock.hour, s->clock.minute, false, b, sizeof(b));
	snprintf(buf, n, "%s instead of %s", a, b);
}

/* "Last synced 14:02 via GPS." under the local zone (UI thread only) */
void text_time_sync(const struct sys_state *s, bool clock_24h, char *buf, size_t n)
{
	static const char *const mon[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug",
		"Sep", "Oct", "Nov", "Dec" };
	const char *via = s->timeview.source == SYS_TIME_GPS ? "GPS" : "network";
	char when[32], hm[16];
	struct tm tm;
	time_t t = (time_t)s->timeview.reference_epoch;
	char ref_date[64];

	if (!s->timeview.verified || s->timeview.reference_epoch < 1767225600.0 ||
	    s->timeview.reference_epoch >= 4102444800.0 || !localtime_r(&t, &tm)) {
		snprintf(buf, n, "Last sync time unknown.");
		return;
	}
	sys_time_full(tm.tm_hour, tm.tm_min, clock_24h, hm, sizeof(hm));
	/* the date too when it is not the clock's own date (the model's, which a
	 * frozen fixture also sets) */
	sys_clock_date_text(&tm, ref_date, sizeof(ref_date));
	if (s->clock.date[0] && strcmp(ref_date, s->clock.date))
		snprintf(when, sizeof(when), "%d %s %s", tm.tm_mday, mon[tm.tm_mon], hm);
	else
		snprintf(when, sizeof(when), "%s", hm);
	snprintf(buf, n, "Last synced %s via %s.", when, via);
}

void text_gb(uint64_t bytes, bool estimate, char *buf, size_t n)
{
	snprintf(buf, n, "%s%.1f GB", estimate ? "~" : "", (double)bytes / 1e9);
}

void text_storage_unit(const struct sys_state *s, char *buf, size_t n)
{
	double total = (double)s->storage.total_bytes / 1e9;

	if (!s->storage.known)
		snprintf(buf, n, "storage unavailable");
	else if (!s->storage.ours)
		snprintf(buf, n, "RAM only");
	else if (total - (double)(long)(total + 0.5) < 0.05 && (double)(long)(total + 0.5) - total < 0.05)
		snprintf(buf, n, "GB free of %ld GB", (long)(total + 0.5));
	else
		snprintf(buf, n, "GB free of %.1f GB", total);
}

void text_uptime(uint64_t seconds, char *buf, size_t n)
{
	unsigned long long m = seconds / 60, h = m / 60, d = h / 24;

	if (d)
		snprintf(buf, n, "%llu d %llu h", d, h % 24);
	else if (h)
		snprintf(buf, n, "%llu h %llu min", h, m % 60);
	else
		snprintf(buf, n, "%llu min", m);
}
