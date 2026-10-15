/*
 * prompt.c - the guided-test prompt overlay (see prompt.h).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#include "chefui.h"
#include "fonts.h"
#include "prompt.h"
#include "theme.h"
#include "widgets.h"

static struct {
	char dir[256], path[300];
	int ifd;
	int64_t (*now_ms)(void);
	uid_t uid;
	lv_obj_t *box, *title, *count, *text;
	lv_timer_t *timer;
	bool shown, has_until;
	int64_t until;
} P = { .ifd = -1 };

/* ------------------------------------------------------------- parsing */

static void copy_ascii(char *dst, size_t n, const char *src, size_t len)
{
	size_t k = 0;

	for (size_t i = 0; i < len && k + 1 < n; i++) {
		unsigned char c = (unsigned char)src[i];

		dst[k++] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
	}
	dst[k] = 0;
}

void prompt_parse(const char *buf, size_t len, struct prompt_data *out)
{
	const char *p = buf, *end = buf + len;

	memset(out, 0, sizeof(*out));
	while (p < end) {
		const char *nl = memchr(p, '\n', (size_t)(end - p));
		size_t n = nl ? (size_t)(nl - p) : (size_t)(end - p);

		if (n && p[n - 1] == '\r')
			n--;
		if (n > 6 && !strncmp(p, "title ", 6)) {
			copy_ascii(out->title, sizeof(out->title), p + 6, n - 6);
		} else if (n > 5 && !strncmp(p, "text ", 5)) {
			copy_ascii(out->text, sizeof(out->text), p + 5, n - 5);
		} else if (n > 6 && !strncmp(p, "until ", 6)) {
			char num[24], *e;
			long long v;

			copy_ascii(num, sizeof(num), p + 6, n - 6);
			v = strtoll(num, &e, 10);
			if (e != num && !*e && v >= 0) {
				out->has_until = true;
				out->until = v;
			}
		}
		p = nl ? nl + 1 : end;
	}
}

/* ------------------------------------------------------------------ UI */

static void count_text(char *b, size_t n)
{
	int64_t left = P.until - P.now_ms();

	snprintf(b, n, "%d s", left <= 0 ? 0 : (int)((left + 999) / 1000));
}

static void count_update(void)
{
	char b[16];

	if (!P.has_until)
		return;
	count_text(b, sizeof(b));
	if (strcmp(lv_label_get_text(P.count), b))
		lv_label_set_text(P.count, b);
}

static void timer_cb(lv_timer_t *t)
{
	(void)t;
	count_update();
	if (P.until - P.now_ms() <= 0 && P.timer) {	/* "0 s" stays; nothing keeps ticking */
		lv_timer_delete(P.timer);
		P.timer = NULL;
	}
}

static void box_create(void)
{
	lv_obj_t *row;

	P.box = kit_box(lv_layer_top());
	lv_obj_add_style(P.box, &kit_st.screen, 0);	/* solid ground */
	lv_obj_add_style(P.box, &kit_st.card, 0);	/* 3 px accent frame, radius 40 */
	lv_obj_set_width(P.box, 1080 - 80);
	lv_obj_set_height(P.box, LV_SIZE_CONTENT);
	lv_obj_align(P.box, LV_ALIGN_BOTTOM_MID, 0, -90);
	lv_obj_set_style_pad_all(P.box, 36, 0);
	lv_obj_set_style_pad_row(P.box, 20, 0);
	lv_obj_set_flex_flow(P.box, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_clickable(P.box, false);
	lv_obj_set_scrollable(P.box, false);
	row = kit_box(P.box);
	lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_clickable(row, false);
	lv_obj_set_scrollable(row, false);
	P.title = lv_label_create(row);
	lv_obj_set_style_text_font(P.title, &chefkit_bc600_60, 0);
	kit_set_ink(P.title, KIT_INK_ACCT);
	lv_obj_align(P.title, LV_ALIGN_TOP_LEFT, 0, 0);
	P.count = lv_label_create(row);
	lv_obj_set_style_text_font(P.count, &chefkit_bc600_60, 0);
	kit_set_ink(P.count, KIT_INK_FG);
	lv_obj_align(P.count, LV_ALIGN_TOP_RIGHT, 0, 0);
	lv_obj_set_height(row, kit_css_line(&chefkit_bc600_60));
	P.text = lv_label_create(P.box);
	lv_obj_set_width(P.text, lv_pct(100));
	lv_label_set_long_mode(P.text, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_font(P.text, &chefkit_b500_64, 0);
	kit_set_ink(P.text, KIT_INK_FG);
	lv_obj_set_clickable(P.title, false);
	lv_obj_set_clickable(P.count, false);
	lv_obj_set_clickable(P.text, false);
}

static void hide(void)
{
	if (P.timer) {
		lv_timer_delete(P.timer);
		P.timer = NULL;
	}
	if (P.box)
		lv_obj_set_hidden(P.box, true);
	P.shown = false;
	P.has_until = false;
}

static void show(const struct prompt_data *d)
{
	if (!P.box)
		box_create();
	lv_label_set_text(P.title, d->title);
	lv_label_set_text(P.text, d->text);
	P.has_until = d->has_until;
	P.until = d->until;
	if (d->has_until) {
		lv_obj_set_hidden(P.count, false);
		count_update();
		if (!P.timer)
			P.timer = lv_timer_create(timer_cb, 1000, NULL);
	} else {
		lv_obj_set_hidden(P.count, true);
		if (P.timer) {
			lv_timer_delete(P.timer);
			P.timer = NULL;
		}
	}
	lv_obj_set_hidden(P.box, false);
	P.shown = true;
}

/* ---------------------------------------------------------------- file */

void prompt_reload(void)
{
	char buf[PROMPT_MAX_BYTES + 1];
	struct prompt_data d;
	struct stat st;
	ssize_t n;
	int fd;

	fd = open(P.path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	if (fd < 0) {
		if (errno != ENOENT)
			chefui_log("prompt ignored: %s: %s", P.path, strerror(errno));
		hide();
		return;
	}
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != P.uid || (st.st_mode & 022) ||
	    st.st_size > PROMPT_MAX_BYTES) {
		close(fd);
		chefui_log("prompt ignored: %s is not a trusted small file", P.path);
		hide();
		return;
	}
	n = read(fd, buf, sizeof(buf));
	close(fd);
	if (n < 0 || n > PROMPT_MAX_BYTES) {
		chefui_log("prompt ignored: %s unreadable or too long", P.path);
		hide();
		return;
	}
	prompt_parse(buf, (size_t)n, &d);
	show(&d);
}

static void inotify_io(int fd, short revents, void *user)
{
	char buf[4096] __attribute__((aligned(8)));
	bool ours = false;
	ssize_t n;

	(void)revents;
	(void)user;
	while ((n = read(fd, buf, sizeof(buf))) > 0) {
		for (char *p = buf; p < buf + n;) {
			const struct inotify_event *e = (const struct inotify_event *)p;

			if ((e->mask & IN_Q_OVERFLOW) || (e->len && !strcmp(e->name, "prompt")))
				ours = true;
			p += sizeof(*e) + e->len;
		}
	}
	if (ours)
		prompt_reload();
}

int prompt_start(const char *dir, int64_t (*now_ms)(void))
{
	prompt_stop();
	snprintf(P.dir, sizeof(P.dir), "%s", dir);
	snprintf(P.path, sizeof(P.path), "%s/prompt", dir);
	P.now_ms = now_ms;
	P.ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (P.ifd < 0)
		return -errno;
	if (inotify_add_watch(P.ifd, dir, IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE | IN_MOVED_FROM) < 0) {
		int e = errno;

		close(P.ifd);
		P.ifd = -1;
		return -e;
	}
	chefui_watch_fd(P.ifd, POLLIN, inotify_io, NULL);
	prompt_reload();	/* one already there */
	return 0;
}

void prompt_stop(void)
{
	if (P.ifd >= 0) {
		chefui_unwatch_fd(P.ifd);
		close(P.ifd);
		P.ifd = -1;
	}
	hide();
}

void prompt_set_trusted_uid(uid_t uid)
{
	P.uid = uid;
}

bool prompt_visible(void)
{
	return P.shown;
}

#ifdef CHEFUI_TEST
lv_obj_t *prompt_test_box(void) { return P.box; }
lv_obj_t *prompt_test_title(void) { return P.title; }
lv_obj_t *prompt_test_text(void) { return P.text; }
lv_obj_t *prompt_test_count(void) { return P.count; }
#endif
