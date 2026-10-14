/*
 * page_about.c - About (P06): the image (name, version, build commit and
 * date from /etc/chef/image-info), LVGL, kernel, display size, boot serial,
 * Bluetooth and Wi-Fi addresses and uptime. Everything is read once per
 * entry/wake on the settings worker (deviceinfo.c); the uptime text is
 * advanced from the cached BOOTTIME sample at minute boundaries only
 * while the page is visible.
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "app.h"
#include "fonts.h"
#include "icons.h"
#include "pages.h"
#include "sys.h"
#include "text.h"

enum { ROW_LVGL, ROW_KERNEL, ROW_DISPLAY, ROW_SERIAL, ROW_BT, ROW_WIFI, ROW_UPTIME, ROW_N };

struct about_page {
	lv_obj_t *title, *build, *value[ROW_N];
	lv_timer_t *tick;
};

static int64_t mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* milliseconds of uptime now: the BOOTTIME sample plus time since it */
static int64_t uptime_ms(void)
{
	const struct sys_device_info *d = &sys_state.device;
	int64_t elapsed = d->sampled_mono_ms ? mono_ms() - d->sampled_mono_ms : 0;

	return (int64_t)d->uptime_s * 1000 + (elapsed > 0 ? elapsed : 0);
}

static const char *or_unavailable(bool known, const char *v)
{
	return known && v[0] ? v : "Unavailable";
}

static void about_tick(lv_timer_t *t);

static void about_refresh(struct page *p)
{
	struct about_page *a = p->priv;
	const struct sys_device_info *d = &sys_state.device;
	char b[96];

	if (!a)
		return;
	if (d->metadata_known) {
		snprintf(b, sizeof(b), "%s %s", d->image_name, d->version);
		lv_label_set_text(a->title, b);
		snprintf(b, sizeof(b), "Build %.7s%s \xC2\xB7 %s", d->commit, d->dirty ? "+dirty" : "",
			 d->build_date);
		lv_label_set_text(a->build, b);
	} else {
		lv_label_set_text(a->title, d->image_name[0] ? d->image_name : "chef-cyclo");
		lv_label_set_text(a->build, "Build information unavailable");
	}
	lv_label_set_text(a->value[ROW_LVGL], or_unavailable(d->lvgl[0], d->lvgl));
	lv_label_set_text(a->value[ROW_KERNEL], or_unavailable(d->kernel[0], d->kernel));
	lv_label_set_text(a->value[ROW_DISPLAY], or_unavailable(d->resolution[0], d->resolution));
	lv_label_set_text(a->value[ROW_SERIAL], or_unavailable(d->serial_known, d->serial));
	lv_label_set_text(a->value[ROW_BT], or_unavailable(d->bluetooth_known, d->bluetooth));
	lv_label_set_text(a->value[ROW_WIFI], or_unavailable(d->wifi_known, d->wifi));
	if (d->sampled_mono_ms || d->uptime_s) {
		text_uptime((uint64_t)(uptime_ms() / 1000), b, sizeof(b));
		lv_label_set_text(a->value[ROW_UPTIME], b);
	} else {
		lv_label_set_text(a->value[ROW_UPTIME], "Unavailable");
	}
}

static void about_arm(struct page *p)
{
	struct about_page *a = p->priv;
	int64_t ms = uptime_ms();

	if (!a)
		return;
	if (a->tick) {
		lv_timer_delete(a->tick);
		a->tick = NULL;
	}
	if (!sys_state.device.sampled_mono_ms && !sys_state.device.uptime_s)
		return;
	/* next whole minute of uptime */
	a->tick = lv_timer_create(about_tick, (uint32_t)(60000 - ms % 60000), p);
	lv_timer_set_repeat_count(a->tick, 1);
}

static void about_tick(lv_timer_t *t)
{
	struct page *p = lv_timer_get_user_data(t);
	struct about_page *a = p->priv;

	a->tick = NULL;			/* one-shot, deleted by LVGL */
	about_refresh(p);
	about_arm(p);
}

static void about_visible(struct page *p, bool visible)
{
	struct about_page *a = p->priv;
	int w, h;

	if (!a)
		return;
	if (!visible) {
		if (a->tick) {
			lv_timer_delete(a->tick);
			a->tick = NULL;
		}
		if (!app.fake_state)
			sys_device_info_want(false, 0, 0, NULL);
		return;
	}
	if (!app.fake_state) {
		char v[24];

		page_screen_size(&w, &h);
		snprintf(v, sizeof(v), "%d.%d.%d", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR,
			 LVGL_VERSION_PATCH);
		sys_device_info_want(true, w, h, v);
	}
	about_refresh(p);
	about_arm(p);
}

static void about_state(struct page *p, unsigned what)
{
	if (what & SYS_DEVICE_INFO) {
		about_refresh(p);
		if (app_page_visible(p))
			about_arm(p);
	}
}

static void build_about(struct page *p, const void *arg)
{
	static const char *const keys[ROW_N] = { "LVGL", "Kernel", "Display", "Serial", "Bluetooth",
		"Wi-Fi", "Uptime" };
	struct about_page *a = calloc(1, sizeof(*a));
	lv_obj_t *body = p->kp.body, *card;

	(void)arg;
	if (!a)
		return;
	p->priv = a;
	/* padding 80 0 56; gap 12; rule below */
	card = kit_box(body);
	lv_obj_set_size(card, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_add_style(card, &kit_st.rule_bottom, 0);
	lv_obj_set_style_pad_top(card, 80, 0);
	lv_obj_set_style_pad_bottom(card, 56, 0);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(card, 12, 0);
	kit_section(card, "FIRMWARE", 0, 0);
	a->title = kit_text(card, &chefkit_bc600_160, 160, KIT_INK_FG, "chef-cyclo");
	kit_text_single_line(a->title);
	a->build = kit_text(card, &chefkit_b400_44, 0, KIT_INK_MUT, "");
	kit_text_single_line(a->build);
	for (int i = 0; i < ROW_N; i++)
		a->value[i] = kit_kv_row_h(body, keys[i], "", 156);
	kit_page_scrollable(&p->kp);
	about_refresh(p);
}

static void about_destroy(struct page *p)
{
	struct about_page *a = p->priv;

	if (a && a->tick)
		lv_timer_delete(a->tick);
	free(a);
}

const struct page_ops page_about_ops = {
	.build = build_about,
	.destroy = about_destroy,
	.on_state = about_state,
	.on_visible = about_visible,
};
