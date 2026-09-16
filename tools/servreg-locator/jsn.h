/* jsn: minimal, bounded parser for the service-registry domain descriptor
 * files under /firmware/image (modemr.jsn, modemuw.jsn, adspr.jsn, ...),
 * confirmed against the device's real firmware partition, e.g.:
 *
 *   {
 *       "sr_version": { "major": 1, "minor": 1, "patch": 1 },
 *       "sr_domain": {
 *           "soc": "msm", "domain": "modem", "subdomain": "root_pd",
 *           "qmi_instance_id": 180
 *       },
 *       "sr_service": [
 *           { "provider": "tms", "service": "servreg",
 *             "service_data_valid": 0, "service_data": 0 },
 *           { "provider": "tms", "service": "pdr_enabled",
 *             "service_data_valid": 0, "service_data": 0 }
 *       ]
 *   }
 *
 * This is not a general JSON parser: it only understands this one schema
 * (a flat "sr_domain" object plus a flat "sr_service" array of flat
 * objects, no nesting, no escapes, no non-ASCII) and is deliberately a
 * bounded scanner rather than a tokenizer/AST -- there is nothing here that
 * needs one. Any file that doesn't match (including the real, confirmed
 * 0-byte modemus.jsn on this device) is skipped with a log line; a
 * malformed *entry* inside an otherwise-good file is skipped the same way
 * without discarding the rest of that file. Every extracted string is
 * bounds-checked against SERVREG_LOC_NAME_LEN before being written into a
 * servreg_loc wire-shaped name -- see servreg_loc.h.
 */
#ifndef CHEF_CYCLO_SERVREG_LOC_JSN_H
#define CHEF_CYCLO_SERVREG_LOC_JSN_H

#include <stddef.h>
#include <stdint.h>

#include "servreg_loc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Real devices have a handful of .jsn files with 1-3 services each; this
 * cap is generous headroom, not a tuned limit. Exceeding it stops adding
 * further entries (logged once) rather than growing without bound. */
#define JSN_MAX_ENTRIES 256

struct jsn_entry {
	char domain_name[SERVREG_LOC_NAME_BUF_LEN];  /* "soc/domain/subdomain" */
	uint32_t instance_id;
	char service_name[SERVREG_LOC_NAME_BUF_LEN]; /* "provider/service" */
	uint8_t service_data_valid;
	uint32_t service_data;
};

struct jsn_table {
	struct jsn_entry entries[JSN_MAX_ENTRIES];
	size_t count;
};

/* Parses every "*.jsn" file directly inside dir into *out (which the caller
 * must have zeroed or otherwise initialized to count=0). Always succeeds in
 * the sense of returning 0 -- an unreadable directory, an unreadable file, a
 * malformed file or a malformed entry are all logged to stderr and skipped,
 * never fatal. verbose enables one line per file parsed (entry count) in
 * addition to the always-on warnings. */
int jsn_load_dir(const char *dir, struct jsn_table *out, int verbose);

/* Fills out[] (capacity max_out) with entries whose service_name matches,
 * starting at the (offset)-th match (0-based, in file/parse order), and
 * sets *total_matches to the total number of matches regardless of offset
 * and max_out -- the GET_DOMAIN_LIST_RESP wire shape's total_domains vs.
 * domain_list_len distinction (see servreg_loc.h). Returns the number of
 * entries written to out[] (0..max_out). A service_name with no matches at
 * all yields a return of 0 and *total_matches = 0. */
size_t jsn_table_lookup(const struct jsn_table *t, const char *service_name,
			 uint32_t offset, struct servreg_loc_entry *out,
			 size_t max_out, uint32_t *total_matches);

#ifdef __cplusplus
}
#endif

#endif
