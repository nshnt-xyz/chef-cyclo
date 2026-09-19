/* Host tests for tools/nmea-broker.c: line trimming, sentence validation
 * and checksum, RMC date/time parsing, the TZ-independent civil-date
 * arithmetic, the set-once clock policy (with an injected clock and
 * setter -- settimeofday() is never called here, and the test checks the
 * host clock did not move), and the whole read-filter-frame loop against a
 * real loopback UDP socket: one datagram per valid sentence, CRLF framing,
 * order, tee byte-exactness (including CRLF, NUL bytes and overlong lines)
 * and the "<uptime> <sentence>" log. nmea-broker.c is included with
 * NMEA_BROKER_NO_MAIN so main() and the system clock hooks are compiled
 * out. Fixture sentences follow the shape of the 2026-09-18 terrace run
 * (empty-field-but-checksummed before the fix, 13-field RMC with the date
 * after it) with made-up coordinates.
 * Build/run: see tools/Makefile ("make test").
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define NMEA_BROKER_NO_MAIN
#include "../nmea-broker.c"

static int g_failures;
static int g_tests;

#define CHECK(cond) do { \
	g_tests++; \
	if (!(cond)) { \
		g_failures++; \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	} \
} while (0)

/* ---- fixtures (terrace-run shapes, fictional positions) ---- */

/* Pre-fix cycle exactly as the modem emits it (checksums are real). */
#define GGA_EMPTY "$GPGGA,,,,,,0,,,,,,,,*66"
#define VTG_EMPTY "$GPVTG,,T,,M,,N,,K,N*2C"
#define GSA_EMPTY "$GPGSA,A,1,,,,,,,,,,,,,,,*1E"
#define RMC_EMPTY "$GPRMC,,V,,,,,,,,,,N*53"
#define GSV_1     "$GPGSV,4,1,16,01,,,,03,09,059,,05,,,,06,75,195,*70"
/* Fixed: 2026-09-18 17:15:41 UTC = 1789751741 (the terrace run's
 * position-report utc_ms was 1789751741000 for this very second). */
#define RMC_FIX   "$GPRMC,171541.00,A,1234.567890,N,01234.567890,E,1.7,237.6,180926,0.9,W,A*2B"
#define RMC_FIX_EPOCH 1789751741LL

static unsigned xor_body(const char *body)
{
	unsigned c = 0;

	while (*body)
		c ^= (unsigned char)*body++;
	return c;
}

/* "$" + body + "*hh" with the right checksum, into a static buffer. */
static const char *mk(const char *body)
{
	static char buf[8][256];
	static int slot;
	char *p = buf[slot++ & 7];

	snprintf(p, 256, "$%s*%02X", body, xor_body(body));
	return p;
}

/* ---- trimming ---- */

static void test_trim(void)
{
	CHECK(nmea_trim("abc\r\n", 5) == 3);
	CHECK(nmea_trim("abc\n", 4) == 3);
	CHECK(nmea_trim("abc", 3) == 3);
	CHECK(nmea_trim("abc \t\r\n", 7) == 3);
	CHECK(nmea_trim("\r\n", 2) == 0);
	CHECK(nmea_trim("", 0) == 0);
	/* only trailing whitespace goes */
	CHECK(nmea_trim(" abc\n", 5) == 4);
}

/* ---- checksum + validation ---- */

static void test_checksum(void)
{
	CHECK(nmea_checksum(GGA_EMPTY, strlen(GGA_EMPTY)) == 0x66);
	CHECK(nmea_checksum(VTG_EMPTY, strlen(VTG_EMPTY)) == 0x2c);
	CHECK(nmea_checksum(GSA_EMPTY, strlen(GSA_EMPTY)) == 0x1e);
	CHECK(nmea_checksum(RMC_EMPTY, strlen(RMC_EMPTY)) == 0x53);
	CHECK(nmea_checksum(GSV_1, strlen(GSV_1)) == 0x70);
	CHECK(nmea_checksum(RMC_FIX, strlen(RMC_FIX)) == 0x2b);
	CHECK(hexval('0') == 0 && hexval('9') == 9 && hexval('A') == 10 &&
	      hexval('F') == 15 && hexval('a') == 10 && hexval('f') == 15);
	CHECK(hexval('G') < 0 && hexval('g') < 0 && hexval(' ') < 0 && hexval('*') < 0);
}

static int ok(const char *s)
{
	return nmea_sentence_ok(s, strlen(s));
}

static void test_sentence_ok(void)
{
	char big[NMEA_MAX_SENTENCE + 10];
	char ctl[64];

	/* the real pre-fix cycle passes as-is (gpsd parses empty fields) */
	CHECK(ok(GGA_EMPTY));
	CHECK(ok(VTG_EMPTY));
	CHECK(ok(GSA_EMPTY));
	CHECK(ok(RMC_EMPTY));
	CHECK(ok(GSV_1));
	CHECK(ok(RMC_FIX));
	/* lowercase hex is accepted */
	CHECK(ok("$GPRMC,,V,,,,,,,,,,N*53"));
	CHECK(ok(mk("PQWTXT,1,proprietary")));

	/* wrong checksum: one bit off, or a swapped digit */
	CHECK(!ok("$GPGGA,,,,,,0,,,,,,,,*67"));
	CHECK(!ok("$GPRMC,,V,,,,,,,,,,N*35"));
	/* a corrupted body with the original checksum */
	CHECK(!ok("$GPGGA,,,,,,1,,,,,,,,*66"));
	/* no checksum at all, or a malformed one */
	CHECK(!ok("$GPGGA,,,,,,0,,,,,,,,"));
	CHECK(!ok("$GPGGA,,,,,,0,,,,,,,,*6"));
	CHECK(!ok("$GPGGA,,,,,,0,,,,,,,,*6G"));
	CHECK(!ok("$GPGGA,,,,,,0,,,,,,,,*66 "));
	CHECK(!ok("$GPGGA,,,,,,0,,,,,,,,*666"));
	/* '*' anywhere else, second '$', non-printable bytes */
	CHECK(!ok("$GPG*A,,,,,,0,,,,,,,,*66"));
	CHECK(!ok("$GPGGA,,,,,,0,,,,,,,,*66$GPVTG,,T,,M,,N,,K,N*2C"));
	CHECK(!ok(mk("GPGGA$,,,,,,0")));
	snprintf(ctl, sizeof(ctl), "$GPGGA,\x01,0*%02X", xor_body("GPGGA,\x01,0"));
	CHECK(!ok(ctl));
	snprintf(ctl, sizeof(ctl), "$GPGGA,\x7f,0*%02X", xor_body("GPGGA,\x7f,0"));
	CHECK(!ok(ctl));
	snprintf(ctl, sizeof(ctl), "$GPGGA,\xc3\xa9,0*%02X", xor_body("GPGGA,\xc3\xa9,0"));
	CHECK(!ok(ctl));
	/* embedded CR is a control byte too (a CRLF that was not trimmed) */
	CHECK(!ok("$GPGGA,,,,,,0,,,,,,,,*66\r\n"));
	/* not sentences: the qmicli position report lines, prose, empties */
	CHECK(!ok("[position report] status: in-progress"));
	CHECK(!ok("   latitude:  n/a"));
	CHECK(!ok("cancelling the operation..."));
	CHECK(!ok(""));
	CHECK(!ok("$"));
	CHECK(!ok("$*00"));
	CHECK(!ok(" $GPGGA,,,,,,0,,,,,,,,*66"));
	/* minimum: "$X*hh" */
	CHECK(ok(mk("X")));
	CHECK(!ok("$*58"));

	/* length bound: exactly NMEA_MAX_SENTENCE passes, one more fails */
	memset(big, 'A', sizeof(big));
	big[0] = '$';
	{
		char body[NMEA_MAX_SENTENCE];

		memset(body, 'A', NMEA_MAX_SENTENCE - 4);
		body[NMEA_MAX_SENTENCE - 4] = '\0';
		snprintf(big, sizeof(big), "$%s*%02X", body, xor_body(body));
		CHECK(strlen(big) == NMEA_MAX_SENTENCE);
		CHECK(ok(big));
		memset(body, 'A', NMEA_MAX_SENTENCE - 3);
		body[NMEA_MAX_SENTENCE - 3] = '\0';
		snprintf(big, sizeof(big), "$%s*%02X", body, xor_body(body));
		CHECK(strlen(big) == NMEA_MAX_SENTENCE + 1);
		CHECK(!ok(big));
	}
}

/* ---- framing ---- */

static void test_frame(void)
{
	char out[NMEA_MAX_SENTENCE + 2];
	size_t n;

	n = nmea_frame(GGA_EMPTY, strlen(GGA_EMPTY), out, sizeof(out));
	CHECK(n == strlen(GGA_EMPTY) + 2);
	CHECK(memcmp(out, GGA_EMPTY, strlen(GGA_EMPTY)) == 0);
	CHECK(out[n - 2] == '\r' && out[n - 1] == '\n');
	/* exactly one CRLF, never a doubled one */
	CHECK(out[n - 3] == '6');
	/* does not fit: refused, nothing written past cap */
	CHECK(nmea_frame(GGA_EMPTY, strlen(GGA_EMPTY), out, strlen(GGA_EMPTY) + 1) == 0);
	CHECK(nmea_frame(GGA_EMPTY, strlen(GGA_EMPTY), out, strlen(GGA_EMPTY) + 2) == strlen(GGA_EMPTY) + 2);
}

/* ---- civil dates ---- */

static void test_days_from_civil(void)
{
	int y, m, d;
	int mismatches = 0;

	CHECK(days_from_civil(1970, 1, 1) == 0);
	CHECK(days_from_civil(1970, 1, 2) == 1);
	CHECK(days_from_civil(1969, 12, 31) == -1);
	CHECK(days_from_civil(2000, 1, 1) == 946684800LL / 86400);
	CHECK(days_from_civil(2000, 3, 1) == 11017);
	CHECK(days_from_civil(2020, 1, 1) == 1577836800LL / 86400);
	CHECK(days_from_civil(2024, 2, 29) == 19782);
	CHECK(days_from_civil(2024, 3, 1) == 19783);
	CHECK(days_from_civil(2026, 9, 18) == RMC_FIX_EPOCH / 86400);
	CHECK(days_from_civil(2100, 3, 1) - days_from_civil(2100, 2, 28) == 1); /* 2100 not leap */
	CHECK(days_from_civil(2000, 3, 1) - days_from_civil(2000, 2, 28) == 2); /* 2000 leap */
	CHECK(utc_epoch(2026, 9, 18, 17, 15, 41) == RMC_FIX_EPOCH);
	CHECK(utc_epoch(1970, 1, 1, 0, 0, 0) == 0);
	CHECK(utc_epoch(2000, 1, 1, 0, 0, 0) == CLOCK_UNSET_BEFORE);
	CHECK(utc_epoch(2020, 1, 1, 0, 0, 0) == CLOCK_FLOOR);

	/* every day of the two-digit-year range against the host's timegm */
	for (y = 2000; y <= 2099; y++)
		for (m = 1; m <= 12; m++)
			for (d = 1; d <= days_in_month(y, m); d++) {
				struct tm tm;
				time_t t;

				memset(&tm, 0, sizeof(tm));
				tm.tm_year = y - 1900;
				tm.tm_mday = d;
				tm.tm_mon = m - 1;
				t = timegm(&tm);
				if ((int64_t)t != days_from_civil(y, m, d) * 86400)
					mismatches++;
			}
	CHECK(mismatches == 0);

	CHECK(is_leap(2000) && is_leap(2024) && !is_leap(2100) && !is_leap(2023));
	CHECK(days_in_month(2024, 2) == 29 && days_in_month(2023, 2) == 28);
	CHECK(days_in_month(2026, 9) == 30 && days_in_month(2026, 12) == 31);
}

/* ---- RMC ---- */

static int rmc(const char *s, struct rmc_time *t)
{
	memset(t, 0, sizeof(*t));
	return rmc_parse(s, strlen(s), t);
}

static void test_rmc_parse(void)
{
	struct rmc_time t;

	/* the fixed sentence */
	CHECK(rmc(RMC_FIX, &t) == 1);
	CHECK(t.year == 2026 && t.month == 9 && t.day == 18);
	CHECK(t.hour == 17 && t.min == 15 && t.sec == 41);
	CHECK(t.epoch == RMC_FIX_EPOCH);

	/* the pre-fix sentence: status V, no date -> not usable, but an RMC */
	CHECK(rmc(RMC_EMPTY, &t) == 0);
	/* time present but V and no date (what the modem shows just before the fix) */
	CHECK(rmc(mk("GPRMC,171540.00,V,,,,,,,,,N"), &t) == 0);
	/* status A but no date */
	CHECK(rmc(mk("GPRMC,171541.00,A,1234.56,N,01234.56,E,1.7,237.6,,0.9,W,A"), &t) == 0);
	/* status A, date but no time */
	CHECK(rmc(mk("GPRMC,,A,1234.56,N,01234.56,E,1.7,237.6,180926,0.9,W,A"), &t) == 0);
	/* not an RMC at all */
	CHECK(rmc(GGA_EMPTY, &t) == -1);
	CHECK(rmc(GSV_1, &t) == -1);
	CHECK(rmc("$GPRMX,171541.00,A,,,,,,,180926,,,A*00", &t) == -1);
	CHECK(rmc("$G1RMC,171541.00,A,,,,,,,180926,,,A*00", &t) == -1);
	CHECK(rmc("$PRMC,171541.00,A,,,,,,,180926,,,A*00", &t) == -1);
	CHECK(rmc("GPRMC,171541.00,A,,,,,,,180926,,,A*00", &t) == -1);
	CHECK(rmc("", &t) == -1);
	CHECK(rmc("$GPRMC", &t) == -1);
	CHECK(rmc("$GPRMC,", &t) == 0);

	/* other talkers */
	CHECK(rmc(mk("GNRMC,171541.00,A,1234.56,N,01234.56,E,1.7,237.6,180926,0.9,W,A"), &t) == 1);
	CHECK(t.epoch == RMC_FIX_EPOCH);
	CHECK(rmc(mk("GLRMC,171541,A,1234.56,N,01234.56,E,1.7,237.6,180926,0.9,W"), &t) == 1);
	CHECK(t.epoch == RMC_FIX_EPOCH);
	/* NMEA 2.x shape: 12 fields, no nav status */
	CHECK(rmc(mk("GPRMC,171541.00,A,1234.56,N,01234.56,E,1.7,237.6,180926,,"), &t) == 1);
	CHECK(t.epoch == RMC_FIX_EPOCH);
	/* date as the last field, without the checksum marker */
	CHECK(rmc_parse("$GPRMC,171541,A,,,,,,,180926", strlen("$GPRMC,171541,A,,,,,,,180926"), &t) == 1);
	CHECK(t.epoch == RMC_FIX_EPOCH);
	/* time without fraction, with a long fraction; fraction is ignored */
	CHECK(rmc(mk("GPRMC,171541,A,,,,,,,180926,,,A"), &t) == 1 && t.sec == 41);
	CHECK(rmc(mk("GPRMC,171541.999999,A,,,,,,,180926,,,A"), &t) == 1 && t.sec == 41);
	/* leap second 60 is legal */
	CHECK(rmc(mk("GPRMC,235960.00,A,,,,,,,311226,,,A"), &t) == 1);
	CHECK(t.epoch == utc_epoch(2026, 12, 31, 23, 59, 60));
	CHECK(t.epoch == utc_epoch(2027, 1, 1, 0, 0, 0));

	/* malformed times */
	CHECK(rmc(mk("GPRMC,241541.00,A,,,,,,,180926,,,A"), &t) == 0);
	CHECK(rmc(mk("GPRMC,176041.00,A,,,,,,,180926,,,A"), &t) == 0);
	CHECK(rmc(mk("GPRMC,171561.00,A,,,,,,,180926,,,A"), &t) == 0);
	CHECK(rmc(mk("GPRMC,17154.00,A,,,,,,,180926,,,A"), &t) == 0);
	CHECK(rmc(mk("GPRMC,1715411.00,A,,,,,,,180926,,,A"), &t) == 0);
	CHECK(rmc(mk("GPRMC,1715a1.00,A,,,,,,,180926,,,A"), &t) == 0);
	CHECK(rmc(mk("GPRMC,171541.,A,,,,,,,180926,,,A"), &t) == 0);
	CHECK(rmc(mk("GPRMC,171541.0x,A,,,,,,,180926,,,A"), &t) == 0);
	CHECK(rmc(mk("GPRMC,171541:00,A,,,,,,,180926,,,A"), &t) == 0);
	/* malformed dates */
	CHECK(rmc(mk("GPRMC,171541.00,A,,,,,,,181326,,,A"), &t) == 0);
	CHECK(rmc(mk("GPRMC,171541.00,A,,,,,,,180026,,,A"), &t) == 0);
	CHECK(rmc(mk("GPRMC,171541.00,A,,,,,,,000926,,,A"), &t) == 0);
	CHECK(rmc(mk("GPRMC,171541.00,A,,,,,,,310926,,,A"), &t) == 0);  /* Sep 31 */
	CHECK(rmc(mk("GPRMC,171541.00,A,,,,,,,290223,,,A"), &t) == 0);  /* 2023 not leap */
	CHECK(rmc(mk("GPRMC,171541.00,A,,,,,,,290224,,,A"), &t) == 1);  /* 2024 leap */
	CHECK(rmc(mk("GPRMC,171541.00,A,,,,,,,18092,,,A"), &t) == 0);
	CHECK(rmc(mk("GPRMC,171541.00,A,,,,,,,1809261,,,A"), &t) == 0);
	CHECK(rmc(mk("GPRMC,171541.00,A,,,,,,,18O926,,,A"), &t) == 0);
	/* status that is not exactly "A" */
	CHECK(rmc(mk("GPRMC,171541.00,AA,,,,,,,180926,,,A"), &t) == 0);
	CHECK(rmc(mk("GPRMC,171541.00,a,,,,,,,180926,,,A"), &t) == 0);
	CHECK(rmc(mk("GPRMC,171541.00,,,,,,,,180926,,,A"), &t) == 0);
	/* too few fields */
	CHECK(rmc(mk("GPRMC,171541.00,A,,,,,,"), &t) == 0);
	/* absurdly many fields is refused, not overrun */
	CHECK(rmc(mk("GPRMC,171541.00,A,,,,,,,180926,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,"), &t) == 0);
}

/* The epoch must not depend on the process time zone. */
static void test_rmc_timezone(void)
{
	static const char *zones[] = { "UTC", "Asia/Kolkata", "America/Los_Angeles",
				       "XYZ-13", "ABC+11:30", "Europe/Berlin", "" };
	struct rmc_time t;
	size_t i;

	for (i = 0; i < sizeof(zones) / sizeof(zones[0]); i++) {
		setenv("TZ", zones[i], 1);
		tzset();
		CHECK(rmc(RMC_FIX, &t) == 1);
		CHECK(t.epoch == RMC_FIX_EPOCH);
		CHECK(utc_epoch(2026, 9, 18, 17, 15, 41) == RMC_FIX_EPOCH);
	}
	unsetenv("TZ");
	tzset();
}

/* ---- clock policy ---- */

static time_t g_now;
static int g_set_calls;
static time_t g_set_value;
static int g_set_rc;

static time_t fake_now(void)
{
	return g_now;
}

static int fake_set(time_t utc)
{
	g_set_calls++;
	g_set_value = utc;
	return g_set_rc;
}

static double fake_uptime(void)
{
	return 123.45;
}

static void init_test_broker(struct broker *b)
{
	broker_init(b);
	b->now = fake_now;
	b->set_clock = fake_set;
	b->uptime = fake_uptime;
	g_set_calls = 0;
	g_set_value = 0;
	g_set_rc = 0;
}

static void feed(struct broker *b, const char *s)
{
	char line[300];

	snprintf(line, sizeof(line), "%s\r\n", s);
	forward(b, line, strlen(line));
}

static void test_clock_sets_once_when_1970(void)
{
	struct broker b;

	init_test_broker(&b);
	g_now = 86400 + 4 * 3600;                    /* 1970-01-02, as on the phone */
	feed(&b, GGA_EMPTY);
	feed(&b, RMC_EMPTY);                         /* V: nothing */
	CHECK(g_set_calls == 0 && !b.clock_done);
	feed(&b, mk("GPRMC,171540.00,V,,,,,,,,,N"));/* time but V: nothing */
	CHECK(g_set_calls == 0 && !b.clock_done);
	feed(&b, RMC_FIX);
	CHECK(g_set_calls == 1);
	CHECK(g_set_value == RMC_FIX_EPOCH);
	CHECK(b.clock_done && b.st.clock_set == 1);
	CHECK(b.st.rmc_valid == 1);
	/* later valid RMCs never touch the clock again */
	feed(&b, mk("GPRMC,171542.00,A,1234.56,N,01234.56,E,1.7,237.6,180926,0.9,W,A"));
	feed(&b, mk("GPRMC,171543.00,A,1234.56,N,01234.56,E,1.7,237.6,180926,0.9,W,A"));
	CHECK(g_set_calls == 1);
	CHECK(b.st.rmc_valid == 3);
	CHECK(b.st.forwarded == 6 && b.st.dropped == 0);
}

static void test_clock_left_alone_when_already_set(void)
{
	struct broker b;

	init_test_broker(&b);
	g_now = RMC_FIX_EPOCH - 5;                   /* a sane clock, a few seconds off */
	feed(&b, RMC_FIX);
	CHECK(g_set_calls == 0);
	CHECK(b.clock_done && b.st.clock_set == 0);
	feed(&b, RMC_FIX);
	CHECK(g_set_calls == 0);
	CHECK(b.st.rmc_valid == 2);

	/* boundary: exactly 2000-01-01 counts as set */
	init_test_broker(&b);
	g_now = CLOCK_UNSET_BEFORE;
	feed(&b, RMC_FIX);
	CHECK(g_set_calls == 0 && b.clock_done);
	/* one second before: unset, stepped */
	init_test_broker(&b);
	g_now = CLOCK_UNSET_BEFORE - 1;
	feed(&b, RMC_FIX);
	CHECK(g_set_calls == 1 && g_set_value == RMC_FIX_EPOCH);
}

static void test_clock_disabled(void)
{
	struct broker b;

	init_test_broker(&b);
	b.no_clock = 1;
	g_now = 86400;
	feed(&b, RMC_FIX);
	feed(&b, RMC_FIX);
	CHECK(g_set_calls == 0);
	CHECK(!b.clock_done);
	CHECK(b.st.rmc_valid == 2);              /* still counted as evidence */
	CHECK(b.st.forwarded == 2);              /* still forwarded */
}

static void test_clock_rejects_untrusted_dates(void)
{
	struct broker b;

	init_test_broker(&b);
	g_now = 86400;
	/* a week-rollover-style 2019 date: valid RMC, refused, decision still open */
	feed(&b, mk("GPRMC,171541.00,A,1234.56,N,01234.56,E,1.7,237.6,061019,0.9,W,A"));
	CHECK(g_set_calls == 0);
	CHECK(!b.clock_done);
	CHECK(b.st.rmc_valid == 1);
	/* 1999 / 2000 (two-digit years wrap into 20xx, still below the floor) */
	feed(&b, mk("GPRMC,171541.00,A,,,,,,,010100,,,A"));
	feed(&b, mk("GPRMC,171541.00,A,,,,,,,311219,,,A"));
	CHECK(g_set_calls == 0 && !b.clock_done);
	/* floor boundary: 2020-01-01T00:00:00Z is accepted */
	feed(&b, mk("GPRMC,000000.00,A,,,,,,,010120,,,A"));
	CHECK(g_set_calls == 1 && g_set_value == CLOCK_FLOOR);
	CHECK(b.clock_done);
}

static void test_clock_set_failure_not_retried(void)
{
	struct broker b;

	init_test_broker(&b);
	g_now = 86400;
	g_set_rc = -1;
	errno = EPERM;
	feed(&b, RMC_FIX);
	CHECK(g_set_calls == 1);
	CHECK(b.clock_done && b.st.clock_set == 0);
	feed(&b, RMC_FIX);
	CHECK(g_set_calls == 1);
}

static void test_clock_ignores_bad_checksum_rmc(void)
{
	struct broker b;

	init_test_broker(&b);
	g_now = 86400;
	/* the right shape with a wrong checksum is dropped before the clock logic */
	feed(&b, "$GPRMC,171541.00,A,1234.567890,N,01234.567890,E,1.7,237.6,180926,0.9,W,A*2C");
	CHECK(g_set_calls == 0 && !b.clock_done);
	CHECK(b.st.dropped == 1 && b.st.forwarded == 0 && b.st.rmc_valid == 0);
}

/* ---- the loop against a real loopback UDP socket ---- */

static int bind_loopback(struct sockaddr_in *sa)
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	socklen_t sl = sizeof(*sa);

	if (fd < 0)
		return -1;
	memset(sa, 0, sizeof(*sa));
	sa->sin_family = AF_INET;
	sa->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sa->sin_port = 0;
	if (bind(fd, (struct sockaddr *)sa, sizeof(*sa)) < 0 ||
	    getsockname(fd, (struct sockaddr *)sa, &sl) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static FILE *tmp_with(const char *data, size_t len)
{
	FILE *f = tmpfile();

	if (!f)
		return NULL;
	fwrite(data, 1, len, f);
	rewind(f);
	return f;
}

static size_t slurp(FILE *f, char *out, size_t cap)
{
	size_t n;

	rewind(f);
	n = fread(out, 1, cap, f);
	return n;
}

static ssize_t recv_one(int fd, char *buf, size_t cap)
{
	return recv(fd, buf, cap, MSG_DONTWAIT);
}

static void test_loop_udp_framing_and_tee(void)
{
	/* A realistic slice of follower output: a pre-fix cycle with CRLF
	 * sentences, a position report block, a GSV, a corrupted sentence, a
	 * bare-LF sentence, qmicli chatter, an empty line, a line with a NUL,
	 * then the fix. Expected datagrams: the valid sentences in order. */
	static const char input[] =
		GGA_EMPTY "\r\n"
		VTG_EMPTY "\r\n"
		GSA_EMPTY "\r\n"
		RMC_EMPTY "\r\n"
		"[position report] status: in-progress\n"
		"   latitude:  n/a\n"
		"   UTC timestamp: n/a\n"
		"   Altitude assumed: n/a\n"
		GSV_1 "\r\n"
		"$GPGSV,4,2,16,07,,,,09,,,,11,38,209,,12,,,*44\r\n"   /* bad checksum */
		"$GPGSV,4,3,16,13,,,,14,48,088,,15,,,,17,39,026,*7E\n" /* bare LF */
		"cancelling the operation...\n"
		"\n"
		"\r\n"
		"nul\0byte line\n"
		RMC_FIX "\r\n";
	static const char *expect[] = {
		GGA_EMPTY, VTG_EMPTY, GSA_EMPTY, RMC_EMPTY, GSV_1,
		"$GPGSV,4,3,16,13,,,,14,48,088,,15,,,,17,39,026,*7E", RMC_FIX,
	};
	struct sockaddr_in sa;
	struct broker b;
	FILE *in, *tee, *nlog;
	char buf[1024], tee_out[sizeof(input)];
	size_t i, n;
	int fd;

	fd = bind_loopback(&sa);
	CHECK(fd >= 0);
	if (fd < 0)
		return;
	init_test_broker(&b);
	g_now = 86400;
	b.sock = socket(AF_INET, SOCK_DGRAM, 0);
	CHECK(b.sock >= 0);
	b.dst = sa;
	in = tmp_with(input, sizeof(input) - 1);
	tee = tmpfile();
	nlog = tmpfile();
	b.tee = tee;
	b.nmealog = nlog;
	CHECK(in && tee && nlog);

	CHECK(broker_run(&b, in) == 0);

	/* one datagram per valid sentence, in order, sentence + exactly CRLF */
	for (i = 0; i < sizeof(expect) / sizeof(expect[0]); i++) {
		ssize_t r = recv_one(fd, buf, sizeof(buf));
		size_t want = strlen(expect[i]);

		CHECK(r == (ssize_t)(want + 2));
		if (r == (ssize_t)(want + 2)) {
			CHECK(memcmp(buf, expect[i], want) == 0);
			CHECK(buf[want] == '\r' && buf[want + 1] == '\n');
		}
	}
	CHECK(recv_one(fd, buf, sizeof(buf)) < 0);  /* nothing more */

	/* counters */
	CHECK(b.st.lines == 16);
	CHECK(b.st.sentences == 8);
	CHECK(b.st.forwarded == 7);
	CHECK(b.st.dropped == 1);
	CHECK(b.st.send_failed == 0);
	CHECK(b.st.overlong == 0);
	CHECK(b.st.rmc_valid == 1);
	/* the clock decision ran on the fix: 1970 -> stepped once */
	CHECK(g_set_calls == 1 && g_set_value == RMC_FIX_EPOCH);

	/* tee is byte-exact, including CRLFs and the NUL byte */
	n = slurp(tee, tee_out, sizeof(tee_out));
	CHECK(n == sizeof(input) - 1);
	CHECK(n == sizeof(input) - 1 && memcmp(tee_out, input, n) == 0);

	/* nmea log: "<uptime> <sentence>\n" for each forwarded sentence */
	n = slurp(nlog, buf, sizeof(buf) - 1);
	buf[n] = '\0';
	{
		char *p = buf;
		size_t k = 0;

		while (*p) {
			char *nl = strchr(p, '\n');
			char want[300];

			CHECK(nl != NULL);
			if (!nl)
				break;
			*nl = '\0';
			CHECK(k < sizeof(expect) / sizeof(expect[0]));
			if (k < sizeof(expect) / sizeof(expect[0])) {
				snprintf(want, sizeof(want), "123.45 %s", expect[k]);
				CHECK(strcmp(p, want) == 0);
			}
			k++;
			p = nl + 1;
		}
		CHECK(k == sizeof(expect) / sizeof(expect[0]));
	}

	fclose(in);
	fclose(tee);
	fclose(nlog);
	close(b.sock);
	close(fd);
}

static void test_loop_overlong_and_partial_lines(void)
{
	struct sockaddr_in sa;
	struct broker b;
	FILE *in, *tee;
	char *input, *p, buf[1024], *tee_out;
	size_t len, n;
	int fd;

	/* LINE_BUF*3 bytes of 'A' containing a valid sentence in the middle,
	 * then a normal sentence, then an unterminated one. */
	len = LINE_BUF * 3 + 200;
	input = malloc(len);
	tee_out = malloc(len);
	CHECK(input && tee_out);
	if (!input || !tee_out)
		return;
	p = input;
	*p++ = '$';
	memset(p, 'A', LINE_BUF);
	p += LINE_BUF;
	memcpy(p, GGA_EMPTY, strlen(GGA_EMPTY));
	p += strlen(GGA_EMPTY);
	memset(p, 'A', LINE_BUF * 2 - 100);
	p += LINE_BUF * 2 - 100;
	*p++ = '\n';
	memcpy(p, VTG_EMPTY "\r\n", strlen(VTG_EMPTY) + 2);
	p += strlen(VTG_EMPTY) + 2;
	memcpy(p, GSA_EMPTY, strlen(GSA_EMPTY));   /* no newline: partial */
	p += strlen(GSA_EMPTY);
	len = (size_t)(p - input);

	fd = bind_loopback(&sa);
	CHECK(fd >= 0);
	if (fd < 0)
		return;
	init_test_broker(&b);
	b.sock = socket(AF_INET, SOCK_DGRAM, 0);
	b.dst = sa;
	in = tmp_with(input, len);
	tee = tmpfile();
	b.tee = tee;
	CHECK(broker_run(&b, in) == 0);

	/* only the VTG made it, the overlong line and the partial tail did not */
	n = (size_t)recv_one(fd, buf, sizeof(buf));
	CHECK(n == strlen(VTG_EMPTY) + 2);
	CHECK(n == strlen(VTG_EMPTY) + 2 && memcmp(buf, VTG_EMPTY "\r\n", n) == 0);
	CHECK(recv_one(fd, buf, sizeof(buf)) < 0);
	CHECK(b.st.overlong == 1);
	CHECK(b.st.lines == 1);
	CHECK(b.st.forwarded == 1);
	CHECK(b.st.dropped == 0);
	/* everything, including the overlong line and the partial tail, was teed */
	n = slurp(tee, tee_out, len);
	CHECK(n == len && memcmp(tee_out, input, len) == 0);

	fclose(in);
	fclose(tee);
	close(b.sock);
	close(fd);
	free(input);
	free(tee_out);
}

static void test_loop_no_gpsd_is_not_fatal(void)
{
	struct sockaddr_in sa;
	struct broker b;
	FILE *in;
	int fd;

	/* bind to learn a free port, close it, then send there: nobody listens */
	fd = bind_loopback(&sa);
	CHECK(fd >= 0);
	if (fd < 0)
		return;
	close(fd);
	init_test_broker(&b);
	b.sock = socket(AF_INET, SOCK_DGRAM, 0);
	b.dst = sa;
	in = tmp_with(GGA_EMPTY "\r\n" VTG_EMPTY "\r\n" GSA_EMPTY "\r\n",
		      strlen(GGA_EMPTY "\r\n" VTG_EMPTY "\r\n" GSA_EMPTY "\r\n"));
	CHECK(broker_run(&b, in) == 0);
	/* an unconnected UDP socket does not see ICMP unreachable: every send
	 * "succeeds"; whichever way the kernel reports it, the loop ran through */
	CHECK(b.st.sentences == 3);
	CHECK(b.st.forwarded + b.st.send_failed == 3);
	fclose(in);
	close(b.sock);
}

static void test_loop_without_socket_counts(void)
{
	struct broker b;
	FILE *in;

	/* sock < 0: pure filter, used by the tests above without UDP */
	init_test_broker(&b);
	in = tmp_with(GGA_EMPTY "\n" "junk\n" "$bad*00\n", strlen(GGA_EMPTY "\n" "junk\n" "$bad*00\n"));
	CHECK(broker_run(&b, in) == 0);
	CHECK(b.st.lines == 3 && b.st.sentences == 2 && b.st.forwarded == 1 && b.st.dropped == 1);
	broker_report(&b);   /* the EOF summary line; must not crash on a bare broker */
	fclose(in);
}

static void test_set_dst(void)
{
	struct broker b;

	broker_init(&b);
	CHECK(b.sock == -1);
	CHECK(broker_set_dst(&b, "127.0.0.1", 20175));
	CHECK(b.dst.sin_family == AF_INET);
	CHECK(ntohs(b.dst.sin_port) == 20175);
	CHECK(ntohl(b.dst.sin_addr.s_addr) == INADDR_LOOPBACK);
	CHECK(!broker_set_dst(&b, "localhost", 20175));
	CHECK(!broker_set_dst(&b, "::1", 20175));
	CHECK(!broker_set_dst(&b, "127.0.0.1", 0));
	CHECK(!broker_set_dst(&b, "127.0.0.1", 65536));
	CHECK(!broker_set_dst(&b, "127.0.0.1", -1));
}

static void test_read_chunk(void)
{
	char buf[8];
	FILE *f = tmp_with("ab\ncdefghij\n\nk", 14);
	int complete;

	CHECK(f != NULL);
	if (!f)
		return;
	CHECK(read_chunk(f, buf, sizeof(buf), &complete) == 3 && complete && buf[2] == '\n');
	CHECK(read_chunk(f, buf, sizeof(buf), &complete) == 8 && !complete);  /* cdefghij */
	CHECK(read_chunk(f, buf, sizeof(buf), &complete) == 1 && complete);   /* \n */
	CHECK(read_chunk(f, buf, sizeof(buf), &complete) == 1 && complete);   /* empty line */
	CHECK(read_chunk(f, buf, sizeof(buf), &complete) == 1 && !complete && buf[0] == 'k');
	CHECK(read_chunk(f, buf, sizeof(buf), &complete) == 0);
	fclose(f);
}

int main(void)
{
	time_t before = time(NULL);
	time_t after;

	test_trim();
	test_checksum();
	test_sentence_ok();
	test_frame();
	test_days_from_civil();
	test_rmc_parse();
	test_rmc_timezone();
	test_clock_sets_once_when_1970();
	test_clock_left_alone_when_already_set();
	test_clock_disabled();
	test_clock_rejects_untrusted_dates();
	test_clock_set_failure_not_retried();
	test_clock_ignores_bad_checksum_rmc();
	test_loop_udp_framing_and_tee();
	test_loop_overlong_and_partial_lines();
	test_loop_no_gpsd_is_not_fatal();
	test_loop_without_socket_counts();
	test_set_dst();
	test_read_chunk();

	/* Nothing above may have moved the host clock: the only setter wired
	 * in is fake_set, and a real step to 2026-09-18 or 1970 would show. */
	after = time(NULL);
	CHECK(after >= before && after - before < 60);

	printf("test-nmea-broker: %d checks, %d failures\n", g_tests, g_failures);
	return g_failures ? 1 : 0;
}
