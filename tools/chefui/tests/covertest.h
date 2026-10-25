/*
 * covertest.h - what test_splash.c and test_shutdown.c share: the app on a
 * headless LVGL display with test-controlled time, the platform fakes, the
 * captured log lines, the temporary OpenRC and run trees, and the readers of
 * a cover's step text, count and bar fill (apps/settings/cover.h).
 */
#ifndef CHEFUI_TEST_COVERTEST_H
#define CHEFUI_TEST_COVERTEST_H

#define _GNU_SOURCE
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app.h"
#include "check.h"
#include "chefui.h"
#include "fakeloop.h"
#include "sys.h"
#include "sysstate.h"
#include "testbase.h"
#include "theme.h"
#include "widgets.h"

static int64_t t_now = 100000;
static bool screen_on = true;
static int screen_sets;
/* the captured log lines, as the test files name them */
#define logs g_log
#define nlogs g_nlog

static int64_t pf_now(void) { return t_now; }
static bool pf_on(void) { return screen_on; }

static int pf_set(bool on)
{
	screen_sets++;
	if (on != screen_on) {
		screen_on = on;
		app_on_screen(on);
	}
	return 0;
}

static int pf_bright(int level)
{
	(void)level;
	return 0;
}

static void advance(int ms)
{
	while (ms > 0) {
		int step = ms > 10 ? 10 : ms;

		t_now += step;
		fakeloop_advance((uint32_t)step);
		ms -= step;
	}
}

static bool logged(const char *sub)
{
	return log_has(sub);
}

static void covertest_tree(const char *name)
{
	testbase_tree(name);
}

/* A cover's labels (the step text, the count) and bar: its children are the
 * title, the build line, the step text, the count and the track. */
static const char *cover_text(lv_obj_t *c, int i)
{
	return lv_label_get_text(lv_obj_get_child(lv_obj_get_child(c, i), 0));
}

static int cover_fill(lv_obj_t *c)
{
	lv_obj_t *fill;

	lv_obj_update_layout(c);
	fill = lv_obj_get_child(lv_obj_get_child(c, 4), 0);
	return lv_obj_is_hidden(fill) ? 0 : lv_obj_get_width(fill);
}

static bool cover_shows(lv_obj_t *c, const char *step, const char *count, int fill)
{
	return !strcmp(cover_text(c, 2), step) && !strcmp(cover_text(c, 3), count) && cover_fill(c) == fill;
}

/* The app on a 1080x2246 headless display, sleep 30 s. */
static void covertest_app(void)
{
	lv_obj_t *scr, *content;
	static struct ui_settings set;
	static struct app_platform pf;

	pf = (struct app_platform){ .now_ms = pf_now, .screen_is_on = pf_on, .screen_set = pf_set,
				    .brightness_set = pf_bright };
	signal(SIGPIPE, SIG_IGN);
	fakeloop_init(true);
	fakeloop_display(1080, 2246);
	scr = lv_screen_active();
	kit_theme_init(KIT_THEME_DARK);
	ui_settings_defaults(&set);
	set.sleep_s = 30;
	content = kit_box(scr);
	lv_obj_set_size(content, 1080, 2246 - 96);
	lv_obj_set_y(content, 96);
	memset(&sys_state, 0, sizeof(sys_state));
	app.fake_state = true;
	app_init(&pf, &set, scr, content);
}

#endif
