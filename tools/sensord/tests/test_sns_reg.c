/* sns_reg.c tests: map parsing and validation, loading the registry
 * copy (exact size only), item/group reads and RAM-only writes. Uses
 * tests/fixtures/sns_reg.map and data it generates; no device data. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sns_reg.h"

static int failures;
static char tmpdir[] = "/tmp/test-sns-reg.XXXXXX";

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
		failures++; \
	} \
} while (0)

static const char *write_file(const char *name, const void *data, size_t len)
{
	static char path[4][256];
	static int k;
	char *p = path[k++ % 4];
	FILE *f;

	snprintf(p, 256, "%s/%s", tmpdir, name);
	f = fopen(p, "wb");
	fwrite(data, 1, len, f);
	fclose(f);
	return p;
}

static const char *make_reg(const char *name, size_t size)
{
	static uint8_t buf[2048];
	size_t i;

	for (i = 0; i < size; i++)
		buf[i] = (uint8_t)(i * 7 + 3);
	return write_file(name, buf, size);
}

static void test_map_errors(void)
{
	static const char *bad[] = {
		"size 1024\n",					/* no groups */
		"group 1 0 16\n",				/* no size */
		"size 1024\ngroup 1 1020 16\n",			/* past the end */
		"size 1024\ngroup 1 0 300\n",			/* group > 256 */
		"size 1024\ngroup 1 0 16\nitem 5 0 9\n",	/* item > 8 */
		"size 1024\ngroup 1 0 16\ngroup 1 16 16\n",	/* duplicate group */
		"size 1024\ngroup 1 0 16\nitem 5 0 1\nitem 5 1 1\n", /* duplicate item */
		"size 1024\ngroup 1 0 16\nbogus 1 2 3\n",
		"size 1024\ngroup 70000 0 16\n",		/* id beyond u16 */
	};
	struct sns_reg r;
	size_t i;
	int line;

	for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		const char *p = write_file("bad.map", bad[i], strlen(bad[i]));

		CHECK(sns_reg_load_map(&r, p, &line) == -EINVAL);
	}
	CHECK(sns_reg_load_map(&r, "/nonexistent/sns_reg.map", &line) == -ENOENT);
}

static void test_fixture(const char *map)
{
	struct sns_reg r;
	uint8_t out[256], in[16];
	size_t got = 0;
	int line = 0, n, i;

	CHECK(sns_reg_load_map(&r, map, &line) == 0);
	CHECK(r.size == 1024 && r.ngroups == 3 && r.nitems == 7);

	/* data must be exactly the map's size */
	CHECK(sns_reg_load_data(&r, make_reg("short.bin", 1000), 0, &got) == -EMSGSIZE && got == 1000);
	CHECK(sns_reg_load_data(&r, make_reg("long.bin", 1025), 0, &got) == -EMSGSIZE);
	CHECK(sns_reg_load_data(&r, make_reg("short.bin", 1000), 1, &got) == 0);
	CHECK(sns_reg_load_data(&r, "/nonexistent/sns.reg", 0, &got) == -ENOENT);
	CHECK(sns_reg_load_data(&r, make_reg("ok.bin", 1024), 0, &got) == 0 && got == 1024);

	n = sns_reg_read(&r, 1, 2695, out, sizeof(out));
	CHECK(n == 40);
	for (i = 0; i < 40; i++)
		CHECK(out[i] == (uint8_t)((256 + i) * 7 + 3));
	n = sns_reg_read(&r, 0, 2309, out, sizeof(out));
	CHECK(n == 2 && out[0] == (uint8_t)(290 * 7 + 3) && out[1] == (uint8_t)(291 * 7 + 3));
	CHECK(sns_reg_read(&r, 0, 2695, out, sizeof(out)) == -1);	/* groups and items apart */
	CHECK(sns_reg_read(&r, 1, 2310, out, sizeof(out)) == -1);
	CHECK(sns_reg_read(&r, 1, 2695, out, 8) == -1);		/* caller buffer too small */
	CHECK(r.reads == 2 && r.misses == 3);

	/* writes: RAM only, whole or prefix, never longer than the entry */
	for (i = 0; i < 16; i++)
		in[i] = (uint8_t)(0xa0 + i);
	CHECK(sns_reg_write(&r, 1, 1000, in, 16) == 0);
	CHECK(sns_reg_read(&r, 0, 700, out, sizeof(out)) == 1 && out[0] == 0xa0);
	CHECK(sns_reg_read(&r, 0, 702, out, sizeof(out)) == 1 && out[0] == 0xa2);
	CHECK(sns_reg_write(&r, 0, 2309, in, 3) == -EINVAL);
	CHECK(sns_reg_write(&r, 0, 2309, in, 0) == -EINVAL);
	CHECK(sns_reg_write(&r, 0, 2309, in + 4, 1) == 0);
	CHECK(sns_reg_read(&r, 0, 2309, out, sizeof(out)) == 2 && out[0] == 0xa4 &&
	      out[1] == (uint8_t)(291 * 7 + 3));
	CHECK(sns_reg_write(&r, 0, 4242, in, 1) == -ENOENT);
	CHECK(r.writes == 2);
	sns_reg_free(&r);
}

/* The data file is opened read-only: a read-only file must load. */
static void test_readonly_source(const char *map)
{
	struct sns_reg r;
	const char *p = make_reg("ro.bin", 1024);
	int line;

	chmod(p, 0444);
	CHECK(sns_reg_load_map(&r, map, &line) == 0);
	CHECK(sns_reg_load_data(&r, p, 0, NULL) == 0);
	sns_reg_free(&r);
}

int main(int argc, char **argv)
{
	const char *map = argc > 1 ? argv[1] : "tests/fixtures/sns_reg.map";
	char cmd[300];

	if (!mkdtemp(tmpdir))
		return 1;
	test_map_errors();
	test_fixture(map);
	test_readonly_source(map);
	snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
	if (system(cmd))
		failures++;
	if (failures) {
		fprintf(stderr, "test-sns-reg: %d failure(s)\n", failures);
		return 1;
	}
	printf("test-sns-reg: all passed\n");
	return 0;
}
