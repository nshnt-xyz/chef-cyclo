/*
 * text.c - hub subtitles and the status bar mapping (see text.h).
 */
#include <stdio.h>
#include <string.h>

#include "icons.h"
#include "text.h"

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
	else if (s->bt.connected > 0)
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

void text_display(const struct ui_settings *st, char *buf, size_t n)
{
	const char *t = st->theme == UI_THEME_LIGHT ? "Light" : st->theme == UI_THEME_AUTO ? "Auto"
		: "Dark";

	snprintf(buf, n, "%s" DOT "%d%%", t, st->brightness);
}

void text_time(const struct sys_state *s, char *buf, size_t n)
{
	if (s->clock.zone[0])
		snprintf(buf, n, "%s" DOT "%s", s->clock.hhmm, s->clock.zone);
	else
		snprintf(buf, n, "%s", s->clock.hhmm);
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

void status_from_state(const struct sys_state *s, struct kit_status *out)
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
	snprintf(out->time, sizeof(out->time), "%s", s->clock.hhmm[0] ? s->clock.hhmm : "--:--");
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
