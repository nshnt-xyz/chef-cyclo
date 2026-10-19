/*
 * test_settings_file.c - a stand-alone settings reader built from
 * sys/settings_file.c and sys/sha256.c alone (no store, no LVGL include
 * path; Makefile: test-settings-file), as another program would read the
 * file: which file is in use (/data ours or not), files written by
 * ui_settings_format, v1/v2 upgrades, a bad sha256, over-long and odd
 * files giving an error (the caller's defaults), and the reader never
 * creating anything. The parser's line rules are in test-settings.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "check.h"
#include "settings_file.h"
#include "sha256.h"

static char g_data[256], g_run[256];
static bool g_mounted;

static bool fake_mountpoint(const char *path)
{
	return g_mounted && !strcmp(path, g_data);
}

static void put(const char *path, const char *buf, size_t len)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);

	CHECK(fd >= 0 && write(fd, buf, len) == (ssize_t)len);
	if (fd >= 0)
		close(fd);
}

static void put_settings(const char *path, const struct ui_settings *s)
{
	char buf[512];
	int n = ui_settings_format(s, buf, sizeof(buf));

	CHECK(n > 0);
	put(path, buf, (size_t)n);
}

static int seal(const char *body, char *out, size_t size)
{
	char hex[65];

	sha256_hex(body, strlen(body), hex);
	return snprintf(out, size, "%ssha256 %s\n", body, hex);
}

static int entries(const char *dir)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	int n = 0;

	if (!d)
		return -1;
	while ((e = readdir(d)))
		n++;
	closedir(d);
	return n;
}

static int read_now(struct ui_settings *s, bool *ours, const char **why)
{
	return ui_settings_read(g_data, g_run, fake_mountpoint, s, ours, why);
}

int main(void)
{
	char tmp[] = "/tmp/chef-settings-file-XXXXXX", path[300], data_file[300], run_file[300];
	char buf[UI_SETTINGS_MAX_BYTES + 64], big[300];
	struct ui_settings a, b, def;
	const char *why;
	bool ours;
	int n, before;
	struct stat st;

	CHECK(mkdtemp(tmp) != NULL);
	snprintf(g_data, sizeof(g_data), "%s/data", tmp);
	snprintf(g_run, sizeof(g_run), "%s/run", tmp);
	CHECK(mkdir(g_data, 0755) == 0 && mkdir(g_run, 0755) == 0);
	ui_settings_defaults(&def);

	/* the paths, and a root too long for them */
	CHECK(ui_settings_file_path(data_file, sizeof(data_file), g_data, g_run, true) == 0);
	CHECK(ui_settings_file_path(run_file, sizeof(run_file), g_data, g_run, false) == 0);
	snprintf(path, sizeof(path), "%s/v1/ui/settings", g_data);
	CHECK(!strcmp(data_file, path));
	snprintf(path, sizeof(path), "%s/ui/settings", g_run);
	CHECK(!strcmp(run_file, path));
	CHECK(ui_settings_file_path(path, sizeof(path), NULL, NULL, true) == 0 &&
	      !strcmp(path, "/data/v1/ui/settings"));
	CHECK(ui_settings_file_path(path, sizeof(path), NULL, NULL, false) == 0 &&
	      !strcmp(path, "/run/ui/settings"));
	memset(big, 'x', sizeof(big) - 1);
	big[0] = '/';
	big[sizeof(big) - 1] = 0;
	CHECK(ui_settings_file_path(path, sizeof(path), big, g_run, true) == -ENAMETOOLONG);

	/* nothing yet: -ENOENT, /data not ours, and no directory made */
	before = entries(g_run);
	CHECK(read_now(&b, &ours, &why) == -ENOENT && !ours && !why);
	CHECK(entries(g_run) == before);
	snprintf(path, sizeof(path), "%s/ui", g_run);
	CHECK(stat(path, &st) == -1 && errno == ENOENT);

	/* on /run: what the writer formats is read back */
	CHECK(mkdir(path, 0700) == 0);
	ui_settings_defaults(&a);
	a.brightness = 73;
	a.theme = UI_THEME_AUTO;
	a.sleep_s = 300;
	snprintf(a.tz, sizeof(a.tz), "UTC0");
	a.alert_volume = 15;
	a.haptics = false;
	a.quiet_paused = true;
	a.auto_brightness = true;
	a.auto_offset = -20;
	put_settings(run_file, &a);
	before = entries(path);
	CHECK(read_now(&b, &ours, &why) == 0 && !ours && ui_settings_equal(&a, &b));
	CHECK(entries(path) == before);

	/* /data a mount point but not ours (no marker, a wrong one): still /run */
	g_mounted = true;
	CHECK(read_now(&b, &ours, &why) == 0 && !ours && ui_settings_equal(&a, &b));
	snprintf(path, sizeof(path), "%s/chef-layout", g_data);
	put(path, "chef-cyclo-data-v2\n", 19);
	CHECK(read_now(&b, &ours, &why) == 0 && !ours && ui_settings_equal(&a, &b));

	/* /data ours: its file, even when missing (the /run one is not used) */
	put(path, "chef-cyclo-data-v1\n", 19);
	CHECK(read_now(&b, &ours, &why) == -ENOENT && ours);
	snprintf(path, sizeof(path), "%s/v1", g_data);
	CHECK(mkdir(path, 0755) == 0);
	snprintf(path, sizeof(path), "%s/v1/ui", g_data);
	CHECK(mkdir(path, 0700) == 0);
	b = a;
	b.brightness = 55;
	b.theme = UI_THEME_LIGHT;
	put_settings(data_file, &b);
	CHECK(read_now(&a, &ours, &why) == 0 && ours && a.brightness == 55 && a.theme == UI_THEME_LIGHT);
	/* not a mount point any more: back to /run, re-checked on every read */
	g_mounted = false;
	CHECK(read_now(&a, &ours, &why) == 0 && !ours && a.brightness == 73);
	g_mounted = true;

	/* v1 and v2 files: their fields, the newer ones at the defaults */
	n = seal("chef-ui-settings 1\nbrightness 80\ntheme light\nsleep 30\ntz UTC0\n", buf, sizeof(buf));
	put(data_file, buf, (size_t)n);
	CHECK(read_now(&b, &ours, &why) == 0 && ours);
	CHECK(b.brightness == 80 && b.theme == UI_THEME_LIGHT && b.sleep_s == 30 && !strcmp(b.tz, "UTC0"));
	CHECK(b.alert_volume == def.alert_volume && b.haptics == def.haptics &&
	      b.alert_sounds == def.alert_sounds && b.quiet_paused == def.quiet_paused &&
	      b.clock_24h == def.clock_24h && b.auto_brightness == def.auto_brightness &&
	      b.auto_offset == def.auto_offset);
	n = seal("chef-ui-settings 2\nbrightness 50\ntheme dark\nsleep 0\ntz IST-5:30\n"
		 "alert_volume 0\nhaptics 0\nalert_sounds 0\nquiet_paused 1\nclock_24h 0\n", buf, sizeof(buf));
	put(data_file, buf, (size_t)n);
	CHECK(read_now(&b, &ours, &why) == 0);
	CHECK(b.sleep_s == 0 && b.alert_volume == 0 && !b.haptics && !b.alert_sounds &&
	      b.quiet_paused && !b.clock_24h && !b.auto_brightness && b.auto_offset == 0);

	/* a bad sha256: an error with its reason (the caller uses the defaults) */
	ui_settings_defaults(&a);
	n = ui_settings_format(&a, buf, sizeof(buf));
	CHECK(n > 0);
	buf[n - 2] = buf[n - 2] == '0' ? '1' : '0';
	put(data_file, buf, (size_t)n);
	CHECK(read_now(&b, &ours, &why) == -EINVAL && why && !strcmp(why, "checksum mismatch"));
	/* a changed value under the old sha256 */
	n = ui_settings_format(&a, buf, sizeof(buf));
	buf[strlen("chef-ui-settings 3\nbrightness ")] = '9';
	put(data_file, buf, (size_t)n);
	CHECK(read_now(&b, &ours, &why) == -EINVAL && why && !strcmp(why, "checksum mismatch"));

	/* over-long: one byte over the limit is "size", at the limit it is parsed */
	memset(buf, '\n', sizeof(buf));
	put(data_file, buf, UI_SETTINGS_MAX_BYTES + 1);
	CHECK(read_now(&b, &ours, &why) == -EINVAL && why && !strcmp(why, "size"));
	put(data_file, buf, UI_SETTINGS_MAX_BYTES);
	CHECK(read_now(&b, &ours, &why) == -EINVAL && why && strcmp(why, "size") != 0);

	/* not a regular file, or a link: an error, never followed or blocked on */
	CHECK(unlink(data_file) == 0 && mkfifo(data_file, 0600) == 0);
	CHECK(read_now(&b, &ours, &why) == -EINVAL && !why);
	CHECK(unlink(data_file) == 0 && symlink(run_file, data_file) == 0);
	CHECK(read_now(&b, &ours, &why) == -ELOOP);
	CHECK(unlink(data_file) == 0 && mkdir(data_file, 0700) == 0);
	CHECK(read_now(&b, &ours, &why) == -EINVAL && !why);
	CHECK(rmdir(data_file) == 0);

	snprintf(path, sizeof(path), "rm -rf '%s'", tmp);
	CHECK(system(path) == 0);
	return check_report("test-settings-file");
}
