/*
 * settings_file.c - the UI settings file itself (settings_file.h): the
 * defaults, the checks, parse and format with the sha256 line, the
 * ownership check of /data and which file is in use. No LVGL, no threads:
 * chef-settings' store (settings.c) runs it on its worker, and another
 * program can read the file with it alone.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "settings_file.h"
#include "sha256.h"

#define MAGIC1 "chef-ui-settings 1"
#define MAGIC2 "chef-ui-settings 2"
#define MAGIC3 "chef-ui-settings 3"
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
	s->auto_brightness = false;
	s->auto_offset = 0;
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
	       s->alert_volume >= 0 && s->alert_volume <= 100 &&
	       s->auto_offset >= UI_AUTO_OFFSET_MIN && s->auto_offset <= UI_AUTO_OFFSET_MAX;
}

bool ui_settings_equal(const struct ui_settings *a, const struct ui_settings *b)
{
	return a->brightness == b->brightness && a->theme == b->theme && a->sleep_s == b->sleep_s &&
	       strcmp(a->tz, b->tz) == 0 && a->alert_volume == b->alert_volume &&
	       a->haptics == b->haptics && a->alert_sounds == b->alert_sounds &&
	       a->quiet_paused == b->quiet_paused && a->clock_24h == b->clock_24h &&
	       a->auto_brightness == b->auto_brightness && a->auto_offset == b->auto_offset;
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

/* Canonical small signed decimal: optional "-", no "+", no leading zero, no "-0". */
static bool parse_int(const char *s, int min, int max, int *out)
{
	bool neg = s[0] == '-';
	int v;

	if (!parse_uint(s + neg, neg ? -min : max, &v) || (neg && v == 0))
		return false;
	v = neg ? -v : v;
	if (v < min || v > max)
		return false;
	*out = v;
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
	bool v3 = !strcmp(line, MAGIC3);
	bool v2 = v3 || !strcmp(line, MAGIC2);
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
	if (v3) {
		int value;

		if (!next_line(&p, end, line, sizeof(line)) || strncmp(line, "auto_brightness ", 16) ||
		    !parse_uint(line + 16, 1, &value)) {
			*why = "bad auto_brightness line";
			return -1;
		}
		s.auto_brightness = value != 0;
		if (!next_line(&p, end, line, sizeof(line)) || strncmp(line, "auto_offset ", 12) ||
		    !parse_int(line + 12, UI_AUTO_OFFSET_MIN, UI_AUTO_OFFSET_MAX, &s.auto_offset)) {
			*why = "bad auto_offset line";
			return -1;
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
	n = snprintf(buf, size, MAGIC3 "\nbrightness %d\ntheme %s\nsleep %d\ntz %s\n"
		     "alert_volume %d\nhaptics %d\nalert_sounds %d\nquiet_paused %d\nclock_24h %d\n"
		     "auto_brightness %d\nauto_offset %d\n",
		     s->brightness, theme_name(s->theme), s->sleep_s, s->tz, s->alert_volume,
		     s->haptics, s->alert_sounds, s->quiet_paused, s->clock_24h,
		     s->auto_brightness, s->auto_offset);
	if (n < 0 || (size_t)n >= size)
		return -1;
	sha256_hex(buf, (size_t)n, hex);
	m = snprintf(buf + n, size - (size_t)n, "sha256 %s\n", hex);
	if (m < 0 || (size_t)m >= size - (size_t)n)
		return -1;
	return n + m;
}

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

int ui_settings_file_path(char *buf, size_t size, const char *data, const char *run, bool on_data)
{
	int n = on_data ? snprintf(buf, size, "%s/v1/ui/settings", data ? data : "/data")
			: snprintf(buf, size, "%s/ui/settings", run ? run : "/run");

	return n < 0 || (size_t)n >= size ? -ENAMETOOLONG : 0;
}

int ui_settings_read(const char *data, const char *run, bool (*mountpoint)(const char *path),
		     struct ui_settings *out, bool *ours, const char **why)
{
	char path[300], buf[UI_SETTINGS_MAX_BYTES + 1];
	size_t len = 0;
	int err;

	*why = NULL;
	*ours = ui_data_ours(data ? data : "/data", mountpoint);
	err = ui_settings_file_path(path, sizeof(path), data, run, *ours);
	if (!err)
		err = read_small(path, buf, sizeof(buf), &len);
	if (err == -EFBIG) {
		*why = "size";
		return -EINVAL;
	}
	if (!err && ui_settings_parse(buf, len, out, why))
		return -EINVAL;
	return err;
}
