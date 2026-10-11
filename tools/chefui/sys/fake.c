/*
 * fake.c - fixed system state from a file, for host runs, tests and the
 * reference captures (no real source is started with it). One field per
 * line, '#' comments:
 *
 *   time 14:16              status bar clock (frozen)
 *   zone IST
 *   batt 78 unpowered       capacity, then powered|unpowered; or "batt unknown"
 *   gps running 3 3         unavailable|off|failed|searching|running [mode [eph m]]
 *   wifi connected HomeNet  unknown|off|on|connecting|connected SSID
 *   bt connected 2          unknown|none|off|on|connected N
 *   storage 18200000000     free bytes on an owned /data; or "storage ram"
 *
 * The Wi-Fi pages' view (wifi.h); the SSID is the rest of the line:
 *
 *   wifinet SEC FREQ STRENGTH known|- SSID      one network in range
 *   wifiactive SEC FREQ STRENGTH IP/PREFIX GW CONN SSID
 *                           the connection: CONN unknown|none|portal|limited|full
 *   wifiscan 1              the scanning indicator
 *
 * SEC: open wep wpa wpa2 wpa3 wpa2/wpa3 owe enterprise. Several files may
 * be given (--fake-state twice): later lines win.
 *
 * The design reference values are tools/chefui/apps/settings/design.state.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chefui.h"
#include "sys.h"
#include "sysstate.h"
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

static int field(struct sys_state *s, char *key, char *a, char *b, char *c)
{
	if (!strcmp(key, "time") && a && strlen(a) < sizeof(s->clock.hhmm)) {
		strcpy(s->clock.hhmm, a);
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
		static const char *const names[] = { "unavailable", "off", "failed", "searching", "running" };
		unsigned i;

		for (i = 0; i < 5 && strcmp(a, names[i]); i++)
			;
		if (i == 5)
			return -1;
		s->gps.state = (enum sys_gps)i;
		s->gps.mode = b ? atoi(b) : 0;
		s->gps.eph = c ? atof(c) : -1;
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
	} else {
		return -1;
	}
	return 0;
}

int sys_fake_load(const char *path)
{
	struct sys_state s = sys_state;
	struct wifi_view v = wifi_view;
	char line[256];
	int n = 0;
	FILE *f = fopen(path, "re");

	if (!f)
		return -errno;
	while (fgets(line, sizeof(line), f)) {
		char *save = NULL, *key, *a, *b, *c;

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
		if (!strcmp(key, "wifiscan")) {
			a = strtok_r(NULL, " \t", &save);
			v.scanning = a && atoi(a);
			continue;
		}
		a = strtok_r(NULL, " \t", &save);
		b = strtok_r(NULL, " \t", &save);
		c = strtok_r(NULL, " \t", &save);
		if (field(&s, key, a, b, c)) {
			chefui_log("fake state %s:%d: bad line", path, n);
			fclose(f);
			return -EINVAL;
		}
	}
	fclose(f);
	sys_state = s;
	wifi_view = v;
	sys_changed(SYS_BATT | SYS_CLOCK | SYS_GPS | SYS_WIFI | SYS_BT | SYS_STORAGE | SYS_WIFI_VIEW);
	return 0;
}
