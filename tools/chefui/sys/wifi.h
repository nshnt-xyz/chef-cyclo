/*
 * wifi.h - the Wi-Fi pages' view of NetworkManager: networks in range
 * (grouped by SSID), the active connection's details and the state of a
 * join. sys_state.wifi (sysstate.h) stays the status bar's summary; this
 * is only kept current while a Wi-Fi page asks for it (sys_wifi_watch).
 *
 * wifi_model.c holds the pure parts (security from AP flags, band and
 * channel from the frequency, grouping, texts), unit tested without a
 * bus; wifi.c talks to NetworkManager over the netbus connection.
 *
 * Profiles are created with a UUID of our own and deleted by UUID, never
 * by an object path that NetworkManager may reuse after a restart.
 *
 * Secrets: a passphrase lives only in the caller's buffer and, for the
 * duration of one AddAndActivateConnection call, in that D-Bus message;
 * sys_wifi_join_new() wipes the caller's buffer before it returns and
 * wipes the message body once NetworkManager has answered. Nothing here
 * logs an SSID or a passphrase.
 */
#ifndef CHEF_WIFI_H
#define CHEF_WIFI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WIFI_SSID_MAX 32		/* bytes, 802.11 */
#define WIFI_SSID_TEXT 100		/* displayable UTF-8, NUL-terminated */
#define WIFI_NETS_MAX 24
#define WIFI_PSK_MIN 8
#define WIFI_PSK_MAX 63
#define WIFI_PATH_MAX 128
#define WIFI_UUID_LEN 36

enum wifi_sec {
	WIFI_SEC_OPEN,
	WIFI_SEC_WEP,
	WIFI_SEC_WPA,		/* WPA1 PSK */
	WIFI_SEC_WPA2,		/* RSN PSK */
	WIFI_SEC_WPA3,		/* RSN SAE only */
	WIFI_SEC_WPA2_WPA3,	/* RSN PSK and SAE (transition) */
	WIFI_SEC_OWE,
	WIFI_SEC_ENTERPRISE,	/* 802.1X: shown, not joinable */
	WIFI_SEC_N
};

struct wifi_ssid {
	uint8_t b[WIFI_SSID_MAX];
	int len;
};

struct wifi_net {
	struct wifi_ssid ssid;
	char text[WIFI_SSID_TEXT];	/* displayable form of ssid */
	enum wifi_sec sec;
	uint32_t freq;			/* MHz, of the strongest AP */
	int strength;			/* 0..100 */
	bool known;			/* a saved profile has this SSID */
	char ap[WIFI_PATH_MAX];		/* strongest AP object path */
};

struct wifi_active {
	bool valid;			/* wlan0 has an active connection */
	char id[64];			/* connection Id */
	char text[WIFI_SSID_TEXT];	/* the AP's SSID (or the Id) */
	uint32_t dev_state;		/* NMDeviceState */
	enum wifi_sec sec;
	bool ap_known;			/* freq/strength/sec below are valid */
	uint32_t freq;
	int strength;
	char ip4[16];			/* "" until an address is set */
	int prefix;
	char gw[16];
	/* NetworkManager connectivity: only claimed when its check runs */
	bool connectivity_known;
	uint32_t connectivity;		/* NMConnectivityState */
	char conn[WIFI_PATH_MAX];	/* settings path */
	char uuid[WIFI_UUID_LEN + 1];	/* the profile's identity (Forget) */
};

enum wifi_join {
	WIFI_JOIN_IDLE,
	WIFI_JOIN_BUSY,
	WIFI_JOIN_DONE,
	WIFI_JOIN_BAD_PASSWORD,		/* the new profile was deleted */
	WIFI_JOIN_FAILED,		/* a new profile was deleted; a saved one kept */
};

struct wifi_view {
	bool scanning;
	int n;
	struct wifi_net nets[WIFI_NETS_MAX];	/* strongest first, active SSID excluded */
	struct wifi_active cur;
	struct {
		enum wifi_join st;
		char text[WIFI_SSID_TEXT];
	} join;
};

extern struct wifi_view wifi_view;

/* ------------------------------------------------------------ pure model */

/* NM80211ApFlags / NM80211ApSecurityFlags -> one of the above. */
enum wifi_sec wifi_sec_from_flags(uint32_t flags, uint32_t wpa, uint32_t rsn);
/* Joinable from this UI: open, or a PSK/SAE network (with a passphrase). */
bool wifi_sec_joinable(enum wifi_sec s);
bool wifi_sec_needs_psk(enum wifi_sec s);
/* "WPA2", "WPA2/WPA3", "Open" (short) and "WPA2 Personal" (long). */
const char *wifi_sec_short(enum wifi_sec s);
const char *wifi_sec_long(enum wifi_sec s);
/* NM key-mgmt for a new profile ("wpa-psk", "sae"), NULL for open. */
const char *wifi_sec_key_mgmt(enum wifi_sec s);

/* "2.4 GHz", "5 GHz", "6 GHz" or "" ; channel 0 when unknown. */
const char *wifi_band(uint32_t freq);
int wifi_channel(uint32_t freq);

/* Signal: 2 high, 1 low, 0 zero (the list's wifi-high/-low/-zero) and a
 * word ("strong", "good", "fair", "weak"). */
int wifi_signal_level(int strength);
const char *wifi_signal_word(int strength);

/* Raw SSID bytes -> displayable UTF-8 (invalid bytes and controls '?'). */
void wifi_ssid_text(const struct wifi_ssid *s, char *out, size_t n);
bool wifi_ssid_eq(const struct wifi_ssid *a, const struct wifi_ssid *b);

/* One access point as read from NetworkManager. */
struct wifi_ap {
	struct wifi_ssid ssid;
	uint32_t flags, wpa, rsn, freq;
	int strength;
	char path[WIFI_PATH_MAX];
};

/* Group APs by SSID (strongest wins), drop hidden (empty) SSIDs and the
 * active one, mark known ones, sort strongest first (then by name), keep
 * at most max. Returns the count. */
int wifi_group(const struct wifi_ap *aps, int n_aps, const struct wifi_ssid *active,
	       const struct wifi_ssid *known, int n_known, struct wifi_net *out, int max);

/* A device state reason (NMDeviceStateReason) that means the passphrase
 * was refused: no secrets after a failed handshake, supplicant
 * disconnect. */
bool wifi_reason_bad_password(uint32_t reason);

/* Texts for the pages. */
void wifi_net_subtitle(const struct wifi_net *n, char *buf, size_t size);
void wifi_active_subtitle(const struct wifi_active *a, char *buf, size_t size);

/* ------------------------------------------------- NetworkManager actions */

/* Join the netbus connection (after sys_netbus_start). Without it (fake
 * state, no sd-bus) the actions below return -ENOTCONN. */
void sys_wifi_init(void);

/* Keep wifi_view current (and scan every WIFI_SCAN_PERIOD_MS while scan
 * is set). Call with false when no Wi-Fi page is visible or the screen
 * is off; that drops the matches and stops scanning. */
void sys_wifi_watch(bool watch, bool scan);
#define WIFI_SCAN_PERIOD_MS 20000

/* WirelessEnabled. 0 or -errno (the request is asynchronous). */
int sys_wifi_set_enabled(bool on);
/* Activate a saved profile for, or add an open profile for, n. */
int sys_wifi_join(const struct wifi_net *n);
/* Add and activate a new profile with a passphrase. psk (NUL-terminated,
 * 8..63 printable ASCII; or empty for open) is wiped before this
 * returns, whatever the result. hidden: SSID not broadcast, ap may be
 * NULL. */
int sys_wifi_join_new(const struct wifi_ssid *ssid, enum wifi_sec sec, char *psk, bool hidden,
		      const char *ap);
/* Delete the profile with this UUID (the details page passes the one it
 * was opened for, so a connection that changed meanwhile is never the one
 * deleted). Queued until NetworkManager confirms, retried; see wifi.c. */
int sys_wifi_forget(const char *uuid);
/* A profile delete is still queued (tests). */
bool sys_wifi_cleanup_pending(void);
/* The page showed the join result: back to IDLE. */
void sys_wifi_join_ack(void);

/* Wipe a buffer so the compiler cannot drop it. */
void wifi_wipe(void *p, size_t n);

#endif /* CHEF_WIFI_H */
