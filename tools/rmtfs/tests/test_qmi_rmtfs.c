/* Host regression test for the RMTFS open request decode: a path of the
 * maximum length rmtfs_open_req_ei allows (256 bytes) must decode with
 * its NUL inside struct rmtfs_open_req, and a longer one must be
 * rejected. Built with ASan (see the Makefile): the struct and the
 * packet are exact-size heap blocks, so a one-byte overrun is caught.
 * Before the fix (char path[256]) the 256-byte case wrote the NUL one
 * byte past the struct. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libqrtr.h>
#include "qmi_rmtfs.h"

static int decode_open(size_t pathlen, struct rmtfs_open_req *req)
{
	size_t tlvlen = 3 + pathlen;
	uint8_t *buf = malloc(7 + tlvlen);
	struct qrtr_packet pkt;
	unsigned int txn;
	int rc;

	assert(buf);
	buf[0] = QMI_REQUEST;
	buf[1] = 1;		/* txn */
	buf[2] = 0;
	buf[3] = QMI_RMTFS_OPEN;
	buf[4] = 0;
	buf[5] = (uint8_t)tlvlen;
	buf[6] = (uint8_t)(tlvlen >> 8);
	buf[7] = 1;		/* TLV 0x01: path, no length prefix at top level */
	buf[8] = (uint8_t)pathlen;
	buf[9] = (uint8_t)(pathlen >> 8);
	memset(buf + 10, 'a', pathlen);
	memset(&pkt, 0, sizeof(pkt));
	pkt.data = buf;
	pkt.data_len = 7 + tlvlen;
	rc = qmi_decode_message(req, &txn, &pkt, QMI_REQUEST, QMI_RMTFS_OPEN,
				rmtfs_open_req_ei);
	free(buf);
	return rc;
}

int main(void)
{
	struct rmtfs_open_req *req = malloc(sizeof(*req));
	int rc;

	assert(req);

	memset(req, 0x55, sizeof(*req));
	rc = decode_open(strlen("/boot/modem_fs1"), req);
	assert(rc >= 0);

	memset(req, 0x55, sizeof(*req));
	rc = decode_open(256, req);
	assert(rc >= 0);
	assert(strlen(req->path) == 256);

	memset(req, 0x55, sizeof(*req));
	rc = decode_open(257, req);
	assert(rc < 0);

	free(req);
	printf("test-qmi-rmtfs: all passed\n");
	return 0;
}
