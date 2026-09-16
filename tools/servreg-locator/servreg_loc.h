/* servreg_loc: wire structs and qmi_elem_info tables for the SERVREG_LOC
 * (service-registry locator, a.k.a. pd-mapper) QMI service, 0x40 instance
 * 0x101 (version 1, instance 1 -- see servreg-locator.c's qrtr_publish()
 * call). Ported from this tree's own kernel client (verbatim wire shapes:
 * kernel/drivers/soc/qcom/service-locator-private.h, service-locator.c) and
 * cross-checked against mainline's userspace-free replacement
 * (drivers/remoteproc/qcom_pd_mapper.c, out-of-tree here as
 * scratchpad/qcom_pd_mapper.c) for the message IDs and PFR shape the kernel
 * table doesn't have. Encoded/decoded with the vendored qrtr/qmi.c
 * (qmi_elem_info) codec, exactly as tools/rmtfs/qmi_rmtfs.{h,c} already does
 * for RMTFS -- see that pair for the established house pattern this mirrors.
 *
 * Codec caveat (do not "fix" by shrinking these buffers): qrtr/qmi.c's
 * qmi_decode_string_elem() writes the NUL terminator at buf[string_len], and
 * string_len is allowed to equal elem_len (only ">" is rejected, not ">=").
 * A peer that sends a full-width string therefore writes one byte past a
 * buffer sized exactly elem_len. Every decoded string field below is
 * declared elem_len+1 bytes (elem_len itself is unchanged, so the wire
 * behavior/limits stay identical to the kernel/mainline tables) so that
 * write always lands inside the buffer.
 */
#ifndef CHEF_CYCLO_SERVREG_LOC_H
#define CHEF_CYCLO_SERVREG_LOC_H

#include <stdint.h>

#include "libqrtr.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SERVREG_LOC_SERVICE_ID       0x40
#define SERVREG_LOC_SERVICE_VERSION  1 /* qrtr_publish(sock, 0x40, 1, 1) */
#define SERVREG_LOC_SERVICE_INSTANCE 1 /* -> wire instance (1<<8)|1 = 0x101 */

/* Real max content length of a domain/service name on the wire (kernel:
 * QMI_SERVREG_LOC_NAME_LENGTH_V01). The wire TLV elem_len is one more than
 * this (room for the length byte to reach 65, matching the kernel's own
 * char[65] arrays) -- see the codec-caveat note above for why our C buffers
 * are a further byte larger still. */
#define SERVREG_LOC_NAME_LEN      64
#define SERVREG_LOC_NAME_WIRE_LEN (SERVREG_LOC_NAME_LEN + 1) /* = elem_len, 65 */
#define SERVREG_LOC_NAME_BUF_LEN  (SERVREG_LOC_NAME_WIRE_LEN + 1) /* NUL slack */

#define SERVREG_LOC_LIST_LENGTH   32 /* kernel: QMI_SERVREG_LOC_LIST_LENGTH_V01 */

/* PFR string fields are unrelated to the NAME_LENGTH convention above --
 * confirmed against this device's modem firmware strings and mainline
 * pdr_internal.h by the review pass on this change (service content max 64,
 * reason content max 256; wire elem_len is content+1, matching mainline's
 * SERVREG_PFR_LENGTH+1, applied below and in servreg_loc.c's ei table). */
#define SERVREG_LOC_PFR_SERVICE_LEN 64
#define SERVREG_LOC_PFR_REASON_LEN  256

/* QMI message IDs (kernel: QMI_SERVREG_LOC_*_V01). Request and response
 * share the same numeric ID; only the QMI header's type field (REQUEST vs
 * RESPONSE) differs, matching qmi_decode_message()'s (type, id) pair. */
#define SERVREG_LOC_INDICATION_REGISTER_REQ   0x0020
#define SERVREG_LOC_GET_DOMAIN_LIST_REQ       0x0021
#define SERVREG_LOC_REGISTER_SERVICE_LIST_REQ 0x0022
#define SERVREG_LOC_DATABASE_UPDATED_IND      0x0023
#define SERVREG_LOC_PFR_REQ                   0x0024

/* Generous fixed buffer for the largest response this server ever encodes
 * (GET_DOMAIN_LIST_RESP, kernel MAX_MSG_LEN 2389, + qmi header + margin). */
#define SERVREG_LOC_RESP_BUF_LEN 3072

struct servreg_loc_entry {
	char name[SERVREG_LOC_NAME_BUF_LEN];
	uint32_t instance_id;
	uint8_t service_data_valid;
	uint32_t service_data;
};

struct servreg_loc_get_domain_list_req {
	char service_name[SERVREG_LOC_NAME_BUF_LEN];
	uint8_t domain_offset_valid;
	uint32_t domain_offset;
};

struct servreg_loc_get_domain_list_resp {
	struct qmi_response_type_v01 resp;
	uint8_t total_domains_valid;
	uint16_t total_domains;
	uint8_t db_rev_count_valid;
	uint16_t db_rev_count;
	uint8_t domain_list_valid;
	uint32_t domain_list_len; /* QMI_DATA_LEN destination: must be >= 4 bytes */
	struct servreg_loc_entry domain_list[SERVREG_LOC_LIST_LENGTH];
};

struct servreg_loc_indication_register_req {
	uint8_t enable_database_updated_indication_valid;
	uint8_t enable_database_updated_indication;
};

/* Shared by every response that carries nothing but the common QMI result
 * TLV: INDICATION_REGISTER, PFR, and the generic NOT_SUPPORTED NACK. */
struct servreg_loc_generic_resp {
	struct qmi_response_type_v01 resp;
};

/* Best-effort only (see servreg-locator.c's handling of this message): the
 * modem sends this to report a crashed protection domain, TLV 0x01
 * (service) and TLV 0x02 (reason), both mandatory strings. If a real device
 * ever sends a shape this table doesn't match, decode fails cleanly (returns
 * < 0) and the caller still acks -- see the file header note above. */
struct servreg_loc_pfr_req {
	char service[SERVREG_LOC_PFR_SERVICE_LEN + 2]; /* +1 wire, +1 NUL slack */
	char reason[SERVREG_LOC_PFR_REASON_LEN + 2];
};

extern struct qmi_elem_info servreg_loc_entry_ei[];
extern struct qmi_elem_info servreg_loc_get_domain_list_req_ei[];
extern struct qmi_elem_info servreg_loc_get_domain_list_resp_ei[];
extern struct qmi_elem_info servreg_loc_indication_register_req_ei[];
extern struct qmi_elem_info servreg_loc_generic_resp_ei[];
extern struct qmi_elem_info servreg_loc_pfr_req_ei[];

#ifdef __cplusplus
}
#endif

#endif
