#define _GNU_SOURCE /* memmem() */
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jsn.h"

/* Real .jsn files are a few hundred bytes; this is headroom, not a tuned
 * limit. A file at or over this size is skipped rather than truncated and
 * silently misparsed. */
#define JSN_MAX_FILE_LEN 16384

static int read_file(const char *path, char *buf, size_t cap, size_t *out_len)
{
	FILE *f;
	size_t n;

	f = fopen(path, "rb");
	if (!f)
		return -1;

	n = fread(buf, 1, cap - 1, f);
	if (ferror(f)) {
		fclose(f);
		return -1;
	}
	/* A file that still has bytes left after filling cap-1 is over our
	 * bound; treat it as unparseable rather than silently truncating. */
	if (n == cap - 1 && fgetc(f) != EOF) {
		fclose(f);
		errno = EFBIG;
		return -1;
	}
	fclose(f);

	buf[n] = '\0';
	*out_len = n;
	return 0;
}

/* Finds the next literal occurrence of "key" (with its quotes) at or after
 * buf[from], within buf[0..len). Returns the offset just past the closing
 * quote, or (size_t)-1 if not found before len. */
static size_t find_key(const char *buf, size_t len, size_t from, const char *key)
{
	size_t keylen = strlen(key);
	char pat[80];
	const char *hit;
	size_t patlen;

	/* All keys in this schema are short and fixed; a key that doesn't
	 * fit our scratch buffer can't be one we're looking for. */
	if (keylen + 2 >= sizeof(pat))
		return (size_t)-1;
	pat[0] = '"';
	memcpy(pat + 1, key, keylen);
	pat[1 + keylen] = '"';
	pat[2 + keylen] = '\0';
	patlen = keylen + 2;

	if (from > len)
		return (size_t)-1;

	hit = memmem(buf + from, len - from, pat, patlen);
	if (!hit)
		return (size_t)-1;

	return (size_t)(hit - buf) + patlen;
}

static size_t skip_ws(const char *buf, size_t len, size_t pos)
{
	while (pos < len && (buf[pos] == ' ' || buf[pos] == '\t' ||
			      buf[pos] == '\n' || buf[pos] == '\r'))
		pos++;
	return pos;
}

/* After a matched "key", expects (skipping whitespace) a ':' then more
 * whitespace, and returns the offset of the value's first byte, or
 * (size_t)-1 if the ':' isn't there before len. */
static size_t skip_colon(const char *buf, size_t len, size_t pos)
{
	pos = skip_ws(buf, len, pos);
	if (pos >= len || buf[pos] != ':')
		return (size_t)-1;
	pos = skip_ws(buf, len, pos + 1);
	return pos;
}

/* buf[pos] must be the opening quote of a JSON string (no escape handling --
 * see file header). Copies the content into out (bounded, NUL-terminated)
 * and returns the offset just past the closing quote, or (size_t)-1 on any
 * of: no opening quote, unterminated string, content too long for out. */
static size_t parse_string(const char *buf, size_t len, size_t pos,
			    char *out, size_t out_cap)
{
	size_t start;
	size_t n;

	if (pos >= len || buf[pos] != '"')
		return (size_t)-1;
	start = pos + 1;

	pos = start;
	while (pos < len && buf[pos] != '"')
		pos++;
	if (pos >= len)
		return (size_t)-1; /* unterminated */

	n = pos - start;
	if (n >= out_cap)
		return (size_t)-1; /* too long -- reject, never truncate */

	memcpy(out, buf + start, n);
	out[n] = '\0';

	return pos + 1;
}

/* buf[pos] must be the first byte of a JSON number (only unsigned integers
 * are used anywhere in this schema). Returns the offset just past it, or
 * (size_t)-1 on a malformed or out-of-range number. */
static size_t parse_uint(const char *buf, size_t len, size_t pos, uint32_t *out)
{
	unsigned long v;
	char *end;
	char tmp[16];
	size_t n = 0;

	while (pos + n < len && n < sizeof(tmp) - 1 &&
	       buf[pos + n] >= '0' && buf[pos + n] <= '9')
		n++;
	if (n == 0)
		return (size_t)-1;

	memcpy(tmp, buf + pos, n);
	tmp[n] = '\0';

	errno = 0;
	v = strtoul(tmp, &end, 10);
	if (errno == ERANGE || v > UINT32_MAX || *end != '\0')
		return (size_t)-1;

	*out = (uint32_t)v;
	return pos + n;
}

/* buf[pos] must be an opening bracket (open); returns the offset of its
 * matching close bracket, respecting nesting and skipping the contents of
 * quoted strings, or (size_t)-1 if unterminated within len. Bytes between
 * pos+1 and the returned offset are the object/array's contents. */
static size_t find_matching(const char *buf, size_t len, size_t pos,
			     char open, char close)
{
	int depth = 0;

	if (pos >= len || buf[pos] != open)
		return (size_t)-1;

	for (; pos < len; pos++) {
		if (buf[pos] == '"') {
			pos++;
			while (pos < len && buf[pos] != '"')
				pos++;
			if (pos >= len)
				return (size_t)-1;
			continue;
		}
		if (buf[pos] == open)
			depth++;
		else if (buf[pos] == close) {
			depth--;
			if (depth == 0)
				return pos;
		}
	}

	return (size_t)-1;
}

static int table_add(struct jsn_table *t, const char *path,
		      const char *domain_name, uint32_t instance_id,
		      const char *service_name, uint8_t service_data_valid,
		      uint32_t service_data)
{
	struct jsn_entry *e;

	if (t->count >= JSN_MAX_ENTRIES) {
		static int warned;

		if (!warned) {
			fprintf(stderr, "[SERVREG-LOC] jsn: table full at %d entries, "
					"dropping further entries (starting at %s)\n",
				JSN_MAX_ENTRIES, path);
			warned = 1;
		}
		return -1;
	}

	e = &t->entries[t->count];
	strcpy(e->domain_name, domain_name); /* both already length-checked by caller */
	e->instance_id = instance_id;
	strcpy(e->service_name, service_name);
	e->service_data_valid = service_data_valid;
	e->service_data = service_data;
	t->count++;

	return 0;
}

static void parse_services(const char *buf, size_t arr_start, size_t arr_end,
			    const char *path, const char *domain_name,
			    uint32_t instance_id, struct jsn_table *out,
			    int verbose)
{
	size_t pos = arr_start;

	for (;;) {
		size_t obj_start, obj_end, k, v_end;
		char provider[SERVREG_LOC_NAME_LEN + 1];
		char service[SERVREG_LOC_NAME_LEN + 1];
		char service_name[SERVREG_LOC_NAME_LEN + 1];
		uint32_t service_data = 0;
		uint32_t service_data_valid = 0;
		int have_provider = 0, have_service = 0;
		int n;

		while (pos < arr_end && buf[pos] != '{')
			pos++;
		if (pos >= arr_end)
			break;
		obj_start = pos;

		obj_end = find_matching(buf, arr_end, obj_start, '{', '}');
		if (obj_end == (size_t)-1) {
			fprintf(stderr, "[SERVREG-LOC] jsn: %s: unterminated sr_service "
					"entry, stopping\n", path);
			break;
		}

		k = find_key(buf, obj_end, obj_start, "provider");
		if (k != (size_t)-1) {
			v_end = skip_colon(buf, obj_end, k);
			if (v_end != (size_t)-1 &&
			    parse_string(buf, obj_end, v_end, provider, sizeof(provider)) != (size_t)-1)
				have_provider = 1;
		}

		k = find_key(buf, obj_end, obj_start, "service");
		if (k != (size_t)-1) {
			v_end = skip_colon(buf, obj_end, k);
			if (v_end != (size_t)-1 &&
			    parse_string(buf, obj_end, v_end, service, sizeof(service)) != (size_t)-1)
				have_service = 1;
		}

		k = find_key(buf, obj_end, obj_start, "service_data_valid");
		if (k != (size_t)-1) {
			v_end = skip_colon(buf, obj_end, k);
			if (v_end != (size_t)-1)
				parse_uint(buf, obj_end, v_end, &service_data_valid);
		}

		k = find_key(buf, obj_end, obj_start, "service_data");
		if (k != (size_t)-1) {
			v_end = skip_colon(buf, obj_end, k);
			if (v_end != (size_t)-1)
				parse_uint(buf, obj_end, v_end, &service_data);
		}

		if (!have_provider || !have_service) {
			fprintf(stderr, "[SERVREG-LOC] jsn: %s: sr_service entry missing "
					"provider/service, skipping entry\n", path);
			pos = obj_end + 1;
			continue;
		}

		n = snprintf(service_name, sizeof(service_name), "%s/%s", provider, service);
		if (n < 0 || (size_t)n >= sizeof(service_name)) {
			fprintf(stderr, "[SERVREG-LOC] jsn: %s: service name \"%s/%s\" too "
					"long, skipping entry\n", path, provider, service);
			pos = obj_end + 1;
			continue;
		}

		if (table_add(out, path, domain_name, instance_id, service_name,
			      (uint8_t)service_data_valid, service_data) == 0 && verbose)
			fprintf(stderr, "[SERVREG-LOC] jsn: %s: %s -> %s (instance %u)\n",
				path, service_name, domain_name, instance_id);

		pos = obj_end + 1;
	}
}

static void parse_file(const char *buf, size_t len, const char *path,
			struct jsn_table *out, int verbose)
{
	size_t k, v_end;
	char soc[SERVREG_LOC_NAME_LEN + 1];
	char domain[SERVREG_LOC_NAME_LEN + 1];
	char subdomain[SERVREG_LOC_NAME_LEN + 1];
	char domain_name[SERVREG_LOC_NAME_LEN + 1];
	uint32_t instance_id;
	size_t dom_start, dom_end;
	size_t arr_start, arr_end;
	int n;

	k = find_key(buf, len, 0, "sr_domain");
	if (k == (size_t)-1) {
		fprintf(stderr, "[SERVREG-LOC] jsn: %s: no sr_domain, skipping file\n", path);
		return;
	}
	v_end = skip_colon(buf, len, k);
	if (v_end == (size_t)-1 || v_end >= len || buf[v_end] != '{') {
		fprintf(stderr, "[SERVREG-LOC] jsn: %s: malformed sr_domain, skipping file\n", path);
		return;
	}
	dom_start = v_end;
	dom_end = find_matching(buf, len, dom_start, '{', '}');
	if (dom_end == (size_t)-1) {
		fprintf(stderr, "[SERVREG-LOC] jsn: %s: unterminated sr_domain, skipping file\n", path);
		return;
	}

	k = find_key(buf, dom_end, dom_start, "soc");
	if (k == (size_t)-1 || (v_end = skip_colon(buf, dom_end, k)) == (size_t)-1 ||
	    parse_string(buf, dom_end, v_end, soc, sizeof(soc)) == (size_t)-1)
		goto bad_domain;

	k = find_key(buf, dom_end, dom_start, "domain");
	if (k == (size_t)-1 || (v_end = skip_colon(buf, dom_end, k)) == (size_t)-1 ||
	    parse_string(buf, dom_end, v_end, domain, sizeof(domain)) == (size_t)-1)
		goto bad_domain;

	k = find_key(buf, dom_end, dom_start, "subdomain");
	if (k == (size_t)-1 || (v_end = skip_colon(buf, dom_end, k)) == (size_t)-1 ||
	    parse_string(buf, dom_end, v_end, subdomain, sizeof(subdomain)) == (size_t)-1)
		goto bad_domain;

	k = find_key(buf, dom_end, dom_start, "qmi_instance_id");
	if (k == (size_t)-1 || (v_end = skip_colon(buf, dom_end, k)) == (size_t)-1 ||
	    parse_uint(buf, dom_end, v_end, &instance_id) == (size_t)-1)
		goto bad_domain;

	n = snprintf(domain_name, sizeof(domain_name), "%s/%s/%s", soc, domain, subdomain);
	if (n < 0 || (size_t)n >= sizeof(domain_name)) {
		fprintf(stderr, "[SERVREG-LOC] jsn: %s: domain name \"%s/%s/%s\" too long, "
				"skipping file\n", path, soc, domain, subdomain);
		return;
	}

	k = find_key(buf, len, 0, "sr_service");
	if (k == (size_t)-1) {
		if (verbose)
			fprintf(stderr, "[SERVREG-LOC] jsn: %s: %s has no sr_service, "
					"nothing to index\n", path, domain_name);
		return;
	}
	v_end = skip_colon(buf, len, k);
	if (v_end == (size_t)-1 || v_end >= len || buf[v_end] != '[') {
		fprintf(stderr, "[SERVREG-LOC] jsn: %s: malformed sr_service, skipping\n", path);
		return;
	}
	arr_start = v_end;
	arr_end = find_matching(buf, len, arr_start, '[', ']');
	if (arr_end == (size_t)-1) {
		fprintf(stderr, "[SERVREG-LOC] jsn: %s: unterminated sr_service, skipping\n", path);
		return;
	}

	parse_services(buf, arr_start + 1, arr_end, path, domain_name, instance_id, out, verbose);
	return;

bad_domain:
	fprintf(stderr, "[SERVREG-LOC] jsn: %s: malformed/incomplete sr_domain, "
			"skipping file\n", path);
}

int jsn_load_dir(const char *dir, struct jsn_table *out, int verbose)
{
	DIR *d;
	struct dirent *de;

	d = opendir(dir);
	if (!d) {
		fprintf(stderr, "[SERVREG-LOC] jsn: cannot open %s: %s\n", dir, strerror(errno));
		return 0;
	}

	while ((de = readdir(d)) != NULL) {
		char path[512];
		char filebuf[JSN_MAX_FILE_LEN];
		size_t flen;
		size_t namelen = strlen(de->d_name);
		int n;

		if (namelen < 4 || strcmp(de->d_name + namelen - 4, ".jsn") != 0)
			continue;

		n = snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
		if (n < 0 || (size_t)n >= sizeof(path)) {
			fprintf(stderr, "[SERVREG-LOC] jsn: path %s/%s too long, skipping\n",
				dir, de->d_name);
			continue;
		}

		if (read_file(path, filebuf, sizeof(filebuf), &flen) != 0) {
			fprintf(stderr, "[SERVREG-LOC] jsn: %s: %s, skipping\n",
				path, strerror(errno));
			continue;
		}
		if (flen == 0) {
			if (verbose)
				fprintf(stderr, "[SERVREG-LOC] jsn: %s: empty, skipping\n", path);
			continue;
		}

		parse_file(filebuf, flen, path, out, verbose);
	}

	closedir(d);
	return 0;
}

size_t jsn_table_lookup(const struct jsn_table *t, const char *service_name,
			 uint32_t offset, struct servreg_loc_entry *out,
			 size_t max_out, uint32_t *total_matches)
{
	uint32_t total = 0;
	size_t written = 0;
	size_t i;

	for (i = 0; i < t->count; i++) {
		const struct jsn_entry *e = &t->entries[i];

		if (strcmp(e->service_name, service_name) != 0)
			continue;

		if (total >= offset && written < max_out) {
			struct servreg_loc_entry *o = &out[written];

			strcpy(o->name, e->domain_name); /* both bounded to the same buf size */
			o->instance_id = e->instance_id;
			o->service_data_valid = e->service_data_valid;
			o->service_data = e->service_data;
			written++;
		}
		total++;
	}

	*total_matches = total;
	return written;
}
