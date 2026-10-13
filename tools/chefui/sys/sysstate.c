/*
 * sysstate.c - the shared system state and its change notification.
 */
#include "sysstate.h"

struct sys_state sys_state = {
	.gps = { .state = SYS_GPS_UNAVAILABLE, .enabled = -1, .eph = -1 },
};

static sys_change_cb change_cb;
static void *change_user;

void sys_on_change(sys_change_cb cb, void *user)
{
	change_cb = cb;
	change_user = user;
}

void sys_changed(unsigned what)
{
	if (change_cb && what)
		change_cb(what, change_user);
}

bool sys_wifi_on(const struct sys_state *s)
{
	return s->wifi.known && s->wifi.enabled && s->wifi.device;
}

bool sys_wifi_connected(const struct sys_state *s)
{
	return sys_wifi_on(s) && s->wifi.dev_state == SYS_NM_DEVICE_ACTIVATED && s->wifi.ssid[0];
}
