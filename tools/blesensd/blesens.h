/*
 * blesens.h - BLE sensor measurements for blesensd: the GATT values of
 * Heart Rate (0x2A37), Cycling Power Measurement (0x2A63) and Cycling
 * Speed and Cadence Measurement (0x2A5B), and the revolution counters
 * that turn into cadence and speed. Pure code (no bus), unit tested in
 * tools/tests/test_blesens.c.
 */
#ifndef BLESENS_H
#define BLESENS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Sensor kinds, by the service a device exposes. */
enum bs_kind { BS_HR, BS_POWER, BS_CSC, BS_KINDS };

#define BS_UUID_HR	0x180D
#define BS_UUID_POWER	0x1818
#define BS_UUID_CSC	0x1816
#define BS_UUID_BATTERY	0x180F
#define BS_CHR_HR	0x2A37
#define BS_CHR_POWER	0x2A63
#define BS_CHR_CSC	0x2A5B
#define BS_CHR_BATTERY	0x2A19

const char *bs_kind_name(enum bs_kind k);	/* "hr", "power", "csc" */
int bs_kind_from_name(const char *s);		/* -1 if none */
/* The 16-bit UUID of a 128-bit Bluetooth base UUID string, or -1. */
int bs_uuid16(const char *uuid);
int bs_kind_of_service(int uuid16);		/* -1 if not a sensor service */
int bs_chr_of_kind(enum bs_kind k);

/* Heart Rate Measurement: bpm, or -1 if malformed (also when an optional
 * field the flags announce is truncated). */
int bs_parse_hr(const uint8_t *b, size_t n);

/*
 * Revolutions with a 16-bit event time, as CSC and CP report them (1/1024 s,
 * except CP wheel data: 1/2048 s). Feed each (cumulative revolutions, last
 * event time) pair; the rate is per minute (cadence, or wheel revolutions
 * for speed). The event time and the counters wrap; a repeat of the same
 * event time means no new revolution, and after BS_STOP_MS of those the
 * rate is 0 (stopped). A gap longer than the event time spans (65536 / hz
 * seconds) gives no rate for that pair.
 */
struct bs_revs {
	bool have;		/* a previous sample exists */
	uint32_t revs;
	uint16_t time;		/* 1/1024 s */
	int64_t last_change_ms;	/* when a new event arrived */
	double rate;		/* revolutions per minute, < 0 unknown */
};
#define BS_STOP_MS 3000

/* bits: 16 for crank counters, 32 for wheel counters; hz: event time
 * ticks per second (1024 or 2048). Returns the rate in revolutions per
 * minute (>= 0), or < 0 while unknown. */
double bs_revs_feed(struct bs_revs *r, uint32_t revs, uint16_t time, int bits, int hz, int64_t now_ms);
void bs_revs_reset(struct bs_revs *r);

/* What one notification carried. */
struct bs_sample {
	bool has_hr, has_power, has_crank, has_wheel;
	int hr, power;
	uint32_t crank_revs, wheel_revs;
	uint16_t crank_time, wheel_time;	/* 1/1024 s; CP wheel 1/2048 s */
};

/* Cycling Power Measurement / CSC Measurement. 0, or -1 if malformed
 * (also when a field the flags announce is truncated). */
int bs_parse_power(const uint8_t *b, size_t n, struct bs_sample *s);
int bs_parse_csc(const uint8_t *b, size_t n, struct bs_sample *s);

/* Plausibility: heart rate, cadence and speed above these are dropped. */
#define BS_HR_MAX 300		/* bpm */
#define BS_CADENCE_MAX 250.0	/* rpm */
#define BS_SPEED_MAX 120.0	/* km/h */

/* km/h from wheel revolutions per minute and the circumference in mm. */
double bs_speed_kmh(double wheel_rpm, int circ_mm);

#endif /* BLESENS_H */
