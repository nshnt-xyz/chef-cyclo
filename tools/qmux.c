#include <string.h>
#include <stdlib.h>

#include "qmux.h"

/* ---- QMUX frame ---- */

ssize_t qmux_frame_scan(const uint8_t *buf, size_t avail)
{
	uint16_t length;
	size_t total;

	if (avail < 1)
		return 0;
	if (buf[0] != QMUX_MARKER)
		return -1;
	if (avail < 3)
		return 0;

	length = (uint16_t)(buf[1] | (buf[2] << 8));
	if (length < 5) /* can't be shorter than the header it includes */
		return -1;

	total = 1 + (size_t)length;
	if (avail < total)
		return 0;

	return (ssize_t)total;
}

int qmux_frame_parse(const uint8_t *buf, size_t frame_size, struct qmux_frame *out)
{
	if (frame_size < QMUX_HDR_LEN)
		return -1;

	out->flags = buf[3];
	out->service = buf[4];
	out->client = buf[5];
	out->sdu = buf + QMUX_HDR_LEN;
	out->sdu_len = frame_size - QMUX_HDR_LEN;

	return 0;
}

ssize_t qmux_frame_build(uint8_t *out, size_t out_cap, uint8_t flags,
			  uint8_t service, uint8_t client,
			  const uint8_t *sdu, size_t sdu_len)
{
	size_t total = QMUX_HDR_LEN + sdu_len;
	uint16_t length;

	if (sdu_len > 0xffff - 5)
		return -1;
	if (total > out_cap)
		return -1;

	length = (uint16_t)(5 + sdu_len);

	out[0] = QMUX_MARKER;
	out[1] = (uint8_t)(length & 0xff);
	out[2] = (uint8_t)((length >> 8) & 0xff);
	out[3] = flags;
	out[4] = service;
	out[5] = client;
	if (sdu_len)
		memcpy(out + QMUX_HDR_LEN, sdu, sdu_len);

	return (ssize_t)total;
}

/* ---- QMI SDU header ---- */

size_t qmi_sdu_hdr_len(uint8_t service)
{
	return service == QMI_SERVICE_CTL ? 6 : 7;
}

int qmi_sdu_parse_header(const uint8_t *sdu, size_t len, uint8_t service,
			  struct qmi_sdu_header *out)
{
	size_t hlen = qmi_sdu_hdr_len(service);
	uint16_t tlv_len;

	if (len < hlen)
		return -1;

	out->ctl_flag = sdu[0];
	if (service == QMI_SERVICE_CTL) {
		out->txn = sdu[1];
		out->msg_id = (uint16_t)(sdu[2] | (sdu[3] << 8));
		tlv_len = (uint16_t)(sdu[4] | (sdu[5] << 8));
	} else {
		out->txn = (uint16_t)(sdu[1] | (sdu[2] << 8));
		out->msg_id = (uint16_t)(sdu[3] | (sdu[4] << 8));
		tlv_len = (uint16_t)(sdu[5] | (sdu[6] << 8));
	}
	out->tlv_len = tlv_len;

	if ((size_t)tlv_len != len - hlen)
		return -1; /* declared TLV length doesn't match what we actually got */

	return (int)hlen;
}

ssize_t qmi_sdu_build(uint8_t *out, size_t out_cap, uint8_t service,
		      uint8_t ctl_flag, uint16_t txn, uint16_t msg_id,
		      const uint8_t *tlvs, size_t tlv_len)
{
	size_t hlen = qmi_sdu_hdr_len(service);
	size_t total = hlen + tlv_len;

	if (tlv_len > 0xffff)
		return -1;
	if (total > out_cap)
		return -1;

	out[0] = ctl_flag;
	if (service == QMI_SERVICE_CTL) {
		out[1] = (uint8_t)txn;
		out[2] = (uint8_t)(msg_id & 0xff);
		out[3] = (uint8_t)((msg_id >> 8) & 0xff);
		out[4] = (uint8_t)(tlv_len & 0xff);
		out[5] = (uint8_t)((tlv_len >> 8) & 0xff);
	} else {
		out[1] = (uint8_t)(txn & 0xff);
		out[2] = (uint8_t)((txn >> 8) & 0xff);
		out[3] = (uint8_t)(msg_id & 0xff);
		out[4] = (uint8_t)((msg_id >> 8) & 0xff);
		out[5] = (uint8_t)(tlv_len & 0xff);
		out[6] = (uint8_t)((tlv_len >> 8) & 0xff);
	}
	if (tlv_len)
		memcpy(out + hlen, tlvs, tlv_len);

	return (ssize_t)total;
}

/* ---- TLVs ---- */

ssize_t tlv_put_raw(uint8_t *out, size_t cap, uint8_t type, const uint8_t *val, size_t len)
{
	size_t total = 3 + len;

	if (len > 0xffff)
		return -1;
	if (total > cap)
		return -1;

	out[0] = type;
	out[1] = (uint8_t)(len & 0xff);
	out[2] = (uint8_t)((len >> 8) & 0xff);
	if (len)
		memcpy(out + 3, val, len);

	return (ssize_t)total;
}

ssize_t tlv_put_u8(uint8_t *out, size_t cap, uint8_t type, uint8_t val)
{
	return tlv_put_raw(out, cap, type, &val, 1);
}

ssize_t tlv_put_u16(uint8_t *out, size_t cap, uint8_t type, uint16_t val)
{
	uint8_t b[2] = { (uint8_t)(val & 0xff), (uint8_t)((val >> 8) & 0xff) };
	return tlv_put_raw(out, cap, type, b, 2);
}

ssize_t tlv_put_result(uint8_t *out, size_t cap, uint16_t result, uint16_t error)
{
	uint8_t b[4] = {
		(uint8_t)(result & 0xff), (uint8_t)((result >> 8) & 0xff),
		(uint8_t)(error & 0xff), (uint8_t)((error >> 8) & 0xff),
	};
	return tlv_put_raw(out, cap, 0x02, b, 4);
}

int tlv_find(const uint8_t *tlvs, size_t len, uint8_t type,
	     const uint8_t **val, size_t *val_len)
{
	size_t off = 0;

	while (off < len) {
		uint8_t t;
		uint16_t l;

		if (off + 3 > len)
			return -2;

		t = tlvs[off];
		l = (uint16_t)(tlvs[off + 1] | (tlvs[off + 2] << 8));

		if (off + 3 + (size_t)l > len)
			return -2;

		if (t == type) {
			if (val)
				*val = tlvs + off + 3;
			if (val_len)
				*val_len = l;
			return 0;
		}

		off += 3 + (size_t)l;
	}

	return -1;
}

/* ---- CTL emulation ---- */

static ssize_t ctl_build_response(uint16_t msg_id, uint16_t txn, uint16_t result, uint16_t error,
				   const uint8_t *extra_tlvs, size_t extra_len,
				   uint8_t *out, size_t cap)
{
	uint8_t tlvbuf[1300];
	ssize_t n;
	size_t tlen;

	n = tlv_put_result(tlvbuf, sizeof(tlvbuf), result, error);
	if (n < 0)
		return -1;
	tlen = (size_t)n;

	if (extra_len) {
		if (tlen + extra_len > sizeof(tlvbuf))
			return -1;
		memcpy(tlvbuf + tlen, extra_tlvs, extra_len);
		tlen += extra_len;
	}

	return qmi_sdu_build(out, cap, QMI_SERVICE_CTL, QMI_CTL_FLAG_RESPONSE, txn, msg_id, tlvbuf, tlen);
}

ssize_t ctl_build_set_instance_id_resp(uint16_t txn, uint8_t *out, size_t cap)
{
	uint8_t extra[8];
	ssize_t n = tlv_put_u16(extra, sizeof(extra), 0x01, 0);

	if (n < 0)
		return -1;
	return ctl_build_response(QMI_CTL_MSG_SET_INSTANCE_ID, txn,
				   QMI_CTL_RESULT_SUCCESS, QMI_CTL_ERR_NONE,
				   extra, (size_t)n, out, cap);
}

ssize_t ctl_build_get_version_info_resp(const struct qmi_svc_version *list, size_t n,
					 uint16_t txn, uint8_t *out, size_t cap)
{
	uint8_t val[1 + 255 * 5];
	uint8_t extra[3 + sizeof(val)];
	size_t off = 0;
	size_t i;
	ssize_t tn;

	if (n > 255)
		return -1;

	val[off++] = (uint8_t)n;
	for (i = 0; i < n; i++) {
		val[off++] = list[i].service;
		val[off++] = (uint8_t)(list[i].major & 0xff);
		val[off++] = (uint8_t)((list[i].major >> 8) & 0xff);
		val[off++] = (uint8_t)(list[i].minor & 0xff);
		val[off++] = (uint8_t)((list[i].minor >> 8) & 0xff);
	}

	tn = tlv_put_raw(extra, sizeof(extra), 0x01, val, off);
	if (tn < 0)
		return -1;

	return ctl_build_response(QMI_CTL_MSG_GET_VERSION_INFO, txn,
				   QMI_CTL_RESULT_SUCCESS, QMI_CTL_ERR_NONE,
				   extra, (size_t)tn, out, cap);
}

ssize_t ctl_build_allocate_cid_resp(uint8_t service, uint8_t cid, uint16_t txn, uint8_t *out, size_t cap)
{
	uint8_t v[2] = { service, cid };
	uint8_t extra[8];
	ssize_t n = tlv_put_raw(extra, sizeof(extra), 0x01, v, 2);

	if (n < 0)
		return -1;
	return ctl_build_response(QMI_CTL_MSG_ALLOCATE_CID, txn,
				   QMI_CTL_RESULT_SUCCESS, QMI_CTL_ERR_NONE,
				   extra, (size_t)n, out, cap);
}

ssize_t ctl_build_allocate_cid_fail(uint16_t error, uint16_t txn, uint8_t *out, size_t cap)
{
	return ctl_build_response(QMI_CTL_MSG_ALLOCATE_CID, txn,
				   QMI_CTL_RESULT_FAILURE, error, NULL, 0, out, cap);
}

ssize_t ctl_build_release_cid_resp(uint8_t service, uint8_t cid, uint16_t txn, uint8_t *out, size_t cap)
{
	uint8_t v[2] = { service, cid };
	uint8_t extra[8];
	ssize_t n = tlv_put_raw(extra, sizeof(extra), 0x01, v, 2);

	if (n < 0)
		return -1;
	return ctl_build_response(QMI_CTL_MSG_RELEASE_CID, txn,
				   QMI_CTL_RESULT_SUCCESS, QMI_CTL_ERR_NONE,
				   extra, (size_t)n, out, cap);
}

ssize_t ctl_build_release_cid_fail(uint16_t error, uint16_t txn, uint8_t *out, size_t cap)
{
	return ctl_build_response(QMI_CTL_MSG_RELEASE_CID, txn,
				   QMI_CTL_RESULT_FAILURE, error, NULL, 0, out, cap);
}

ssize_t ctl_build_set_data_format_resp(uint16_t protocol, uint16_t txn, uint8_t *out, size_t cap)
{
	uint8_t extra[8];
	ssize_t n = tlv_put_u16(extra, sizeof(extra), 0x10, protocol);

	if (n < 0)
		return -1;
	return ctl_build_response(QMI_CTL_MSG_SET_DATA_FORMAT, txn,
				   QMI_CTL_RESULT_SUCCESS, QMI_CTL_ERR_NONE,
				   extra, (size_t)n, out, cap);
}

ssize_t ctl_build_sync_resp(uint16_t txn, uint8_t *out, size_t cap)
{
	return ctl_build_response(QMI_CTL_MSG_SYNC, txn,
				   QMI_CTL_RESULT_SUCCESS, QMI_CTL_ERR_NONE,
				   NULL, 0, out, cap);
}

ssize_t ctl_build_unsupported_resp(uint16_t msg_id, uint16_t txn, uint8_t *out, size_t cap)
{
	return ctl_build_response(msg_id, txn, QMI_CTL_RESULT_FAILURE,
				   QMI_CTL_ERR_NOT_SUPPORTED, NULL, 0, out, cap);
}

int ctl_parse_set_instance_id_req(const uint8_t *tlvs, size_t len, uint8_t *id_out)
{
	const uint8_t *v;
	size_t vl;

	if (tlv_find(tlvs, len, 0x01, &v, &vl) != 0)
		return -1;
	if (vl != 1)
		return -1;

	*id_out = v[0];
	return 0;
}

int ctl_parse_allocate_cid_req(const uint8_t *tlvs, size_t len, uint8_t *service_out)
{
	const uint8_t *v;
	size_t vl;

	if (tlv_find(tlvs, len, 0x01, &v, &vl) != 0)
		return -1;
	if (vl != 1)
		return -1;

	*service_out = v[0];
	return 0;
}

int ctl_parse_release_cid_req(const uint8_t *tlvs, size_t len, uint8_t *service_out, uint8_t *cid_out)
{
	const uint8_t *v;
	size_t vl;

	if (tlv_find(tlvs, len, 0x01, &v, &vl) != 0)
		return -1;
	if (vl != 2)
		return -1;

	*service_out = v[0];
	*cid_out = v[1];
	return 0;
}

int ctl_parse_set_data_format_req(const uint8_t *tlvs, size_t len, uint16_t *protocol_out)
{
	const uint8_t *v;
	size_t vl;
	int rc = tlv_find(tlvs, len, 0x10, &v, &vl);

	if (rc == -2)
		return -1;
	if (rc == -1) {
		*protocol_out = 0;
		return 0;
	}
	if (vl != 2)
		return -1;

	*protocol_out = (uint16_t)(v[0] | (v[1] << 8));
	return 0;
}

/* ---- CID table ---- */

int cid_table_init(struct qmuxd_cid_table *t, int capacity)
{
	if (capacity <= 0)
		return -1;

	t->slots = calloc((size_t)capacity, sizeof(*t->slots));
	if (!t->slots)
		return -1;
	t->capacity = capacity;

	for (int i = 0; i < capacity; i++)
		t->slots[i].sock = -1;

	return 0;
}

void cid_table_free(struct qmuxd_cid_table *t)
{
	free(t->slots);
	t->slots = NULL;
	t->capacity = 0;
}

int cid_alloc(struct qmuxd_cid_table *t, uint8_t service, int sock,
	      uint32_t node, uint32_t port, void *owner, uint8_t *cid_out)
{
	int want;

	for (want = 1; want <= QMUX_MAX_CID; want++) {
		int taken = 0;
		int i;

		for (i = 0; i < t->capacity; i++) {
			if (t->slots[i].allocated && t->slots[i].service == service &&
			    t->slots[i].cid == (uint8_t)want) {
				taken = 1;
				break;
			}
		}
		if (taken)
			continue;

		for (i = 0; i < t->capacity; i++) {
			if (!t->slots[i].allocated) {
				t->slots[i].allocated = 1;
				t->slots[i].service = service;
				t->slots[i].cid = (uint8_t)want;
				t->slots[i].sock = sock;
				t->slots[i].node = node;
				t->slots[i].port = port;
				t->slots[i].owner = owner;
				*cid_out = (uint8_t)want;
				return 0;
			}
		}
		return -1; /* table capacity exhausted */
	}

	return -1; /* all 254 cids for this service in use */
}

int cid_release(struct qmuxd_cid_table *t, uint8_t service, uint8_t cid)
{
	int i;

	for (i = 0; i < t->capacity; i++) {
		if (t->slots[i].allocated && t->slots[i].service == service && t->slots[i].cid == cid) {
			t->slots[i].allocated = 0;
			t->slots[i].sock = -1;
			t->slots[i].owner = NULL;
			return 0;
		}
	}

	return -1;
}

int cid_find(struct qmuxd_cid_table *t, uint8_t service, uint8_t cid)
{
	int i;

	for (i = 0; i < t->capacity; i++) {
		if (t->slots[i].allocated && t->slots[i].service == service && t->slots[i].cid == cid)
			return i;
	}

	return -1;
}

int cid_get_sock(struct qmuxd_cid_table *t, uint8_t service, uint8_t cid)
{
	int i = cid_find(t, service, cid);

	return i < 0 ? -1 : t->slots[i].sock;
}

int cid_get_target(struct qmuxd_cid_table *t, uint8_t service, uint8_t cid, uint32_t *node, uint32_t *port)
{
	int i = cid_find(t, service, cid);

	if (i < 0)
		return -1;
	if (node)
		*node = t->slots[i].node;
	if (port)
		*port = t->slots[i].port;
	return 0;
}

void *cid_get_owner(struct qmuxd_cid_table *t, uint8_t service, uint8_t cid)
{
	int i = cid_find(t, service, cid);

	return i < 0 ? NULL : t->slots[i].owner;
}

int cid_find_by_sock(struct qmuxd_cid_table *t, int sock, uint8_t *service_out, uint8_t *cid_out)
{
	int i;

	for (i = 0; i < t->capacity; i++) {
		if (t->slots[i].allocated && t->slots[i].sock == sock) {
			if (service_out)
				*service_out = t->slots[i].service;
			if (cid_out)
				*cid_out = t->slots[i].cid;
			return 0;
		}
	}

	return -1;
}

int cid_set_owner(struct qmuxd_cid_table *t, uint8_t service, uint8_t cid, void *owner)
{
	int i = cid_find(t, service, cid);

	if (i < 0)
		return -1;
	t->slots[i].owner = owner;
	return 0;
}

void cid_clear_owner_all(struct qmuxd_cid_table *t, void *owner)
{
	int i;

	for (i = 0; i < t->capacity; i++) {
		if (t->slots[i].allocated && t->slots[i].owner == owner)
			t->slots[i].owner = NULL;
	}
}
