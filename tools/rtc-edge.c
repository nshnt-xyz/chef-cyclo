/*
 * rtc-edge: wall clock against the PMIC RTC at sub-second precision, for
 * chef-state (initramfs/usr/bin/chef-state, docs/features/storage.md).
 *
 * The PM660 RTC (rtc0, qpnp_rtc) is write-disabled and only exposes whole
 * seconds (`since_epoch`, counting from battery connect), and busybox
 * `date` can neither read nor set sub-second time (no %N, and 1.37 even
 * rejects `date -s @N`). The wall-minus-RTC offset is still good to a few
 * milliseconds if it is taken exactly where since_epoch ticks:
 *
 *   rtc-edge [-r RTCDIR] sample        wait for the next since_epoch tick and
 *                                      print "SINCE WALL_NS" for that instant
 *   rtc-edge [-r RTCDIR] [-n] set OFFSET_NS
 *                                      wait for the next tick and set the
 *                                      clock to SINCE * 1e9 + OFFSET_NS (plus
 *                                      the time since the tick)
 *   rtc-edge [-n] floor WALL_S         set the clock to WALL_S if it is
 *                                      behind it; exit 4 if it is not
 *
 * The tick is found by reading since_epoch every 2 ms (each read goes to
 * the PMIC over SPMI); its instant is the middle of the interval between
 * the start of the last read that returned the old value and the end of
 * the first read that returned the new one, so it is good to about +-1.5 ms.
 * Times are carried across with CLOCK_MONOTONIC. No tick within 1.1 s, or a
 * jump of more than one second, is exit 3. -n prints "set WALL_NS" instead
 * of calling clock_settime (host tests). Exit 1: usage or unreadable
 * since_epoch; 2: clock_settime failed.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define RTC_DIR		"/sys/class/rtc/rtc0"
#define POLL_NS		2000000LL
#define EDGE_WAIT_NS	1100000000LL
#define NS		1000000000LL

static int64_t now_ns(clockid_t c)
{
	struct timespec ts;

	clock_gettime(c, &ts);
	return (int64_t)ts.tv_sec * NS + ts.tv_nsec;
}

/* since_epoch as a positive integer, or -1. */
static int64_t read_since(int fd)
{
	char buf[32], *end;
	ssize_t n = pread(fd, buf, sizeof(buf) - 1, 0);
	long long v;

	if (n <= 0)
		return -1;
	buf[n] = '\0';
	/* sysfs returns decimal digits and one optional final newline. Reject
	 * embedded NULs, trailing bytes, truncation and signed/space inputs. */
	if (n == (ssize_t)sizeof(buf) - 1 || memchr(buf, '\0', (size_t)n))
		return -1;
	if (buf[n - 1] == '\n')
		buf[--n] = '\0';
	if (!n)
		return -1;
	for (ssize_t i = 0; i < n; i++)
		if (!isdigit((unsigned char)buf[i]))
			return -1;
	errno = 0;
	v = strtoll(buf, &end, 10);
	if (errno || end == buf || (*end && *end != '\n') || v <= 0 || v > UINT32_MAX)
		return -1;
	return v;
}

static bool parse_i64(const char *s, int64_t *out)
{
	char *end;
	long long v;

	errno = 0;
	v = strtoll(s, &end, 10);
	if (errno || end == s || *end)
		return false;
	*out = v;
	return true;
}

/*
 * Wait for since_epoch to tick. On success *since is the new value and
 * *edge_mono the CLOCK_MONOTONIC instant of the tick.
 */
static int wait_edge(const char *dir, int64_t *since, int64_t *edge_mono)
{
	char path[512];
	int64_t first, v, t_start, t_prev_start, t_end, deadline;
	struct timespec gap = { 0, POLL_NS };
	int fd;

	snprintf(path, sizeof(path), "%s/since_epoch", dir);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "rtc-edge: %s: %s\n", path, strerror(errno));
		return 1;
	}
	t_prev_start = now_ns(CLOCK_MONOTONIC);
	first = read_since(fd);
	if (first < 0) {
		fprintf(stderr, "rtc-edge: %s: not a positive integer\n", path);
		close(fd);
		return 1;
	}
	deadline = t_prev_start + EDGE_WAIT_NS;
	for (;;) {
		nanosleep(&gap, NULL);
		t_start = now_ns(CLOCK_MONOTONIC);
		v = read_since(fd);
		t_end = now_ns(CLOCK_MONOTONIC);
		if (v < 0) {
			fprintf(stderr, "rtc-edge: %s: not a positive integer\n", path);
			close(fd);
			return 1;
		}
		if (v != first)
			break;
		if (t_end > deadline) {
			fprintf(stderr, "rtc-edge: since_epoch did not tick within 1.1 s\n");
			close(fd);
			return 3;
		}
		t_prev_start = t_start;
	}
	close(fd);
	if (v - first != 1) {
		fprintf(stderr, "rtc-edge: since_epoch jumped from %" PRId64 " to %" PRId64 "\n", first, v);
		return 3;
	}
	*since = v;
	*edge_mono = t_prev_start + (t_end - t_prev_start) / 2;
	return 0;
}

static int set_wall(int64_t wall_ns, bool dry)
{
	struct timespec ts = { (time_t)(wall_ns / NS), (long)(wall_ns % NS) };

	if (dry) {
		printf("set %" PRId64 "\n", wall_ns);
		return 0;
	}
	if (clock_settime(CLOCK_REALTIME, &ts) < 0) {
		fprintf(stderr, "rtc-edge: clock_settime: %s\n", strerror(errno));
		return 2;
	}
	return 0;
}

static int usage(void)
{
	fprintf(stderr, "usage: rtc-edge [-r RTCDIR] [-n] sample | set OFFSET_NS | floor WALL_S\n");
	return 1;
}

int main(int argc, char **argv)
{
	const char *dir = RTC_DIR;
	bool dry = false;
	int64_t since, edge, arg, wall, mono;
	int opt, rc;

	while ((opt = getopt(argc, argv, "+r:n")) != -1) {
		switch (opt) {
		case 'r': dir = optarg; break;
		case 'n': dry = true; break;
		default: return usage();
		}
	}
	if (optind + 1 == argc && !strcmp(argv[optind], "sample")) {
		rc = wait_edge(dir, &since, &edge);
		if (rc)
			return rc;
		wall = now_ns(CLOCK_REALTIME);
		mono = now_ns(CLOCK_MONOTONIC);
		printf("%" PRId64 " %" PRId64 "\n", since, wall - (mono - edge));
		return 0;
	}
	if (optind + 2 != argc || !parse_i64(argv[optind + 1], &arg))
		return usage();
	if (!strcmp(argv[optind], "set")) {
		rc = wait_edge(dir, &since, &edge);
		if (rc)
			return rc;
		mono = now_ns(CLOCK_MONOTONIC);
		if (__builtin_mul_overflow(since, NS, &wall) ||
		    __builtin_add_overflow(wall, arg, &wall) ||
		    __builtin_add_overflow(wall, mono - edge, &wall) || wall <= 0) {
			fprintf(stderr, "rtc-edge: target clock out of range\n");
			return 1;
		}
		return set_wall(wall, dry);
	}
	if (!strcmp(argv[optind], "floor")) {
		if (arg <= 0 || arg > INT64_MAX / NS)
			return usage();
		if (now_ns(CLOCK_REALTIME) >= arg * NS)
			return 4;
		return set_wall(arg * NS, dry);
	}
	return usage();
}
