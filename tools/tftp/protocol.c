/* See protocol.h. Parsing shapes (filename\0mode\0[opt\0val\0]*) and the
 * option names/ranges follow upstream linux-msm/tqftpserv's parse_options()
 * (BSD-3-Clause, see LICENSE), extended with the plain RFC 2349 "timeout"
 * option and a real "append" flag -- see tftpserv.c's file header for the
 * full list of behavioral differences from upstream.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "protocol.h"

int tftp_parse_request(const char *buf, size_t len, const char **filename,
			size_t *opts_off, enum tftp_error *err, const char **err_msg)
{
	const char *end = buf + len;
	const char *p = buf + 2;
	const char *mode;
	size_t flen, mlen;

	if (p >= end) {
		*err = TFTP_ERROR_EACCESS;
		*err_msg = "truncated request";
		return -1;
	}

	*filename = p;
	flen = strnlen(p, (size_t)(end - p));
	if (flen == (size_t)(end - p) || flen == 0 || flen > TFTP_MAX_PATH) {
		*err = TFTP_ERROR_EACCESS;
		*err_msg = "invalid filename";
		return -1;
	}
	p += flen + 1;

	if (p >= end) {
		*err = TFTP_ERROR_EBADOP;
		*err_msg = "missing mode";
		return -1;
	}
	mode = p;
	mlen = strnlen(p, (size_t)(end - p));
	if (mlen == (size_t)(end - p)) {
		*err = TFTP_ERROR_EBADOP;
		*err_msg = "malformed mode";
		return -1;
	}
	p += mlen + 1;

	if (strcasecmp(mode, "octet") != 0) {
		*err = TFTP_ERROR_EBADOP;
		*err_msg = "only octet mode supported";
		return -1;
	}

	*opts_off = (size_t)(p - buf);
	return 0;
}

int tftp_parse_options(const char *buf, size_t len, struct tftp_options *opt)
{
	const char *end = buf + len;
	const char *p = buf;
	long long val;
	char *endptr;

	memset(opt, 0, sizeof(*opt));

	while (p < end) {
		const char *name = p;
		size_t name_len = strnlen(name, (size_t)(end - p));
		const char *value;
		size_t value_len;

		if (name_len == (size_t)(end - p))
			return -1;
		p += name_len + 1;

		if (p >= end)
			return -1;
		value = p;
		value_len = strnlen(value, (size_t)(end - p));
		if (value_len == (size_t)(end - p))
			return -1;
		p += value_len + 1;

		errno = 0;
		val = strtoll(value, &endptr, 10);

		if (strcasecmp(name, "blksize") == 0) {
			/* RFC 2348 explicitly allows the server to answer with
			 * a smaller blksize than requested; the OACK value is
			 * authoritative (the client must use it), so clamp
			 * instead of rejecting the transfer outright (Round 1
			 * review F5). Only a value below the protocol floor,
			 * or a non-numeric one, is still a hard error. */
			if (errno || *endptr || val < TFTP_MIN_BLKSIZE)
				return -1;
			opt->have_blksize = 1;
			opt->blksize = (size_t)(val > TFTP_MAX_BLKSIZE ? TFTP_MAX_BLKSIZE : val);
		} else if (strcasecmp(name, "timeoutms") == 0) {
			if (errno || *endptr || val < TFTP_MIN_TIMEOUTMS || val > TFTP_MAX_TIMEOUTMS)
				return -1;
			opt->have_timeoutms = 1;
			opt->timeout_is_seconds = 0;
			opt->timeoutms = (unsigned int)val;
		} else if (strcasecmp(name, "timeout") == 0) {
			/* RFC 2349: whole seconds, 1-255. Tracked separately
			 * from "timeoutms" so the OACK echoes back whichever
			 * option name the client actually sent (Round 1
			 * review F4) -- RFC 2347 requires an OACK to only
			 * contain options under the same name the client
			 * used, or a strict client may ERROR 8 the transfer. */
			if (errno || *endptr || val < 1 || val > 255)
				return -1;
			opt->have_timeoutms = 1;
			opt->timeout_is_seconds = 1;
			opt->timeoutms = (unsigned int)val * 1000u;
		} else if (strcasecmp(name, "tsize") == 0) {
			if (errno || *endptr || val < 0)
				return -1;
			opt->have_tsize = 1;
			opt->tsize = (off_t)val;
		} else if (strcasecmp(name, "rsize") == 0) {
			if (errno || *endptr || val < 1)
				return -1;
			opt->have_rsize = 1;
			opt->rsize = (size_t)val;
		} else if (strcasecmp(name, "wsize") == 0) {
			/* Wire-format sanity (RFC 7440's field is 16-bit) is
			 * still enforced as a hard reject; a value that fits
			 * the wire but exceeds what this daemon will actually
			 * buffer (TFTP_MAX_WSIZE) is clamped instead, and the
			 * OACK carries the clamped value (Round 1 review F2 --
			 * rw_buf_size == blksize * wsize is calloc'd per
			 * client). */
			if (errno || *endptr || val < 1 || val > 65535)
				return -1;
			opt->have_wsize = 1;
			opt->wsize = (size_t)(val > TFTP_MAX_WSIZE ? TFTP_MAX_WSIZE : val);
		} else if (strcasecmp(name, "seek") == 0) {
			if (errno || *endptr || val < 0)
				return -1;
			opt->have_seek = 1;
			opt->seek = (off_t)val;
		} else if (strcasecmp(name, "append") == 0) {
			opt->have_append = 1;
		}
		/* else: unrecognized option name, ignored (REPORT.md 8.1
		 * point 2: "Unknown options ignored, never fatal"). */
	}

	return 0;
}

ssize_t tftp_encode_error(uint8_t *buf, size_t bufsz, enum tftp_error code, const char *msg)
{
	size_t msg_len = strnlen(msg, bufsz > 5 ? bufsz - 5 : 0);
	size_t len = 4 + msg_len + 1;

	if (bufsz < len)
		return -1;

	buf[0] = 0;
	buf[1] = TFTP_OP_ERROR;
	buf[2] = 0;
	buf[3] = (uint8_t)code;
	memcpy(buf + 4, msg, msg_len);
	buf[4 + msg_len] = '\0';

	return (ssize_t)len;
}

ssize_t tftp_encode_ack(uint8_t *buf, size_t bufsz, uint16_t block)
{
	if (bufsz < 4)
		return -1;

	buf[0] = 0;
	buf[1] = TFTP_OP_ACK;
	buf[2] = (uint8_t)(block >> 8);
	buf[3] = (uint8_t)block;

	return 4;
}

ssize_t tftp_encode_oack(uint8_t *buf, size_t bufsz, const struct tftp_options *opt)
{
	char *p = (char *)buf;
	char *end = (char *)buf + bufsz;
	int n;

	if (bufsz < 2)
		return -1;

	*p++ = 0;
	*p++ = TFTP_OP_OACK;

#define PUT_OPT(name, fmt, val) do { \
		n = snprintf(p, (size_t)(end - p), "%s%c" fmt "%c", name, '\0', (val), '\0'); \
		if (n < 0 || n >= end - p) \
			return -1; \
		p += n; \
	} while (0)

	if (opt->have_blksize)
		PUT_OPT("blksize", "%zu", opt->blksize);
	if (opt->have_timeoutms) {
		/* Echo back whichever option name the client used (Round 1
		 * review F4): "timeout" wants whole seconds, "timeoutms"
		 * wants milliseconds -- opt->timeoutms is always stored in
		 * ms internally (tftp_parse_options()). */
		if (opt->timeout_is_seconds)
			PUT_OPT("timeout", "%u", opt->timeoutms / 1000u);
		else
			PUT_OPT("timeoutms", "%u", opt->timeoutms);
	}
	if (opt->have_tsize)
		PUT_OPT("tsize", "%jd", (intmax_t)opt->tsize);
	if (opt->have_wsize)
		PUT_OPT("wsize", "%zu", opt->wsize);
	if (opt->have_rsize)
		PUT_OPT("rsize", "%zu", opt->rsize);
	if (opt->have_seek)
		PUT_OPT("seek", "%jd", (intmax_t)opt->seek);
#undef PUT_OPT

	return (ssize_t)(p - (char *)buf);
}
