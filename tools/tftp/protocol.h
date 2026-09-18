/* Pure TFTP wire encode/parse: no sockets, no RAM shadow, no allowlist --
 * split out of tftpserv.c specifically so tests/test_protocol.c can drive
 * it directly on a host with no AF_MSM_IPC (this vendor kernel's transport
 * doesn't exist outside the real device, unlike a real UDP socket upstream
 * tqftpserv could at least loop back to itself on any Linux host).
 * tftpserv.c's send/parse wrappers add the socket I/O and allowlist calls
 * around these.
 */
#ifndef CHEF_CYCLO_TFTP_PROTOCOL_H
#define CHEF_CYCLO_TFTP_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "tftp.h"

#ifdef __cplusplus
extern "C" {
#endif

struct tftp_options {
	int have_blksize;
	size_t blksize;
	int have_tsize;
	off_t tsize;
	int have_wsize;
	size_t wsize;
	int have_timeoutms;
	unsigned int timeoutms;	/* always milliseconds internally */
	int timeout_is_seconds;	/* client used plain "timeout" (RFC 2349), not
					 * "timeoutms" -- OACK must echo the same name
					 * (Round 1 review F4) or a strict client can
					 * ERROR 8 the whole transfer. */
	int have_rsize;
	size_t rsize;
	int have_seek;
	off_t seek;
	int have_append;
};

/* Parses "<filename>\0<mode>\0[options]" starting at buf+2 (the 2-byte
 * opcode the caller already consumed to decide this is an RRQ/WRQ).
 * Returns 0 and fills *filename (points into buf) and *opts_off (byte
 * offset of the options section, == len if there is none) on success.
 * On failure returns -1 and fills *err and *err_msg with what the caller
 * should send back -- this function does no I/O itself. */
int tftp_parse_request(const char *buf, size_t len, const char **filename,
			size_t *opts_off, enum tftp_error *err, const char **err_msg);

/* Parses the options section (buf/len is exactly the bytes after the mode
 * string's NUL). Unknown option names are ignored, never fatal (REPORT.md
 * 8.1 point 2). Returns 0 with opt->have_* marking which options had a
 * well-formed value (opt->have_append is a bare presence flag); -1 on a
 * malformed options section or a value outside the option's *wire* range
 * (e.g. blksize below the RFC 2348 floor of 8, or a wsize/timeout that
 * doesn't parse as a non-negative integer at all). Two options are
 * clamped rather than rejected when the client asks for more than this
 * daemon will actually honor, because RFC 2347/2348 make the OACK'd value
 * authoritative -- a compliant client is required to go along with it,
 * not treat it as a negotiation failure: blksize is clamped to
 * TFTP_MAX_BLKSIZE (Round 1 review F5) and wsize to TFTP_MAX_WSIZE (Round
 * 1 review F2, since rw_buf_size == blksize * wsize is calloc'd per
 * client). Both clamped values are what tftp_encode_oack() below actually
 * sends. */
int tftp_parse_options(const char *buf, size_t len, struct tftp_options *opt);

/* Each encoder writes into buf (capacity bufsz) and returns the packet
 * length, or -1 if it doesn't fit. */
ssize_t tftp_encode_error(uint8_t *buf, size_t bufsz, enum tftp_error code, const char *msg);
ssize_t tftp_encode_ack(uint8_t *buf, size_t bufsz, uint16_t block);
ssize_t tftp_encode_oack(uint8_t *buf, size_t bufsz, const struct tftp_options *opt);

#ifdef __cplusplus
}
#endif

#endif
