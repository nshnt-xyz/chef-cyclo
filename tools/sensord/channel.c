/*
 * sensord's channel table and names, sample conversion and the sample
 * lines, timestamps (DSP ticks to CLOCK_MONOTONIC), and the log helpers
 * and clocks every part shares. See main.c for the daemon as a whole.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "sensord.h"

bool sn_verbose;

void sn_kmsg_note(const char *fmt, ...)
{
	char buf[320];
	va_list ap;
	int fd, n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	fprintf(stderr, "sensord: %s\n", buf);
	fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return;
	dprintf(fd, "sensord: %s\n", buf);
	close(fd);
}

/* Detail worth keeping but too frequent for the kernel log: stderr. */
void sn_stderr_note(const char *fmt, ...)
{
	va_list ap;

	fputs("sensord: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

/* Debug detail: stderr only, and only with -v. */
void sn_vlog(const char *fmt, ...)
{
	va_list ap;

	if (!sn_verbose)
		return;
	fputs("sensord: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

const struct channel sn_channel_defaults[CH_COUNT] = {
	[CH_ACCEL]   = { "accel", "m/s^2", CH_VEC3, SNS_SMGR_ID_ACCEL, 0, 10 },
	[CH_ANGLVEL] = { "anglvel", "rad/s", CH_VEC3, SNS_SMGR_ID_GYRO, 0, 10 },
	[CH_MAGN]    = { "magn", "gauss", CH_VEC3, SNS_SMGR_ID_MAG, 0, 10 },
	[CH_ILLUM]   = { "illuminance", "lux", CH_SCALAR, SNS_SMGR_ID_PROX_LIGHT, 1, 5 },
	[CH_PROX_]   = { "proximity", "near", CH_PROX, SNS_SMGR_ID_PROX_LIGHT, 0, 5 },
	[CH_MAGF]    = { "magn_factory", "gauss", CH_VEC3, SNS_SMGR_ID_MAG, 0, SENSORD_HEADING_FAC_HZ,
			 .cal = SNS_SMGR_CAL_FACTORY, .internal = true },
	[CH_HEADING] = { "heading", "deg", CH_VIRT, 0xff, 0xff, 10,
			 .max_hz = SENSORD_HEADING_MAX_HZ },
	[CH_ROTVEC]  = { "rotvec", "quaternion", CH_VIRT, 0xff, 0xff, 10,
			 .max_hz = SENSORD_ROTVEC_HZ },
};

const char *const sn_cal_names[] = { "full", "factory", "raw" };

const char *sn_cal_name(uint8_t cal)
{
	return cal <= SNS_SMGR_CAL_RAW ? sn_cal_names[cal] : "?";
}

int sn_channel_from_name(const char *name)
{
	int i;

	for (i = 0; i < CH_COUNT; i++)
		if (!sn_channel_defaults[i].internal && strcmp(sn_channel_defaults[i].name, name) == 0)
			return i;
	return -1;
}

/* SMGR frame -> Android/IIO frame and Q16 -> float, as the stock HAL. */
void sn_convert_sample(enum ch_kind kind, const int32_t d[3], double out[3])
{
	const double q16 = 1.0 / 65536.0;

	switch (kind) {
	case CH_VEC3:
		out[0] = d[1] * q16;
		out[1] = d[0] * q16;
		out[2] = -(double)d[2] * q16;
		break;
	case CH_SCALAR:
		out[0] = d[0] * q16;
		out[1] = out[2] = 0;
		break;
	case CH_PROX:
		out[0] = d[0] / 65536 != 0;	/* C truncates toward zero, as fcvtzs */
		out[1] = d[1];
		out[2] = 0;
		break;
	case CH_VIRT:
		out[0] = out[1] = out[2] = 0;	/* not an SMGR sample */
		break;
	}
}

int sn_format_sample(char *buf, size_t len, const struct channel *ch, int64_t t,
			 const int32_t d[3])
{
	double v[3];

	sn_convert_sample(ch->kind, d, v);
	switch (ch->kind) {
	case CH_VEC3:
		return snprintf(buf, len, "{\"sensor\":\"%s\",\"t\":%" PRId64
				",\"x\":%.6g,\"y\":%.6g,\"z\":%.6g}\n", ch->name, t, v[0], v[1], v[2]);
	case CH_SCALAR:
		return snprintf(buf, len, "{\"sensor\":\"%s\",\"t\":%" PRId64 ",\"%s\":%.6g}\n",
				ch->name, t, ch->name, v[0]);
	case CH_PROX:
		return snprintf(buf, len, "{\"sensor\":\"%s\",\"t\":%" PRId64
				",\"near\":%d,\"proximity\":%.6g}\n", ch->name, t, (int)v[0], v[1]);
	case CH_VIRT:
		break;
	}
	return -1;
}

/* DSP tick stamp -> CLOCK_MONOTONIC ns, given one (ticks, ns) pair read
 * at the same moment. Wrap-safe for stamps within +-18 h of the anchor. */
int64_t sn_ticks_to_ns(uint32_t ts, uint32_t anchor_ticks, int64_t anchor_ns)
{
	int32_t d = (int32_t)(anchor_ticks - ts);

	return anchor_ns - (int64_t)d * 1000000000LL / SENSORD_DSP_HZ;
}

/* Copy a length-counted byte array from the DSP into a C string. */
void sn_copy_name(char *dst, size_t dstsz, const uint8_t *src, uint32_t len)
{
	size_t n = len < dstsz - 1 ? len : dstsz - 1;
	size_t i;

	for (i = 0; i < n && src[i]; i++)
		dst[i] = (char)src[i];
	dst[i] = '\0';
}

/* JSON string body (no quotes); drops what JSON cannot carry raw. */
void sn_json_escape(char *dst, size_t dstsz, const char *src)
{
	size_t o = 0;

	for (; *src && o + 3 < dstsz; src++) {
		unsigned char c = (unsigned char)*src;

		if (c == '"' || c == '\\') {
			dst[o++] = '\\';
			dst[o++] = (char)c;
		} else if (c >= 0x20 && c < 0x7f) {
			dst[o++] = (char)c;
		}
	}
	dst[o] = '\0';
}

static int64_t mono_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

int64_t sn_real_now_ms(void)
{
	return mono_ns() / 1000000;
}

/* ---------------------------------------------------------- timestamps */

bool sn_read_dsp_ticks(struct server *s, uint32_t *ticks)
{
	if (s->ts_mode == TS_DSPS && s->dsps_fd >= 0) {
		unsigned int v = 0;

		if (ioctl(s->dsps_fd, SENSORD_DSPS_READ_SLOW_TIMER, &v) == 0) {
			*ticks = v;
			return true;
		}
		return false;
	}
#if defined(__aarch64__)
	if (s->ts_mode == TS_CNTVCT) {
		uint64_t v;

		__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
		*ticks = (uint32_t)((v << 4) / 9375);
		return true;
	}
#endif
	return false;
}

/* (ticks, ns) read back to back for one indication. In rx mode, or if
 * the counter cannot be read, the newest stamp of the indication is
 * taken as "now". */
void sn_time_anchor(struct server *s, uint32_t newest, uint32_t *a_ticks, int64_t *a_ns)
{
	uint32_t t;

	if (sn_read_dsp_ticks(s, &t)) {
		*a_ns = mono_ns();
		*a_ticks = t;
		/* A sample from the future or from long ago means the counter
		 * we read is not the one SMGR stamps with: report, once a
		 * minute, so the live run can see it. */
		{
			int32_t d = (int32_t)(t - newest);
			int64_t now = s->now_ms();

			if ((d < -SENSORD_DSP_HZ || d > 10 * SENSORD_DSP_HZ) &&
			    now - s->skew_logged > 60000) {
				s->skew_logged = now;
				s->log("timestamp skew: counter %u, newest sample %u (%.3f s); check -T",
				       t, newest, d / (double)SENSORD_DSP_HZ);
			}
		}
		return;
	}
	*a_ns = mono_ns();
	*a_ticks = newest;
}
