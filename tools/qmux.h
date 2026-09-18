/* qmux: QMUX framing, QMI SDU header codec, TLV helpers, CTL-service (0)
 * emulation and the CID allocation table used by qmuxd-lite.
 *
 * This is the "only new protocol code" the GPS bridge needs (see
 * gps-userspace-handoff.md 3.4) and is deliberately split out from
 * qmuxd-lite.c's socket/poll loop: everything declared here is a pure
 * function of its inputs (no fds, no blocking calls) so it can be exercised
 * by host unit tests without a real AF_MSM_IPC socket or a live modem --
 * see tools/tests/test_qmux.c.
 *
 * Wire formats (verified against libqmi 1.38 sources, see the handoff doc
 * sections 2.3-2.5 for the byte tables this implements):
 *
 *   QMUX frame (what a qmicli process writes/reads on /run/qmux_socket):
 *     u8  marker              always 0x01
 *     u16 length      (LE)    byte count following this field: itself (2) +
 *                             flags(1) + service(1) + client(1) + sdu_len
 *     u8  flags               0x00 control-point->service, 0x80 reverse
 *     u8  service              QMI service id (0 = CTL)
 *     u8  client               CID (meaningless for service 0)
 *     ... sdu (sdu_len bytes) ...
 *
 *   QMI SDU (== the IPC-router payload once the QMUX header above is
 *   stripped; identical to a QRTR packet's payload):
 *     u8  ctl_flag             services: 0 request / 2 response / 4 indication
 *                              CTL (service 0): 0 request / 1 response / 2 indication
 *                              (libqmi QmiCtlFlag vs QmiServiceFlag -- they
 *                              differ, and libqmi drops a CTL reply carrying
 *                              the service-style 0x02 as an indication)
 *     u8|u16 txn      (LE)     1 byte for service 0 (CTL), 2 bytes otherwise
 *     u16 msg_id      (LE)
 *     u16 tlv_len     (LE)     byte count of the TLVs that follow
 *     ... tlv_len bytes of {u8 type; u16 length; u8 value[length];}* ...
 */
#ifndef CHEF_CYCLO_QMUX_H
#define CHEF_CYCLO_QMUX_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#include "qrtr/libqrtr.h" /* QMI_REQUEST/QMI_RESPONSE/QMI_INDICATION (service SDUs) */

/* CTL (service 0) SDU flag byte. Not the same encoding as the service
 * ones above: libqmi's QmiCtlFlag is RESPONSE = 1<<0, INDICATION = 1<<1,
 * whereas QmiServiceFlag is RESPONSE = 1<<1, INDICATION = 1<<2. A CTL
 * reply sent with QMI_RESPONSE (2) is parsed by libqmi as a CTL
 * *indication* and never matched to the waiting transaction (seen live
 * 2026-09-17 as qmicli "Transaction timed out" on Get Version Info). */
#define QMI_CTL_FLAG_RESPONSE   1
#define QMI_CTL_FLAG_INDICATION 2

#ifdef __cplusplus
extern "C" {
#endif

#define QMUX_MARKER          0x01
#define QMUX_HDR_LEN         6   /* marker(1) + length(2) + flags(1) + service(1) + client(1) */
#define QMUX_FLAG_C2S        0x00
#define QMUX_FLAG_S2C        0x80
#define QMI_SERVICE_CTL      0
#define QMUX_MAX_FRAME       65536 /* 1 + u16_max; the length field cannot address more */

/* ---- QMUX frame ---- */

/* Looks for one complete frame at the front of buf[0..avail).
 * Returns: >0 = frame size incl. marker (safe to qmux_frame_parse());
 *           0 = incomplete, need more bytes, buf is untouched/valid so far;
 *          -1 = malformed (bad marker, or a length that can never be a
 *               valid frame) -- caller should drop the connection. */
ssize_t qmux_frame_scan(const uint8_t *buf, size_t avail);

struct qmux_frame {
	uint8_t flags;
	uint8_t service;
	uint8_t client;
	const uint8_t *sdu;
	size_t sdu_len;
};

/* Decodes a frame of exactly frame_size bytes (as returned by a prior
 * qmux_frame_scan() on the same bytes). Returns 0, or -1 if frame_size is
 * too small to be a valid frame (defensive; should not happen if scan was
 * used first). out->sdu points into buf (no copy). */
int qmux_frame_parse(const uint8_t *buf, size_t frame_size, struct qmux_frame *out);

/* Builds marker+header+sdu into out[0..out_cap). Returns total bytes
 * written, or -1 if it would not fit or sdu_len overflows the u16 length
 * field. */
ssize_t qmux_frame_build(uint8_t *out, size_t out_cap, uint8_t flags,
			  uint8_t service, uint8_t client,
			  const uint8_t *sdu, size_t sdu_len);

/* ---- QMI SDU header ---- */

struct qmi_sdu_header {
	uint8_t ctl_flag;
	uint16_t txn;
	uint16_t msg_id;
	uint16_t tlv_len;
};

/* Header size on the wire for this service: 6 bytes for CTL (1-byte txn),
 * 7 bytes for every other service (2-byte txn). */
size_t qmi_sdu_hdr_len(uint8_t service);

/* Parses the header from sdu[0..len) and validates tlv_len against the
 * actual remaining length. Returns the header size consumed (== TLVs start
 * at sdu+return value), or -1 if len is too short or tlv_len is wrong for
 * the buffer actually supplied (malformed). */
int qmi_sdu_parse_header(const uint8_t *sdu, size_t len, uint8_t service,
			  struct qmi_sdu_header *out);

/* Builds a full SDU (header + tlvs) into out[0..out_cap). Returns total
 * length, or -1 on overflow. */
ssize_t qmi_sdu_build(uint8_t *out, size_t out_cap, uint8_t service,
		      uint8_t ctl_flag, uint16_t txn, uint16_t msg_id,
		      const uint8_t *tlvs, size_t tlv_len);

/* ---- TLVs ---- */

ssize_t tlv_put_raw(uint8_t *out, size_t cap, uint8_t type, const uint8_t *val, size_t len);
ssize_t tlv_put_u8(uint8_t *out, size_t cap, uint8_t type, uint8_t val);
ssize_t tlv_put_u16(uint8_t *out, size_t cap, uint8_t type, uint16_t val);
/* The common QMI result TLV (type 0x02): u16 result, u16 error, both LE. */
ssize_t tlv_put_result(uint8_t *out, size_t cap, uint16_t result, uint16_t error);

/* Scans tlvs[0..len) for the first TLV of `type`.
 * Returns  0 = found (*val and *val_len set, pointing into tlvs);
 *         -1 = not found, but the TLV stream up to here was well-formed;
 *         -2 = malformed (a TLV header or value runs past len). */
int tlv_find(const uint8_t *tlvs, size_t len, uint8_t type,
	     const uint8_t **val, size_t *val_len);

/* ---- CTL (service 0) emulation, per gps-userspace-handoff.md 2.5 ---- */

#define QMI_CTL_MSG_SET_INSTANCE_ID   0x0020
#define QMI_CTL_MSG_GET_VERSION_INFO  0x0021
#define QMI_CTL_MSG_ALLOCATE_CID      0x0022
#define QMI_CTL_MSG_RELEASE_CID       0x0023
#define QMI_CTL_MSG_SET_DATA_FORMAT   0x0026
#define QMI_CTL_MSG_SYNC              0x0027

#define QMI_CTL_RESULT_SUCCESS        0
#define QMI_CTL_RESULT_FAILURE        1

/* Bridge-only error space: these are libqmi/qmicli's generic QMI wire
 * protocol error codes (QMI_PROTOCOL_ERROR_*), NOT the same numbering as
 * the QMI_ERR_*_V01 constants in qrtr/libqrtr.h -- those belong to a
 * different, service-internal ("V01") error table used by things like
 * rmtfs's own QMI service, and mixing the two up would send a wire-invalid
 * error code to a real client. QMI_CTL_ERR_NOT_SUPPORTED is libqmi's
 * QMI_PROTOCOL_ERROR_NOT_SUPPORTED (94 = 0x5E; the handoff doc's 0x11 is
 * QMI_PROTOCOL_ERROR_MISSING_ARGUMENT, which is what qmicli printed for a
 * failed Allocate CID until this was corrected 2026-09-18). It is reused
 * here as the generic "bridge could not do this" code (CID exhaustion,
 * lookup failure) since this is an adapter of convenience, not a real
 * modem-side service -- qmicli treats any non-zero result as a formatted
 * failure either way. */
#define QMI_CTL_ERR_NONE              0
#define QMI_CTL_ERR_NOT_SUPPORTED     0x005e

struct qmi_svc_version {
	uint8_t service;
	uint16_t major;
	uint16_t minor;
};

ssize_t ctl_build_set_instance_id_resp(uint16_t txn, uint8_t *out, size_t cap);
ssize_t ctl_build_get_version_info_resp(const struct qmi_svc_version *list, size_t n,
					 uint16_t txn, uint8_t *out, size_t cap);
ssize_t ctl_build_allocate_cid_resp(uint8_t service, uint8_t cid, uint16_t txn, uint8_t *out, size_t cap);
ssize_t ctl_build_allocate_cid_fail(uint16_t error, uint16_t txn, uint8_t *out, size_t cap);
ssize_t ctl_build_release_cid_resp(uint8_t service, uint8_t cid, uint16_t txn, uint8_t *out, size_t cap);
ssize_t ctl_build_release_cid_fail(uint16_t error, uint16_t txn, uint8_t *out, size_t cap);
ssize_t ctl_build_set_data_format_resp(uint16_t protocol, uint16_t txn, uint8_t *out, size_t cap);
ssize_t ctl_build_sync_resp(uint16_t txn, uint8_t *out, size_t cap);
ssize_t ctl_build_unsupported_resp(uint16_t msg_id, uint16_t txn, uint8_t *out, size_t cap);

/* Request-TLV parsers. Return 0 and fill the out-params, or -1 if the
 * expected TLV is absent or malformed. ctl_parse_set_data_format_req's TLV
 * (0x10, "Protocol") is optional per the CTL JSON: absence is not an error,
 * *protocol_out is set to 0. */
int ctl_parse_set_instance_id_req(const uint8_t *tlvs, size_t len, uint8_t *id_out);
int ctl_parse_allocate_cid_req(const uint8_t *tlvs, size_t len, uint8_t *service_out);
int ctl_parse_release_cid_req(const uint8_t *tlvs, size_t len, uint8_t *service_out, uint8_t *cid_out);
int ctl_parse_set_data_format_req(const uint8_t *tlvs, size_t len, uint16_t *protocol_out);

/* ---- CID allocation table ----
 *
 * One table is shared by every client connection (deliberately: a session
 * a qmicli process allocated with --client-no-release-cid must survive
 * that process exiting, so a second qmicli --client-cid=N can attach to
 * the same modem-side LOC session -- handoff 3.4). `owner` is an opaque
 * pointer qmuxd-lite uses for its own per-connection struct; this module
 * never dereferences it. */

#define QMUX_MAX_CID 254 /* CIDs are 1..254 per service; 0 and 255 are reserved */

struct cid_slot {
	int allocated;
	uint8_t service;
	uint8_t cid;
	int sock;        /* AF_MSM_IPC fd for this session, or -1 */
	uint32_t node;   /* qrtr_sendto() target for this session, from the */
	uint32_t port;   /* Allocate-CID-time service lookup (handoff 3.4) */
	void *owner;     /* connection currently receiving this cid's indications, or NULL */
};

struct qmuxd_cid_table {
	struct cid_slot *slots;
	int capacity;
};

int cid_table_init(struct qmuxd_cid_table *t, int capacity);
void cid_table_free(struct qmuxd_cid_table *t);

/* Allocates the lowest unused CID (1..254) for `service`, remembering
 * (node,port) as this session's qrtr_sendto() target. Returns 0 and
 * *cid_out set, or -1 if either the table has no free slot or all 254 CIDs
 * for this service are already in use. */
int cid_alloc(struct qmuxd_cid_table *t, uint8_t service, int sock,
	      uint32_t node, uint32_t port, void *owner, uint8_t *cid_out);

/* Frees the slot. Returns 0, or -1 if (service,cid) was not allocated
 * (double release / unknown cid) -- the caller must not close a socket it
 * does not own. */
int cid_release(struct qmuxd_cid_table *t, uint8_t service, uint8_t cid);

int cid_find(struct qmuxd_cid_table *t, uint8_t service, uint8_t cid);       /* slot index, or -1 */
int cid_get_sock(struct qmuxd_cid_table *t, uint8_t service, uint8_t cid);   /* fd, or -1 if unknown */
/* Returns 0 and fills *node and *port, or -1 if (service,cid) is not allocated. */
int cid_get_target(struct qmuxd_cid_table *t, uint8_t service, uint8_t cid, uint32_t *node, uint32_t *port);
void *cid_get_owner(struct qmuxd_cid_table *t, uint8_t service, uint8_t cid); /* NULL if unknown/unset */
int cid_set_owner(struct qmuxd_cid_table *t, uint8_t service, uint8_t cid, void *owner); /* 0, or -1 if unknown */
/* Finds the (service,cid) whose socket fd is `sock`. Returns 0 and fills
 * *service_out and *cid_out, or -1 if no allocated slot has that fd (used by
 * qmuxd-lite's poll loop to route an incoming AF_MSM_IPC datagram back to
 * the session that owns the fd it arrived on). */
int cid_find_by_sock(struct qmuxd_cid_table *t, int sock, uint8_t *service_out, uint8_t *cid_out);

/* Clears `owner` from every slot that currently points at it (call this
 * when a client connection closes: its CIDs stay allocated -- the modem
 * session lives on -- but nothing should route indications to a dead fd
 * until a new connection presents the same --client-cid). */
void cid_clear_owner_all(struct qmuxd_cid_table *t, void *owner);

#ifdef __cplusplus
}
#endif

#endif
