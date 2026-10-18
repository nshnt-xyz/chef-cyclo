/* Host tests for tools/common/json.c: the reader on valid input (every
 * value type, whitespace, nested object spans parsed again, arrays walked,
 * escaped keys, duplicates, a span that is not NUL-terminated), escapes
 * and \u decoding (BMP, surrogate pairs, lone surrogates, \u0000), string
 * truncation, the number getters, invalid input (grammar errors, trailing
 * garbage, nested errors), the member and depth limits, and json_escape()
 * with its truncation and its round trip through the reader.
 * Build/run: see tools/common/Makefile ("make test").
 */
#include <stdio.h>
#include <string.h>

#include "json.h"

static int g_failures;
static int g_tests;

#define CHECK(cond) do { \
	g_tests++; \
	if (!(cond)) { \
		g_failures++; \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	} \
} while (0)

static int parse(struct json_obj *o, const char *text)
{
	return json_parse(o, text, strlen(text));
}

static void test_valid(void)
{
	struct json_obj o;
	char buf[64];
	double d;
	long l;
	bool b;

	CHECK(parse(&o, "{\"type\":\"hello\",\"daemon\":\"buttond\",\"proto\":1}") == JSON_OK);
	CHECK(o.n == 3);
	CHECK(json_get_string(&o, "type", buf, sizeof(buf)) && !strcmp(buf, "hello"));
	CHECK(json_get_string(&o, "daemon", buf, sizeof(buf)) && !strcmp(buf, "buttond"));
	CHECK(json_get_long(&o, "proto", &l) && l == 1);
	CHECK(json_get_double(&o, "proto", &d) && d == 1.0);
	/* missing, or another type */
	CHECK(!json_get_string(&o, "gesture", buf, sizeof(buf)));
	CHECK(!json_get_string(&o, "proto", buf, sizeof(buf)));
	CHECK(!json_get_long(&o, "type", &l));
	CHECK(!json_get_bool(&o, "type", &b));
	CHECK(!json_is_null(&o, "type"));
	CHECK(json_find(&o, "nope") == NULL);

	CHECK(parse(&o, " \t{ } \r\n") == JSON_OK && o.n == 0);
	CHECK(parse(&o, "{}") == JSON_OK && o.n == 0);

	CHECK(parse(&o, "{ \"s\" : \"x\" , \"i\":-42,\"f\":1.5e3,\"z\":0,\"t\":true,\"n\":null,"
			"\"F\":false,\"o\":{\"a\":[1,2]},\"a\":[],\"e\":-0.25E-2 }\n") == JSON_OK);
	CHECK(o.n == 10);
	CHECK(json_find(&o, "s")->type == JSON_STRING);
	CHECK(json_get_long(&o, "i", &l) && l == -42);
	CHECK(json_get_double(&o, "f", &d) && d == 1500.0);
	CHECK(!json_get_long(&o, "f", &l));	/* exponent: not an integer */
	CHECK(json_get_long(&o, "z", &l) && l == 0);
	CHECK(json_get_bool(&o, "t", &b) && b);
	CHECK(json_get_bool(&o, "F", &b) && !b);
	CHECK(json_is_null(&o, "n"));
	CHECK(!json_get_bool(&o, "n", &b));
	CHECK(json_find(&o, "o")->type == JSON_OBJECT);
	CHECK(json_find(&o, "a")->type == JSON_ARRAY);
	CHECK(json_get_double(&o, "e", &d) && d == -0.0025);
	CHECK(!json_get_long(&o, "e", &l));
	/* the raw spans */
	CHECK(json_find(&o, "s")->val.len == 3 && !memcmp(json_find(&o, "s")->val.p, "\"x\"", 3));
	CHECK(json_find(&o, "o")->val.len == 11 && !memcmp(json_find(&o, "o")->val.p, "{\"a\":[1,2]}", 11));

	/* integers: range and fractions */
	CHECK(parse(&o, "{\"big\":9223372036854775807,\"over\":99999999999999999999,\"half\":1.5}") == JSON_OK);
	CHECK(json_get_long(&o, "big", &l) && l == 9223372036854775807L);
	CHECK(!json_get_long(&o, "over", &l));
	CHECK(json_get_double(&o, "over", &d) && d > 9.9e19);
	CHECK(!json_get_long(&o, "half", &l));

	/* the first of two members of one name wins */
	CHECK(parse(&o, "{\"k\":\"first\",\"k\":\"second\"}") == JSON_OK && o.n == 2);
	CHECK(json_get_string(&o, "k", buf, sizeof(buf)) && !strcmp(buf, "first"));

	/* keys compare unescaped */
	CHECK(parse(&o, "{\"t\\u0079pe\":\"gesture\",\"a\\\"b\":1}") == JSON_OK);
	CHECK(json_get_string(&o, "type", buf, sizeof(buf)) && !strcmp(buf, "gesture"));
	CHECK(json_get_long(&o, "a\"b", &l) && l == 1);

	/* a span inside a larger buffer: only LEN bytes count */
	{
		const char text[] = "{\"a\":1}garbage";

		CHECK(json_parse(&o, text, 7) == JSON_OK && o.n == 1);
		CHECK(json_parse(&o, text, sizeof(text) - 1) == JSON_ESYNTAX);
		CHECK(json_parse(&o, text, 6) == JSON_ESYNTAX);
	}
}

static void test_nested(void)
{
	struct json_obj o, sub, el;
	const struct json_member *m;
	enum json_type t;
	struct json_span s;
	size_t pos;
	char buf[32];
	long l;
	int n;

	CHECK(parse(&o, "{\"type\":\"status\",\"buttons\":{\"power\":\"up\",\"voldown\":\"down\"},"
			"\"claims\":{},\"sats\":[{\"prn\":3,\"used\":true},{\"prn\":17}],"
			"\"mix\":[ 1 , \"two\" , null , [3] , {} ],\"none\":[ ]}") == JSON_OK);
	m = json_find(&o, "buttons");
	CHECK(m && m->type == JSON_OBJECT);
	CHECK(json_parse(&sub, m->val.p, m->val.len) == JSON_OK && sub.n == 2);
	CHECK(json_get_string(&sub, "voldown", buf, sizeof(buf)) && !strcmp(buf, "down"));
	m = json_find(&o, "claims");
	CHECK(json_parse(&sub, m->val.p, m->val.len) == JSON_OK && sub.n == 0);

	m = json_find(&o, "sats");
	CHECK(m && m->type == JSON_ARRAY);
	pos = 0;
	n = 0;
	while (json_array_next(m->val, &pos, &t, &s)) {
		CHECK(t == JSON_OBJECT);
		CHECK(json_parse(&el, s.p, s.len) == JSON_OK);
		CHECK(json_get_long(&el, "prn", &l) && l == (n ? 17 : 3));
		n++;
	}
	CHECK(n == 2);

	m = json_find(&o, "mix");
	pos = 0;
	CHECK(json_array_next(m->val, &pos, &t, &s) && t == JSON_NUMBER && json_span_long(t, s, &l) && l == 1);
	CHECK(json_array_next(m->val, &pos, &t, &s) && t == JSON_STRING &&
	      json_span_string(t, s, buf, sizeof(buf)) && !strcmp(buf, "two"));
	CHECK(json_array_next(m->val, &pos, &t, &s) && t == JSON_NULL);
	CHECK(json_array_next(m->val, &pos, &t, &s) && t == JSON_ARRAY && s.len == 3);
	CHECK(json_array_next(m->val, &pos, &t, &s) && t == JSON_OBJECT && s.len == 2);
	CHECK(!json_array_next(m->val, &pos, &t, &s));
	CHECK(!json_array_next(m->val, &pos, &t, &s));	/* stays at the end */

	m = json_find(&o, "none");
	pos = 0;
	CHECK(!json_array_next(m->val, &pos, &t, &s));
	/* an array span is not an object */
	CHECK(json_parse(&sub, m->val.p, m->val.len) == JSON_ESYNTAX);
}

static void test_strings(void)
{
	struct json_obj o;
	char buf[64], small[4];

	CHECK(parse(&o, "{\"s\":\"q\\\" b\\\\ s\\/ \\b\\f\\n\\r\\t\"}") == JSON_OK);
	CHECK(json_get_string(&o, "s", buf, sizeof(buf)) && !strcmp(buf, "q\" b\\ s/ \b\f\n\r\t"));

	CHECK(parse(&o, "{\"a\":\"\\u0041\\u00e9\\u20AC\",\"p\":\"\\ud83d\\ude00\",\"hi\":\"\\ud83dx\","
			"\"lo\":\"\\ude00\",\"hh\":\"\\ud83d\\ud83d\",\"z\":\"a\\u0000b\",\"u\":\"\xc3\xa9\"}") == JSON_OK);
	CHECK(json_get_string(&o, "a", buf, sizeof(buf)) && !strcmp(buf, "A\xc3\xa9\xe2\x82\xac"));
	CHECK(json_get_string(&o, "p", buf, sizeof(buf)) && !strcmp(buf, "\xf0\x9f\x98\x80"));
	CHECK(!json_get_string(&o, "hi", buf, sizeof(buf)));	/* lone high surrogate */
	CHECK(!json_get_string(&o, "lo", buf, sizeof(buf)));	/* lone low surrogate */
	CHECK(!json_get_string(&o, "hh", buf, sizeof(buf)));	/* high, high */
	CHECK(!json_get_string(&o, "z", buf, sizeof(buf)));	/* \u0000 */
	CHECK(json_get_string(&o, "u", buf, sizeof(buf)) && !strcmp(buf, "\xc3\xa9"));	/* raw UTF-8 */

	/* truncation: false, never a cut string; exactly fitting is fine */
	CHECK(parse(&o, "{\"s\":\"abc\",\"e\":\"\\u20ac\",\"x\":\"\"}") == JSON_OK);
	CHECK(json_get_string(&o, "s", small, 4) && !strcmp(small, "abc"));
	CHECK(!json_get_string(&o, "s", small, 3));
	CHECK(json_get_string(&o, "e", small, 4) && !strcmp(small, "\xe2\x82\xac"));
	CHECK(!json_get_string(&o, "e", small, 3));	/* not half a character */
	CHECK(json_get_string(&o, "x", small, 1) && small[0] == '\0');
	CHECK(!json_get_string(&o, "x", small, 0));
}

static void test_invalid(void)
{
	static const char *const bad[] = {
		"", " ", "[]", "1", "\"s\"", "null", "{", "}", "{\"a\":1", "{\"a\":1}x", "{\"a\":1}{}",
		"{\"a\":1},", "{\"a\":}", "{\"a\" 1}", "{a:1}", "{'a':1}", "{\"a\":1,}", "{,}",
		"{\"a\":1 \"b\":2}", "{\"a\":01}", "{\"a\":1.}", "{\"a\":.5}", "{\"a\":-}", "{\"a\":+1}",
		"{\"a\":1e}", "{\"a\":1e+}", "{\"a\":0x10}", "{\"a\":NaN}", "{\"a\":Infinity}",
		"{\"a\":tru}", "{\"a\":nul}", "{\"a\":True}", "{\"a\":\"x}", "{\"a\":\"x\ty\"}",
		"{\"a\":\"\\x\"}", "{\"a\":\"\\u12\"}", "{\"a\":\"\\u12G4\"}", "{\"a\":\"\\\"}",
		"{\"a\":[1,]}", "{\"a\":[1 2]}", "{\"a\":[}", "{\"a\":{\"b\"}}", "{\"a\":{\"b\":1,}}",
		"{\"a\":[{\"b\":x}]}", "{\"a\":1}\x01", "\xef\xbb\xbf{}",
	};
	struct json_obj o;
	size_t i;

	for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		int rc = parse(&o, bad[i]);

		CHECK(rc == JSON_ESYNTAX);
		CHECK(o.n == 0);
		if (rc != JSON_ESYNTAX)
			fprintf(stderr, "  accepted: %s\n", bad[i]);
	}
	/* an embedded NUL is a control character, not the end */
	CHECK(json_parse(&o, "{\"a\":\"x\0\"}", 10) == JSON_ESYNTAX);
	CHECK(json_parse(&o, "{}\0", 3) == JSON_ESYNTAX);
}

static void test_limits(void)
{
	struct json_obj o;
	char text[1024];
	size_t off;
	int i;

	off = (size_t)snprintf(text, sizeof(text), "{");
	for (i = 0; i < JSON_MAX_MEMBERS; i++)
		off += (size_t)snprintf(text + off, sizeof(text) - off, "%s\"k%d\":%d", i ? "," : "", i, i);
	snprintf(text + off, sizeof(text) - off, "}");
	CHECK(parse(&o, text) == JSON_OK && o.n == JSON_MAX_MEMBERS);
	snprintf(text + off, sizeof(text) - off, ",\"one\":1}");
	CHECK(parse(&o, text) == JSON_ELIMIT && o.n == 0);
	/* nested members do not count towards the top level's limit */
	off = (size_t)snprintf(text, sizeof(text), "{\"n\":{");
	for (i = 0; i < JSON_MAX_MEMBERS + 8; i++)
		off += (size_t)snprintf(text + off, sizeof(text) - off, "%s\"k%d\":%d", i ? "," : "", i, i);
	snprintf(text + off, sizeof(text) - off, "}}");
	CHECK(parse(&o, text) == JSON_OK && o.n == 1);

	/* depth: the top level is 1 */
	off = (size_t)snprintf(text, sizeof(text), "{\"a\":");
	for (i = 1; i < JSON_MAX_DEPTH; i++)
		off += (size_t)snprintf(text + off, sizeof(text) - off, "[");
	for (i = 1; i < JSON_MAX_DEPTH; i++)
		off += (size_t)snprintf(text + off, sizeof(text) - off, "]");
	snprintf(text + off, sizeof(text) - off, "}");
	CHECK(parse(&o, text) == JSON_OK);
	off = (size_t)snprintf(text, sizeof(text), "{\"a\":");
	for (i = 0; i < JSON_MAX_DEPTH; i++)
		off += (size_t)snprintf(text + off, sizeof(text) - off, "{\"b\":");
	off += (size_t)snprintf(text + off, sizeof(text) - off, "1");
	for (i = 0; i <= JSON_MAX_DEPTH; i++)
		off += (size_t)snprintf(text + off, sizeof(text) - off, "}");
	CHECK(parse(&o, text) == JSON_ELIMIT);
}

static void test_escape(void)
{
	struct json_obj o;
	char src[128], esc[1024], line[1100], back[256];
	size_t n;
	int i;

	CHECK(json_escape(esc, sizeof(esc), "power.short") == 11 && !strcmp(esc, "power.short"));
	CHECK(json_escape(esc, sizeof(esc), "a\"b\\c") == 7 && !strcmp(esc, "a\\\"b\\\\c"));
	CHECK(json_escape(esc, sizeof(esc), "\x01\n\x1f") == 18 && !strcmp(esc, "\\u0001\\u000a\\u001f"));
	CHECK(json_escape(esc, sizeof(esc), "\x7f\xff\xc3\xa9x") == 5 && !strcmp(esc, "????x"));
	CHECK(json_escape(esc, sizeof(esc), "") == 0 && esc[0] == '\0');

	/* truncation stops before an escape that does not fit whole */
	CHECK(json_escape(esc, 4, "abcdef") == 3 && !strcmp(esc, "abc"));
	CHECK(json_escape(esc, 3, "a\"") == 1 && !strcmp(esc, "a"));
	CHECK(json_escape(esc, 4, "a\"") == 3 && !strcmp(esc, "a\\\""));
	CHECK(json_escape(esc, 6, "\x01") == 0 && esc[0] == '\0');
	CHECK(json_escape(esc, 7, "\x01") == 6 && !strcmp(esc, "\\u0001"));
	CHECK(json_escape(esc, 1, "abc") == 0 && esc[0] == '\0');
	n = json_escape(esc, 0, "abc");
	CHECK(n == 0);
	/* at most N source bytes */
	CHECK(json_escape_n(esc, sizeof(esc), "abcdef", 3) == 3 && !strcmp(esc, "abc"));
	CHECK(json_escape_n(esc, sizeof(esc), "ab", 32) == 2 && !strcmp(esc, "ab"));

	/* round trip: every byte 0x01..0x7e through escape and the reader,
	 * and 0x7f..0xfd as '?' */
	for (i = 0; i < 126; i++)
		src[i] = (char)(i + 1);
	src[126] = '\0';
	json_escape(esc, sizeof(esc), src);
	snprintf(line, sizeof(line), "{\"k\":\"%s\"}", esc);
	CHECK(parse(&o, line) == JSON_OK);
	CHECK(json_get_string(&o, "k", back, sizeof(back)) && !strcmp(back, src));
	for (i = 0; i < 127; i++)
		src[i] = (char)(0x7f + i);
	src[127] = '\0';
	json_escape(esc, sizeof(esc), src);
	snprintf(line, sizeof(line), "{\"k\":\"%s\"}", esc);
	CHECK(parse(&o, line) == JSON_OK);
	CHECK(json_get_string(&o, "k", back, sizeof(back)) && strlen(back) == 127 &&
	      strspn(back, "?") == 127);
}

int main(void)
{
	test_valid();
	test_nested();
	test_strings();
	test_invalid();
	test_limits();
	test_escape();
	printf("test-json: %d/%d checks passed\n", g_tests - g_failures, g_tests);
	return g_failures ? 1 : 0;
}
