/*
 * netbus_none.c - netbus for a host build without libsystemd's sd-bus:
 * Wi-Fi and Bluetooth stay unknown (use --fake-state for those).
 */
#include <errno.h>
#include <string.h>

#include "chefui.h"
#include "sys.h"
#include "wifi.h"

int sys_netbus_start(void)
{
	chefui_log("netbus: built without sd-bus; Wi-Fi and Bluetooth state unavailable");
	return -ENOTSUP;
}

void sys_netbus_stop(void)
{
}

/* No NetworkManager client either: the Wi-Fi pages show fake state only. */
void sys_wifi_init(void)
{
}

void sys_wifi_watch(bool watch, bool scan)
{
	(void)watch;
	(void)scan;
}

int sys_wifi_set_enabled(bool on)
{
	(void)on;
	return -ENOTCONN;
}

int sys_wifi_join(const struct wifi_net *n)
{
	(void)n;
	return -ENOTCONN;
}

int sys_wifi_join_new(const struct wifi_ssid *ssid, enum wifi_sec sec, char *psk, bool hidden,
		      const char *ap)
{
	(void)ssid;
	(void)sec;
	(void)hidden;
	(void)ap;
	wifi_wipe(psk, strlen(psk));
	return -ENOTCONN;
}

int sys_wifi_forget(const char *uuid)
{
	(void)uuid;
	return -ENOTCONN;
}

bool sys_wifi_cleanup_pending(void)
{
	return false;
}

void sys_wifi_join_ack(void)
{
}
