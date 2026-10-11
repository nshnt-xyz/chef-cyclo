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
 * The design reference values are tools/chefui/apps/settings/design.state.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chefui.h"
#include "sys.h"
#include "sysstate.h"

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
	sys_changed(SYS_BATT | SYS_CLOCK | SYS_GPS | SYS_WIFI | SYS_BT | SYS_STORAGE);
	return 0;
}
