/* Host tests for tools/blesens.c: HR 8/16-bit, CP with optional fields,
 * CSC wheel and crank data, rates with 16-bit event-time and counter
 * rollover, stopped after BS_STOP_MS, the 64 s gap, UUIDs and kinds. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../blesens.h"

static int g_tests, g_failures;

#define CHECK(c) do { g_tests++; if (!(c)) { g_failures++; \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static bool near(double a, double b)
{
	return fabs(a - b) < 0.01;
}

int main(void)
{
	struct bs_sample s;
	struct bs_revs r;

	/* heart rate */
	CHECK(bs_parse_hr((const uint8_t[]){ 0x00, 84 }, 2) == 84);
	CHECK(bs_parse_hr((const uint8_t[]){ 0x01, 0x2c, 0x01 }, 3) == 300);
	CHECK(bs_parse_hr((const uint8_t[]){ 0x16, 142, 0x10, 0x03 }, 4) == 142);	/* RR follows */
	CHECK(bs_parse_hr((const uint8_t[]){ 0x01, 0x2c }, 2) == -1);
	CHECK(bs_parse_hr((const uint8_t[]){ 0x00 }, 1) == -1);
	/* energy expended and RR intervals: present when flagged, else malformed */
	CHECK(bs_parse_hr((const uint8_t[]){ 0x08, 90, 0x10, 0x00 }, 4) == 90);
	CHECK(bs_parse_hr((const uint8_t[]){ 0x08, 90 }, 2) == -1);
	CHECK(bs_parse_hr((const uint8_t[]){ 0x08, 90, 0x10 }, 3) == -1);
	CHECK(bs_parse_hr((const uint8_t[]){ 0x10, 90, 0x10 }, 3) == -1);		/* one-byte RR */
	CHECK(bs_parse_hr((const uint8_t[]){ 0x10, 90 }, 2) == -1);			/* RR flagged, none */
	CHECK(bs_parse_hr((const uint8_t[]){ 0x18, 90, 1, 0, 0x10, 0x03, 0x20 }, 7) == -1);
	CHECK(bs_parse_hr((const uint8_t[]){ 0x19, 0x2c, 0x01, 1, 0, 0x10, 0x03 }, 7) == 300);

	/* cycling power: flags, power; balance + torque + wheel + crank */
	CHECK(bs_parse_power((const uint8_t[]){ 0x00, 0x00, 0xf5, 0x00 }, 4, &s) == 0 && s.power == 245 &&
	      !s.has_crank);
	CHECK(bs_parse_power((const uint8_t[]){ 0x00, 0x00, 0xf6, 0xff }, 4, &s) == 0 && s.power == -10);
	CHECK(bs_parse_power((const uint8_t[]){ 0x35, 0x00, 0xf5, 0x00, 50, 0x10, 0x00,
			     1, 0, 0, 0, 0x00, 0x08, 0x2a, 0x00, 0x00, 0x04 }, 17, &s) == 0);
	CHECK(s.power == 245 && s.has_wheel && s.wheel_revs == 1 && s.wheel_time == 0x0800 &&
	      s.has_crank && s.crank_revs == 42 && s.crank_time == 0x0400);
	CHECK(bs_parse_power((const uint8_t[]){ 0x20, 0x00, 0xf5, 0x00, 0x2a }, 5, &s) == -1);
	CHECK(bs_parse_power((const uint8_t[]){ 0x00, 0x00, 0xf5 }, 3, &s) == -1);
	/* optional fields announced but missing: balance, torque, extremes, energy */
	CHECK(bs_parse_power((const uint8_t[]){ 0x01, 0x00, 0xf5, 0x00 }, 4, &s) == -1);
	CHECK(bs_parse_power((const uint8_t[]){ 0x04, 0x00, 0xf5, 0x00, 0x01 }, 5, &s) == -1);
	CHECK(bs_parse_power((const uint8_t[]){ 0x40, 0x00, 0xf5, 0x00 }, 4, &s) == -1);
	CHECK(bs_parse_power((const uint8_t[]){ 0x80, 0x00, 0xf5, 0x00, 1, 2, 3 }, 7, &s) == -1);
	CHECK(bs_parse_power((const uint8_t[]){ 0x00, 0x01, 0xf5, 0x00, 1, 2 }, 6, &s) == -1);
	CHECK(bs_parse_power((const uint8_t[]){ 0x00, 0x08, 0xf5, 0x00, 1 }, 5, &s) == -1);
	CHECK(bs_parse_power((const uint8_t[]){ 0x60, 0x00, 0xf5, 0x00, 0x2a, 0x00, 0x00, 0x04 }, 8, &s) == -1);
	/* all of them present: crank data still found after balance and torque */
	CHECK(bs_parse_power((const uint8_t[]){ 0xe5, 0x0f, 0xf5, 0x00, 50, 0x10, 0x00, 0x2a, 0x00, 0x00, 0x04,
			     1, 2, 3, 4, 5, 6, 7, 8, 1, 2, 3, 1, 2, 1, 2, 1, 2 }, 28, &s) == 0 &&
	      s.has_crank && s.crank_revs == 42 && s.crank_time == 0x0400 && !s.has_wheel);

	/* CSC */
	CHECK(bs_parse_csc((const uint8_t[]){ 0x03, 0x10, 0, 0, 0, 0x00, 0x04, 0x05, 0, 0x00, 0x08 }, 11, &s) == 0);
	CHECK(s.has_wheel && s.wheel_revs == 16 && s.wheel_time == 1024 && s.has_crank && s.crank_revs == 5 &&
	      s.crank_time == 2048);
	CHECK(bs_parse_csc((const uint8_t[]){ 0x02, 0x05, 0x00, 0x00, 0x08 }, 5, &s) == 0 && !s.has_wheel &&
	      s.crank_revs == 5);
	CHECK(bs_parse_csc((const uint8_t[]){ 0x01, 0x10, 0, 0 }, 4, &s) == -1);
	CHECK(bs_parse_csc((const uint8_t[]){ 0x00 }, 1, &s) == -1);

	/* rates: 1 crank revolution per 1024/1024 s = 60 rpm */
	bs_revs_reset(&r);
	CHECK(bs_revs_feed(&r, 100, 1000, 16, 1024, 0) < 0);			/* first: unknown */
	CHECK(near(bs_revs_feed(&r, 101, 2024, 16, 1024, 1000), 60));
	CHECK(near(bs_revs_feed(&r, 101, 2024, 16, 1024, 2000), 60));		/* same event: held */
	CHECK(near(bs_revs_feed(&r, 101, 2024, 16, 1024, 4100), 0));		/* 3 s: stopped */
	/* 90 rpm across the event-time and counter rollover */
	bs_revs_reset(&r);
	bs_revs_feed(&r, 65535, 65000, 16, 1024, 0);
	CHECK(near(bs_revs_feed(&r, 2, (uint16_t)(65000 + 2048), 16, 1024, 2000), 90));
	/* wheel counters are 32-bit */
	bs_revs_reset(&r);
	bs_revs_feed(&r, 0xfffffffe, 0, 32, 1024, 0);
	CHECK(near(bs_revs_feed(&r, 3, 1024, 32, 1024, 1000), 300));
	/* a gap beyond 64 s: no rate from that pair */
	CHECK(bs_revs_feed(&r, 10, 2048, 32, 1024, 70000) < 0);
	CHECK(near(bs_revs_feed(&r, 11, 3072, 32, 1024, 71000), 60));
	/* CP wheel event time: 1/2048 s, so 2048 ticks is one second */
	bs_revs_reset(&r);
	bs_revs_feed(&r, 0, 0, 32, 2048, 0);
	CHECK(near(bs_revs_feed(&r, 2, 2048, 32, 2048, 1000), 120));
	/* its span is 32 s: a 40 s gap gives no rate */
	CHECK(bs_revs_feed(&r, 3, 4096, 32, 2048, 41000) < 0);
	/* speed: 2105 mm at 200 wheel rpm = 25.26 km/h */
	CHECK(near(bs_speed_kmh(200, 2105), 25.26));

	/* UUIDs and kinds */
	CHECK(bs_uuid16("0000180d-0000-1000-8000-00805f9b34fb") == 0x180d);
	CHECK(bs_uuid16("00001816-0000-1000-8000-00805F9B34FB") == 0x1816);
	CHECK(bs_uuid16("6e400001-b5a3-f393-e0a9-e50e24dcca9e") == -1);
	CHECK(bs_kind_of_service(0x1818) == BS_POWER && bs_kind_of_service(0x180f) == -1);
	CHECK(bs_kind_from_name("csc") == BS_CSC && bs_kind_from_name("x") == -1);
	CHECK(!strcmp(bs_kind_name(BS_HR), "hr") && bs_chr_of_kind(BS_CSC) == 0x2a5b);

	printf("test-blesens: %d/%d checks passed\n", g_tests - g_failures, g_tests);
	return g_failures != 0;
}
