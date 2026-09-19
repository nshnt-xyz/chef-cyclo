/*
 * nmea-broker - feeds gpsd from the qmicli LOC follower (chef-cyclo).
 *
 * gpsd cannot read a pipe or FIFO (a non-tty file is treated as log replay
 * and stops at EOF) and there is no serial GNSS device on this phone: the
 * NMEA stream comes out of `qmicli --loc-follow-nmea` on stdout, one
 * sentence per line with the modem's own CRLF still attached, interleaved
 * with the multi-line "[position report]" blocks when
 * --loc-follow-position-report is on too (the ride image runs both in one
 * process). This tool sits on that stdout and forwards every valid-looking
 * NMEA sentence -- "$...*hh", printable ASCII, checksum verified -- as one
 * UDP datagram ("$...*hh\r\n") to a gpsd started as
 *
 *   gpsd -N -n -b udp://127.0.0.1:20175
 *
 * Everything else on stdin (position report lines, qmicli chatter, garbage,
 * sentences with a bad checksum) is dropped from the gpsd feed. UDP is
 * stateless, so the follower and gpsd can start, die and restart in any
 * order; a network send failure is counted, never fatal. UDP does not report
 * an absent listener, so packets sent before gpsd binds are simply discarded.
 *
 * The follower-side evidence path the ride image relies on is preserved:
 *   -t       tee: every stdin byte is passed through to stdout unchanged
 *            (so `qmicli ... | nmea-broker -t > follow.fifo` leaves
 *            ride-logger's follow_reader, follow.raw, nmea.log and
 *            positions.log exactly as they are today);
 *   -l FILE  append "<uptime> <sentence>" for every forwarded sentence,
 *            the same format as the ride image's nmea.log, so gpsd's
 *            output can be diffed against the raw stream (README 7.4).
 *
 * Clock: the initramfs boots with a 1970 wallclock. From the first RMC with
 * status 'A' that carries both a time and a date, and only while the system
 * year is still below 2000, the broker calls settimeofday() -- exactly once
 * per process, never again, and never when the clock already looks sane
 * (chrony, README item 8, will own the clock later; this is the stop-gap so
 * gpsd stops warning about bogus system time and local timestamps mean
 * something). The RMC date/time is validated field by field and rejected
 * outright if it decodes to before 2020-01-01 (a week-rollover or garbage
 * date must not be written to the clock). -n disables the clock step. No
 * hwclock, no RTC write, ever.
 *
 * Usage: nmea-broker [-a ADDR] [-p PORT] [-t] [-l FILE] [-n] [-v]
 *   -a  gpsd UDP address (default 127.0.0.1)
 *   -p  gpsd UDP port (default 20175)
 *   -t  tee stdin to stdout byte for byte
 *   -l  append "<uptime> <sentence>" per forwarded sentence to FILE
 *   -n  never touch the system clock
 *   -v  log every dropped line on stderr
 * Exits 0 at stdin EOF (the follower ended; the supervisor restarts the
 * pipeline), 1 on a usage/socket error or when the tee target is gone.
 * Counters (lines, sentences, forwarded, dropped, clock) go to stderr at
 * EOF.
 *
 * The pure parts (line trimming, sentence validation, checksum, RMC
 * date/time parsing, the civil-date arithmetic, datagram framing, the
 * set-once clock policy with an injectable clock and setter) are separated
 * from main() and covered by tools/tests/test_nmea-broker.c, which includes
 * this file with NMEA_BROKER_NO_MAIN and never calls settimeofday().
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define BROKER_DEFAULT_ADDR "127.0.0.1"
#define BROKER_DEFAULT_PORT 20175

/* NMEA 0183 caps a sentence at 82 bytes including "$" and CRLF; the
 * modem's longest so far is 77 ("$GPGSV"). Proprietary "$PQW*" sentences
 * are allowed some slack, anything longer is not a sentence. */
#define NMEA_MAX_SENTENCE 128
/* "$X*hh": the shortest thing that can carry a verified checksum. */
#define NMEA_MIN_SENTENCE 5
/* One input line. Longer lines are teed unchanged but never forwarded. */
#define LINE_BUF 512

/* 2000-01-01T00:00:00Z: below this the system clock is the 1970 boot
 * default (or the RTC's battery-connect count) and may be stepped. */
#define CLOCK_UNSET_BEFORE 946684800L
/* 2020-01-01T00:00:00Z: an RMC date that decodes to before this is not
 * believed (GPS week rollover, garbage), never written to the clock. */
#define CLOCK_FLOOR 1577836800L

struct broker_stats {
	unsigned long lines;        /* complete input lines seen */
	unsigned long overlong;     /* lines longer than LINE_BUF (teed only) */
	unsigned long sentences;    /* lines starting with '$' */
	unsigned long forwarded;    /* datagrams handed to the socket */
	unsigned long dropped;      /* '$' lines that failed validation */
	unsigned long send_failed;  /* sendto() errors (gpsd absent, etc.) */
	unsigned long rmc_valid;    /* status-A RMCs with date and time */
	int clock_set;              /* 1 once settimeofday() was called */
};

struct broker {
	int sock;
	struct sockaddr_in dst;
	FILE *tee;                  /* -t: stdout, or NULL */
	FILE *nmealog;              /* -l: "<uptime> <sentence>" log, or NULL */
	int no_clock;               /* -n */
	int verbose;                /* -v */
	int clock_done;             /* the one-shot clock decision was taken */
	/* Injectable for the host tests: wallclock reader and clock setter.
	 * main() wires time(NULL) and settimeofday(); the tests never do. */
	time_t (*now)(void);
	int (*set_clock)(time_t utc);
	double (*uptime)(void);
	struct broker_stats st;
};

static void logmsg(const char *fmt, ...)
{
	va_list ap;

	fputs("nmea-broker: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	fflush(stderr);
}

/* ---- pure: line trimming and sentence validation ---- */

/* Length of the line without its trailing CR/LF/whitespace. The modem
 * emits "...*hh\r\n" and qmicli adds nothing, but a bare "\n" (nmea.log
 * replay, tests) must work the same. */
static size_t nmea_trim(const char *line, size_t len)
{
	while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n' ||
			   line[len - 1] == ' ' || line[len - 1] == '\t'))
		len--;
	return len;
}

static int hexval(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return -1;
}

/* XOR of the bytes between '$' and '*' of a trimmed sentence; the caller
 * guarantees s[0] == '$' and s[len - 3] == '*'. */
static unsigned nmea_checksum(const char *s, size_t len)
{
	unsigned c = 0;
	size_t i;

	for (i = 1; i + 3 < len; i++)
		c ^= (unsigned char)s[i];
	return c & 0xff;
}

/* 1 if s[0..len) (already trimmed) is a sentence gpsd should see:
 * "$", 1..(MAX-5) printable ASCII body bytes with no further '$' or '*',
 * then "*hh" whose value matches the body's XOR. Both hex cases accepted. */
static int nmea_sentence_ok(const char *s, size_t len)
{
	size_t i;
	int hi, lo;

	if (len < NMEA_MIN_SENTENCE || len > NMEA_MAX_SENTENCE)
		return 0;
	if (s[0] != '$' || s[len - 3] != '*')
		return 0;
	for (i = 1; i < len - 3; i++) {
		unsigned char c = (unsigned char)s[i];

		if (c < 0x20 || c > 0x7e || c == '$' || c == '*')
			return 0;
	}
	hi = hexval((unsigned char)s[len - 2]);
	lo = hexval((unsigned char)s[len - 1]);
	if (hi < 0 || lo < 0)
		return 0;
	return nmea_checksum(s, len) == (unsigned)(hi << 4 | lo);
}

/* One datagram per sentence, canonical NMEA framing: the trimmed sentence
 * plus exactly one CRLF. Returns the datagram length, 0 if it does not fit. */
static size_t nmea_frame(const char *s, size_t len, char *out, size_t cap)
{
	if (len + 2 > cap)
		return 0;
	memcpy(out, s, len);
	out[len] = '\r';
	out[len + 1] = '\n';
	return len + 2;
}

/* ---- pure: RMC date/time ---- */

/* Days since 1970-01-01 of a proleptic-Gregorian civil date (Howard
 * Hinnant's algorithm), so the epoch never depends on the process's TZ or
 * on the C library having timegm(). Valid for any year >= 1. */
static int64_t days_from_civil(int y, int m, int d)
{
	int64_t era, yoe, doy, doe;

	y -= m <= 2;
	era = (y >= 0 ? y : y - 399) / 400;
	yoe = y - era * 400;
	doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

static int is_leap(int y)
{
	return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static int days_in_month(int y, int m)
{
	static const int dm[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

	return m == 2 && is_leap(y) ? 29 : dm[m - 1];
}

static int64_t utc_epoch(int y, int mo, int d, int h, int mi, int s)
{
	return days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s;
}

/* Exactly n ASCII digits at p, value out. */
static int digits(const char *p, size_t n, int *out)
{
	int v = 0;
	size_t i;

	for (i = 0; i < n; i++) {
		if (p[i] < '0' || p[i] > '9')
			return 0;
		v = v * 10 + (p[i] - '0');
	}
	*out = v;
	return 1;
}

struct rmc_time {
	int year, month, day, hour, min, sec;
	int64_t epoch;
};

/* Parse a trimmed RMC sentence's UTC time and date.
 * Returns  1: status 'A' with a well-formed hhmmss[.sss] time and ddmmyy
 *             date (fraction ignored; sec 60 allowed for a leap second),
 *          0: an RMC that is not usable (status V, empty/malformed fields),
 *         -1: not an RMC at all ("$" + two-letter talker + "RMC," only). */
static int rmc_parse(const char *s, size_t len, struct rmc_time *out)
{
	const char *f[16];
	size_t nf = 0, i, body;
	int hh, mm, ss, dd, mo, yy;

	if (len < 7 || s[0] != '$' || !isalpha((unsigned char)s[1]) ||
	    !isalpha((unsigned char)s[2]) || memcmp(s + 3, "RMC,", 4) != 0)
		return -1;
	/* Field starts (after each comma), body ends at '*' or at len. */
	body = len;
	for (i = 1; i < len; i++)
		if (s[i] == '*') {
			body = i;
			break;
		}
	for (i = 0; i < body; i++)
		if (s[i] == ',') {
			if (nf == sizeof(f) / sizeof(f[0]))
				return 0;
			f[nf++] = s + i + 1;
		}
	/* f[0]=time f[1]=status f[2..7]=lat/lon/speed/course f[8]=date */
	if (nf < 9)
		return 0;
	if (!(f[1] + 1 <= s + body && f[1][0] == 'A' && (f[1] + 1 == s + body || f[1][1] == ',')))
		return 0;
	/* time: 6 digits, then either the next comma or ".<digits>" */
	if (f[0] + 6 > s + body || !digits(f[0], 6, &hh))
		return 0;
	if (f[0][6] == '.') {
		const char *p = f[0] + 7;

		if (p >= s + body || *p == ',')
			return 0;
		while (p < s + body && *p != ',') {
			if (*p < '0' || *p > '9')
				return 0;
			p++;
		}
	} else if (f[0][6] != ',') {
		return 0;
	}
	ss = hh % 100;
	mm = (hh / 100) % 100;
	hh = hh / 10000;
	if (hh > 23 || mm > 59 || ss > 60)
		return 0;
	/* date: exactly 6 digits then a comma (or the body end) */
	if (f[8] + 6 > s + body || !digits(f[8], 6, &dd))
		return 0;
	if (f[8] + 6 < s + body && f[8][6] != ',')
		return 0;
	yy = dd % 100;
	mo = (dd / 100) % 100;
	dd = dd / 10000;
	if (mo < 1 || mo > 12 || dd < 1 || dd > days_in_month(2000 + yy, mo))
		return 0;
	out->year = 2000 + yy;
	out->month = mo;
	out->day = dd;
	out->hour = hh;
	out->min = mm;
	out->sec = ss;
	out->epoch = utc_epoch(out->year, mo, dd, hh, mm, ss);
	return 1;
}

/* ---- clock policy (set-once, injectable) ---- */

/* Called with every usable (status-A, dated) RMC until the decision is
 * taken: leave the clock alone if it is already past 2000, otherwise step it
 * exactly once. A too-old date is not a decision -- a later sane RMC may
 * still set the clock. */
static void clock_consider(struct broker *b, const struct rmc_time *t)
{
	time_t now;

	if (b->clock_done || b->no_clock)
		return;
	if (t->epoch < CLOCK_FLOOR) {
		logmsg("RMC date %04d-%02d-%02d is before 2020, not trusted for the clock",
		       t->year, t->month, t->day);
		return;
	}
	now = b->now();
	if (now >= CLOCK_UNSET_BEFORE) {
		logmsg("system clock already set (%ld), leaving it alone", (long)now);
		b->clock_done = 1;
		return;
	}
	b->clock_done = 1;
	if (b->set_clock((time_t)t->epoch) == 0) {
		b->st.clock_set = 1;
		logmsg("system clock set from RMC: %04d-%02d-%02dT%02d:%02d:%02dZ (was %ld, epoch %lld)",
		       t->year, t->month, t->day, t->hour, t->min, t->sec, (long)now, (long long)t->epoch);
	} else {
		logmsg("settimeofday failed: %s (not retried)", strerror(errno));
	}
}

/* ---- the loop ---- */

/* Read one line into buf: up to cap bytes, stopping after '\n'. Returns the
 * byte count (0 at EOF with nothing read); *complete is 1 if the chunk ends
 * with '\n'. A chunk that fills the buffer without a newline is the head of
 * an overlong line -- the caller keeps reading chunks until one completes,
 * teeing them all and forwarding none. Byte-exact: NUL bytes are kept. */
static size_t read_chunk(FILE *in, char *buf, size_t cap, int *complete)
{
	size_t n = 0;
	int c;

	*complete = 0;
	while (n < cap) {
		c = getc(in);
		if (c == EOF)
			break;
		buf[n++] = (char)c;
		if (c == '\n') {
			*complete = 1;
			break;
		}
	}
	return n;
}

static void forward(struct broker *b, const char *line, size_t len)
{
	char dgram[NMEA_MAX_SENTENCE + 2];
	struct rmc_time t;
	size_t slen, dlen;

	slen = nmea_trim(line, len);
	if (slen == 0 || line[0] != '$')
		return;
	b->st.sentences++;
	if (!nmea_sentence_ok(line, slen)) {
		b->st.dropped++;
		if (b->verbose)
			logmsg("drop: %.*s", (int)(slen > 60 ? 60 : slen), line);
		return;
	}
	dlen = nmea_frame(line, slen, dgram, sizeof(dgram));
	if (dlen == 0) {
		b->st.dropped++;
		return;
	}
	if (b->sock >= 0) {
		if (sendto(b->sock, dgram, dlen, 0, (struct sockaddr *)&b->dst, sizeof(b->dst)) < 0) {
			if (b->st.send_failed++ == 0)
				logmsg("sendto: %s (network error; counting, not fatal)", strerror(errno));
		} else if (++b->st.forwarded == 1) {
			logmsg("first sentence forwarded: %.6s", line);
		}
	} else {
		b->st.forwarded++;
	}
	if (b->nmealog) {
		fprintf(b->nmealog, "%.2f %.*s\n", b->uptime(), (int)slen, line);
		fflush(b->nmealog);
	}
	if (rmc_parse(line, slen, &t) == 1) {
		b->st.rmc_valid++;
		clock_consider(b, &t);
	}
}

/* Runs until stdin EOF. Returns 0, or 1 if the tee target went away. */
static int broker_run(struct broker *b, FILE *in)
{
	char buf[LINE_BUF];
	size_t n;
	int complete, in_overlong = 0;

	while ((n = read_chunk(in, buf, sizeof(buf), &complete)) > 0) {
		if (b->tee) {
			if (fwrite(buf, 1, n, b->tee) != n || (complete && fflush(b->tee) != 0)) {
				logmsg("tee write failed: %s", strerror(errno));
				return 1;
			}
		}
		if (in_overlong) {
			/* continuation of a line that did not fit */
			if (complete)
				in_overlong = 0;
			continue;
		}
		if (!complete) {
			if (n == sizeof(buf)) {
				b->st.overlong++;
				in_overlong = 1;
			}
			/* else: unterminated final line at EOF -- teed, not forwarded */
			continue;
		}
		b->st.lines++;
		forward(b, buf, n);
	}
	if (b->tee)
		fflush(b->tee);
	return 0;
}

static void broker_init(struct broker *b)
{
	memset(b, 0, sizeof(*b));
	b->sock = -1;
}

static int broker_set_dst(struct broker *b, const char *addr, int port)
{
	if (port < 1 || port > 65535)
		return 0;
	memset(&b->dst, 0, sizeof(b->dst));
	b->dst.sin_family = AF_INET;
	b->dst.sin_port = htons((uint16_t)port);
	return inet_pton(AF_INET, addr, &b->dst.sin_addr) == 1;
}

static void broker_report(const struct broker *b)
{
	logmsg("eof: lines=%lu overlong=%lu sentences=%lu forwarded=%lu dropped=%lu send_failed=%lu rmc_valid=%lu clock=%s",
	       b->st.lines, b->st.overlong, b->st.sentences, b->st.forwarded, b->st.dropped,
	       b->st.send_failed, b->st.rmc_valid,
	       b->st.clock_set ? "set" : b->no_clock ? "disabled" : b->clock_done ? "left" : "untouched");
}

#ifndef NMEA_BROKER_NO_MAIN

static time_t clock_now_system(void)
{
	return time(NULL);
}

static int clock_set_system(time_t utc)
{
	struct timeval tv;

	tv.tv_sec = utc;
	tv.tv_usec = 0;
	return settimeofday(&tv, NULL);
}

/* Same reference as /proc/uptime's first field (ride-logger's nmea.log). */
static double uptime_system(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0 && clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0.0;
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void usage(void)
{
	fputs("usage: nmea-broker [-a ADDR] [-p PORT] [-t] [-l FILE] [-n] [-v]\n"
	      "  reads qmicli --loc-follow-nmea output on stdin, sends each valid\n"
	      "  NMEA sentence as one UDP datagram to gpsd (default 127.0.0.1:20175)\n"
	      "  -t  tee stdin to stdout unchanged   -l  log '<uptime> <sentence>' to FILE\n"
	      "  -n  never set the system clock      -v  log dropped lines\n", stderr);
}

int main(int argc, char **argv)
{
	struct broker b;
	const char *addr = BROKER_DEFAULT_ADDR;
	const char *logpath = NULL;
	int port = BROKER_DEFAULT_PORT;
	int opt, rc;

	broker_init(&b);
	b.now = clock_now_system;
	b.set_clock = clock_set_system;
	b.uptime = uptime_system;
	while ((opt = getopt(argc, argv, "a:p:tl:nvh")) != -1) {
		switch (opt) {
		case 'a': addr = optarg; break;
		case 'p': port = atoi(optarg); break;
		case 't': b.tee = stdout; break;
		case 'l': logpath = optarg; break;
		case 'n': b.no_clock = 1; break;
		case 'v': b.verbose = 1; break;
		default: usage(); return opt == 'h' ? 0 : 1;
		}
	}
	if (optind != argc) {
		usage();
		return 1;
	}
	if (!broker_set_dst(&b, addr, port)) {
		logmsg("bad address/port: %s:%d", addr, port);
		return 1;
	}
	b.sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (b.sock < 0) {
		logmsg("socket: %s", strerror(errno));
		return 1;
	}
	if (logpath) {
		b.nmealog = fopen(logpath, "a");
		if (!b.nmealog) {
			logmsg("%s: %s", logpath, strerror(errno));
			return 1;
		}
	}
	/* A dead tee reader must not kill us silently; broker_run reports it. */
	signal(SIGPIPE, SIG_IGN);
	logmsg("forwarding NMEA from stdin to udp://%s:%d%s%s", addr, port,
	       b.tee ? ", tee to stdout" : "", b.no_clock ? ", clock untouched" : "");
	rc = broker_run(&b, stdin);
	broker_report(&b);
	if (b.nmealog)
		fclose(b.nmealog);
	close(b.sock);
	return rc;
}

#endif /* NMEA_BROKER_NO_MAIN */
