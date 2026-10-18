/*
 * json.h - JSON for the chef-cyclo local sockets: json_escape() for the
 * daemons writing lines, and a reader for the clients parsing them.
 *
 * Reader: json_parse() takes one line (or a nested span from an earlier
 * parse) and fills a struct json_obj with the members of its top-level
 * object, without allocating and without copying: keys and values are
 * spans into the caller's text, which must outlive the object. It is
 * strict: anything that is not exactly one JSON object (RFC 8259 grammar,
 * nested values validated too, at most JSON_MAX_DEPTH deep), with
 * optional whitespace around it, is an error. More than JSON_MAX_MEMBERS
 * members is an error, not a silent cut. Bytes >= 0x80 inside strings
 * pass through unchecked.
 *
 * Getters look a key up (the first member of that name; keys compare
 * unescaped) and are false when it is missing or of another type.
 * json_get_string() unescapes into the caller's buffer, decoding \uXXXX
 * and surrogate pairs to UTF-8; it is false on truncation, a lone
 * surrogate or \u0000. json_get_long() takes integers only (no fraction
 * or exponent, in range). The number getters refuse a number written with
 * 64 characters or more, valid as it may be. A nested object is re-parsed
 * with json_parse(&sub, m->val.p, m->val.len); an array is walked with
 * json_array_next().
 */
#ifndef CHEF_JSON_H
#define CHEF_JSON_H

#include <stdbool.h>
#include <stddef.h>

#define JSON_MAX_MEMBERS 32
#define JSON_MAX_DEPTH 16

enum json_type {
	JSON_STRING,
	JSON_NUMBER,
	JSON_TRUE,
	JSON_FALSE,
	JSON_NULL,
	JSON_OBJECT,
	JSON_ARRAY,
};

struct json_span {
	const char *p;
	size_t len;
};

struct json_member {
	struct json_span key;	/* between the quotes, still escaped */
	enum json_type type;
	struct json_span val;	/* the raw value: strings with their quotes */
};

struct json_obj {
	int n;
	struct json_member m[JSON_MAX_MEMBERS];
};

/* Why json_parse() failed. */
enum {
	JSON_OK = 0,
	JSON_ESYNTAX = -1,	/* not exactly one valid JSON object */
	JSON_ELIMIT = -2,	/* more than JSON_MAX_MEMBERS, or too deep */
};

int json_parse(struct json_obj *o, const char *text, size_t len);

const struct json_member *json_find(const struct json_obj *o, const char *key);
bool json_get_string(const struct json_obj *o, const char *key, char *buf, size_t len);
bool json_get_double(const struct json_obj *o, const char *key, double *v);
bool json_get_long(const struct json_obj *o, const char *key, long *v);
bool json_get_bool(const struct json_obj *o, const char *key, bool *v);
bool json_is_null(const struct json_obj *o, const char *key);

/* The same conversions on a value span (a member's, or an array
 * element's from json_array_next()). */
bool json_span_string(enum json_type type, struct json_span v, char *buf, size_t len);
bool json_span_double(enum json_type type, struct json_span v, double *out);
bool json_span_long(enum json_type type, struct json_span v, long *out);

/* Walk the elements of an array span (a JSON_ARRAY member's val, already
 * validated by json_parse()). *POS starts at 0. True with the next
 * element's type and raw span, false at the end. */
bool json_array_next(struct json_span arr, size_t *pos, enum json_type *type, struct json_span *elem);

/* Writing: SRC escaped into DST for use between quotes, NUL-terminated.
 * Printable ASCII passes, '"' and '\\' are backslash-escaped, control
 * characters become \u00XX and bytes >= 0x7f become '?'. Stops before an
 * escape that would not fit whole. The length written. json_escape_n()
 * reads at most N bytes of SRC. */
size_t json_escape(char *dst, size_t dstsz, const char *src);
size_t json_escape_n(char *dst, size_t dstsz, const char *src, size_t n);

#endif /* CHEF_JSON_H */
