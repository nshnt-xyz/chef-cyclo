/*
 * wifi_model.c - the pure part of the Wi-Fi view (see wifi.h): no bus,
 * no LVGL, unit tested in test-sys.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sysstate.h"
#include "wifi.h"

#define DOT " \xC2\xB7 "	/* " · " */
#define ELLIPSIS "\xE2\x80\xA6"	/* "…" */

struct wifi_view wifi_view;

/* NetworkManager's NM80211ApFlags and NM80211ApSecurityFlags. */
#define AP_FLAGS_PRIVACY	0x1
#define SEC_KEY_MGMT_PSK	0x100
#define SEC_KEY_MGMT_802_1X	0x200
#define SEC_KEY_MGMT_SAE	0x400
#define SEC_KEY_MGMT_OWE	0x800
#define SEC_KEY_MGMT_OWE_TM	0x1000
#define SEC_KEY_MGMT_EAP_SUITE_B_192 0x2000

void wifi_wipe(void *p, size_t n)
{
	volatile unsigned char *v = p;

	while (n--)
		*v++ = 0;
	__asm__ __volatile__("" : : "r"(p) : "memory");
}

enum wifi_sec wifi_sec_from_flags(uint32_t flags, uint32_t wpa, uint32_t rsn)
{
	uint32_t any = wpa | rsn;

	if (any & (SEC_KEY_MGMT_802_1X | SEC_KEY_MGMT_EAP_SUITE_B_192))
		return WIFI_SEC_ENTERPRISE;
	if ((rsn & SEC_KEY_MGMT_SAE) && (any & SEC_KEY_MGMT_PSK))
		return WIFI_SEC_WPA2_WPA3;
	if (rsn & SEC_KEY_MGMT_SAE)
		return WIFI_SEC_WPA3;
	if (rsn & SEC_KEY_MGMT_PSK)
		return WIFI_SEC_WPA2;
	if (wpa & SEC_KEY_MGMT_PSK)
		return WIFI_SEC_WPA;
	if (rsn & (SEC_KEY_MGMT_OWE | SEC_KEY_MGMT_OWE_TM))
		return WIFI_SEC_OWE;
	if (flags & AP_FLAGS_PRIVACY)
		return WIFI_SEC_WEP;	/* privacy without WPA/RSN */
	return WIFI_SEC_OPEN;
}

bool wifi_sec_needs_psk(enum wifi_sec s)
{
	return s == WIFI_SEC_WPA || s == WIFI_SEC_WPA2 || s == WIFI_SEC_WPA3 ||
	       s == WIFI_SEC_WPA2_WPA3;
}

bool wifi_sec_joinable(enum wifi_sec s)
{
	return s == WIFI_SEC_OPEN || wifi_sec_needs_psk(s);
}

const char *wifi_sec_short(enum wifi_sec s)
{
	static const char *const n[WIFI_SEC_N] = { "Open", "WEP", "WPA", "WPA2", "WPA3",
		"WPA2/WPA3", "OWE", "Enterprise" };

	return (unsigned)s < WIFI_SEC_N ? n[s] : "";
}

const char *wifi_sec_long(enum wifi_sec s)
{
	static const char *const n[WIFI_SEC_N] = { "Open", "WEP", "WPA Personal", "WPA2 Personal",
		"WPA3 Personal", "WPA2/WPA3 Personal", "Enhanced Open", "Enterprise" };

	return (unsigned)s < WIFI_SEC_N ? n[s] : "";
}

const char *wifi_sec_key_mgmt(enum wifi_sec s)
{
	/* transition networks take WPA2-PSK, which every supplicant has */
	return s == WIFI_SEC_WPA3 ? "sae" : wifi_sec_needs_psk(s) ? "wpa-psk" : NULL;
}

const char *wifi_band(uint32_t f)
{
	if (f >= 2400 && f < 2500)
		return "2.4 GHz";
	if (f >= 4900 && f < 5925)
		return "5 GHz";
	if (f >= 5925 && f <= 7125)
		return "6 GHz";
	return "";
}

int wifi_channel(uint32_t f)
{
	if (f == 2484)
		return 14;
	if (f >= 2412 && f < 2484)
		return (int)(f - 2407) / 5;
	if (f >= 4900 && f < 5925)
		return (int)(f - 5000) / 5;
	if (f >= 5955 && f <= 7115)
		return (int)(f - 5950) / 5;
	return 0;
}

int wifi_signal_level(int strength)
{
	return strength >= 50 ? 2 : strength >= 25 ? 1 : 0;
}

const char *wifi_signal_word(int strength)
{
	return strength >= 67 ? "strong" : strength >= 45 ? "good" : strength >= 25 ? "fair" : "weak";
}

/* Length of the valid UTF-8 sequence at s (n bytes left), 0 if invalid. */
static int utf8_len(const uint8_t *s, int n)
{
	int len, i;
	uint32_t cp;

	if (s[0] < 0x80)
		return 1;
	if ((s[0] & 0xE0) == 0xC0) {
		len = 2;
		cp = s[0] & 0x1F;
	} else if ((s[0] & 0xF0) == 0xE0) {
		len = 3;
		cp = s[0] & 0x0F;
	} else if ((s[0] & 0xF8) == 0xF0) {
		len = 4;
		cp = s[0] & 0x07;
	} else {
		return 0;
	}
	if (len > n)
		return 0;
	for (i = 1; i < len; i++) {
		if ((s[i] & 0xC0) != 0x80)
			return 0;
		cp = cp << 6 | (s[i] & 0x3F);
	}
	/* overlong forms, surrogates, beyond Unicode, C1 controls */
	if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) ||
	    (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF || (cp >= 0x80 && cp < 0xA0))
		return 0;
	return len;
}

void wifi_ssid_text(const struct wifi_ssid *s, char *out, size_t n)
{
	size_t o = 0;
	int i = 0;

	if (!n)
		return;
	while (i < s->len && i < WIFI_SSID_MAX) {
		int l = utf8_len(s->b + i, s->len - i);

		if (l <= 1) {
			uint8_t c = s->b[i];
			char r = l == 1 && c >= 0x20 && c != 0x7F ? (char)c : '?';

			if (o + 1 >= n)
				break;
			out[o++] = r;
			i++;
			continue;
		}
		if (o + (size_t)l >= n)
			break;
		memcpy(out + o, s->b + i, (size_t)l);
		o += (size_t)l;
		i += l;
	}
	out[o] = 0;
}

bool wifi_ssid_eq(const struct wifi_ssid *a, const struct wifi_ssid *b)
{
	return a->len == b->len && !memcmp(a->b, b->b, (size_t)a->len);
}

static int net_cmp(const void *pa, const void *pb)
{
	const struct wifi_net *a = pa, *b = pb;

	if (a->strength != b->strength)
		return b->strength - a->strength;
	return strcmp(a->text, b->text);
}

int wifi_group(const struct wifi_ap *aps, int n_aps, const struct wifi_ssid *active,
	       const struct wifi_ssid *known, int n_known, struct wifi_net *out, int max)
{
	struct wifi_net *all;
	int n = 0, i, j;

	if (n_aps <= 0 || max <= 0)
		return 0;
	all = calloc((size_t)n_aps, sizeof(*all));
	if (!all)
		return 0;
	for (i = 0; i < n_aps; i++) {
		const struct wifi_ap *a = &aps[i];
		struct wifi_net *g = NULL;

		if (a->ssid.len <= 0 || a->ssid.len > WIFI_SSID_MAX)
			continue;	/* hidden network: "Add hidden network" */
		if (active && active->len && wifi_ssid_eq(&a->ssid, active))
			continue;	/* shown in the Connected card */
		for (j = 0; j < n && !g; j++)
			if (wifi_ssid_eq(&all[j].ssid, &a->ssid))
				g = &all[j];
		if (g && g->strength >= a->strength)
			continue;
		if (!g)
			g = &all[n++];
		g->ssid = a->ssid;
		g->sec = wifi_sec_from_flags(a->flags, a->wpa, a->rsn);
		g->freq = a->freq;
		g->strength = a->strength < 0 ? 0 : a->strength > 100 ? 100 : a->strength;
		snprintf(g->ap, sizeof(g->ap), "%s", a->path);
	}
	for (i = 0; i < n; i++) {
		wifi_ssid_text(&all[i].ssid, all[i].text, sizeof(all[i].text));
		for (j = 0; j < n_known && !all[i].known; j++)
			all[i].known = wifi_ssid_eq(&all[i].ssid, &known[j]);
	}
	qsort(all, (size_t)n, sizeof(*all), net_cmp);
	if (n > max)
		n = max;
	memcpy(out, all, (size_t)n * sizeof(*all));
	free(all);
	return n;
}

bool wifi_reason_bad_password(uint32_t reason)
{
	/* NM_DEVICE_STATE_REASON_NO_SECRETS (7): the handshake failed and no
	 * agent had new secrets; SUPPLICANT_DISCONNECT (8): the 4-way
	 * handshake was refused */
	return reason == 7 || reason == 8;
}

void wifi_net_subtitle(const struct wifi_net *n, char *buf, size_t size)
{
	const char *band = wifi_band(n->freq);

	if (!wifi_sec_joinable(n->sec))
		snprintf(buf, size, "%s" DOT "not supported", wifi_sec_short(n->sec));
	else
		snprintf(buf, size, "%s%s%s%s", n->known ? "Saved" DOT : "", wifi_sec_short(n->sec),
			 *band ? DOT : "", band);
}

void wifi_active_subtitle(const struct wifi_active *a, char *buf, size_t size)
{
	const char *band = a->ap_known ? wifi_band(a->freq) : "";
	const char *net = "";

	if (a->dev_state != SYS_NM_DEVICE_ACTIVATED) {
		snprintf(buf, size, a->dev_state >= SYS_NM_DEVICE_PREPARE &&
			 a->dev_state < SYS_NM_DEVICE_ACTIVATED ? "Connecting" ELLIPSIS : "Not connected");
		return;
	}
	/* NMConnectivityState: 1 none, 2 portal, 3 limited, 4 full */
	if (a->connectivity_known)
		net = a->connectivity == 4 ? DOT "internet OK" : a->connectivity == 2
			? DOT "sign-in needed" : a->connectivity == 1 || a->connectivity == 3
			? DOT "no internet" : "";
	snprintf(buf, size, "Connected%s%s%s", net, *band ? DOT : "", band);
}
