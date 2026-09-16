/* Host unit tests for tools/servreg-locator/jsn.c: the *.jsn domain
 * descriptor parser and the resulting lookup table. No sockets, no kernel
 * dependency. Uses the real, device-pulled fixtures under tests/fixtures/
 * (including the confirmed 0-byte modemus.jsn) plus synthetic files
 * generated into a temp directory for edge cases that don't occur in the
 * checked-in fixtures (pagination with many matches, an overlong name, and
 * the JSN_MAX_ENTRIES capacity cap).
 * Build/run: see Makefile ("make test").
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "../jsn.h"

static int g_failures;
static int g_tests;

#define CHECK(cond) do { \
	g_tests++; \
	if (!(cond)) { \
		g_failures++; \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	} \
} while (0)

static void test_fixtures_dir(void)
{
	struct jsn_table t;
	struct servreg_loc_entry out[SERVREG_LOC_LIST_LENGTH];
	uint32_t total;
	size_t n;

	memset(&t, 0, sizeof(t));
	CHECK(jsn_load_dir("fixtures", &t, 0) == 0);

	/* modemr.jsn (2) + modemuw.jsn (3) + adspr.jsn (1) + modemus.jsn (0,
	 * empty) + malformed_no_instance.jsn (0, skipped file) +
	 * bad_entry.jsn (2 good + 1 skipped entry) + not_a_jsn.txt (ignored,
	 * wrong extension) */
	CHECK(t.count == 8);

	n = jsn_table_lookup(&t, "tms/servreg", 0, out, SERVREG_LOC_LIST_LENGTH, &total);
	CHECK(total == 4); /* modemr, modemuw, adspr, bad_entry all provide it */
	CHECK(n == 4);

	n = jsn_table_lookup(&t, "kernel/elf_loader", 0, out, SERVREG_LOC_LIST_LENGTH, &total);
	CHECK(total == 1);
	CHECK(n == 1);
	CHECK(strcmp(out[0].name, "msm/modem/wlan_pd") == 0);
	CHECK(out[0].instance_id == 180);

	n = jsn_table_lookup(&t, "tms/pdr_enabled", 0, out, SERVREG_LOC_LIST_LENGTH, &total);
	CHECK(total == 2); /* modemr + bad_entry */
	CHECK(n == 2);

	n = jsn_table_lookup(&t, "wlan/fw", 0, out, SERVREG_LOC_LIST_LENGTH, &total);
	CHECK(total == 1);
	CHECK(n == 1);

	/* Unknown service: SUCCESS-shaped zero match, not an error -- the
	 * caller (servreg-locator.c) turns this into total_domains=0 with no
	 * domain_list TLV, matching mainline pd-mapper's behavior. */
	n = jsn_table_lookup(&t, "no/such/service", 0, out, SERVREG_LOC_LIST_LENGTH, &total);
	CHECK(total == 0);
	CHECK(n == 0);

	/* The one entry in bad_entry.jsn missing "service" must not have
	 * silently become some other entry -- confirm the skip didn't leak a
	 * bogus service name a real client could ever ask for. */
	n = jsn_table_lookup(&t, "tms/", 0, out, SERVREG_LOC_LIST_LENGTH, &total);
	CHECK(total == 0);
}

static void test_bad_entry_domain_present(void)
{
	struct jsn_table t;
	struct servreg_loc_entry out[SERVREG_LOC_LIST_LENGTH];
	uint32_t total;

	memset(&t, 0, sizeof(t));
	CHECK(jsn_load_dir("fixtures", &t, 0) == 0);

	/* The rest of bad_entry.jsn is still usable even though one entry in
	 * its sr_service array was malformed -- a bad entry must not discard
	 * its whole file. */
	jsn_table_lookup(&t, "tms/servreg", 0, out, SERVREG_LOC_LIST_LENGTH, &total);
	CHECK(total == 4);
}

static char *mkdtemp_or_die(char *tmpl)
{
	char *d = mkdtemp(tmpl);

	if (!d) {
		perror("mkdtemp");
		exit(1);
	}
	return d;
}

static void write_file(const char *dir, const char *name, const char *content)
{
	char path[512];
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	f = fopen(path, "w");
	if (!f) {
		perror("fopen");
		exit(1);
	}
	fputs(content, f);
	fclose(f);
}

static void rm_dir(const char *dir)
{
	char cmd[600];

	/* Test-only cleanup of a directory this test itself created under
	 * mkdtemp(); not used anywhere in the daemon or the parser. */
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
	if (system(cmd) != 0)
		fprintf(stderr, "warning: cleanup of %s failed\n", dir);
}

static void test_pagination(void)
{
	char tmpl[] = "/tmp/servreg_loc_test_page_XXXXXX";
	char *dir = mkdtemp_or_die(tmpl);
	struct jsn_table t;
	struct servreg_loc_entry out[32];
	uint32_t total;
	size_t n;
	int i;

	for (i = 0; i < 5; i++) {
		char fname[32];
		char body[512];

		snprintf(fname, sizeof(fname), "d%d.jsn", i);
		snprintf(body, sizeof(body),
			"{\"sr_domain\":{\"soc\":\"msm\",\"domain\":\"d%d\","
			"\"subdomain\":\"root_pd\",\"qmi_instance_id\":%d},"
			"\"sr_service\":[{\"provider\":\"test\",\"service\":\"paginate\","
			"\"service_data_valid\":0,\"service_data\":0}]}",
			i, i);
		write_file(dir, fname, body);
	}

	memset(&t, 0, sizeof(t));
	CHECK(jsn_load_dir(dir, &t, 0) == 0);
	CHECK(t.count == 5);

	n = jsn_table_lookup(&t, "test/paginate", 0, out, 2, &total);
	CHECK(total == 5);
	CHECK(n == 2);

	/* Exactly one entry left after skipping the first 4 -- the
	 * "offset < total must yield >= 1 entry" invariant the modem's
	 * client and this AP's own kernel client both rely on to terminate
	 * their read loops. */
	n = jsn_table_lookup(&t, "test/paginate", 4, out, 2, &total);
	CHECK(total == 5);
	CHECK(n == 1);

	/* Past the end: a well-formed client never asks this (it stops once
	 * domains_read >= total_domains), but it must not underflow or
	 * fabricate an entry. */
	n = jsn_table_lookup(&t, "test/paginate", 5, out, 2, &total);
	CHECK(total == 5);
	CHECK(n == 0);

	n = jsn_table_lookup(&t, "test/paginate", 0, out, 32, &total);
	CHECK(total == 5);
	CHECK(n == 5);

	rm_dir(dir);
}

static void test_overlong_name_rejected(void)
{
	char tmpl[] = "/tmp/servreg_loc_test_long_XXXXXX";
	char *dir = mkdtemp_or_die(tmpl);
	struct jsn_table t;
	char body[1024];

	/* "soc/domain/subdomain" well past SERVREG_LOC_NAME_LEN (64) --
	 * must be rejected before it ever reaches the encoder, not
	 * truncated. */
	snprintf(body, sizeof(body),
		"{\"sr_domain\":{"
		"\"soc\":\"aaaaaaaaaaaaaaaaaaaaaaaa\","
		"\"domain\":\"bbbbbbbbbbbbbbbbbbbbbbbb\","
		"\"subdomain\":\"cccccccccccccccccccccccccccccccc\","
		"\"qmi_instance_id\":1},"
		"\"sr_service\":[{\"provider\":\"x\",\"service\":\"y\","
		"\"service_data_valid\":0,\"service_data\":0}]}");
	write_file(dir, "toolong.jsn", body);

	memset(&t, 0, sizeof(t));
	CHECK(jsn_load_dir(dir, &t, 0) == 0);
	CHECK(t.count == 0);

	rm_dir(dir);
}

static void test_capacity_cap(void)
{
	char tmpl[] = "/tmp/servreg_loc_test_cap_XXXXXX";
	char *dir = mkdtemp_or_die(tmpl);
	struct jsn_table t;
	FILE *f;
	char path[512];
	int i;
	const int n_services = JSN_MAX_ENTRIES + 20;

	snprintf(path, sizeof(path), "%s/big.jsn", dir);
	f = fopen(path, "w");
	if (!f) {
		perror("fopen");
		exit(1);
	}
	/* service_data_valid/service_data are optional (default to 0 when
	 * absent) -- left out here to keep this synthetic file comfortably
	 * under JSN_MAX_FILE_LEN despite JSN_MAX_ENTRIES+20 entries. */
	fprintf(f, "{\"sr_domain\":{\"soc\":\"msm\",\"domain\":\"big\","
		   "\"subdomain\":\"root_pd\",\"qmi_instance_id\":1},"
		   "\"sr_service\":[");
	for (i = 0; i < n_services; i++)
		fprintf(f, "%s{\"provider\":\"p%d\",\"service\":\"s%d\"}",
			i ? "," : "", i, i);
	fprintf(f, "]}");
	fclose(f);

	memset(&t, 0, sizeof(t));
	CHECK(jsn_load_dir(dir, &t, 0) == 0);
	CHECK(t.count == JSN_MAX_ENTRIES);

	rm_dir(dir);
}

static void test_missing_dir_is_not_fatal(void)
{
	struct jsn_table t;

	memset(&t, 0, sizeof(t));
	CHECK(jsn_load_dir("/no/such/directory/for/this/test", &t, 0) == 0);
	CHECK(t.count == 0);
}

int main(void)
{
	struct stat st;

	/* Fixtures are referenced relative to this binary's working
	 * directory, matching tests/test-qmux and tests/test-msmipc; the
	 * Makefile's `test` target runs it from tools/servreg-locator/. */
	if (stat("fixtures/modemr.jsn", &st) != 0) {
		fprintf(stderr, "test_jsn: run from tools/servreg-locator/ "
				"(fixtures/modemr.jsn not found)\n");
		return 1;
	}

	test_fixtures_dir();
	test_bad_entry_domain_present();
	test_pagination();
	test_overlong_name_rejected();
	test_capacity_cap();
	test_missing_dir_is_not_fatal();

	fprintf(stderr, "%s: %d/%d checks passed\n", g_failures ? "FAIL" : "PASS",
		g_tests - g_failures, g_tests);
	return g_failures ? 1 : 0;
}
