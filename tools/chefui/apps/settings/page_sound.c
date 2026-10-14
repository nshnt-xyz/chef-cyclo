/* P03 Sound & haptics. Preferences are persisted for future ride consumers;
 * only tap, Back, and wake haptics are active in this milestone. */
#include <stdio.h>
#include <stdlib.h>

#include "app.h"
#include "fonts.h"
#include "icons.h"
#include "pages.h"

struct sound_page {
	struct kit_slider volume;
	lv_obj_t *value, *haptics, *sounds, *quiet;
};

static void volume_text(struct sound_page *s)
{
	char value[8];
	snprintf(value, sizeof(value), "%d%%", app.set.alert_volume);
	lv_label_set_text(s->value, value);
}

static void volume_changed(lv_event_t *e)
{
	struct sound_page *s = lv_event_get_user_data(e);
	app_set_alert_volume(s->volume.value);
	volume_text(s);
}

static void switch_changed(lv_event_t *e)
{
	struct sound_page *s = lv_event_get_user_data(e);
	lv_obj_t *sw = lv_event_get_target(e);
	if (sw == s->haptics) app_set_haptics(kit_switch_get(sw));
	else if (sw == s->sounds) app_set_alert_sounds(kit_switch_get(sw));
	else if (sw == s->quiet) app_set_quiet_paused(kit_switch_get(sw));
}

static void build_sound(struct page *p, const void *arg)
{
	struct sound_page *s = calloc(1, sizeof(*s));
	lv_obj_t *body = p->kp.body, *head, *label;
	(void)arg;
	if (!s) return;
	p->priv = s;
	head = kit_box(body);
	lv_obj_set_size(head, lv_pct(100), 64 + 86 + 28);
	lv_obj_set_style_pad_top(head, 64, 0);
	label = lv_label_create(head);
	lv_label_set_text(label, "ALERT VOLUME");
	lv_obj_set_style_text_font(label, &chefkit_bc600_40, 0);
	lv_obj_set_style_text_letter_space(label, 5, 0);
	kit_set_ink(label, KIT_INK_MUT);
	lv_obj_set_y(label, kit_label_y(&chefkit_bc600_40, 72));
	s->value = lv_label_create(head);
	lv_obj_set_style_text_font(s->value, &chefkit_bc600_72, 0);
	kit_set_ink(s->value, KIT_INK_FG);
	lv_obj_align(s->value, LV_ALIGN_TOP_RIGHT, 0, kit_label_y(&chefkit_bc600_72, 72));
	volume_text(s);
	kit_slider_create_icons(&s->volume, body, 0, 100, app.set.alert_volume,
				&chefkit_icon_64, 64, KIT_IC_VOLUME,
				&chefkit_icon_72, 72, KIT_IC_VOLUME_2);
	lv_obj_add_event_cb(s->volume.obj, volume_changed, LV_EVENT_VALUE_CHANGED, s);
	page_help_text(body, "Volume rocker is reserved for ride pages.");
	kit_section(body, "FEEDBACK", 72, 8);
	page_info_row(body, "Haptics", "Buzz on alerts and taps", &s->haptics);
	page_info_row(body, "Alert sounds", "Laps, sensor lost, low battery", &s->sounds);
	page_info_row(body, "Quiet when paused", "Mute alerts while stopped", &s->quiet);
	lv_obj_add_event_cb(s->haptics, switch_changed, LV_EVENT_VALUE_CHANGED, s);
	lv_obj_add_event_cb(s->sounds, switch_changed, LV_EVENT_VALUE_CHANGED, s);
	lv_obj_add_event_cb(s->quiet, switch_changed, LV_EVENT_VALUE_CHANGED, s);
	kit_switch_set(s->haptics, app.set.haptics);
	kit_switch_set(s->sounds, app.set.alert_sounds);
	kit_switch_set(s->quiet, app.set.quiet_paused);
}

static void sound_settings(struct page *p)
{
	struct sound_page *s = p->priv;
	if (!s) return;
	kit_slider_set(&s->volume, app.set.alert_volume);
	volume_text(s);
	kit_switch_set(s->haptics, app.set.haptics);
	kit_switch_set(s->sounds, app.set.alert_sounds);
	kit_switch_set(s->quiet, app.set.quiet_paused);
}

static void sound_destroy(struct page *p) { free(p->priv); }

const struct page_ops page_sound_ops = {
	.build = build_sound,
	.destroy = sound_destroy,
	.on_settings = sound_settings,
};
