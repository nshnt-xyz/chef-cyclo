/*
 * test_settings.c - the system UI settings file: format/parse round trip,
 * every rejection (checksum, version, layout, ranges, size, NUL, final
 * newline), /data ownership (mount point + exact marker), the /run
 * fallback, debounce, exit flush, unwritable storage and /data vanishing.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <pthread.h>
#include <sys/time.h>
#include <time.h>

#include "app.h"
#include "check.h"
#include "fakeloop.h"
#include "settings.h"
#include "sha256.h"
#include "sys.h"
#include "sysstate.h"

static char tmp[256], data[300], run[300];
static int mounted = 1;		/* read by the worker thread: atomics */

/* A level-triggered gate standing for a hung eMMC: while closed, the mount
 * check (the first step of every storage job, on the worker) waits inside
 * it. No tokens: opening releases whoever is inside and lets later jobs
 * through; `inside` tells the test a job really is stuck there. */
static pthread_mutex_t gate_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cv = PTHREAD_COND_INITIALIZER;
static bool gate_closed;
static int inside;

static bool fake_mountpoint(const char *path)
{
	(void)path;
	pthread_mutex_lock(&gate_mu);
	if (gate_closed) {
		inside++;
		pthread_cond_broadcast(&gate_cv);
		while (gate_closed)
			pthread_cond_wait(&gate_cv, &gate_mu);
		inside--;
		pthread_cond_broadcast(&gate_cv);
	}
	pthread_mutex_unlock(&gate_mu);
	return __atomic_load_n(&mounted, __ATOMIC_SEQ_CST);
}

static void set_mounted(int v)
{
	__atomic_store_n(&mounted, v, __ATOMIC_SEQ_CST);
}

static void block(bool on)
{
	pthread_mutex_lock(&gate_mu);
	gate_closed = on;
	pthread_cond_broadcast(&gate_cv);
	pthread_mutex_unlock(&gate_mu);
}

/* Wait until the worker is inside the closed gate (true) or 2 s. */
static bool stuck(void)
{
	struct timespec dl;
	bool r;

	clock_gettime(CLOCK_REALTIME, &dl);
	dl.tv_sec += 2;
	pthread_mutex_lock(&gate_mu);
	while (inside == 0 && pthread_cond_timedwait(&gate_cv, &gate_mu, &dl) == 0)
		;
	r = inside > 0;
	pthread_mutex_unlock(&gate_mu);
	return r;
}

/* ---- syscall log: --wrap=mkdir,fsync,rename,open (the store's I/O) */

static pthread_mutex_t oplog_mu = PTHREAD_MUTEX_INITIALIZER;
static char oplog[64][320];
static int noplog;
static int fail_parent_fsync;	/* fail the next fsync of a directory named v1 */
static int fail_rename;		/* fail every rename of a settings temp file while > 0 */
static int rename_attempts;	/* renames of the settings temp file */
static char seq_log[64];	/* R: a save attempt (rename), S: a free-space callback */

static void seq_add(char c)
{
	size_t n = strlen(seq_log);

	pthread_mutex_lock(&oplog_mu);
	if (n + 1 < sizeof(seq_log)) {
		seq_log[n] = c;
		seq_log[n + 1] = 0;
	}
	pthread_mutex_unlock(&oplog_mu);
}

int __real_mkdir(const char *p, mode_t m);
int __real_fsync(int fd);
int __real_rename(const char *a, const char *b);

static void op(const char *fmt, const char *a, const char *b)
{
	pthread_mutex_lock(&oplog_mu);
	if (noplog < 64)
		snprintf(oplog[noplog++], sizeof(oplog[0]), fmt, a, b);
	pthread_mutex_unlock(&oplog_mu);
}

/* the path relative to tmp, for readable assertions */
static const char *rel(const char *p)
{
	size_t n = strlen(tmp);

	return strncmp(p, tmp, n) ? p : p + n;
}

int __wrap_mkdir(const char *p, mode_t m)
{
	op("mkdir %s%s", rel(p), "");
	return __real_mkdir(p, m);
}

int __wrap_fsync(int fd)
{
	char link[64], path[300];
	ssize_t n;

	snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
	n = readlink(link, path, sizeof(path) - 1);
	path[n < 0 ? 0 : n] = 0;
	op("fsync %s%s", rel(path), "");
	if (n > 3 && !strcmp(path + n - 3, "/v1") && fail_parent_fsync) {
		fail_parent_fsync--;
		errno = EIO;
		return -1;
	}
	return __real_fsync(fd);
}

int __wrap_rename(const char *a, const char *b)
{
	size_t n = strlen(a);

	op("rename %s %s", rel(a), rel(b));
	if (n > 4 && !strcmp(a + n - 4, ".tmp")) {
		rename_attempts++;
		seq_add('R');
		if (fail_rename > 0) {
			errno = EIO;
			return -1;
		}
	}
	return __real_rename(a, b);
}

static void oplog_clear(void)
{
	pthread_mutex_lock(&oplog_mu);
	noplog = 0;
	pthread_mutex_unlock(&oplog_mu);
}

/* ops in order, as one string */
static const char *ops(void)
{
	static char all[4096];

	pthread_mutex_lock(&oplog_mu);
	all[0] = 0;
	for (int i = 0; i < noplog; i++) {
		strncat(all, oplog[i], sizeof(all) - strlen(all) - 2);
		strcat(all, ";");
	}
	pthread_mutex_unlock(&oplog_mu);
	return all;
}

static bool idle(void *arg)
{
	(void)arg;
	return !ui_store_busy();
}

/* real time for the worker; fake LVGL time stands still */
static bool settle(void)
{
	return fakeloop_run_until(idle, NULL, 3000);
}

static long now_ms(void);

/* Wait for cond with a monotonic bound (5 s). drive: also move fake time
 * in 50 ms steps, so deferred work (debounce, probe) happens; without it
 * only completions already under way are collected. Completion is always
 * a condition on generations or state, never "the worker is idle". */
static bool await(bool (*cond)(void *), void *arg, bool drive)
{
	long end = now_ms() + 5000;

	while (now_ms() < end) {
		if (cond(arg))
			return true;
		if (drive)
			fakeloop_advance(50);
		fakeloop_run_for(2);
	}
	return cond(arg);
}

static bool c_saved(void *arg)
{
	(void)arg;
	return ui_store_persisted_gen() >= ui_store_wanted_gen();
}

static bool c_persisted(void *arg)
{
	return ui_store_persisted_gen() >= *(unsigned long *)arg;
}

static bool c_failed(void *arg)
{
	return ui_store_failed_gen() >= *(unsigned long *)arg;
}

static bool c_on_data(void *arg)
{
	(void)arg;
	return ui_store_on_data() && !ui_store_busy();
}

static int nreplaced;

static bool c_replaced(void *arg)
{
	return nreplaced >= *(int *)arg;
}

/* every change saved: persisted >= wanted, driving the debounce */
static bool await_saved(void)
{
	return await(c_saved, NULL, true);
}

static bool await_failed(unsigned long gen)
{
	return await(c_failed, &gen, true);
}

static long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static struct ui_settings replaced;
static struct sys_device_info got_info;
static int info_calls;
static struct sys_usage_result got_usage;
static int usage_calls;

static int stat_calls, boot_calls;

static void on_stat(const struct ui_store_stat_result *st, void *user)
{
	(void)st;
	(void)user;
	stat_calls++;
	seq_add('S');
}

static bool c_stat(void *unused)
{
	(void)unused;
	return stat_calls > 0;
}

static void on_boot(enum sys_time_source source, void *user)
{
	(void)source;
	(void)user;
	boot_calls++;
}

static bool c_boot(void *unused)
{
	(void)unused;
	return boot_calls > 0;
}

static void on_info(const struct sys_device_info *info, void *user)
{
	(void)user;
	got_info = *info;
	info_calls++;
}

static bool c_info(void *unused)
{
	(void)unused;
	return info_calls > 0;
}

static void on_usage(const struct sys_usage_result *usage, void *user)
{
	(void)user;
	got_usage = *usage;
	usage_calls++;
}

static bool c_usage(void *unused)
{
	(void)unused;
	return usage_calls > 0;
}

static void on_replaced(const struct ui_settings *s, void *user)
{
	(void)user;
	replaced = *s;
	nreplaced++;
}

static void write_file(const char *path, const char *s, size_t n)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

	CHECK(fd >= 0);
	CHECK(write(fd, s, n) == (ssize_t)n);
	close(fd);
}

static int read_file(const char *path, char *buf, size_t size)
{
	int fd = open(path, O_RDONLY);
	ssize_t n;

	if (fd < 0)
		return -1;
	n = read(fd, buf, size - 1);
	close(fd);
	buf[n < 0 ? 0 : n] = 0;
	return (int)n;
}

/* body + its sha256 line */
static int seal(const char *body, char *out, size_t size)
{
	char hex[65];

	sha256_hex(body, strlen(body), hex);
	return snprintf(out, size, "%ssha256 %s\n", body, hex);
}

static void test_sha256(void)
{
	char hex[65];

	sha256_hex("", 0, hex);
	CHECK(!strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
	sha256_hex("abc", 3, hex);
	CHECK(!strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
	/* two blocks */
	sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, hex);
	CHECK(!strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
}

static void test_parse(void)
{
	struct ui_settings a, b;
	char buf[512], bad[512];
	const char *why;
	int n;

	ui_settings_defaults(&a);
	CHECK(ui_settings_valid(&a));
	CHECK(a.brightness == 40 && a.theme == UI_THEME_DARK && a.sleep_s == 60);
	CHECK(!strcmp(a.tz, "IST-5:30"));
	CHECK(a.alert_volume == 60 && a.haptics && a.alert_sounds && !a.quiet_paused && a.clock_24h);
	a.brightness = 73;
	a.theme = UI_THEME_LIGHT;
	a.sleep_s = 300;
	snprintf(a.tz, sizeof(a.tz), "UTC0");
	n = ui_settings_format(&a, buf, sizeof(buf));
	CHECK(n > 0);
	CHECK(strstr(buf, "chef-ui-settings 2\nbrightness 73\ntheme light\nsleep 300\ntz UTC0\nalert_volume 60\nhaptics 1\nalert_sounds 1\nquiet_paused 0\nclock_24h 1\nsha256 ") == buf);
	CHECK(ui_settings_parse(buf, (size_t)n, &b, &why) == 0);
	CHECK(ui_settings_equal(&a, &b));
	a.theme = UI_THEME_AUTO;
	a.sleep_s = 0;
	n = ui_settings_format(&a, buf, sizeof(buf));
	CHECK(ui_settings_parse(buf, (size_t)n, &b, &why) == 0 && ui_settings_equal(&a, &b));

	/* one flipped byte in the body: checksum */
	memcpy(bad, buf, (size_t)n);
	bad[30] ^= 1;
	CHECK(ui_settings_parse(bad, (size_t)n, &b, &why) == -1 && !strcmp(why, "checksum mismatch"));
	/* truncated: no final newline / no checksum line */
	CHECK(ui_settings_parse(buf, (size_t)n - 1, &b, &why) == -1 && !strcmp(why, "no final newline"));
	CHECK(ui_settings_parse(buf, 60, &b, &why) == -1);
	/* NUL, empty, oversize */
	memcpy(bad, buf, (size_t)n);
	bad[5] = 0;
	CHECK(ui_settings_parse(bad, (size_t)n, &b, &why) == -1 && !strcmp(why, "NUL bytes"));
	CHECK(ui_settings_parse(buf, 0, &b, &why) == -1);
	CHECK(ui_settings_parse(buf, UI_SETTINGS_MAX_BYTES + 1, &b, &why) == -1 && !strcmp(why, "size"));

	/* correctly sealed but wrong content */
	static const struct { const char *body, *why; } cases[] = {
		{ "chef-ui-settings 3\nbrightness 50\ntheme dark\nsleep 60\ntz UTC0\n", "bad version" },
		{ "chef-ui-settings 1\nbrightness 4\ntheme dark\nsleep 60\ntz UTC0\n", "value out of range" },
		{ "chef-ui-settings 1\nbrightness 101\ntheme dark\nsleep 60\ntz UTC0\n", "bad brightness line" },
		{ "chef-ui-settings 1\nbrightness 050\ntheme dark\nsleep 60\ntz UTC0\n", "bad brightness line" },
		{ "chef-ui-settings 1\nbrightness -5\ntheme dark\nsleep 60\ntz UTC0\n", "bad brightness line" },
		{ "chef-ui-settings 1\nbrightness 50\ntheme blue\nsleep 60\ntz UTC0\n", "bad theme line" },
		{ "chef-ui-settings 1\nbrightness 50\ntheme dark\nsleep 45\ntz UTC0\n", "value out of range" },
		{ "chef-ui-settings 1\nbrightness 50\ntheme dark\nsleep 60\ntz IST -5\n", "value out of range" },
		{ "chef-ui-settings 1\nbrightness 50\ntheme dark\nsleep 60\ntz \n", "value out of range" },
		{ "chef-ui-settings 1\nbrightness 50\ntheme dark\nsleep 60\ntz UTC0\nextra 1\n", "extra lines" },
		{ "chef-ui-settings 1\ntheme dark\nbrightness 50\nsleep 60\ntz UTC0\n", "bad brightness line" },
		{ "chef-ui-settings 1\nbrightness 50\ntheme dark\nsleep 60\n", "bad tz line" },
		{ "chef-ui-settings 2\nbrightness 50\ntheme dark\nsleep 60\ntz UTC0\nalert_volume 101\nhaptics 1\nalert_sounds 1\nquiet_paused 0\nclock_24h 1\n", "bad alert_volume line" },
		{ "chef-ui-settings 2\nbrightness 50\ntheme dark\nsleep 60\ntz UTC0\nalert_volume 0\nhaptics 2\nalert_sounds 1\nquiet_paused 0\nclock_24h 1\n", "bad boolean line" },
		{ "chef-ui-settings 2\nbrightness 50\ntheme dark\nsleep 60\ntz UTC0\nalert_volume 0\nhaptics 1\nalert_sounds 1\nquiet_paused 0\n", "bad boolean line" },
	};
	for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		n = seal(cases[i].body, bad, sizeof(bad));
		CHECK(ui_settings_parse(bad, (size_t)n, &b, &why) == -1);
		if (!why || strcmp(why, cases[i].why))
			fprintf(stderr, "case %u: why=%s want %s\n", i, why ? why : "-", cases[i].why);
		CHECK(why && !strcmp(why, cases[i].why));
	}
	/* Genuine sealed v1 retains existing values and adopts v2 defaults. */
	{
		const char *v1 = "chef-ui-settings 1\nbrightness 73\ntheme light\nsleep 300\ntz UTC0\n";
		int vn = seal(v1, bad, sizeof(bad));

		CHECK(ui_settings_parse(bad, (size_t)vn, &b, &why) == 0);
		CHECK(b.brightness == 73 && b.theme == UI_THEME_LIGHT && b.sleep_s == 300);
		CHECK(!strcmp(b.tz, "UTC0") && b.alert_volume == 60 && b.haptics && b.alert_sounds &&
		      !b.quiet_paused && b.clock_24h);
		n = ui_settings_format(&b, buf, sizeof(buf));
		CHECK(n > 0 && !strncmp(buf, "chef-ui-settings 2\n", 19));
	}
	/* v2 accepts zero volume and all canonical boolean combinations. */
	{
		const char *v2 = "chef-ui-settings 2\nbrightness 50\ntheme dark\nsleep 60\ntz UTC0\n"
			"alert_volume 0\nhaptics 0\nalert_sounds 0\nquiet_paused 1\nclock_24h 0\n";
		int vn = seal(v2, bad, sizeof(bad));

		CHECK(ui_settings_parse(bad, (size_t)vn, &b, &why) == 0);
		CHECK(b.alert_volume == 0 && !b.haptics && !b.alert_sounds && b.quiet_paused && !b.clock_24h);
	}
	CHECK(ui_tz_valid("IST-5:30") && ui_tz_valid("<+0530>-5:30") && !ui_tz_valid("a b"));
	CHECK(ui_brightness_level(100) == 255 && ui_brightness_level(5) == 13 &&
	      ui_brightness_level(70) == 179 && ui_brightness_level(0) == 1);
}

static void setup_dirs(void)
{
	char p[400];

	snprintf(tmp, sizeof(tmp), "/tmp/chef-settings-test.XXXXXX");
	CHECK(mkdtemp(tmp) != NULL);
	snprintf(data, sizeof(data), "%s/data", tmp);
	snprintf(run, sizeof(run), "%s/run", tmp);
	mkdir(data, 0755);
	mkdir(run, 0755);
	snprintf(p, sizeof(p), "%s/v1", data);
	mkdir(p, 0755);
	snprintf(p, sizeof(p), "%s/chef-layout", data);
	write_file(p, "chef-cyclo-data-v1\n", 19);
}

static void test_ours(void)
{
	char p[400], junk[400];
	long t0;

	CHECK(ui_data_ours(data, fake_mountpoint));
	set_mounted(0);
	CHECK(!ui_data_ours(data, fake_mountpoint));
	set_mounted(1);
	snprintf(p, sizeof(p), "%s/chef-layout", data);
	write_file(p, "chef-cyclo-data-v1", 18);
	CHECK(ui_data_ours(data, fake_mountpoint));
	write_file(p, "chef-cyclo-data-v1\n\n\n", 21);
	CHECK(ui_data_ours(data, fake_mountpoint));
	write_file(p, "chef-cyclo-data-v1x\n", 20);
	CHECK(!ui_data_ours(data, fake_mountpoint));
	write_file(p, "chef-cyclo-data-v2\n", 19);
	CHECK(!ui_data_ours(data, fake_mountpoint));
	/* the marker, 46 newlines, then foreign bytes past the old 64 */
	memcpy(junk, "chef-cyclo-data-v1", 18);
	memset(junk + 18, '\n', 46);
	memcpy(junk + 64, "FOREIGN\n", 8);
	write_file(p, junk, 72);
	CHECK(!ui_data_ours(data, fake_mountpoint));
	memset(junk + 18, '\n', 50);
	write_file(p, junk, 68);
	CHECK(!ui_data_ours(data, fake_mountpoint));		/* oversized */
	memcpy(junk, "chef-cyclo-data-v1\0\n", 20);
	write_file(p, junk, 20);
	CHECK(!ui_data_ours(data, fake_mountpoint));
	unlink(p);
	CHECK(!ui_data_ours(data, fake_mountpoint));
	/* impostors: a directory, a FIFO (no writer: must not hang), a link */
	CHECK(mkdir(p, 0755) == 0);
	CHECK(!ui_data_ours(data, fake_mountpoint));
	rmdir(p);
	CHECK(mkfifo(p, 0644) == 0);
	t0 = now_ms();
	CHECK(!ui_data_ours(data, fake_mountpoint));
	CHECK(now_ms() - t0 < 500);
	unlink(p);
	snprintf(junk, sizeof(junk), "%s/real-marker", tmp);
	write_file(junk, "chef-cyclo-data-v1\n", 19);
	CHECK(symlink(junk, p) == 0);
	CHECK(!ui_data_ours(data, fake_mountpoint));
	unlink(p);
	write_file(p, "chef-cyclo-data-v1\n", 19);
	CHECK(ui_data_ours(data, fake_mountpoint));
	/* the real check: a temp dir is not a mount point, / is */
	CHECK(!ui_mountpoint(tmp));
	CHECK(ui_mountpoint("/"));
}

static int read_back(const char *path, struct ui_settings *t)
{
	char buf[1024];
	const char *why;
	int n = read_file(path, buf, sizeof(buf));

	return n > 0 ? ui_settings_parse(buf, (size_t)n, t, &why) : -1;
}

static void test_store(void)
{
	struct ui_store_paths paths = { .data = data, .run = run, .mountpoint = fake_mountpoint,
					.exit_wait_ms = 300, .probe_ms = 500, .load_wait_ms = 300 };
	struct ui_settings s, t;
	char path[400], dir[400], want[1200];
	struct stat st;
	unsigned long gen;
	long t0;

	ui_store_set_paths(&paths);
	ui_store_on_replaced(on_replaced, NULL);
	fakeloop_log_clear();
	CHECK(ui_store_load(&s) == 0);
	{
		struct sys_device_info_request request = { .width = 1080, .height = 2246 };
		strcpy(request.lvgl, "test LVGL");
		ui_store_on_info(on_info, NULL);
		ui_store_info(&request);
		CHECK(await(c_info, NULL, false));
		CHECK(!strcmp(got_info.resolution, "1080 x 2246") && !strcmp(got_info.lvgl, "test LVGL"));
	}
	{
		ui_store_on_usage(on_usage, NULL);
		ui_store_usage();
		CHECK(await(c_usage, NULL, false));
		CHECK(got_usage.known && got_usage.ours && got_usage.complete);
		CHECK(got_usage.total_bytes > 0 && got_usage.free_bytes <= got_usage.total_bytes);
	}
	CHECK(ui_store_on_data());
	snprintf(path, sizeof(path), "%s/v1/ui/settings", data);
	snprintf(dir, sizeof(dir), "%s/v1/ui", data);
	CHECK(!strcmp(ui_store_path(), path));
	CHECK(fakeloop_log_count("no ") == 1);		/* missing: defaults */
	ui_settings_defaults(&t);
	CHECK(ui_settings_equal(&s, &t));

	/* debounce: nothing before 2 s after the last change */
	s.brightness = 55;
	ui_store_changed(&s);
	fakeloop_advance(1500);
	CHECK(!ui_store_busy() && access(path, F_OK) != 0);
	s.brightness = 60;
	ui_store_changed(&s);		/* restarts the 2 s */
	fakeloop_advance(1500);
	CHECK(!ui_store_busy() && access(path, F_OK) != 0);
	CHECK(ui_store_pending());
	oplog_clear();
	CHECK(await_saved());
	CHECK(!ui_store_pending());
	CHECK(read_back(path, &t) == 0 && t.brightness == 60);
	/* first save: mkdir ui, fsync v1, fsync tmp, rename, fsync ui */
	snprintf(want, sizeof(want), "mkdir /data/v1/ui;fsync /data/v1;fsync /data/v1/ui/settings.tmp;"
		 "rename /data/v1/ui/settings.tmp /data/v1/ui/settings;fsync /data/v1/ui;");
	if (strcmp(ops(), want))
		fprintf(stderr, "ops: %s\n", ops());
	CHECK(!strcmp(ops(), want));
	CHECK(stat(dir, &st) == 0 && (st.st_mode & 0777) == 0700);
	CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);
	snprintf(want, sizeof(want), "%s/v1/ui/settings.tmp", data);
	CHECK(access(want, F_OK) != 0);		/* temp renamed away */

	/* a failed parent fsync fails the save; the retry fsyncs it again
	 * although ui exists by then */
	fail_parent_fsync = 1;
	s.brightness = 62;
	ui_store_changed(&s);
	gen = ui_store_wanted_gen();
	CHECK(await_failed(gen));
	CHECK(ui_store_pending() && ui_store_persisted_gen() < gen);
	CHECK(fakeloop_log_count("failed") >= 1);
	CHECK(settle());
	oplog_clear();
	s.brightness = 63;
	ui_store_changed(&s);
	CHECK(await_saved());
	CHECK(!ui_store_pending());
	CHECK(strstr(ops(), "mkdir /data/v1/ui;fsync /data/v1;") == ops());
	CHECK(read_back(path, &t) == 0 && t.brightness == 63);

	/* F1: a failed save blocks nothing. The free-space, usage, device and boot
	 * reads still run (a full /data is when they matter), the failed revision
	 * waits for the next change, and a newer edit is saved first. */
	{
		struct sys_device_info_request ir = { .width = 10, .height = 10 };
		unsigned long g;

		fail_rename = 1000;
		rename_attempts = 0;
		s.brightness = 64;
		ui_store_changed(&s);
		g = ui_store_wanted_gen();
		CHECK(await_failed(g));
		CHECK(settle());
		CHECK(ui_store_pending() && ui_store_failed_gen() >= g);
		stat_calls = info_calls = usage_calls = boot_calls = 0;
		ui_store_on_stat(on_stat, NULL);
		ui_store_on_info(on_info, NULL);
		ui_store_on_usage(on_usage, NULL);
		ui_store_on_time_boot(on_boot, NULL);
		ui_store_stat();
		CHECK(await(c_stat, NULL, false));
		ui_store_info(&ir);
		CHECK(await(c_info, NULL, false));
		ui_store_usage();
		CHECK(await(c_usage, NULL, false));
		ui_store_time_boot(NULL);
		CHECK(await(c_boot, NULL, false));
		/* a new edit is saved before a queued optional job */
		seq_log[0] = 0;
		stat_calls = 0;
		s.brightness = 65;
		ui_store_changed(&s);
		ui_store_stat();			/* queued behind the pending debounce */
		CHECK(await(c_stat, NULL, true));
		CHECK(!strcmp(seq_log, "RSS") || !strcmp(seq_log, "RS"));
		CHECK(seq_log[0] == 'R');
		/* the directory writable again: the next edit saves */
		fail_rename = 0;
		s.brightness = 66;
		ui_store_changed(&s);
		CHECK(await_saved());
		CHECK(!ui_store_pending() && read_back(path, &t) == 0 && t.brightness == 66);
		/* an edit made while a failing save is in flight is retried, not starved */
		CHECK(settle());
		fail_rename = 1000;
		rename_attempts = 0;
		block(true);
		s.brightness = 67;
		ui_store_changed(&s);
		fakeloop_advance(2100);
		CHECK(ui_store_busy() && stuck());
		s.brightness = 68;
		ui_store_changed(&s);
		g = ui_store_wanted_gen();
		block(false);
		CHECK(await_failed(g));
		CHECK(rename_attempts >= 2);
		stat_calls = 0;
		ui_store_stat();
		CHECK(await(c_stat, NULL, true));	/* optional work runs after it settles */
		fail_rename = 0;
		s.brightness = 63;		/* the value the next checks expect saved */
		ui_store_changed(&s);
		CHECK(await_saved());
		CHECK(read_back(path, &t) == 0 && t.brightness == 63);

		/* F11: optional work queued behind a busy worker is dropped when the page is
		 * left, and the other jobs queued for the worker still run */
		CHECK(settle());
		block(true);
		s.brightness = 58;
		ui_store_changed(&s);
		fakeloop_advance(2100);
		CHECK(ui_store_busy() && stuck());
		usage_calls = boot_calls = info_calls = stat_calls = 0;
		ui_store_usage();
		ui_store_time_boot(NULL);
		ui_store_info(&ir);
		ui_store_stat();
		ui_store_usage_cancel();
		ui_store_time_boot_cancel();
		ui_store_info_cancel();
		block(false);
		CHECK(await_saved());
		CHECK(await(c_stat, NULL, false));	/* the cheap read is not cancelled */
		CHECK(settle());
		CHECK(usage_calls == 0 && boot_calls == 0 && info_calls == 0 && stat_calls == 1);
		s.brightness = 63;
		ui_store_changed(&s);
		CHECK(await_saved());
	}

	/* a change back to the saved value cancels the pending save */
	s.brightness = 61;
	ui_store_changed(&s);
	s.brightness = 63;
	ui_store_changed(&s);
	CHECK(!ui_store_pending());
	CHECK(ui_store_flush() == 1);

	/* exit flush writes a pending change at once */
	s.theme = UI_THEME_LIGHT;
	ui_store_changed(&s);
	CHECK(ui_store_flush() == 0);
	CHECK(read_back(path, &t) == 0 && t.theme == UI_THEME_LIGHT);

	/* edits during a blocked save: the UI thread never waits, and the
	 * older completion does not clear the newer change */
	CHECK(settle());
	block(true);
	s.brightness = 70;
	ui_store_changed(&s);
	gen = ui_store_wanted_gen();
	t0 = now_ms();
	fakeloop_advance(2100);
	CHECK(ui_store_busy() && stuck());
	s.brightness = 71;
	ui_store_changed(&s);
	fakeloop_advance(2100);		/* debounce fires while busy */
	s.brightness = 72;
	ui_store_changed(&s);
	CHECK(now_ms() - t0 < 400);	/* timers ran without waiting on storage */
	block(false);
	/* the save of 70 completes (no fake time: 72's debounce cannot run) */
	CHECK(await(c_persisted, &gen, false));
	CHECK(ui_store_persisted_gen() == gen && ui_store_pending());	/* 72 still wanted */
	CHECK(read_back(path, &t) == 0 && t.brightness == 70);
	CHECK(await_saved());
	CHECK(!ui_store_pending() && read_back(path, &t) == 0 && t.brightness == 72);

	/* bounded exit with a hung worker: not reported as saved */
	CHECK(settle());
	block(true);
	s.brightness = 73;
	ui_store_changed(&s);
	gen = ui_store_wanted_gen();
	fakeloop_advance(2100);
	CHECK(ui_store_busy() && stuck());
	fakeloop_log_clear();
	t0 = now_ms();
	CHECK(ui_store_flush() == -ETIMEDOUT);
	CHECK(now_ms() - t0 < 1000);
	CHECK(fakeloop_log_count("still busy") == 1);
	CHECK(ui_store_pending());
	block(false);
	/* the late completion arrives over the eventfd */
	CHECK(await(c_persisted, &gen, false));
	CHECK(!ui_store_pending() && read_back(path, &t) == 0 && t.brightness == 73);
	fakeloop_advance(100);		/* a stale eventfd count is harmless */
	CHECK(!ui_store_busy() && fakeloop_watched() == 1);

	/* unwritable owned storage: kept in memory, logged once, retried on
	 * the next change (not on a timer) and at exit */
	if (geteuid() != 0) {
		chmod(dir, 0500);
		fakeloop_log_clear();
		s.sleep_s = 30;
		ui_store_changed(&s);
		CHECK(await_failed(ui_store_wanted_gen()));
		CHECK(settle() && ui_store_pending());
		oplog_clear();
		fakeloop_advance(10000);
		CHECK(!ui_store_busy() && ops()[0] == 0);	/* no retry loop */
		s.sleep_s = 300;
		ui_store_changed(&s);
		CHECK(await_failed(ui_store_wanted_gen()));
		CHECK(settle());
		CHECK(fakeloop_log_count("failed") == 1);
		CHECK(ui_store_flush() < 0);
		chmod(dir, 0700);
		s.sleep_s = 30;
		ui_store_changed(&s);
		CHECK(await_saved());
		CHECK(!ui_store_pending() && fakeloop_log_count("again") == 1);
		CHECK(read_back(path, &t) == 0 && t.sleep_s == 30);
	}

	/* /data stops being ours: nothing is written under it */
	set_mounted(0);
	read_file(path, want, sizeof(want));
	s.brightness = 99;
	ui_store_changed(&s);
	CHECK(ui_store_flush() == -ENODEV);
	{
		char now[1024];

		read_file(path, now, sizeof(now));
		CHECK(!strcmp(now, want));
	}
	set_mounted(1);
	CHECK(ui_store_flush() == 0);

	/* a corrupt file is ignored (logged, defaults) and replaced on save */
	write_file(path, "chef-ui-settings 1\nbrightness 99\n", 33);
	fakeloop_log_clear();
	CHECK(ui_store_load(&t) == 0);
	ui_settings_defaults(&s);
	CHECK(ui_settings_equal(&t, &s));
	CHECK(fakeloop_log_count("ignoring") == 1);
	t.brightness = 80;
	ui_store_changed(&t);
	CHECK(ui_store_flush() == 0);
	CHECK(read_back(path, &s) == 0 && s.brightness == 80);
	/* a FIFO in its place is ignored without hanging */
	unlink(path);
	CHECK(mkfifo(path, 0600) == 0);
	fakeloop_log_clear();
	t0 = now_ms();
	CHECK(ui_store_load(&t) == 0);
	CHECK(now_ms() - t0 < 300 && fakeloop_log_count("ignoring") == 1);
	unlink(path);
	CHECK(settle());

	/* /data missing at start: /run, then adopted late without edits */
	set_mounted(0);
	fakeloop_log_clear();
	CHECK(ui_store_load(&t) == 0);
	CHECK(!ui_store_on_data());
	snprintf(want, sizeof(want), "%s/ui/settings", run);
	CHECK(!strcmp(ui_store_path(), want));
	CHECK(fakeloop_log_count("is not ours") == 1);
	s = t;
	s.brightness = 33;
	s.theme = UI_THEME_LIGHT;
	{
		char buf[512];
		int n = ui_settings_format(&s, buf, sizeof(buf));

		write_file(path, buf, (size_t)n);	/* what /data holds */
	}
	fakeloop_advance(600);		/* probe: still not ours */
	CHECK(settle() && !ui_store_on_data() && nreplaced == 0);
	set_mounted(1);
	CHECK(await(c_on_data, NULL, true));
	CHECK(ui_store_on_data() && !strcmp(ui_store_path(), path));
	CHECK(nreplaced == 1 && replaced.brightness == 33 && replaced.theme == UI_THEME_LIGHT);
	CHECK(!ui_store_pending());
	fakeloop_advance(2000);		/* the probe stopped */
	CHECK(!ui_store_busy());

	/* ... and with edits this boot: RAM wins and is written to /data */
	set_mounted(0);
	CHECK(ui_store_load(&t) == 0 && !ui_store_on_data());
	t.brightness = 44;
	ui_store_changed(&t);
	CHECK(await_saved());		/* saved to /run */
	snprintf(want, sizeof(want), "%s/ui/settings", run);
	CHECK(read_back(want, &s) == 0 && s.brightness == 44);
	CHECK(stat(want, &st) == 0 && (st.st_mode & 0777) == 0600);
	set_mounted(1);
	CHECK(await(c_on_data, NULL, true));
	CHECK(ui_store_on_data() && nreplaced == 1);
	CHECK(ui_store_pending());	/* this boot's edit, not yet on /data */
	CHECK(await_saved());
	CHECK(read_back(path, &s) == 0 && s.brightness == 44 && s.theme == UI_THEME_DARK);

	/* a first read slower than load_wait_ms: defaults now, the file later */
	CHECK(settle());
	block(true);
	nreplaced = 0;
	fakeloop_log_clear();
	t0 = now_ms();
	CHECK(ui_store_load(&t) == 0);
	CHECK(stuck());
	CHECK(now_ms() - t0 < 700);
	CHECK(fakeloop_log_count("did not answer") == 1);
	ui_settings_defaults(&s);
	CHECK(ui_settings_equal(&t, &s));
	block(false);
	{
		int one = 1;

		CHECK(await(c_replaced, &one, false));
	}
	CHECK(nreplaced == 1 && replaced.brightness == 44 && ui_store_on_data());

	/* bounded waits use the monotonic clock */
	CHECK(ui_store_wait_clock() == CLOCK_MONOTONIC);
}

static bool storage_known(void *arg)
{
	(void)arg;
	return sys_state.storage.known;
}

static bool storage_usage_known(void *arg)
{
	(void)arg;
	return sys_state.storage.usage_known;
}

static int64_t usage_before_refresh;
static bool storage_usage_refreshed(void *arg)
{
	(void)arg;
	return sys_state.storage.usage_known &&
		sys_state.storage.usage_updated_mono_ms != usage_before_refresh;
}

static int64_t pf_now(void)
{
	return now_ms();
}

static bool pf_on(void)
{
	return true;
}

static int pf_set(bool on)
{
	(void)on;
	return 0;
}

static int pf_bright(int level)
{
	(void)level;
	return 0;
}

static int nstat;

static void count_stat(unsigned what, void *user)
{
	(void)user;
	if (what & SYS_STORAGE)
		nstat++;
}

static void *unblock_later(void *arg)
{
	usleep(150000);
	block(false);
	return arg;
}

/* Exit with a free-space read out, another queued and an edit pending:
 * the flush drains the read, drops the queued one and saves the edit with
 * one job at a time. A reload while the worker is hung is refused. */
static void test_queue(void)
{
	char path[400];
	struct ui_settings s, t;
	pthread_t th;

	CHECK(settle());
	set_mounted(1);
	CHECK(ui_store_load(&s) == 0 && ui_store_on_data());
	snprintf(path, sizeof(path), "%s/v1/ui/settings", data);
	sys_on_change(count_stat, NULL);
	nstat = 0;
	sys_state.storage.known = false;
	CHECK(settle());
	block(true);
	sys_storage_refresh();		/* out, blocked */
	CHECK(ui_store_busy() && stuck());
	sys_storage_refresh();		/* queued behind it */
	s.brightness = 91;
	ui_store_changed(&s);		/* pending */
	pthread_create(&th, NULL, unblock_later, NULL);
	CHECK(ui_store_flush() == 0);
	pthread_join(th, NULL);
	CHECK(!ui_store_busy() && !ui_store_pending());
	CHECK(read_back(path, &t) == 0 && t.brightness == 91);
	CHECK(nstat == 1);		/* the first read only */
	fakeloop_run_for(300);
	CHECK(!ui_store_busy() && nstat == 1);	/* the queued one was dropped */

	/* reload while hung: refused, old state kept */
	CHECK(settle());
	block(true);
	sys_storage_refresh();
	CHECK(ui_store_busy() && stuck());
	fakeloop_log_clear();
	CHECK(ui_store_load(&t) == -EBUSY);
	CHECK(fakeloop_log_count("reload refused") == 1);
	CHECK(ui_store_on_data() && !strcmp(ui_store_path(), path));
	block(false);
	CHECK(settle());
	CHECK(ui_store_load(&t) == 0 && t.brightness == 91);
	sys_on_change(NULL, NULL);
}

/* Usage requests coalesce behind a blocked worker, never hold the UI loop,
 * and a due settings save runs before optional usage work. */
static void test_usage_schedule(void)
{
	struct ui_settings s, saved;
	CHECK(settle());
	CHECK(ui_store_load(&s) == 0);
	ui_store_on_usage(on_usage, NULL);
	usage_calls = 0;
	block(true);
	ui_store_usage();
	CHECK(ui_store_busy() && stuck());
	for (int i = 0; i < 8; i++) ui_store_usage();
	s.brightness = s.brightness == 73 ? 74 : 73;
	ui_store_changed(&s);
	fakeloop_advance(2000);
	CHECK(ui_store_busy() && ui_store_pending());
	block(false);
	CHECK(await_saved());
	CHECK(read_back(ui_store_path(), &saved) == 0 && saved.brightness == s.brightness);
	CHECK(await(c_usage, NULL, true));
	CHECK(usage_calls <= 2); /* active scan plus one coalesced request */
	CHECK(settle());
}

/* Hub storage: read on the worker, never on the UI thread. */
static void test_storage(void)
{
	struct app_platform pf = { pf_now, pf_on, pf_set, pf_bright };
	struct ui_settings set;
	lv_obj_t *content;
	long t0;

	CHECK(settle());
	sys_storage_usage_init();
	set_mounted(1);
	memset(&sys_state.storage, 0, sizeof(sys_state.storage));
	sys_storage_refresh();
	CHECK(fakeloop_run_until(storage_known, NULL, 2000));
	CHECK(sys_state.storage.ours && sys_state.storage.free_bytes > 0);
	sys_storage_usage_want(true);
	CHECK(fakeloop_run_until(storage_usage_known, NULL, 2000));
	CHECK(sys_state.storage.usage_complete && !sys_state.storage.usage_maps &&
	      !sys_state.storage.usage_rides && !sys_state.storage.usage_routes);
	int64_t usage_sample = sys_state.storage.usage_updated_mono_ms;
	sys_storage_usage_want(true);
	CHECK(settle());
	CHECK(sys_state.storage.usage_updated_mono_ms == usage_sample); /* cached under 30 s */
	usage_before_refresh = usage_sample;
	sys_state.storage.usage_updated_mono_ms -= 30000;
	sys_storage_refresh();
	CHECK(fakeloop_run_until(storage_usage_refreshed, NULL, 2000));
	CHECK(sys_state.storage.usage_updated_mono_ms != usage_sample);
	sys_storage_usage_want(false);
	set_mounted(0);
	sys_storage_refresh();
	CHECK(settle());
	CHECK(sys_state.storage.known && !sys_state.storage.ours && sys_state.storage.free_bytes == 0);
	set_mounted(1);

	/* the app's Back (hub entry refreshes storage) with storage hung */
	fakeloop_display(1080, 2246);
	content = lv_obj_create(lv_screen_active());
	ui_settings_defaults(&set);
	app_init(&pf, &set, lv_screen_active(), content);
	CHECK(settle());
	app_open(APP_PAGE_DISPLAY, "Display");
	CHECK(settle());
	block(true);
	sys_state.storage.known = false;
	t0 = now_ms();
	CHECK(app_back());
	CHECK(app_on_button("power.short") == false);	/* hub: platform default */
	fakeloop_advance(100);
	CHECK(now_ms() - t0 < 300);
	CHECK(ui_store_busy() && stuck() && !sys_state.storage.known);
	app_open(APP_PAGE_DISPLAY, "Display");
	CHECK(app_back());		/* a second request while busy is queued */
	block(false);
	CHECK(fakeloop_run_until(storage_known, NULL, 2000));
	CHECK(settle());
	CHECK(sys_state.storage.ours);
}

int main(void)
{
	char cmd[700];

	fakeloop_init(true);
	test_sha256();
	test_parse();
	setup_dirs();
	test_ours();
	test_store();
	test_storage();
	test_queue();
	test_usage_schedule();
	snprintf(cmd, sizeof(cmd), "chmod -R u+w '%s'; rm -rf '%s'", tmp, tmp);
	if (system(cmd))
		fprintf(stderr, "cleanup of %s failed\n", tmp);
	return check_report("test-settings");
}
