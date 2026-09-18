/* Host unit tests for tools/tftp/translate.c and ramfs.c: the path
 * allowlist (REPORT.md 8.1 point 3), traversal/symlink/absolute-escape
 * rejection, and the RAM shadow's read/write/append/overwrite/cap
 * semantics. No sockets, no AF_MSM_IPC -- pure filesystem + memory, same
 * shape as tools/servreg-locator/tests/test_jsn.c. Build/run: see
 * Makefile ("make test").
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ramfs.h"
#include "tftp.h"
#include "translate.h"

static int g_failures;
static int g_tests;

#define CHECK(cond) do { \
		g_tests++; \
		if (!(cond)) { \
			g_failures++; \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		} \
	} while (0)

static char g_tmproot[] = "/tmp/tftp-translate-test.XXXXXX";

static void write_file(const char *path, size_t size, unsigned char fill)
{
	unsigned char *buf = calloc(1, size ? size : 1);
	int fd;

	memset(buf, fill, size);
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) {
		perror(path);
		exit(1);
	}
	if (size && write(fd, buf, size) != (ssize_t)size) {
		perror(path);
		exit(1);
	}
	close(fd);
	free(buf);
}

static void mkdir_p(const char *path)
{
	char tmp[512];
	char *p;

	snprintf(tmp, sizeof(tmp), "%s", path);
	for (p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			mkdir(tmp, 0700);
			*p = '/';
		}
	}
	mkdir(tmp, 0700);
}

static char *joinpath(char *out, size_t outsz, const char *base, const char *rel)
{
	snprintf(out, outsz, "%s/%s", base, rel);
	return out;
}

static void setup_fixtures(char *fw_dir, size_t fw_sz, char *seed_dir, size_t seed_sz)
{
	char p[600];

	if (!mkdtemp(g_tmproot)) {
		perror("mkdtemp");
		exit(1);
	}

	joinpath(fw_dir, fw_sz, g_tmproot, "firmware");
	mkdir_p(fw_dir);
	joinpath(p, sizeof(p), fw_dir, "modem.jsn");
	write_file(p, 128, 0xaa);
	mkdir_p(joinpath(p, sizeof(p), fw_dir, "sub"));
	joinpath(p, sizeof(p), fw_dir, "sub/nested.bin");
	write_file(p, 4, 0xbb);
	/* A symlink under the firmware dir pointing outside it -- must never
	 * be followed (O_NOFOLLOW). */
	joinpath(p, sizeof(p), fw_dir, "escape-link");
	if (symlink("/etc/passwd", p) < 0) {
		perror("symlink");
		exit(1);
	}

	joinpath(seed_dir, seed_sz, g_tmproot, "seed");
	mkdir_p(seed_dir);
	joinpath(p, sizeof(p), seed_dir, "shob.bin");
	write_file(p, 37282, 1);
	joinpath(p, sizeof(p), seed_dir, "dhob.bin");
	write_file(p, 16384, 2);
	joinpath(p, sizeof(p), seed_dir, "server_check.txt");
	write_file(p, 5, 'h'); /* content doesn't matter for the size check */
	joinpath(p, sizeof(p), seed_dir, "hob_report.txt");
	write_file(p, 4757, 3);
	joinpath(p, sizeof(p), seed_dir, "dhob_report.txt");
	write_file(p, 138, 4);
	mkdir_p(joinpath(p, sizeof(p), seed_dir, "mot_rfs"));
	joinpath(p, sizeof(p), seed_dir, "mot_rfs/imei_sv");
	write_file(p, 1, 5);
	mkdir_p(joinpath(p, sizeof(p), seed_dir, "datablock"));
	joinpath(p, sizeof(p), seed_dir, "datablock/id_00");
	write_file(p, 2600, 6);
	joinpath(p, sizeof(p), seed_dir, "datablock/id_01");
	write_file(p, 2600, 7);
}

static void test_init_and_seed(const char *fw_dir, const char *seed_dir)
{
	CHECK(translate_init(fw_dir) == 0);
	CHECK(translate_init("/nonexistent/does/not/exist") == -1);
	/* re-init against the real dir for the rest of the tests */
	CHECK(translate_init(fw_dir) == 0);

	CHECK(translate_seed(seed_dir) == 0);

	/* Missing seed dir must fail closed. */
	CHECK(translate_seed("/nonexistent/seed/dir") == -1);
}

static void test_seed_wrong_size_fails_closed(const char *fw_dir)
{
	char bad_seed[600], p[700];

	joinpath(bad_seed, sizeof(bad_seed), g_tmproot, "seed-bad-size");
	mkdir_p(bad_seed);
	joinpath(p, sizeof(p), bad_seed, "shob.bin");
	write_file(p, 100, 1); /* wrong size: spec requires exactly 37282 */
	joinpath(p, sizeof(p), bad_seed, "dhob.bin");
	write_file(p, 16384, 2);
	joinpath(p, sizeof(p), bad_seed, "server_check.txt");
	write_file(p, 5, 'h');
	joinpath(p, sizeof(p), bad_seed, "hob_report.txt");
	write_file(p, 4757, 3);
	joinpath(p, sizeof(p), bad_seed, "dhob_report.txt");
	write_file(p, 138, 4);
	mkdir_p(joinpath(p, sizeof(p), bad_seed, "mot_rfs"));
	joinpath(p, sizeof(p), bad_seed, "mot_rfs/imei_sv");
	write_file(p, 1, 5);
	mkdir_p(joinpath(p, sizeof(p), bad_seed, "datablock"));
	joinpath(p, sizeof(p), bad_seed, "datablock/id_00");
	write_file(p, 2600, 6);
	joinpath(p, sizeof(p), bad_seed, "datablock/id_01");
	write_file(p, 2600, 7);

	ramfs_init();
	CHECK(translate_init(fw_dir) == 0);
	CHECK(translate_seed(bad_seed) == -1);
}

static void test_readonly_firmware(void)
{
	struct tftp_file f;
	off_t start;
	int err;
	const char *msg;
	char buf[8];

	CHECK(translate_open("/readonly/firmware/image/modem.jsn", TFTP_OPEN_RRQ, 0,
			      &f, &start, &err, &msg) == 0);
	CHECK(f.kind == TFTP_FILE_RO);
	CHECK(pread(f.fd, buf, 1, 0) == 1 && (unsigned char)buf[0] == 0xaa);
	translate_close(&f);

	/* Same directory via the /readonly/vendor/... alias. */
	CHECK(translate_open("/readonly/vendor/firmware/image/modem.jsn", TFTP_OPEN_RRQ, 0,
			      &f, &start, &err, &msg) == 0);
	translate_close(&f);

	/* Round 1 review F8: /firmware/image is a flat directory -- any '/'
	 * in the remainder is rejected outright, not resolved. */
	CHECK(translate_open("/readonly/firmware/image/sub/nested.bin", TFTP_OPEN_RRQ, 0,
			      &f, &start, &err, &msg) == -1);
	CHECK(err == TFTP_ERROR_EACCESS);

	/* Round 1 review F8: fstat()+S_ISREG rejects a non-regular-file
	 * open even when the name itself has no '/' and O_NOFOLLOW didn't
	 * reject it (a directory is not a symlink). */
	CHECK(translate_open("/readonly/firmware/image/sub", TFTP_OPEN_RRQ, 0,
			      &f, &start, &err, &msg) == -1);
	CHECK(err == TFTP_ERROR_EACCESS);

	/* WRQ to a read-only firmware path is rejected. */
	CHECK(translate_open("/readonly/firmware/image/modem.jsn", TFTP_OPEN_WRQ, 0,
			      &f, &start, &err, &msg) == -1);
	CHECK(err == TFTP_ERROR_EACCESS);

	/* Unknown file under the firmware dir: ENOENT, not silence. */
	CHECK(translate_open("/readonly/firmware/image/does-not-exist", TFTP_OPEN_RRQ, 0,
			      &f, &start, &err, &msg) == -1);
	CHECK(err == TFTP_ERROR_ENOENT);

	/* fsg is always rejected, dangling on stock too. */
	CHECK(translate_open("/readonly/fsg/anything", TFTP_OPEN_RRQ, 0,
			      &f, &start, &err, &msg) == -1);
	CHECK(translate_open("/readonly/fsg/anything", TFTP_OPEN_WRQ, 0,
			      &f, &start, &err, &msg) == -1);
}

static void test_traversal_and_absolute_escape(void)
{
	struct tftp_file f;
	off_t start;
	int err;
	const char *msg;

	static const char *bad_ro[] = {
		"/readonly/firmware/image/../etc/passwd",
		"/readonly/firmware/image/sub/../../etc/passwd",
		"/readonly/firmware/image/..",
		"/readonly/firmware/image//etc/passwd", /* absolute-escape guard */
	};
	static const char *bad_rw[] = {
		"/readwrite/../etc/passwd",
		"/readwrite/foo/../../etc/passwd",
		"/readwrite//etc/passwd",
	};
	static const char *bad_hlos[] = {
		"/hlos/../../etc/passwd",
		"/hlos//etc/passwd",
	};
	size_t i;

	for (i = 0; i < sizeof(bad_ro) / sizeof(bad_ro[0]); i++) {
		CHECK(translate_open(bad_ro[i], TFTP_OPEN_RRQ, 0, &f, &start, &err, &msg) == -1);
	}
	for (i = 0; i < sizeof(bad_rw) / sizeof(bad_rw[0]); i++) {
		CHECK(translate_open(bad_rw[i], TFTP_OPEN_WRQ, 0, &f, &start, &err, &msg) == -1);
	}
	for (i = 0; i < sizeof(bad_hlos) / sizeof(bad_hlos[0]); i++) {
		CHECK(translate_open(bad_hlos[i], TFTP_OPEN_WRQ, 0, &f, &start, &err, &msg) == -1);
	}

	/* A bare prefix with nothing after it is also rejected, not treated
	 * as a directory listing request. */
	CHECK(translate_open("/readwrite/", TFTP_OPEN_WRQ, 0, &f, &start, &err, &msg) == -1);
}

static void test_symlink_not_followed(void)
{
	struct tftp_file f;
	off_t start;
	int err;
	const char *msg;

	CHECK(translate_open("/readonly/firmware/image/escape-link", TFTP_OPEN_RRQ, 0,
			      &f, &start, &err, &msg) == -1);
}

static void test_unknown_prefix_rejected(void)
{
	struct tftp_file f;
	off_t start;
	int err;
	const char *msg;

	CHECK(translate_open("/etc/passwd", TFTP_OPEN_RRQ, 0, &f, &start, &err, &msg) == -1);
	CHECK(err == TFTP_ERROR_ENOENT);
	CHECK(translate_open("/", TFTP_OPEN_RRQ, 0, &f, &start, &err, &msg) == -1);
}

static void test_shared_server_info(void)
{
	struct tftp_file f;
	off_t start;
	int err;
	const char *msg;
	char buf[32];
	ssize_t n;

	CHECK(translate_open("/shared/server_info.txt", TFTP_OPEN_RRQ, 0,
			      &f, &start, &err, &msg) == 0);
	CHECK(f.kind == TFTP_FILE_RAM);
	n = ramfs_pread(f.rf, buf, sizeof(buf), 0);
	CHECK(n == 15);
	CHECK(memcmp(buf, "TFTP_SERVER.1.0", 15) == 0);

	/* server_info.txt is read-only: WRQ rejected, nothing else under
	 * /shared/ exists at all. */
	CHECK(translate_open("/shared/server_info.txt", TFTP_OPEN_WRQ, 0,
			      &f, &start, &err, &msg) == -1);
	CHECK(err == TFTP_ERROR_EACCESS);
	CHECK(translate_open("/shared/other.txt", TFTP_OPEN_RRQ, 0,
			      &f, &start, &err, &msg) == -1);
	CHECK(err == TFTP_ERROR_ENOENT);
}

static void test_readwrite_overwrite_and_append(void)
{
	struct tftp_file f;
	off_t start;
	int err;
	const char *msg;
	char buf[32];
	ssize_t n;

	/* Fresh (non-append) WRQ truncates first -- overwrite with a longer
	 * value than the seed content, then a shorter one, and confirm no
	 * stale trailing bytes survive the second, shorter write (the
	 * documented improvement over upstream tqftpserv's write()-position
	 * semantics -- see ramfs.h's ramfs_truncate() comment). */
	CHECK(translate_open("/readwrite/server_check.txt", TFTP_OPEN_WRQ, 0,
			      &f, &start, &err, &msg) == 0);
	CHECK(start == 0);
	CHECK(ramfs_pwrite(f.rf, "hello", 5, start) == 5);
	CHECK(f.rf->len == 5);

	CHECK(translate_open("/readwrite/server_check.txt", TFTP_OPEN_WRQ, 0,
			      &f, &start, &err, &msg) == 0);
	CHECK(start == 0); /* truncated back to empty */
	CHECK(f.rf->len == 0);
	CHECK(ramfs_pwrite(f.rf, "hi", 2, start) == 2);
	CHECK(f.rf->len == 2); /* not 5: no stale "llo" left over */

	n = ramfs_pread(f.rf, buf, sizeof(buf), 0);
	CHECK(n == 2 && memcmp(buf, "hi", 2) == 0);

	/* append=1 keeps the existing content and starts at the current
	 * end. */
	CHECK(translate_open("/readwrite/server_check.txt", TFTP_OPEN_WRQ, 1,
			      &f, &start, &err, &msg) == 0);
	CHECK(start == 2);
	CHECK(ramfs_pwrite(f.rf, "!!", 2, start) == 2);
	n = ramfs_pread(f.rf, buf, sizeof(buf), 0);
	CHECK(n == 4 && memcmp(buf, "hi!!", 4) == 0);

	/* RRQ of the seeded imei_sv, then WRQ overwrite -- the two boot-time
	 * writes REPORT.md 8.1 point 4 requires to succeed. */
	CHECK(translate_open("/readwrite/mot_rfs/imei_sv", TFTP_OPEN_RRQ, 0,
			      &f, &start, &err, &msg) == 0);
	CHECK(f.rf->len == 1);
	CHECK(translate_open("/readwrite/mot_rfs/imei_sv", TFTP_OPEN_WRQ, 0,
			      &f, &start, &err, &msg) == 0);
	CHECK(ramfs_pwrite(f.rf, "9", 1, 0) == 1);
	CHECK(f.rf->len == 1);
}

static void test_readwrite_new_name_and_rrq_missing(void)
{
	struct tftp_file f;
	off_t start;
	int err;
	const char *msg;

	/* RRQ of something nobody has written yet: ENOENT, not created. */
	CHECK(translate_open("/readwrite/never-seen.bin", TFTP_OPEN_RRQ, 0,
			      &f, &start, &err, &msg) == -1);
	CHECK(err == TFTP_ERROR_ENOENT);

	/* WRQ creates it; a subsequent RRQ then succeeds. */
	CHECK(translate_open("/readwrite/never-seen.bin", TFTP_OPEN_WRQ, 0,
			      &f, &start, &err, &msg) == 0);
	CHECK(ramfs_pwrite(f.rf, "x", 1, 0) == 1);
	CHECK(translate_open("/readwrite/never-seen.bin", TFTP_OPEN_RRQ, 0,
			      &f, &start, &err, &msg) == 0);
	CHECK(f.rf->len == 1);
}

/* Round 1 review F10: ramfs_open()'s ENAMETOOLONG (name too long for the
 * table's fixed-size key) must surface its own message, distinct from
 * "RAM shadow exhausted" (table full). Within TFTP_MAX_PATH (255) but
 * beyond RAMFS_MAX_NAME (192) once the "/readwrite/" prefix is stripped. */
static void test_readwrite_name_too_long_distinct_message(void)
{
	char path[TFTP_MAX_PATH + 1];
	struct tftp_file f;
	off_t start;
	int err;
	const char *msg;
	int n;

	n = snprintf(path, sizeof(path), "/readwrite/%0200d", 0);
	CHECK(n > 0 && n < (int)sizeof(path) && n <= TFTP_MAX_PATH);

	CHECK(translate_open(path, TFTP_OPEN_WRQ, 0, &f, &start, &err, &msg) == -1);
	CHECK(err == TFTP_ERROR_EACCESS);
	CHECK(strcmp(msg, "path too long for RAM shadow") == 0);
}

static void test_hlos_and_ramdumps_empty_until_written(void)
{
	struct tftp_file f;
	off_t start;
	int err;
	const char *msg;

	CHECK(translate_open("/hlos/tombstone.txt", TFTP_OPEN_RRQ, 0,
			      &f, &start, &err, &msg) == -1);
	CHECK(err == TFTP_ERROR_ENOENT);
	CHECK(translate_open("/hlos/tombstone.txt", TFTP_OPEN_WRQ, 0,
			      &f, &start, &err, &msg) == 0);
	CHECK(ramfs_pwrite(f.rf, "core", 4, 0) == 4);
	CHECK(translate_open("/hlos/tombstone.txt", TFTP_OPEN_RRQ, 0,
			      &f, &start, &err, &msg) == 0);
	CHECK(f.rf->len == 4);

	CHECK(translate_open("/ramdumps/dump.bin", TFTP_OPEN_RRQ, 0,
			      &f, &start, &err, &msg) == -1);
	CHECK(translate_open("/ramdumps/dump.bin", TFTP_OPEN_WRQ, 0,
			      &f, &start, &err, &msg) == 0);

	/* hlos and ramdumps namespaces don't collide on the same relative
	 * name. */
	CHECK(translate_open("/hlos/dump.bin", TFTP_OPEN_RRQ, 0,
			      &f, &start, &err, &msg) == -1);
}

/* Round 2 review F11: /readwrite must not alias /shared or /hlos (or
 * /ramdumps) just because a client's chosen relative path happens to spell
 * out one of those other prefixes' own name. */
static void test_readwrite_does_not_alias_other_namespaces(void)
{
	struct tftp_file f;
	off_t start;
	int err;
	const char *msg;
	char buf[32];
	ssize_t n;

	/* A WRQ to /readwrite/shared/server_info.txt must not touch what
	 * /shared/server_info.txt itself serves. */
	CHECK(translate_open("/readwrite/shared/server_info.txt", TFTP_OPEN_WRQ, 0,
			      &f, &start, &err, &msg) == 0);
	CHECK(ramfs_pwrite(f.rf, "clobbered!", 10, 0) == 10);

	CHECK(translate_open("/shared/server_info.txt", TFTP_OPEN_RRQ, 0,
			      &f, &start, &err, &msg) == 0);
	n = ramfs_pread(f.rf, buf, sizeof(buf), 0);
	CHECK(n == 15 && memcmp(buf, "TFTP_SERVER.1.0", 15) == 0);

	/* /readwrite/hlos/x and /hlos/x must be distinct files. */
	CHECK(translate_open("/readwrite/hlos/x", TFTP_OPEN_WRQ, 0,
			      &f, &start, &err, &msg) == 0);
	CHECK(ramfs_pwrite(f.rf, "rw-side", 7, 0) == 7);

	CHECK(translate_open("/hlos/x", TFTP_OPEN_RRQ, 0,
			      &f, &start, &err, &msg) == -1);
	CHECK(err == TFTP_ERROR_ENOENT);

	CHECK(translate_open("/hlos/x", TFTP_OPEN_WRQ, 0,
			      &f, &start, &err, &msg) == 0);
	CHECK(ramfs_pwrite(f.rf, "hlos-side", 9, 0) == 9);

	CHECK(translate_open("/readwrite/hlos/x", TFTP_OPEN_RRQ, 0,
			      &f, &start, &err, &msg) == 0);
	n = ramfs_pread(f.rf, buf, sizeof(buf), 0);
	CHECK(n == 7 && memcmp(buf, "rw-side", 7) == 0);
}

static void test_ramfs_caps(void)
{
	struct ram_file *rf;
	unsigned char big[RAMFS_MAX_FILE_SIZE];

	ramfs_init();
	memset(big, 0x42, sizeof(big));

	rf = ramfs_open("cap-test");
	CHECK(rf != NULL);
	CHECK(ramfs_pwrite(rf, big, sizeof(big), 0) == (ssize_t)sizeof(big));
	CHECK(rf->len == RAMFS_MAX_FILE_SIZE);
	/* One more byte, for this one file, exceeds the per-file cap. */
	CHECK(ramfs_pwrite(rf, "x", 1, RAMFS_MAX_FILE_SIZE) == -1);
	CHECK(errno == ENOSPC);

	ramfs_truncate(rf);
	CHECK(rf->len == 0);
	CHECK(ramfs_total_size() == 0);

	/* Whole-table cap: four more max-size files exhaust
	 * RAMFS_MAX_TOTAL_SIZE (4 MiB / 1 MiB each) exactly; a fifth must
	 * fail even though each individual file is within its own cap. */
	{
		char name[32];
		int i;
		int ok = 1;

		for (i = 0; i < 4; i++) {
			snprintf(name, sizeof(name), "cap-%d", i);
			rf = ramfs_open(name);
			ok = ok && rf && ramfs_pwrite(rf, big, sizeof(big), 0) == (ssize_t)sizeof(big);
		}
		CHECK(ok);
		CHECK(ramfs_total_size() == RAMFS_MAX_TOTAL_SIZE);

		rf = ramfs_open("cap-overflow");
		CHECK(rf != NULL);
		CHECK(ramfs_pwrite(rf, "x", 1, 0) == -1);
		CHECK(errno == ENOSPC);
	}
}

static void test_ramfs_pread_past_eof(void)
{
	struct ram_file *rf;
	char buf[16];

	ramfs_init();
	rf = ramfs_open("short");
	CHECK(ramfs_pwrite(rf, "abc", 3, 0) == 3);
	CHECK(ramfs_pread(rf, buf, sizeof(buf), 3) == 0);
	CHECK(ramfs_pread(rf, buf, sizeof(buf), 100) == 0);
	CHECK(ramfs_pread(rf, buf, sizeof(buf), 1) == 2 && memcmp(buf, "bc", 2) == 0);
}

int main(void)
{
	char fw_dir[600], seed_dir[600];

	setup_fixtures(fw_dir, sizeof(fw_dir), seed_dir, sizeof(seed_dir));

	test_init_and_seed(fw_dir, seed_dir);
	test_seed_wrong_size_fails_closed(fw_dir);

	/* Re-seed cleanly for the open()-path tests below. */
	ramfs_init();
	CHECK(translate_init(fw_dir) == 0);
	CHECK(translate_seed(seed_dir) == 0);

	test_readonly_firmware();
	test_traversal_and_absolute_escape();
	test_symlink_not_followed();
	test_unknown_prefix_rejected();
	test_shared_server_info();
	test_readwrite_overwrite_and_append();
	test_readwrite_new_name_and_rrq_missing();
	test_readwrite_name_too_long_distinct_message();
	test_hlos_and_ramdumps_empty_until_written();
	test_readwrite_does_not_alias_other_namespaces();
	test_ramfs_caps();
	test_ramfs_pread_past_eof();

	printf("%s: %d/%d checks passed\n", g_failures ? "FAIL" : "PASS",
	       g_tests - g_failures, g_tests);
	return g_failures ? 1 : 0;
}
