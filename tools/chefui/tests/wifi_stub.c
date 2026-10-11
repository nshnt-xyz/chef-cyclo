/*
 * wifi_stub.c - the NetworkManager actions (wifi.h) for tests that link
 * the app without a bus: they record what the pages asked for, and
 * wifi_stub_result is what they return.
 */
#include <stdio.h>
#include <string.h>

#include "wifi_stub.h"

struct wifi_stub wifi_stub;

void sys_wifi_init(void)
{
}

void sys_wifi_watch(bool watch, bool scan)
{
	if (watch != wifi_stub.watch || scan != wifi_stub.scan)
		wifi_stub.watch_changes++;
	wifi_stub.watch = watch;
	wifi_stub.scan = scan;
}

int sys_wifi_set_enabled(bool on)
{
	wifi_stub.enable_calls++;
	wifi_stub.enabled = on;
	return wifi_stub.result;
}

int sys_wifi_join(const struct wifi_net *n)
{
	wifi_stub.join_calls++;
	wifi_stub.last = *n;
	if (!wifi_stub.result) {
		wifi_view.join.st = WIFI_JOIN_BUSY;
		snprintf(wifi_view.join.text, sizeof(wifi_view.join.text), "%s", n->text);
	}
	return wifi_stub.result;
}

int sys_wifi_join_new(const struct wifi_ssid *ssid, enum wifi_sec sec, char *psk, bool hidden,
		      const char *ap)
{
	(void)ap;
	wifi_stub.join_new_calls++;
	wifi_stub.last_ssid = *ssid;
	wifi_stub.last_sec = sec;
	wifi_stub.last_hidden = hidden;
	wifi_stub.last_psk_len = strlen(psk);
	/* the test checks the pages handed over a passphrase of the right
	 * length and that it is wiped here, as the real one does */
	memcpy(wifi_stub.psk_seen, psk, sizeof(wifi_stub.psk_seen) - 1);
	wifi_wipe(psk, wifi_stub.last_psk_len + 1);
	if (!wifi_stub.result) {
		wifi_view.join.st = WIFI_JOIN_BUSY;
		wifi_ssid_text(ssid, wifi_view.join.text, sizeof(wifi_view.join.text));
	}
	return wifi_stub.result;
}

int sys_wifi_forget(const char *uuid)
{
	wifi_stub.forget_calls++;
	snprintf(wifi_stub.forget_uuid, sizeof(wifi_stub.forget_uuid), "%s", uuid);
	return wifi_stub.result;
}

bool sys_wifi_cleanup_pending(void)
{
	return false;
}

void sys_wifi_join_ack(void)
{
	wifi_stub.acks++;
	if (wifi_view.join.st != WIFI_JOIN_BUSY)
		wifi_view.join.st = WIFI_JOIN_IDLE;
}
