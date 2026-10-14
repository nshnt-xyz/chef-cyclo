/*
 * settings.c - persistent UI settings (see settings.h).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include "chefui.h"
#include "settings.h"
#include "sha256.h"
#include "storage_usage.h"

#define MAGIC1 "chef-ui-settings 1"
#define MAGIC2 "chef-ui-settings 2"
#define LAYOUT_MARKER "chef-cyclo-data-v1"

void ui_settings_defaults(struct ui_settings *s)
{
	memset(s, 0, sizeof(*s));
	s->brightness = 40;	/* about the platform's default backlight 96/255 */
	s->theme = UI_THEME_DARK;
	s->sleep_s = 60;
	snprintf(s->tz, sizeof(s->tz), "IST-5:30");
	s->alert_volume = 60;
	s->haptics = true;
	s->alert_sounds = true;
	s->quiet_paused = false;
	s->clock_24h = true;
}

bool ui_tz_valid(const char *tz)
{
	size_t n = strlen(tz);

	if (n == 0 || n > UI_TZ_MAX)
		return false;
	for (size_t i = 0; i < n; i++)
		if (!strchr("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789:+-,./<>",
			    tz[i]))
			return false;
	return true;
}

static bool sleep_valid(int s)
{
	return s == 0 || s == 30 || s == 60 || s == 300;
}

bool ui_settings_valid(const struct ui_settings *s)
{
	return s->brightness >= UI_BRIGHTNESS_MIN && s->brightness <= UI_BRIGHTNESS_MAX &&
	       (s->theme == UI_THEME_DARK || s->theme == UI_THEME_LIGHT || s->theme == UI_THEME_AUTO) &&
	       sleep_valid(s->sleep_s) && ui_tz_valid(s->tz) &&
	       s->alert_volume >= 0 && s->alert_volume <= 100;
}

bool ui_settings_equal(const struct ui_settings *a, const struct ui_settings *b)
{
	return a->brightness == b->brightness && a->theme == b->theme && a->sleep_s == b->sleep_s &&
	       strcmp(a->tz, b->tz) == 0 && a->alert_volume == b->alert_volume &&
	       a->haptics == b->haptics && a->alert_sounds == b->alert_sounds &&
	       a->quiet_paused == b->quiet_paused && a->clock_24h == b->clock_24h;
}

int ui_brightness_level(int percent)
{
	int l = (percent * 255 + 50) / 100;

	return l < 1 ? 1 : l > 255 ? 255 : l;
}

static const char *theme_name(enum ui_theme_mode t)
{
	return t == UI_THEME_LIGHT ? "light" : t == UI_THEME_AUTO ? "auto" : "dark";
}

/* Canonical small non-negative decimal (no sign, no leading zero). */
static bool parse_uint(const char *s, int max, int *out)
{
	long v = 0;

	if (!*s || (s[0] == '0' && s[1]))
		return false;
	for (; *s; s++) {
		if (*s < '0' || *s > '9')
			return false;
		v = v * 10 + (*s - '0');
		if (v > max)
			return false;
	}
	*out = (int)v;
	return true;
}

/* Next line of [*p, end) into line (without the newline). */
static bool next_line(const char **p, const char *end, char *line, size_t size)
{
	const char *nl = memchr(*p, '\n', (size_t)(end - *p));
	size_t n;

	if (!nl)
		return false;
	n = (size_t)(nl - *p);
	if (n >= size)
		return false;
	memcpy(line, *p, n);
	line[n] = 0;
	*p = nl + 1;
	return true;
}

int ui_settings_parse(const char *buf, size_t len, struct ui_settings *out, const char **why)
{
	struct ui_settings s;
	const char *p = buf, *end = buf + len, *last;
	char line[128], hex[65];
	size_t body;

	*why = NULL;
	if (len == 0 || len > UI_SETTINGS_MAX_BYTES) {
		*why = "size";
		return -1;
	}
	if (memchr(buf, 0, len)) {
		*why = "NUL bytes";
		return -1;
	}
	if (buf[len - 1] != '\n') {
		*why = "no final newline";
		return -1;
	}
	/* the checksum line covers everything before it */
	last = buf + len - 1;
	while (last > buf && last[-1] != '\n')
		last--;
	body = (size_t)(last - buf);
	if (strncmp(last, "sha256 ", 7) || (size_t)(end - last) != 7 + 64 + 1) {
		*why = "no checksum line";
		return -1;
	}
	sha256_hex(buf, body, hex);
	if (memcmp(last + 7, hex, 64)) {
		*why = "checksum mismatch";
		return -1;
	}
	end = last;

	ui_settings_defaults(&s);
	if (!next_line(&p, end, line, sizeof(line))) {
		*why = "bad version";
		return -1;
	}
	bool v2 = !strcmp(line, MAGIC2);
	if (!v2 && strcmp(line, MAGIC1)) {
		*why = "bad version";
		return -1;
	}
	if (!next_line(&p, end, line, sizeof(line)) || strncmp(line, "brightness ", 11) ||
	    !parse_uint(line + 11, UI_BRIGHTNESS_MAX, &s.brightness)) {
		*why = "bad brightness line";
		return -1;
	}
	if (!next_line(&p, end, line, sizeof(line)) || strncmp(line, "theme ", 6)) {
		*why = "bad theme line";
		return -1;
	}
	if (!strcmp(line + 6, "dark"))
		s.theme = UI_THEME_DARK;
	else if (!strcmp(line + 6, "light"))
		s.theme = UI_THEME_LIGHT;
	else if (!strcmp(line + 6, "auto"))
		s.theme = UI_THEME_AUTO;
	else {
		*why = "bad theme line";
		return -1;
	}
	if (!next_line(&p, end, line, sizeof(line)) || strncmp(line, "sleep ", 6) ||
	    !parse_uint(line + 6, 3600, &s.sleep_s)) {
		*why = "bad sleep line";
		return -1;
	}
	if (!next_line(&p, end, line, sizeof(line)) || strncmp(line, "tz ", 3) ||
	    strlen(line + 3) > UI_TZ_MAX) {
		*why = "bad tz line";
		return -1;
	}
	memcpy(s.tz, line + 3, strlen(line + 3) + 1);
	if (v2) {
		int value;
		bool *flags[] = { &s.haptics, &s.alert_sounds, &s.quiet_paused, &s.clock_24h };
		const char *keys[] = { "haptics ", "alert_sounds ", "quiet_paused ", "clock_24h " };
		if (!next_line(&p, end, line, sizeof(line)) || strncmp(line, "alert_volume ", 13) ||
		    !parse_uint(line + 13, 100, &s.alert_volume)) {
			*why = "bad alert_volume line";
			return -1;
		}
		for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); i++) {
			if (!next_line(&p, end, line, sizeof(line)) || strncmp(line, keys[i], strlen(keys[i])) ||
			    !parse_uint(line + strlen(keys[i]), 1, &value)) {
				*why = "bad boolean line";
				return -1;
			}
			*flags[i] = value != 0;
		}
	}
	if (p != end) {
		*why = "extra lines";
		return -1;
	}
	if (!ui_settings_valid(&s)) {
		*why = "value out of range";
		return -1;
	}
	*out = s;
	return 0;
}

int ui_settings_format(const struct ui_settings *s, char *buf, size_t size)
{
	char hex[65];
	int n, m;

	if (!ui_settings_valid(s))
		return -1;
	n = snprintf(buf, size, MAGIC2 "\nbrightness %d\ntheme %s\nsleep %d\ntz %s\n"
		     "alert_volume %d\nhaptics %d\nalert_sounds %d\nquiet_paused %d\nclock_24h %d\n",
		     s->brightness, theme_name(s->theme), s->sleep_s, s->tz, s->alert_volume,
		     s->haptics, s->alert_sounds, s->quiet_paused, s->clock_24h);
	if (n < 0 || (size_t)n >= size)
		return -1;
	sha256_hex(buf, (size_t)n, hex);
	m = snprintf(buf + n, size - (size_t)n, "sha256 %s\n", hex);
	if (m < 0 || (size_t)m >= size - (size_t)n)
		return -1;
	return n + m;
}

/* ------------------------------------------------------------- storage */

bool ui_mountpoint(const char *path)
{
	struct stat a, b;
	char parent[256];

	if (snprintf(parent, sizeof(parent), "%s/..", path) >= (int)sizeof(parent))
		return false;
	if (stat(path, &a) || stat(parent, &b) || !S_ISDIR(a.st_mode))
		return false;
	return a.st_dev != b.st_dev || a.st_ino == b.st_ino;
}

/* Open a small regular file without following links or blocking on a
 * FIFO or device, read all of it (at most size - 1 bytes). 0, -errno,
 * -EFBIG when bigger, -EINVAL when not a regular file. */
static int read_small(const char *path, char *buf, size_t size, size_t *len)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	struct stat st;
	size_t got = 0;
	int r = 0;

	if (fd < 0)
		return -errno;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode)) {
		close(fd);
		return -EINVAL;
	}
	if (st.st_size >= (off_t)size) {
		close(fd);
		return -EFBIG;
	}
	for (;;) {
		ssize_t n = read(fd, buf + got, size - 1 - got);

		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0) {
			r = -errno;
			break;
		}
		if (n == 0)
			break;
		got += (size_t)n;
		if (got == size - 1) {
			char c;

			/* grew since fstat */
			if (read(fd, &c, 1) > 0)
				r = -EFBIG;
			break;
		}
	}
	close(fd);
	buf[got] = 0;
	*len = got;
	return r;
}

bool ui_data_ours(const char *data, bool (*mountpoint)(const char *path))
{
	char path[256], buf[64];
	size_t n = 0;

	if (!(mountpoint ? mountpoint : ui_mountpoint)(data))
		return false;
	if (snprintf(path, sizeof(path), "%s/chef-layout", data) >= (int)sizeof(path))
		return false;
	if (read_small(path, buf, sizeof(buf), &n))
		return false;
	/* exactly the marker, as chef-state's $(cat) compare: only trailing
	 * newlines may follow it, and the whole file is under 64 bytes */
	while (n > 0 && buf[n - 1] == '\n')
		n--;
	return n == strlen(LAYOUT_MARKER) && !memcmp(buf, LAYOUT_MARKER, n) &&
	       !memchr(buf, 0, n);
}

/*
 * The store. Everything that touches /data or /run (the ownership check,
 * reads, writes, fsyncs) runs on one persistence worker thread, so a slow
 * or hung eMMC never stalls input or rendering. The worker gets an
 * immutable job (kind, settings with their revision, paths) and returns an
 * immutable result over an eventfd; LVGL calls and all store state stay on
 * the UI thread, which applies results. Revisions: every change bumps
 * `want_rev` (the wanted generation); a successful save (rename and
 * directory fsync done) raises `saved_rev` (the persisted generation) to
 * the generation it wrote, so a completion for older settings never
 * clears a newer change, and a failed save only records `failed_rev`.
 * Generations only grow, across reloads too; waiters (exit flush, tests)
 * wait for persisted >= wanted, never for an idle worker.
 */
enum job_kind { JOB_NONE, JOB_LOAD, JOB_SAVE, JOB_STAT, JOB_TIME_BOOT, JOB_INFO, JOB_USAGE };

struct job {
	enum job_kind kind;
	unsigned long rev;
	struct ui_settings s;
	bool on_data;			/* SAVE: under data (else run) */
	char data[200], run[200];
	char time_boot_path[256];
	bool (*mountpoint)(const char *path);
	struct sys_device_info_request info_request;
};

struct result {
	enum job_kind kind;
	unsigned long rev;
	int err;			/* SAVE: 0 or -errno; LOAD: read status */
	bool ours;			/* LOAD, STAT: /data is ours */
	uint64_t free_bytes;		/* STAT: statvfs of an owned /data (err 0) */
	uint64_t total_bytes;
	uint64_t root_dev, root_ino;
	struct sys_device_info info;
	struct sys_usage_result usage;
	enum sys_time_source boot_source;
	const char *why;		/* LOAD: parse rejection */
	struct ui_settings s;		/* LOAD: what was read (err == 0); SAVE: written */
};

static struct {
	struct ui_store_paths p;
	bool loaded, on_data, adopted, initial_done;
	char path[300];
	struct ui_settings want, saved;
	unsigned long want_rev, saved_rev, failed_rev, edits_since_load;
	int last_err;
	lv_timer_t *timer, *probe;
	ui_store_replace_cb replace_cb;
	void *replace_user;
	ui_store_stat_cb stat_cb;
	void *stat_user;
	bool stat_wanted;
	ui_store_info_cb info_cb;
	void *info_user;
	struct sys_device_info_request info_request;
	bool info_wanted;
	ui_store_usage_cb usage_cb;
	void *usage_user;
	bool usage_wanted;
	ui_store_time_boot_cb time_boot_cb;
	void *time_boot_user;
	char time_boot_path[256];
	bool time_boot_wanted;

	/* worker */
	bool started;
	pthread_t th;
	pthread_mutex_t mu;
	pthread_cond_t cv;		/* on CLOCK_MONOTONIC: waits are bounded */
	clockid_t cv_clock;
	int efd;
	struct job job;			/* submitted, worker takes it */
	bool queued, running, done;
	struct result res;
	bool busy;			/* a job is out (UI thread view) */
	bool quiesce;			/* flush/reload: no follow-up jobs */
} St = { .efd = -1, .mu = PTHREAD_MUTEX_INITIALIZER };

void ui_store_set_paths(const struct ui_store_paths *p)
{
	St.p = *p;
}

void ui_store_on_replaced(ui_store_replace_cb cb, void *user)
{
	St.replace_cb = cb;
	St.replace_user = user;
}

const char *ui_store_path(void)
{
	return St.path;
}

bool ui_store_on_data(void)
{
	return St.on_data;
}

bool ui_store_pending(void)
{
	return St.loaded && St.saved_rev < St.want_rev;
}

/* A save that should run before optional work: pending and not the
 * revision that already failed (that one waits for the next change). */
static bool save_due(void)
{
	return ui_store_pending() && St.failed_rev < St.want_rev;
}

void ui_store_usage_cancel(void)
{
	St.usage_wanted = false;
}

void ui_store_info_cancel(void)
{
	St.info_wanted = false;
}

void ui_store_time_boot_cancel(void)
{
	St.time_boot_wanted = false;
}

void ui_store_on_stat(ui_store_stat_cb cb, void *user)
{
	St.stat_cb = cb;
	St.stat_user = user;
}

void ui_store_on_info(ui_store_info_cb cb, void *user)
{
	St.info_cb = cb;
	St.info_user = user;
}

void ui_store_on_usage(ui_store_usage_cb cb, void *user)
{
	St.usage_cb = cb;
	St.usage_user = user;
}

void ui_store_on_time_boot(ui_store_time_boot_cb cb, void *user)
{
	St.time_boot_cb = cb;
	St.time_boot_user = user;
}

int ui_store_wait_clock(void)
{
	return St.started ? (int)St.cv_clock : -1;
}

unsigned long ui_store_wanted_gen(void)
{
	return St.want_rev;
}

unsigned long ui_store_persisted_gen(void)
{
	return St.saved_rev;
}

unsigned long ui_store_failed_gen(void)
{
	return St.failed_rev;
}

bool ui_store_busy(void)
{
	return St.busy;
}

static const char *data_root(void)
{
	return St.p.data ? St.p.data : "/data";
}

static const char *run_root(void)
{
	return St.p.run ? St.p.run : "/run";
}

static void set_path(void)
{
	if (St.on_data)
		snprintf(St.path, sizeof(St.path), "%s/v1/ui/settings", data_root());
	else
		snprintf(St.path, sizeof(St.path), "%s/ui/settings", run_root());
}

/* ------------------------------------------------------- worker side */

static int fsync_dir(const char *dir)
{
	int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC), r = 0;

	if (fd < 0)
		return -errno;
	if (fsync(fd))
		r = -errno;
	close(fd);
	return r;
}

static int write_all(int fd, const char *p, size_t n)
{
	while (n) {
		ssize_t w = write(fd, p, n);

		if (w < 0 && errno == EINTR)
			continue;
		if (w < 0)
			return -errno;
		p += w;
		n -= (size_t)w;
	}
	return 0;
}

/* mkdir 0700 (or keep it), make the entry durable in its parent (every
 * time: a previous fsync may have failed), tmp 0600 in the same
 * directory, fsync, rename, fsync the directory; under a lock on run. */
static int do_save(const struct job *j)
{
	char buf[512], parent[260], dir[280], path[300], tmp[300], lock[260];
	int n, fd, r, lfd;

	n = ui_settings_format(&j->s, buf, sizeof(buf));
	if (n < 0)
		return -EINVAL;
	if (j->on_data) {
		if (!ui_data_ours(j->data, j->mountpoint))
			return -ENODEV;	/* /data went away: never write under it */
		snprintf(parent, sizeof(parent), "%s/v1", j->data);
	} else {
		snprintf(parent, sizeof(parent), "%s", j->run);
	}
	snprintf(dir, sizeof(dir), "%s/ui", parent);
	snprintf(path, sizeof(path), "%s/settings", dir);
	snprintf(tmp, sizeof(tmp), "%s/settings.tmp", dir);
	snprintf(lock, sizeof(lock), "%s/ui-settings.lock", j->run);
	lfd = open(lock, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (lfd < 0)
		return -errno;
	if (flock(lfd, LOCK_EX | LOCK_NB)) {
		r = -errno;
		close(lfd);
		return r;
	}
	/* /data/v1 belongs to chef-storage: never created here */
	if (mkdir(dir, 0700) && errno != EEXIST) {
		r = -errno;
		goto out;
	}
	r = fsync_dir(parent);
	if (r)
		goto out;
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0) {
		r = -errno;
		goto out;
	}
	r = write_all(fd, buf, (size_t)n);
	if (!r && fsync(fd))
		r = -errno;
	if (close(fd) && !r)
		r = -errno;
	if (!r && rename(tmp, path))
		r = -errno;
	if (r)
		unlink(tmp);
	else
		r = fsync_dir(dir);
out:
	close(lfd);	/* releases the lock, after the last write */
	return r;
}

static void do_load(const struct job *j, struct result *res)
{
	char path[300], buf[UI_SETTINGS_MAX_BYTES + 1];
	size_t len = 0;

	res->ours = ui_data_ours(j->data, j->mountpoint);
	if (res->ours)
		snprintf(path, sizeof(path), "%s/v1/ui/settings", j->data);
	else
		snprintf(path, sizeof(path), "%s/ui/settings", j->run);
	res->err = read_small(path, buf, sizeof(buf), &len);
	if (res->err == -EFBIG) {
		res->err = -EINVAL;
		res->why = "size";
	} else if (!res->err && ui_settings_parse(buf, len, &res->s, &res->why)) {
		res->err = -EINVAL;
	}
}

static void do_stat(const struct job *j, struct result *res)
{
	struct statvfs v;
	struct stat st, end;
	int fd;

	res->ours = ui_data_ours(j->data, j->mountpoint);
	if (!res->ours)
		return;
	fd = open(j->data, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		res->err = -errno;
	else if (fstat(fd, &st) || fstatvfs(fd, &v))
		res->err = -errno;
	else if (!v.f_frsize || (uint64_t)v.f_blocks > UINT64_MAX / v.f_frsize ||
		 (uint64_t)v.f_bavail > UINT64_MAX / v.f_frsize)
		res->err = -EOVERFLOW;
	else {
		res->root_dev = (uint64_t)st.st_dev;
		res->root_ino = (uint64_t)st.st_ino;
		res->total_bytes = (uint64_t)v.f_blocks * v.f_frsize;
		res->free_bytes = (uint64_t)v.f_bavail * v.f_frsize;
		if (fstat(fd, &end) || end.st_dev != st.st_dev || end.st_ino != st.st_ino ||
		    !(j->mountpoint ? j->mountpoint(j->data) : ui_mountpoint(j->data)))
			res->err = -ENODEV;
	}
	if (fd >= 0) close(fd);
}

static void *worker(void *arg)
{
	(void)arg;
	pthread_setname_np(pthread_self(), "ui-store");
	for (;;) {
		struct job j;
		struct result res;
		uint64_t one = 1;

		pthread_mutex_lock(&St.mu);
		while (!St.queued)
			pthread_cond_wait(&St.cv, &St.mu);
		j = St.job;
		St.queued = false;
		St.running = true;
		pthread_mutex_unlock(&St.mu);

		memset(&res, 0, sizeof(res));
		res.kind = j.kind;
		res.rev = j.rev;
		if (j.kind == JOB_SAVE) {
			res.err = do_save(&j);
			res.s = j.s;
		} else if (j.kind == JOB_STAT) {
			do_stat(&j, &res);
		} else if (j.kind == JOB_INFO) {
			res.err = sys_device_info_collect(&j.info_request, &res.info);
		} else if (j.kind == JOB_TIME_BOOT) {
			res.boot_source = sys_time_read_boot(j.time_boot_path);
		} else if (j.kind == JOB_USAGE) {
			sys_storage_usage_scan(j.data, j.mountpoint, &res.usage);
		}
		else
			do_load(&j, &res);

		pthread_mutex_lock(&St.mu);
		St.res = res;
		St.running = false;
		St.done = true;
		pthread_cond_broadcast(&St.cv);
		pthread_mutex_unlock(&St.mu);
		(void)!write(St.efd, &one, sizeof(one));
	}
	return NULL;
}

/* --------------------------------------------------------- UI side */

static void handle_result(const struct result *r);
static void schedule_save(void);

/* One job at a time: false (nothing sent) while one is out. */
static bool submit(enum job_kind kind)
{
	struct job j;

	if (St.busy)
		return false;
	memset(&j, 0, sizeof(j));
	j.kind = kind;
	j.rev = St.want_rev;
	j.s = St.want;
	j.on_data = St.on_data;
	snprintf(j.data, sizeof(j.data), "%s", data_root());
	snprintf(j.run, sizeof(j.run), "%s", run_root());
	j.mountpoint = St.p.mountpoint;
	j.info_request = St.info_request;
	snprintf(j.time_boot_path, sizeof(j.time_boot_path), "%s", St.time_boot_path[0] ? St.time_boot_path : "/run/chef-time");
	pthread_mutex_lock(&St.mu);
	St.job = j;
	St.queued = true;
	St.done = false;
	pthread_cond_broadcast(&St.cv);
	pthread_mutex_unlock(&St.mu);
	St.busy = true;
	return true;
}

/* Take a finished result (if any) and apply it. */
static bool collect(void)
{
	struct result r;
	bool have;

	pthread_mutex_lock(&St.mu);
	have = St.done;
	if (have) {
		r = St.res;
		St.done = false;
	}
	pthread_mutex_unlock(&St.mu);
	if (!have)
		return false;
	St.busy = false;
	handle_result(&r);
	if (St.quiesce)
		return true;	/* flush/reload drive the worker themselves */
	/* an edit made while the worker was busy still needs its save; a
	 * failed save waits for the next change (or exit) instead */
	if (save_due() && !St.busy && !St.timer)
		schedule_save();
	if (St.stat_wanted && !St.busy && !save_due() && !St.timer) {
		St.stat_wanted = false;
		submit(JOB_STAT);
	} else if (St.time_boot_wanted && !St.busy && !save_due() && !St.timer) {
		St.time_boot_wanted = false;
		submit(JOB_TIME_BOOT);
	} else if (St.info_wanted && !St.busy && !save_due() && !St.timer) {
		St.info_wanted = false;
		submit(JOB_INFO);
	} else if (St.usage_wanted && !St.busy && !save_due() && !St.timer) {
		St.usage_wanted = false;
		submit(JOB_USAGE);
	}
	return true;
}

static void efd_readable(int fd, short revents, void *user)
{
	uint64_t n;

	(void)revents;
	(void)user;
	/* one read drains any number of completions (eventfd counter) */
	(void)!read(fd, &n, sizeof(n));
	collect();
}

/* An absolute deadline ms from now on the condition variable's clock
 * (CLOCK_MONOTONIC: a wall clock step cannot stretch a wait). */
static void deadline(struct timespec *dl, int ms)
{
	clock_gettime(St.cv_clock, dl);
	dl->tv_sec += ms / 1000;
	dl->tv_nsec += (long)(ms % 1000) * 1000000;
	if (dl->tv_nsec >= 1000000000) {
		dl->tv_sec++;
		dl->tv_nsec -= 1000000000;
	}
}

/* Wait at most ms for the outstanding job to finish (not collected). */
static bool wait_done(int ms)
{
	struct timespec dl;
	bool done;
	int r = 0;

	deadline(&dl, ms);
	pthread_mutex_lock(&St.mu);
	while (!St.done && r == 0)
		r = pthread_cond_timedwait(&St.cv, &St.mu, &dl);
	done = St.done;
	pthread_mutex_unlock(&St.mu);
	return done;
}

/* Wait (bounded) for the outstanding job and apply it. true when done. */
static bool wait_job(int ms)
{
	wait_done(ms);
	return collect();
}

/* Quiesced: let the job that is out finish (bounded), start nothing new.
 * true when the worker is idle. */
static bool drain(int ms)
{
	if (St.busy)
		wait_job(ms);
	return !St.busy;
}

static int exit_wait_ms(void)
{
	return St.p.exit_wait_ms > 0 ? St.p.exit_wait_ms : UI_SETTINGS_EXIT_WAIT_MS;
}

static void debounce_fired(lv_timer_t *t)
{
	(void)t;
	St.timer = NULL;	/* one-shot: LVGL deletes it */
	if (!St.busy && ui_store_pending())
		submit(JOB_SAVE);
	/* busy: the completion resubmits if still pending */
}

static void schedule_save(void)
{
	if (!St.timer) {
		St.timer = lv_timer_create(debounce_fired, UI_SETTINGS_DEBOUNCE_MS, NULL);
		lv_timer_set_repeat_count(St.timer, 1);
	} else {
		lv_timer_reset(St.timer);
	}
}

static void probe_fired(lv_timer_t *t)
{
	(void)t;
	if (!St.on_data && !St.busy)
		submit(JOB_LOAD);
}

static void stop_probe(void)
{
	if (St.probe) {
		lv_timer_delete(St.probe);
		St.probe = NULL;
	}
}

static void log_load(const char *path, const struct result *r)
{
	if (r->err == -ENOENT)
		chefui_log("settings: no %s yet; defaults", path);
	else if (r->err == -EINVAL && r->why)
		chefui_log("settings: ignoring %s: %s", path, r->why);
	else if (r->err)
		chefui_log("settings: ignoring %s: %s", path, strerror(-r->err));
}

static void handle_result(const struct result *r)
{
	if (r->kind == JOB_STAT) {
		if (St.stat_cb) {
			struct ui_store_stat_result stat = {
				.known = r->err == 0, .ours = r->ours,
				.total_bytes = r->ours && !r->err ? r->total_bytes : 0,
				.free_bytes = r->ours && !r->err ? r->free_bytes : 0,
				.root_dev = r->root_dev, .root_ino = r->root_ino,
			};
			St.stat_cb(&stat, St.stat_user);
		}
		return;
	}
	if (r->kind == JOB_INFO) {
		if (!r->err && St.info_cb)
			St.info_cb(&r->info, St.info_user);
		return;
	}
	if (r->kind == JOB_TIME_BOOT) {
		if (St.time_boot_cb)
			St.time_boot_cb(r->boot_source, St.time_boot_user);
		return;
	}
	if (r->kind == JOB_USAGE) {
		if (St.usage_cb)
			St.usage_cb(&r->usage, St.usage_user);
		return;
	}
	if (r->kind == JOB_SAVE) {
		if (r->err) {
			/* Kept in memory; the next change or exit retries.
			 * Each kind of failure is logged once. */
			if (r->err != St.last_err)
				chefui_log("settings: save to %s failed: %s; keeping the change in memory",
					   St.path, strerror(-r->err));
			St.last_err = r->err;
			if (r->rev > St.failed_rev)
				St.failed_rev = r->rev;
			return;
		}
		if (St.last_err)
			chefui_log("settings: saved to %s again", St.path);
		St.last_err = 0;
		if (r->rev > St.saved_rev) {
			St.saved_rev = r->rev;
			St.saved = r->s;
		}
		return;
	}
	if (!St.initial_done) {
		/* the first read, finished after ui_store_load() gave up waiting */
		St.initial_done = true;
		St.on_data = r->ours;
		set_path();
		if (St.on_data)
			stop_probe();
		log_load(St.path, r);
		if (!St.edits_since_load && !r->err) {
			St.want = St.saved = r->s;
			St.saved_rev = ++St.want_rev;
			St.adopted = true;
			if (St.replace_cb)
				St.replace_cb(&St.want, St.replace_user);
		} else if (St.edits_since_load) {
			St.want_rev++;		/* write this boot's edits there */
		}
		return;
	}
	/* a probe while in RAM: /data may have become ours */
	if (St.on_data || !r->ours)
		return;
	St.on_data = true;
	set_path();
	stop_probe();
	if (St.edits_since_load) {
		/* this boot's edits are newer than anything on /data */
		chefui_log("settings: %s is ours now; writing this boot's settings to %s",
			   data_root(), St.path);
		St.want_rev++;		/* not on /data yet */
		schedule_save();
	} else if (!r->err) {
		chefui_log("settings: %s is ours now; using %s", data_root(), St.path);
		St.want = St.saved = r->s;
		St.saved_rev = ++St.want_rev;
		St.adopted = true;
		if (St.replace_cb)
			St.replace_cb(&St.want, St.replace_user);
	} else {
		log_load(St.path, r);
	}
}

int ui_store_load(struct ui_settings *out)
{
	struct result r;
	int e;

	ui_settings_defaults(out);
	if (!St.started) {
		pthread_condattr_t ca;

		pthread_condattr_init(&ca);
		St.cv_clock = pthread_condattr_setclock(&ca, CLOCK_MONOTONIC) ? CLOCK_REALTIME
			: CLOCK_MONOTONIC;
		pthread_cond_init(&St.cv, &ca);
		pthread_condattr_destroy(&ca);
		St.efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
		if (St.efd < 0)
			return -errno;
		{
			sigset_t all, old;

			/* The worker never takes a signal: SIGUSR1 (signalfd)
			 * and SIGTERM/SIGINT/SIGHUP (chefui's handler) stay with
			 * the UI thread. The thread inherits this mask. */
			sigfillset(&all);
			pthread_sigmask(SIG_SETMASK, &all, &old);
			e = pthread_create(&St.th, NULL, worker, NULL);
			pthread_sigmask(SIG_SETMASK, &old, NULL);
		}
		if (e) {
			close(St.efd);
			St.efd = -1;
			return -e;
		}
		St.started = true;
		chefui_watch_fd(St.efd, POLLIN, efd_readable, NULL);
	}
	/* a job from an earlier load still out: let it finish first, or
	 * refuse (the old state stays) rather than share the result slot */
	St.quiesce = true;
	if (!drain(exit_wait_ms())) {
		St.quiesce = false;
		chefui_log("settings: storage busy; reload refused");
		return -EBUSY;
	}
	St.quiesce = false;
	St.stat_wanted = false;
	St.info_wanted = false;
	St.usage_wanted = false;
	St.time_boot_wanted = false;
	St.loaded = true;
	St.want = St.saved = *out;
	St.saved_rev = St.want_rev;	/* generations keep growing */
	St.edits_since_load = 0;
	St.on_data = false;
	St.last_err = 0;
	St.initial_done = false;
	St.adopted = false;
	set_path();
	/* The first read is waited for briefly; if storage is that slow the
	 * defaults stand and the probe below adopts the file later. */
	submit(JOB_LOAD);
	wait_done(St.p.load_wait_ms > 0 ? St.p.load_wait_ms : UI_SETTINGS_LOAD_WAIT_MS);
	pthread_mutex_lock(&St.mu);
	if (St.done) {
		r = St.res;
		St.done = false;
		pthread_mutex_unlock(&St.mu);
		St.busy = false;
		St.initial_done = true;
		St.on_data = r.ours;
		set_path();
		if (!r.ours)
			chefui_log("settings: %s is not ours; settings live in %s until reboot",
				   data_root(), St.path);
		log_load(St.path, &r);
		if (!r.err)
			*out = St.want = St.saved = r.s;
	} else {
		pthread_mutex_unlock(&St.mu);
		chefui_log("settings: storage did not answer in time; defaults until it does");
	}
	if (!St.on_data) {
		int ms = St.p.probe_ms > 0 ? St.p.probe_ms : UI_SETTINGS_PROBE_MS;

		/* a busy load still in flight is a probe too */
		stop_probe();
		St.probe = lv_timer_create(probe_fired, (uint32_t)ms, NULL);
	}
	return 0;
}

void ui_store_changed(const struct ui_settings *s)
{
	if (!St.loaded || ui_settings_equal(s, &St.want))
		return;
	St.want = *s;
	St.want_rev++;
	St.edits_since_load++;
	if (ui_settings_equal(&St.want, &St.saved) && !St.busy) {
		/* back to what is on disk: nothing to write */
		St.saved_rev = St.want_rev;
		if (St.timer) {
			lv_timer_delete(St.timer);
			St.timer = NULL;
		}
		return;
	}
	schedule_save();
}

void ui_store_stat(void)
{
	if (!St.started)
		return;
	if (St.busy || St.quiesce || save_due() || St.timer)
		St.stat_wanted = true;	/* after the job that is out */
	else
		submit(JOB_STAT);
}

void ui_store_info(const struct sys_device_info_request *request)
{
	if (!St.started || !request || request->width <= 0 || request->height <= 0)
		return;
	St.info_request = *request;
	if (St.busy || St.quiesce || save_due() || St.timer)
		St.info_wanted = true;
	else
		submit(JOB_INFO);
}

#ifdef CHEFUI_TEST
static unsigned test_usage_requests;

unsigned ui_store_test_usage_requests(void)
{
	return test_usage_requests;
}
#endif

void ui_store_usage(void)
{
#ifdef CHEFUI_TEST
	test_usage_requests++;
#endif
	if (!St.started)
		return;
	if (St.busy || St.quiesce || save_due() || St.timer)
		St.usage_wanted = true;
	else
		submit(JOB_USAGE);
}

void ui_store_time_boot(const char *path)
{
	if (!St.started)
		return;
	snprintf(St.time_boot_path, sizeof(St.time_boot_path), "%s", path && *path ? path : "/run/chef-time");
	if (St.busy || St.quiesce || save_due() || St.timer)
		St.time_boot_wanted = true;
	else
		submit(JOB_TIME_BOOT);
}

int ui_store_flush(void)
{
	int ms = exit_wait_ms(), r;

	if (St.timer) {
		lv_timer_delete(St.timer);
		St.timer = NULL;
	}
	if (!St.loaded)
		return 1;
	/* Exit: nothing but this save. A queued free-space read is dropped,
	 * and results arriving now start no follow-up job. */
	St.quiesce = true;
	St.stat_wanted = false;
	if (!drain(ms)) {
		chefui_log("settings: storage still busy after %d ms; %s may be stale", ms, St.path);
		r = -ETIMEDOUT;
	} else if (!ui_store_pending()) {
		r = 1;
	} else if (!submit(JOB_SAVE) || !drain(ms)) {
		chefui_log("settings: save to %s did not finish in %d ms", St.path, ms);
		r = -ETIMEDOUT;
	} else {
		r = ui_store_pending() ? (St.last_err ? St.last_err : -EIO) : 0;
	}
	St.quiesce = false;
	St.stat_wanted = false;	/* one asked for by a callback meanwhile */
	St.info_wanted = false;
	St.usage_wanted = false;
	St.time_boot_wanted = false;
	return r;
}
