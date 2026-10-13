/*
 * blesens.c - BLE sensor measurement parsing (see blesens.h). Layouts from
 * the Bluetooth GATT specifications: Heart Rate Measurement, Cycling Power
 * Measurement, CSC Measurement; all little endian.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blesens.h"

static const char *const kind_names[BS_KINDS] = { "hr", "power", "csc" };

const char *bs_kind_name(enum bs_kind k)
{
	return (unsigned)k < BS_KINDS ? kind_names[k] : "?";
}

int bs_kind_from_name(const char *s)
{
	for (int i = 0; s && i < BS_KINDS; i++)
		if (!strcmp(s, kind_names[i]))
			return i;
	return -1;
}

int bs_uuid16(const char *u)
{
	char *end;
	unsigned long v;

	/* 0000XXXX-0000-1000-8000-00805f9b34fb */
	if (!u || strlen(u) != 36 || strncmp(u, "0000", 4) ||
	    strcasecmp(u + 8, "-0000-1000-8000-00805f9b34fb"))
		return -1;
	{
		char h[5] = { u[4], u[5], u[6], u[7], 0 };

		v = strtoul(h, &end, 16);
		if (*end)
			return -1;
	}
	return (int)v;
}

int bs_kind_of_service(int uuid16)
{
	return uuid16 == BS_UUID_HR ? BS_HR : uuid16 == BS_UUID_POWER ? BS_POWER
		: uuid16 == BS_UUID_CSC ? BS_CSC : -1;
}

int bs_chr_of_kind(enum bs_kind k)
{
	return k == BS_HR ? BS_CHR_HR : k == BS_POWER ? BS_CHR_POWER : BS_CHR_CSC;
}

static uint16_t le16(const uint8_t *b)
{
	return (uint16_t)(b[0] | b[1] << 8);
}

static uint32_t le32(const uint8_t *b)
{
	return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

int bs_parse_hr(const uint8_t *b, size_t n)
{
	size_t o;
	int v;

	if (n < 2)
		return -1;
	if (b[0] & 0x01) {		/* 16-bit value */
		if (n < 3)
			return -1;
		v = le16(b + 1);
		o = 3;
	} else {
		v = b[1];
		o = 2;
	}
	if (b[0] & 0x08)		/* energy expended */
		o += 2;
	if (o > n)
		return -1;
	if ((b[0] & 0x10) && ((n - o) == 0 || (n - o) % 2))	/* RR intervals, 16-bit each */
		return -1;
	return v;
}

void bs_revs_reset(struct bs_revs *r)
{
	memset(r, 0, sizeof(*r));
	r->rate = -1;
}

double bs_revs_feed(struct bs_revs *r, uint32_t revs, uint16_t time, int bits, int hz, int64_t now_ms)
{
	uint16_t dt;
	uint32_t dr;

	if (!r->have) {
		r->have = true;
		r->revs = revs;
		r->time = time;
		r->last_change_ms = now_ms;
		r->rate = -1;
		return r->rate;
	}
	dt = (uint16_t)(time - r->time);
	dr = bits == 16 ? (uint16_t)(revs - r->revs) : revs - r->revs;
	if (dt == 0) {
		/* the same event again: no revolution since; stopped after a while */
		if (now_ms - r->last_change_ms >= BS_STOP_MS)
			r->rate = 0;
		return r->rate;
	}
	/* a gap longer than the event time spans cannot be measured */
	if (now_ms - r->last_change_ms > 65536LL * 1000 / hz)
		r->rate = -1;
	else
		r->rate = (double)dr * 60.0 * (double)hz / (double)dt;
	r->revs = revs;
	r->time = time;
	r->last_change_ms = now_ms;
	return r->rate;
}

int bs_parse_power(const uint8_t *b, size_t n, struct bs_sample *s)
{
	uint16_t flags;
	size_t o = 4;

	memset(s, 0, sizeof(*s));
	if (n < 4)
		return -1;
	flags = le16(b);
	s->has_power = true;
	s->power = (int16_t)le16(b + 2);
	if (flags & 0x0001)		/* pedal power balance */
		o += 1;
	if (flags & 0x0004)		/* accumulated torque */
		o += 2;
	if (flags & 0x0010) {		/* wheel revolution data */
		if (n < o + 6)
			return -1;
		s->has_wheel = true;
		s->wheel_revs = le32(b + o);
		s->wheel_time = le16(b + o + 4);	/* 1/2048 s in CP */
		o += 6;
	}
	if (flags & 0x0020) {		/* crank revolution data */
		if (n < o + 4)
			return -1;
		s->has_crank = true;
		s->crank_revs = le16(b + o);
		s->crank_time = le16(b + o + 2);
		o += 4;
	}
	if (flags & 0x0040)		/* extreme force magnitudes */
		o += 4;
	if (flags & 0x0080)		/* extreme torque magnitudes */
		o += 4;
	if (flags & 0x0100)		/* extreme angles */
		o += 3;
	if (flags & 0x0200)		/* top dead spot angle */
		o += 2;
	if (flags & 0x0400)		/* bottom dead spot angle */
		o += 2;
	if (flags & 0x0800)		/* accumulated energy */
		o += 2;
	return o > n ? -1 : 0;
}

int bs_parse_csc(const uint8_t *b, size_t n, struct bs_sample *s)
{
	size_t o = 1;

	memset(s, 0, sizeof(*s));
	if (n < 1)
		return -1;
	if (b[0] & 0x01) {
		if (n < o + 6)
			return -1;
		s->has_wheel = true;
		s->wheel_revs = le32(b + o);
		s->wheel_time = le16(b + o + 4);
		o += 6;
	}
	if (b[0] & 0x02) {
		if (n < o + 4)
			return -1;
		s->has_crank = true;
		s->crank_revs = le16(b + o);
		s->crank_time = le16(b + o + 2);
	}
	return s->has_wheel || s->has_crank ? 0 : -1;
}

double bs_speed_kmh(double wheel_rpm, int circ_mm)
{
	return wheel_rpm * (double)circ_mm / 1000.0 * 60.0 / 1000.0;
}
