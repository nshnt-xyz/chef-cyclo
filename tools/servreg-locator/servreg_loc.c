/* qmi_elem_info tables for servreg_loc.h. Field-for-field port of
 * kernel/drivers/soc/qcom/service-locator-private.h's elem_info tables
 * (same tlv_type/data_type/array_type/offset values, just renamed to this
 * codec's struct qmi_elem_info and this file's struct names) -- see
 * servreg_loc.h's file header for why the string buffers are a byte wider
 * than what these tables declare as elem_len.
 */
#include <stddef.h>

#include "servreg_loc.h"

struct qmi_elem_info servreg_loc_entry_ei[] = {
	{
		.data_type = QMI_STRING,
		.elem_len = SERVREG_LOC_NAME_WIRE_LEN,
		.elem_size = sizeof(char),
		.array_type = NO_ARRAY,
		.tlv_type = 0,
		.offset = offsetof(struct servreg_loc_entry, name),
	},
	{
		.data_type = QMI_UNSIGNED_4_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(uint32_t),
		.array_type = NO_ARRAY,
		.tlv_type = 0,
		.offset = offsetof(struct servreg_loc_entry, instance_id),
	},
	{
		.data_type = QMI_UNSIGNED_1_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(uint8_t),
		.array_type = NO_ARRAY,
		.tlv_type = 0,
		.offset = offsetof(struct servreg_loc_entry, service_data_valid),
	},
	{
		.data_type = QMI_UNSIGNED_4_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(uint32_t),
		.array_type = NO_ARRAY,
		.tlv_type = 0,
		.offset = offsetof(struct servreg_loc_entry, service_data),
	},
	{}
};

struct qmi_elem_info servreg_loc_get_domain_list_req_ei[] = {
	{
		.data_type = QMI_STRING,
		.elem_len = SERVREG_LOC_NAME_WIRE_LEN,
		.elem_size = sizeof(char),
		.array_type = NO_ARRAY,
		.tlv_type = 0x01,
		.offset = offsetof(struct servreg_loc_get_domain_list_req, service_name),
	},
	{
		.data_type = QMI_OPT_FLAG,
		.elem_len = 1,
		.elem_size = sizeof(uint8_t),
		.array_type = NO_ARRAY,
		.tlv_type = 0x10,
		.offset = offsetof(struct servreg_loc_get_domain_list_req, domain_offset_valid),
	},
	{
		.data_type = QMI_UNSIGNED_4_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(uint32_t),
		.array_type = NO_ARRAY,
		.tlv_type = 0x10,
		.offset = offsetof(struct servreg_loc_get_domain_list_req, domain_offset),
	},
	{}
};

struct qmi_elem_info servreg_loc_get_domain_list_resp_ei[] = {
	{
		.data_type = QMI_STRUCT,
		.elem_len = 1,
		.elem_size = sizeof(struct qmi_response_type_v01),
		.array_type = NO_ARRAY,
		.tlv_type = 0x02,
		.offset = offsetof(struct servreg_loc_get_domain_list_resp, resp),
		.ei_array = qmi_response_type_v01_ei,
	},
	{
		.data_type = QMI_OPT_FLAG,
		.elem_len = 1,
		.elem_size = sizeof(uint8_t),
		.array_type = NO_ARRAY,
		.tlv_type = 0x10,
		.offset = offsetof(struct servreg_loc_get_domain_list_resp, total_domains_valid),
	},
	{
		.data_type = QMI_UNSIGNED_2_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(uint16_t),
		.array_type = NO_ARRAY,
		.tlv_type = 0x10,
		.offset = offsetof(struct servreg_loc_get_domain_list_resp, total_domains),
	},
	{
		.data_type = QMI_OPT_FLAG,
		.elem_len = 1,
		.elem_size = sizeof(uint8_t),
		.array_type = NO_ARRAY,
		.tlv_type = 0x11,
		.offset = offsetof(struct servreg_loc_get_domain_list_resp, db_rev_count_valid),
	},
	{
		.data_type = QMI_UNSIGNED_2_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(uint16_t),
		.array_type = NO_ARRAY,
		.tlv_type = 0x11,
		.offset = offsetof(struct servreg_loc_get_domain_list_resp, db_rev_count),
	},
	{
		.data_type = QMI_OPT_FLAG,
		.elem_len = 1,
		.elem_size = sizeof(uint8_t),
		.array_type = NO_ARRAY,
		.tlv_type = 0x12,
		.offset = offsetof(struct servreg_loc_get_domain_list_resp, domain_list_valid),
	},
	{
		.data_type = QMI_DATA_LEN,
		.elem_len = 1,
		.elem_size = sizeof(uint8_t),
		.array_type = NO_ARRAY,
		.tlv_type = 0x12,
		.offset = offsetof(struct servreg_loc_get_domain_list_resp, domain_list_len),
	},
	{
		.data_type = QMI_STRUCT,
		.elem_len = SERVREG_LOC_LIST_LENGTH,
		.elem_size = sizeof(struct servreg_loc_entry),
		.array_type = VAR_LEN_ARRAY,
		.tlv_type = 0x12,
		.offset = offsetof(struct servreg_loc_get_domain_list_resp, domain_list),
		.ei_array = servreg_loc_entry_ei,
	},
	{}
};

struct qmi_elem_info servreg_loc_indication_register_req_ei[] = {
	{
		.data_type = QMI_OPT_FLAG,
		.elem_len = 1,
		.elem_size = sizeof(uint8_t),
		.array_type = NO_ARRAY,
		.tlv_type = 0x10,
		.offset = offsetof(struct servreg_loc_indication_register_req,
				    enable_database_updated_indication_valid),
	},
	{
		.data_type = QMI_UNSIGNED_1_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(uint8_t),
		.array_type = NO_ARRAY,
		.tlv_type = 0x10,
		.offset = offsetof(struct servreg_loc_indication_register_req,
				    enable_database_updated_indication),
	},
	{}
};

struct qmi_elem_info servreg_loc_generic_resp_ei[] = {
	{
		.data_type = QMI_STRUCT,
		.elem_len = 1,
		.elem_size = sizeof(struct qmi_response_type_v01),
		.array_type = NO_ARRAY,
		.tlv_type = 0x02,
		.offset = offsetof(struct servreg_loc_generic_resp, resp),
		.ei_array = qmi_response_type_v01_ei,
	},
	{}
};

/* Best-effort decode only -- see servreg_loc.h's struct comment. */
struct qmi_elem_info servreg_loc_pfr_req_ei[] = {
	{
		.data_type = QMI_STRING,
		.elem_len = SERVREG_LOC_PFR_SERVICE_LEN + 1,
		.elem_size = sizeof(char),
		.array_type = NO_ARRAY,
		.tlv_type = 0x01,
		.offset = offsetof(struct servreg_loc_pfr_req, service),
	},
	{
		.data_type = QMI_STRING,
		.elem_len = SERVREG_LOC_PFR_REASON_LEN + 1,
		.elem_size = sizeof(char),
		.array_type = NO_ARRAY,
		.tlv_type = 0x02,
		.offset = offsetof(struct servreg_loc_pfr_req, reason),
	},
	{}
};
