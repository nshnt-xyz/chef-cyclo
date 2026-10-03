/* SDL-only demo calibration/layout check; regular unit tests remain SDL-free.
 * Exercise the public app button dispatch and observe actual LVGL geometry. */
#define main chefui_demo_main
#include "../demo.c"
#undef main
#include "../chefui_internal.h"
#include "lvgl_private.h"
#include "check.h"

int main(int argc, char **argv)
{
	struct chefui_config cfg = { .app_name = "calibration-test", .backend = CHEFUI_BACKEND_SDL };
	lv_obj_t *root, *content;
	lv_area_t a;
	int w, h, count = 0;

	if (argc != 2 && argc != 3) return 64;
	opt.rot = cfg.rotation = atoi(argv[1]);
	opt.calibrate = 1;
	CHECK(chefui_init(&cfg) != NULL);
	if (argc == 3) {
		/* No scene edits between off/on: initial full invalidation is
		 * still queued. Its dedup must not leave refresh paused. */
		lv_timer_t *refresh = lv_display_get_refr_timer(lv_display_get_default());
		CHECK(cu_stats.frames == 0);
		CHECK(chefui_screen_set(false) == 0);
		CHECK(lv_timer_get_paused(refresh));
		CHECK(chefui_screen_set(true) == 0);
		CHECK(!lv_timer_get_paused(refresh));
		lv_timer_t *stop = lv_timer_create(quit_tick, 100, NULL);
		lv_timer_set_repeat_count(stop, 1);
		CHECK(chefui_run() == 0);
		CHECK(cu_stats.frames > 0);
		return check_report("test-host-off-before-frame");
	}
	chefui_on_button(button_cb, NULL);
	build();
	root = chefui_root();
	content = chefui_content_root();
	w = lv_display_get_horizontal_resolution(lv_display_get_default());
	h = lv_display_get_vertical_resolution(lv_display_get_default());
	CHECK(chefui_safe_top_get() == 96);
	CHECK(opt.fps == 60);
	CHECK(lv_display_get_refr_timer(lv_display_get_default())->period == 16);
	cu_dispatch_gesture("volup.short");
	CHECK(chefui_safe_top_get() == 100);
	lv_obj_update_layout(root);
	a = chefui_safe_area();
	CHECK(lv_obj_get_x(content) == a.x1 && lv_obj_get_y(content) == a.y1);
	CHECK(lv_obj_get_width(content) == a.x2 - a.x1 + 1);
	CHECK(lv_obj_get_height(content) == a.y2 - a.y1 + 1);
	CHECK(strstr(lv_label_get_text(guide_text), "100 px") != NULL);
	CHECK(lv_obj_get_width(guide) == (opt.rot % 180 ? 100 : w));
	CHECK(lv_obj_get_height(guide) == (opt.rot % 180 ? h : 100));
	cu_dispatch_gesture("voldown.short");
	CHECK(chefui_safe_top_get() == 96);
	CHECK(chefui_brightness_get() == 96); /* calibration keys do not change brightness */
	for (uint32_t i = 0; i < lv_obj_get_child_count(root); i++) {
		lv_obj_t *child = lv_obj_get_child(root, i);
		if (lv_obj_get_width(child) == 90 && lv_obj_get_height(child) == 90) count++;
	}
	CHECK(count == 4); /* full-panel diagnostic targets outside content root */
	for (int i = 0; i < CHEFUI_MAX_CONTACTS; i++)
		CHECK(lv_obj_get_parent(dots[i]) == lv_layer_top());
	CHECK(chefui_safe_top_set(-1) == 0);
	calibration_update();
	lv_obj_update_layout(root);
	CHECK(lv_obj_get_width(content) == w && lv_obj_get_height(content) == h);
	CHECK(lv_obj_is_hidden(guide));
	chefui_quit(0);
	CHECK(chefui_run() == 0);
	return check_report("test-demo-host");
}
