/*
 * netbus_none.c - netbus for a host build without libsystemd's sd-bus:
 * Wi-Fi and Bluetooth stay unknown (use --fake-state for those).
 */
#include <errno.h>

#include "chefui.h"
#include "sys.h"

int sys_netbus_start(void)
{
	chefui_log("netbus: built without sd-bus; Wi-Fi and Bluetooth state unavailable");
	return -ENOTSUP;
}

void sys_netbus_stop(void)
{
}
