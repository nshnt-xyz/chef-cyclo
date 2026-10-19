/*
 * blesensd's state D, the helpers (clock, log, addresses, kinds, names)
 * and the lookups every part shares. See main.c for the daemon as a
 * whole.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "blesensd.h"

bool bd_verbose;
int bd_scale = 1;
int bd_retry_s = 300;	/* -r: reconnect this long without a hold */

pid_t bd_self;

/* ------------------------------------------------------------ helpers */

int64_t bd_now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void bd_notef(const char *fmt, ...)
{
	char buf[300];
	va_list ap;
	int fd;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	fprintf(stderr, "blesensd: %s\n", buf);
	fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC | O_NONBLOCK);
	if (fd >= 0) {
		dprintf(fd, "blesensd: %s\n", buf);
		close(fd);
	}
}



/* "AA:BB:CC:DD:EE:FF" (upper case hex) */
bool bd_addr_ok(const char *a)
{
	if (!a || strlen(a) != 17)
		return false;
	for (int i = 0; i < 17; i++)
		if (i % 3 == 2 ? a[i] != ':' : !isxdigit((unsigned char)a[i]) || islower((unsigned char)a[i]))
			return false;
	return true;
}

/* JSON string body: quotes, backslashes and controls escaped. */
void bd_json_str(char *out, size_t n, const char *s)
{
	size_t o = 0;

	for (; s && *s && o + 7 < n; s++) {
		unsigned char c = (unsigned char)*s;

		if (c == '"' || c == '\\') {
			out[o++] = '\\';
			out[o++] = (char)c;
		} else if (c < 0x20 || c == 0x7f) {
			o += (size_t)snprintf(out + o, n - o, "\\u%04x", c);
		} else {
			out[o++] = (char)c;
		}
	}
	out[o] = 0;
}

void bd_kinds_str(unsigned kinds, char *out, size_t n)
{
	size_t o = 0;

	out[0] = 0;
	for (int k = 0; k < BS_KINDS; k++)
		if (kinds & (1u << k))
			o += (size_t)snprintf(out + o, n - o, "%s%s", o ? "," : "", bs_kind_name(k));
}

unsigned bd_kinds_parse(const char *s)
{
	char buf[64], *save = NULL, *t;
	unsigned k = 0;

	snprintf(buf, sizeof(buf), "%s", s ? s : "");
	for (t = strtok_r(buf, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
		int i = bs_kind_from_name(t);

		if (i < 0)
			return 0;
		k |= 1u << i;
	}
	return k;
}

/* Printable ASCII only, at most n-1 bytes, no leading or trailing spaces:
 * names go into records and JSON. */
void bd_name_clean(char *out, size_t n, const char *s)
{
	size_t o = 0;

	while (s && *s == ' ')
		s++;
	for (; s && *s && o + 1 < n; s++)
		out[o++] = (*s >= 0x20 && *s < 0x7f) ? *s : '?';
	while (o && out[o - 1] == ' ')
		o--;
	out[o] = 0;
}


struct bd_state D = { .bus_fd = -1, .load_fd = -1 };

struct dev *bd_dev_by_path(const char *p)
{
	for (int i = 0; i < MAX_DEVS; i++)
		if (D.devs[i].used && !strcmp(D.devs[i].path, p))
			return &D.devs[i];
	return NULL;
}

struct dev *bd_dev_by_addr(const char *a)
{
	for (int i = 0; i < MAX_DEVS; i++)
		if (D.devs[i].used && !strcmp(D.devs[i].addr, a))
			return &D.devs[i];
	return NULL;
}

struct sensor *bd_sensor_by_addr(const char *a)
{
	for (int i = 0; i < MAX_SENSORS; i++)
		if (D.sensors[i].used && !strcmp(D.sensors[i].addr, a))
			return &D.sensors[i];
	return NULL;
}

struct sensor *bd_sensor_by_sid(uint32_t sid)
{
	for (int i = 0; i < MAX_SENSORS; i++)
		if (D.sensors[i].used && D.sensors[i].sid == sid)
			return &D.sensors[i];
	return NULL;
}

struct chr *bd_chr_by_path(const char *p)
{
	for (int i = 0; i < MAX_CHRS; i++)
		if (D.chrs[i].used && !strcmp(D.chrs[i].path, p))
			return &D.chrs[i];
	return NULL;
}

/* The characteristic uuid16 of the device at dev path. */
struct chr *bd_chr_of(const char *dev, int uuid16)
{
	for (int i = 0; i < MAX_CHRS; i++)
		if (D.chrs[i].used && D.chrs[i].uuid16 == uuid16 && !strcmp(D.chrs[i].dev, dev))
			return &D.chrs[i];
	return NULL;
}

/* "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF/service0010/char0011" -> device */
void bd_dev_of_path(const char *p, char *out, size_t n)
{
	const char *d = strstr(p, "/dev_");
	const char *e = d ? strchr(d + 1, '/') : NULL;
	size_t l = d ? (e ? (size_t)(e - p) : strlen(p)) : 0;

	if (l >= n)
		l = 0;
	memcpy(out, p, l);
	out[l] = 0;
}

bool bd_is_sensor_chr(int u)
{
	return u == BS_CHR_HR || u == BS_CHR_POWER || u == BS_CHR_CSC || u == BS_CHR_BATTERY;
}

int bd_sensor_battery(const struct sensor *s)
{
	const struct dev *d = bd_dev_by_addr(s->addr);

	if (d && d->battery >= 0 && d->connected)
		return d->battery;
	return s->gatt_battery;
}

bool bd_sensor_connected(const struct sensor *s)
{
	const struct dev *d = bd_dev_by_addr(s->addr);

	return D.powered && d && d->connected && d->resolved;
}

bool bd_held(void)
{
	for (int i = 0; i < MAX_CLIENTS; i++)
		if (D.clients[i].fd >= 0 && D.clients[i].hold)
			return true;
	return false;
}

/* Reconnect now? While held, or within the window after a trigger. */
bool bd_links_wanted(void)
{
	return bd_held() || bd_now_ms() < D.retry_until;
}


const char *bd_sensor_state(const struct sensor *s)
{
	const struct dev *d = bd_dev_by_addr(s->addr);

	if (s->forgetting)
		return "forgetting";
	if (!D.powered)
		return "off";
	if (bd_sensor_connected(s))
		return "connected";
	return bd_links_wanted() || s->connect_inflight || (d && d->connected) ? "connecting" : "waiting";
}
