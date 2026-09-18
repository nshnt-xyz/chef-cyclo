/* Wire constants shared by tftpserv.c, translate.c and the host tests.
 * Opcodes and error codes are RFC 1350 §5 verbatim (same values upstream
 * linux-msm/tqftpserv uses); the size limits below are this daemon's own,
 * tightened from upstream's per REPORT.md 8.1 point 5.
 */
#ifndef CHEF_CYCLO_TFTP_H
#define CHEF_CYCLO_TFTP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum tftp_opcode {
	TFTP_OP_RRQ	= 1,
	TFTP_OP_WRQ	= 2,
	TFTP_OP_DATA	= 3,
	TFTP_OP_ACK	= 4,
	TFTP_OP_ERROR	= 5,
	TFTP_OP_OACK	= 6,
};

/* RFC 1350 §5. */
enum tftp_error {
	TFTP_ERROR_UNDEF	= 0,
	TFTP_ERROR_ENOENT	= 1,
	TFTP_ERROR_EACCESS	= 2,
	TFTP_ERROR_ENOSPACE	= 3,
	TFTP_ERROR_EBADOP	= 4,
	TFTP_ERROR_EBADID	= 5,
	TFTP_ERROR_EEXISTS	= 6,
	TFTP_ERROR_ENOUSER	= 7,
	TFTP_ERROR_EOPTNEG	= 8,	/* RFC 2347 */
};

/* RFC 2348 allows up to 65464; REPORT.md 8.1 point 5 requires "blksize <=
 * 8192" for this daemon specifically (stock's own tftp_server.strings.txt
 * OACK trace never shows a larger value either). */
#define TFTP_MIN_BLKSIZE	8
#define TFTP_MAX_BLKSIZE	8192
#define TFTP_DEFAULT_BLKSIZE	512

#define TFTP_MIN_TIMEOUTMS	10
#define TFTP_MAX_TIMEOUTMS	60000
#define TFTP_DEFAULT_TIMEOUTMS	1000

/* RFC 7440 allows up to 65535; capped much lower here because
 * rw_buf_size == blksize * wsize is calloc'd per client (Round 1 review
 * F2) -- 8192 * 16 = 128 KiB/client, x TFTP_MAX_CLIENTS = 1 MiB worst
 * case, instead of up to 512 MiB/client uncapped. A requested wsize above
 * this is silently clamped and the clamped value is what gets OACK'd
 * (RFC 2347: the OACK value is authoritative, the client must honor it). */
#define TFTP_MAX_WSIZE		16

/* Bounded path length (REPORT.md 8.1 point 5's "bounded path length");
 * generous relative to every real RFS path this modem is known to use
 * (the longest observed is "mot_rfs/imei_sv", 15 bytes). */
#define TFTP_MAX_PATH		255

/* REPORT.md 8.1 point 5: "max concurrent transfers (e.g. 8)". Counted
 * across readers and writers together. */
#define TFTP_MAX_CLIENTS	8

#ifdef __cplusplus
}
#endif

#endif
