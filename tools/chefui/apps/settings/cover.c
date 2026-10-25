/*
 * cover.c - the progress view of the boot splash and the shutdown screen
 * (cover.h).
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "chefui.h"
#include "cover.h"
#include "deviceinfo.h"
#include "fonts.h"
#include "steptrack.h"
#include "sysstate.h"
#include "widgets.h"

#define SIDE_PAD 120
#define BAR_H 24
#define BAR_BOTTOM 360
#define BAR_GAP 32		/* between the text row and the bar */
#define TITLE_H 150		/* line-height 1 */
#define TITLE_GAP 20
#define COUNT_W 200
#define IMAGE_INFO_MAX 1024

static void (*g_log)(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

void cover_log_to(void (*log)(const char *fmt, ...) __attribute__((format(printf, 1, 2))))
{
	g_log = log;
}

void cover_log(const char *fmt, ...)
{
	char buf[160];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (g_log)
		g_log("%s", buf);
	else
		chefui_log("%s", buf);
}

/* "<version> · <commit, 8 hex>[+dirty]", empty without image facts. */
static void version_line(char *out, size_t n, const char *image_info, bool from_sys_state)
{
	const char *version = NULL, *commit = NULL;
	bool dirty = false;
	struct sys_image_info info;
	char buf[IMAGE_INFO_MAX + 1];

	out[0] = 0;
	if (from_sys_state) {
		if (!sys_state.device.metadata_known)
			return;
		version = sys_state.device.version;
		commit = sys_state.device.commit;
		dirty = sys_state.device.dirty;
	} else {
		int fd = open(image_info ? image_info : "/etc/chef/image-info", O_RDONLY | O_CLOEXEC);
		ssize_t r;

		if (fd < 0)
			return;
		r = read(fd, buf, sizeof(buf));
		close(fd);
		if (r <= 0 || r > IMAGE_INFO_MAX || !sys_image_info_parse(buf, (size_t)r, &info))
			return;
		version = info.version;
		commit = info.commit;
		dirty = info.dirty;
	}
	snprintf(out, n, "%s \xC2\xB7 %.8s%s", version, commit, dirty ? "+dirty" : "");
}

static lv_obj_t *place(lv_obj_t *label, int x, int y)
{
	lv_obj_t *box = lv_obj_get_parent(label);

	lv_obj_set_pos(box, x, y);
	return box;
}

void cover_new(struct cover *c, lv_obj_t *p, const char *image_info, bool from_sys_state)
{
	lv_obj_t *title, *version, *track;
	int w = lv_obj_get_width(p), h = lv_obj_get_height(p);
	int centre = (h - 200) / 2, ty = centre - (TITLE_H + TITLE_GAP + kit_css_line(&chefkit_b400_40)) / 2;
	int bar_y = h - BAR_BOTTOM - BAR_H, row_h = kit_css_line(&chefkit_b500_48);
	int row_y = bar_y - BAR_GAP - row_h;
	char vl[96];

	c->bar_w = w - 2 * SIDE_PAD;
	/* the whole panel, over everything below it, taking every touch */
	c->obj = lv_obj_create(p);
	lv_obj_remove_style_all(c->obj);
	lv_obj_set_size(c->obj, w, h);
	lv_obj_set_pos(c->obj, 0, 0);
	lv_obj_set_scrollable(c->obj, false);
	lv_obj_set_clickable(c->obj, true);
	lv_obj_add_style(c->obj, &kit_st.screen, 0);

	title = kit_text(c->obj, &chefkit_bc600_150, TITLE_H, KIT_INK_FG, "chef-cyclo");
	lv_obj_set_style_text_letter_space(title, 3, 0);	/* 0.02 em */
	version_line(vl, sizeof(vl), image_info, from_sys_state);
	version = kit_text(c->obj, &chefkit_b400_40, 0, KIT_INK_MUT, vl);
	lv_obj_update_layout(c->obj);
	/* CSS letter-spacing also trails the last glyph: its box is 3 px wider, so
	 * the glyphs sit 1.5 px left of LVGL's centre (1 px matches the render) */
	place(title, (w - lv_obj_get_width(lv_obj_get_parent(title))) / 2 - 1, ty);
	place(version, (w - lv_obj_get_width(lv_obj_get_parent(version))) / 2, ty + TITLE_H + TITLE_GAP);

	c->text = kit_text(c->obj, &chefkit_b500_48, 0, KIT_INK_FG, "");
	place(c->text, SIDE_PAD, row_y);
	c->count = kit_text(c->obj, &chefkit_bc600_48, 0, KIT_INK_MUT, "");
	lv_obj_set_width(lv_obj_get_parent(c->count), COUNT_W);
	lv_obj_set_width(c->count, COUNT_W);
	lv_obj_set_style_text_align(c->count, LV_TEXT_ALIGN_RIGHT, 0);
	place(c->count, w - SIDE_PAD - COUNT_W, row_y);

	track = kit_box(c->obj);
	lv_obj_set_size(track, c->bar_w, BAR_H);
	lv_obj_set_pos(track, SIDE_PAD, bar_y);
	lv_obj_add_style(track, &kit_st.splash_track, 0);
	c->fill = kit_box(track);
	lv_obj_set_pos(c->fill, 0, 0);
	lv_obj_set_size(c->fill, 0, BAR_H);
	lv_obj_add_style(c->fill, &kit_st.splash_fill, 0);
}

void cover_set(struct cover *c, const char *text, bool ellipsis, int step, int n, int finished)
{
	char t[80], cnt[16];
	int fw = c->bar_w * finished / n;

	snprintf(t, sizeof(t), ellipsis ? "%s\xE2\x80\xA6" : "%s", text);
	snprintf(cnt, sizeof(cnt), "%d / %d", step < n ? step + 1 : n, n);
	if (strcmp(lv_label_get_text(c->text), t))
		lv_label_set_text(c->text, t);
	if (strcmp(lv_label_get_text(c->count), cnt))
		lv_label_set_text(c->count, cnt);
	lv_obj_set_width(c->fill, fw);
	lv_obj_set_hidden(c->fill, fw == 0);
}

void cover_clock_defaults(int64_t (**now)(void), int64_t (**boot_ms)(void))
{
	if (!*now)
		*now = steptrack_mono_ms;
	if (!*boot_ms)
		*boot_ms = steptrack_boot_ms;
}

void cover_timer_at(lv_timer_t *timer, int64_t at, int64_t now)
{
	if (at < 0) {
		lv_timer_pause(timer);
		return;
	}
	lv_timer_set_period(timer, at > now ? (uint32_t)(at - now) : 1);
	lv_timer_reset(timer);
	lv_timer_resume(timer);
}

void cover_log_at(const char *prefix, const char *what, int64_t boot_ms, const char *extra)
{
	char stamp[24];

	steptrack_stamp(stamp, sizeof(stamp), boot_ms);
	cover_log("%s: %s at %s s%s", prefix, what, stamp, extra);
}

void cover_follow_fd(int *watched, int fd, void (*cb)(int fd, short revents, void *user))
{
	if (fd == *watched)
		return;
	if (*watched >= 0)
		chefui_unwatch_fd(*watched);
	*watched = fd;
	if (fd >= 0 && chefui_watch_fd(fd, POLLIN, cb, NULL))
		*watched = -1;
}

void cover_delete(struct cover *c)
{
	lv_obj_delete(c->obj);
	c->obj = NULL;
}
