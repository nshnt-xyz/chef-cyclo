/* wifi_stub.h - see wifi_stub.c */
#ifndef WIFI_STUB_H
#define WIFI_STUB_H

#include "wifi.h"

struct wifi_stub {
	int result;			/* returned by every action */
	bool watch, scan, enabled;
	int watch_changes, enable_calls, join_calls, join_new_calls, forget_calls, acks;
	struct wifi_net last;
	struct wifi_ssid last_ssid;
	enum wifi_sec last_sec;
	bool last_hidden;
	size_t last_psk_len;
	char psk_seen[WIFI_PSK_MAX + 1];
	char forget_uuid[WIFI_UUID_LEN + 1];
	/* the Bluetooth switch (netbus) */
	int bt_power_calls;
	bool bt_power_last;
};

extern struct wifi_stub wifi_stub;

#endif
