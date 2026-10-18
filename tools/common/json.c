/*
 * json.c - see json.h. A recursive-descent validator over a cursor; the
 * top level records its members, nested values are only checked and
 * kept as spans.
 */
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"

struct cur {
	const char *p, *end;
};

static void skip_ws(struct cur *c)
{
	while (c->p < c->end && (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r'))
		c->p++;
}

static bool is_digit(char ch)
{
	return ch >= '0' && ch <= '9';
}

static int hexval(char ch)
{
	if (ch >= '0' && ch <= '9')
		return ch - '0';
	if (ch >= 'a' && ch <= 'f')
		return ch - 'a' + 10;
	if (ch >= 'A' && ch <= 'F')
		return ch - 'A' + 10;
	return -1;
}

/* At the opening quote; leaves the cursor after the closing one. */
static int parse_string(struct cur *c)
{
	c->p++;
	while (c->p < c->end) {
		unsigned char ch = (unsigned char)*c->p++;

		if (ch == '"')
			return JSON_OK;
		if (ch < 0x20)
			return JSON_ESYNTAX;
		if (ch != '\\')
			continue;
		if (c->p >= c->end)
			return JSON_ESYNTAX;
		ch = (unsigned char)*c->p++;
		if (ch == 'u') {
			int i;

			if (c->end - c->p < 4)
				return JSON_ESYNTAX;
			for (i = 0; i < 4; i++)
				if (hexval(c->p[i]) < 0)
					return JSON_ESYNTAX;
			c->p += 4;
		} else if (!strchr("\"\\/bfnrt", ch) || !ch) {
			return JSON_ESYNTAX;
		}
	}
	return JSON_ESYNTAX;
}

static int parse_number(struct cur *c)
{
	if (c->p < c->end && *c->p == '-')
		c->p++;
	if (c->p >= c->end)
		return JSON_ESYNTAX;
	if (*c->p == '0') {
		c->p++;
	} else if (is_digit(*c->p)) {
		while (c->p < c->end && is_digit(*c->p))
			c->p++;
	} else {
		return JSON_ESYNTAX;
	}
	if (c->p < c->end && *c->p == '.') {
		c->p++;
		if (c->p >= c->end || !is_digit(*c->p))
			return JSON_ESYNTAX;
		while (c->p < c->end && is_digit(*c->p))
			c->p++;
	}
	if (c->p < c->end && (*c->p == 'e' || *c->p == 'E')) {
		c->p++;
		if (c->p < c->end && (*c->p == '+' || *c->p == '-'))
			c->p++;
		if (c->p >= c->end || !is_digit(*c->p))
			return JSON_ESYNTAX;
		while (c->p < c->end && is_digit(*c->p))
			c->p++;
	}
	return JSON_OK;
}

static int parse_literal(struct cur *c, const char *lit)
{
	size_t n = strlen(lit);

	if ((size_t)(c->end - c->p) < n || memcmp(c->p, lit, n))
		return JSON_ESYNTAX;
	c->p += n;
	return JSON_OK;
}

static int parse_object(struct cur *c, int depth, struct json_obj *o);
static int parse_array(struct cur *c, int depth);

/* One value at the cursor (no leading whitespace); its type and span. */
static int parse_value(struct cur *c, int depth, enum json_type *type, struct json_span *span)
{
	const char *start = c->p;
	int rc;

	if (c->p >= c->end)
		return JSON_ESYNTAX;
	switch (*c->p) {
	case '"': *type = JSON_STRING; rc = parse_string(c); break;
	case '{': *type = JSON_OBJECT; rc = parse_object(c, depth + 1, NULL); break;
	case '[': *type = JSON_ARRAY; rc = parse_array(c, depth + 1); break;
	case 't': *type = JSON_TRUE; rc = parse_literal(c, "true"); break;
	case 'f': *type = JSON_FALSE; rc = parse_literal(c, "false"); break;
	case 'n': *type = JSON_NULL; rc = parse_literal(c, "null"); break;
	default: *type = JSON_NUMBER; rc = parse_number(c); break;
	}
	span->p = start;
	span->len = (size_t)(c->p - start);
	return rc;
}

/* At '{'; records the members in O when it is not NULL. */
static int parse_object(struct cur *c, int depth, struct json_obj *o)
{
	if (depth > JSON_MAX_DEPTH)
		return JSON_ELIMIT;
	c->p++;
	skip_ws(c);
	if (c->p < c->end && *c->p == '}') {
		c->p++;
		return JSON_OK;
	}
	for (;;) {
		struct json_member m;
		const char *key;
		int rc;

		skip_ws(c);
		if (c->p >= c->end || *c->p != '"')
			return JSON_ESYNTAX;
		key = c->p;
		rc = parse_string(c);
		if (rc)
			return rc;
		m.key.p = key + 1;
		m.key.len = (size_t)(c->p - key) - 2;
		skip_ws(c);
		if (c->p >= c->end || *c->p != ':')
			return JSON_ESYNTAX;
		c->p++;
		skip_ws(c);
		rc = parse_value(c, depth, &m.type, &m.val);
		if (rc)
			return rc;
		if (o) {
			if (o->n == JSON_MAX_MEMBERS)
				return JSON_ELIMIT;
			o->m[o->n++] = m;
		}
		skip_ws(c);
		if (c->p >= c->end)
			return JSON_ESYNTAX;
		if (*c->p == '}') {
			c->p++;
			return JSON_OK;
		}
		if (*c->p != ',')
			return JSON_ESYNTAX;
		c->p++;
	}
}

static int parse_array(struct cur *c, int depth)
{
	if (depth > JSON_MAX_DEPTH)
		return JSON_ELIMIT;
	c->p++;
	skip_ws(c);
	if (c->p < c->end && *c->p == ']') {
		c->p++;
		return JSON_OK;
	}
	for (;;) {
		enum json_type t;
		struct json_span s;
		int rc;

		skip_ws(c);
		rc = parse_value(c, depth, &t, &s);
		if (rc)
			return rc;
		skip_ws(c);
		if (c->p >= c->end)
			return JSON_ESYNTAX;
		if (*c->p == ']') {
			c->p++;
			return JSON_OK;
		}
		if (*c->p != ',')
			return JSON_ESYNTAX;
		c->p++;
	}
}

int json_parse(struct json_obj *o, const char *text, size_t len)
{
	struct cur c = { text, text + len };
	int rc;

	o->n = 0;
	skip_ws(&c);
	if (c.p >= c.end || *c.p != '{')
		return JSON_ESYNTAX;
	rc = parse_object(&c, 1, o);
	if (rc == JSON_OK) {
		skip_ws(&c);
		if (c.p != c.end)
			rc = JSON_ESYNTAX;
	}
	if (rc)
		o->n = 0;
	return rc;
}

static size_t utf8_put(char *out, uint32_t cp)
{
	if (cp < 0x80) {
		out[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		out[0] = (char)(0xc0 | (cp >> 6));
		out[1] = (char)(0x80 | (cp & 0x3f));
		return 2;
	}
	if (cp < 0x10000) {
		out[0] = (char)(0xe0 | (cp >> 12));
		out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
		out[2] = (char)(0x80 | (cp & 0x3f));
		return 3;
	}
	out[0] = (char)(0xf0 | (cp >> 18));
	out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
	out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
	out[3] = (char)(0x80 | (cp & 0x3f));
	return 4;
}

static uint32_t hex4(const char *p)
{
	return (uint32_t)(hexval(p[0]) << 12 | hexval(p[1]) << 8 | hexval(p[2]) << 4 | hexval(p[3]));
}

/* The validated string body P..P+N (no quotes) unescaped into BUF. */
static bool unescape(const char *p, size_t n, char *buf, size_t len)
{
	const char *end = p + n;
	size_t o = 0;

	while (p < end) {
		char tmp[4];
		size_t k = 1;

		if (*p != '\\') {
			tmp[0] = *p++;
		} else {
			p++;
			switch (*p++) {
			case 'b': tmp[0] = '\b'; break;
			case 'f': tmp[0] = '\f'; break;
			case 'n': tmp[0] = '\n'; break;
			case 'r': tmp[0] = '\r'; break;
			case 't': tmp[0] = '\t'; break;
			case 'u': {
				uint32_t cp = hex4(p);

				p += 4;
				if (cp >= 0xdc00 && cp <= 0xdfff)
					return false;	/* lone low surrogate */
				if (cp >= 0xd800 && cp <= 0xdbff) {
					uint32_t lo;

					if (end - p < 6 || p[0] != '\\' || p[1] != 'u')
						return false;
					lo = hex4(p + 2);
					if (lo < 0xdc00 || lo > 0xdfff)
						return false;
					p += 6;
					cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
				}
				if (!cp)
					return false;	/* would end the C string */
				k = utf8_put(tmp, cp);
				break;
			}
			default: tmp[0] = p[-1]; break;	/* " \ / */
			}
		}
		if (o + k >= len)
			return false;
		memcpy(buf + o, tmp, k);
		o += k;
	}
	if (o >= len)
		return false;
	buf[o] = '\0';
	return true;
}

static bool key_is(const struct json_span *k, const char *key)
{
	char buf[128];
	size_t n = strlen(key);

	if (!memchr(k->p, '\\', k->len))
		return k->len == n && !memcmp(k->p, key, n);
	return unescape(k->p, k->len, buf, sizeof(buf)) && !strcmp(buf, key);
}

const struct json_member *json_find(const struct json_obj *o, const char *key)
{
	int i;

	for (i = 0; i < o->n; i++)
		if (key_is(&o->m[i].key, key))
			return &o->m[i];
	return NULL;
}

bool json_span_string(enum json_type type, struct json_span v, char *buf, size_t len)
{
	if (type != JSON_STRING || v.len < 2)
		return false;
	return unescape(v.p + 1, v.len - 2, buf, len);
}

/* A number span as a C string; numbers longer than that are refused. */
static bool number_text(enum json_type type, struct json_span v, char *buf, size_t len)
{
	if (type != JSON_NUMBER || v.len >= len)
		return false;
	memcpy(buf, v.p, v.len);
	buf[v.len] = '\0';
	return true;
}

bool json_span_double(enum json_type type, struct json_span v, double *out)
{
	char buf[64];

	if (!number_text(type, v, buf, sizeof(buf)))
		return false;
	*out = strtod(buf, NULL);
	return true;
}

bool json_span_long(enum json_type type, struct json_span v, long *out)
{
	char buf[64];
	long n;

	if (!number_text(type, v, buf, sizeof(buf)) || strpbrk(buf, ".eE"))
		return false;
	errno = 0;
	n = strtol(buf, NULL, 10);
	if (errno == ERANGE)
		return false;
	*out = n;
	return true;
}

bool json_get_string(const struct json_obj *o, const char *key, char *buf, size_t len)
{
	const struct json_member *m = json_find(o, key);

	return m && json_span_string(m->type, m->val, buf, len);
}

bool json_get_double(const struct json_obj *o, const char *key, double *v)
{
	const struct json_member *m = json_find(o, key);

	return m && json_span_double(m->type, m->val, v);
}

bool json_get_long(const struct json_obj *o, const char *key, long *v)
{
	const struct json_member *m = json_find(o, key);

	return m && json_span_long(m->type, m->val, v);
}

bool json_get_bool(const struct json_obj *o, const char *key, bool *v)
{
	const struct json_member *m = json_find(o, key);

	if (!m || (m->type != JSON_TRUE && m->type != JSON_FALSE))
		return false;
	*v = m->type == JSON_TRUE;
	return true;
}

bool json_is_null(const struct json_obj *o, const char *key)
{
	const struct json_member *m = json_find(o, key);

	return m && m->type == JSON_NULL;
}

bool json_array_next(struct json_span arr, size_t *pos, enum json_type *type, struct json_span *elem)
{
	struct cur c = { arr.p + *pos, arr.p + arr.len };

	if (*pos == 0) {
		if (c.p >= c.end || *c.p != '[')
			return false;
		c.p++;
	}
	skip_ws(&c);
	if (c.p >= c.end || *c.p == ']')
		return false;
	if (parse_value(&c, 1, type, elem))
		return false;
	skip_ws(&c);
	if (c.p < c.end && *c.p == ',')
		c.p++;
	*pos = (size_t)(c.p - arr.p);
	return true;
}

size_t json_escape_n(char *dst, size_t dstsz, const char *src, size_t n)
{
	static const char hex[] = "0123456789abcdef";
	size_t o = 0, i;

	if (!dstsz)
		return 0;
	for (i = 0; i < n && src[i]; i++) {
		unsigned char ch = (unsigned char)src[i];
		char tmp[6];
		size_t k;

		if (ch == '"' || ch == '\\') {
			tmp[0] = '\\';
			tmp[1] = (char)ch;
			k = 2;
		} else if (ch < 0x20) {
			memcpy(tmp, "\\u00", 4);
			tmp[4] = hex[ch >> 4];
			tmp[5] = hex[ch & 0xf];
			k = 6;
		} else {
			tmp[0] = ch < 0x7f ? (char)ch : '?';
			k = 1;
		}
		if (o + k >= dstsz)
			break;
		memcpy(dst + o, tmp, k);
		o += k;
	}
	dst[o] = '\0';
	return o;
}

size_t json_escape(char *dst, size_t dstsz, const char *src)
{
	return json_escape_n(dst, dstsz, src, (size_t)-1);
}
