/*
 * test_prompt.c - the guided-test prompt overlay (apps/settings/prompt.c) on a
 * headless LVGL display with a real inotify watch and a fake monotonic clock.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "check.h"
#include "fakeloop.h"
#include "fonts.h"
#include "prompt.h"
#include "theme.h"
#include "widgets.h"

static char dir[96];
static int64_t t_now = 50000;

static int64_t pf_now(void)
{
	return t_now;
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

static void write_file(const char *name, const char *text, mode_t mode)
{
	char path[160];
	int fd;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	unlink(path);
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
	CHECK(fd >= 0 && write(fd, text, strlen(text)) == (ssize_t)strlen(text));
	close(fd);
	chmod(path, mode);
}

/* the way a writer does it: temp file in the directory, renamed over */
static void publish(const char *text)
{
	char a[160], b[160];

	write_file("prompt.tmp", text, 0600);
	snprintf(a, sizeof(a), "%s/prompt.tmp", dir);
	snprintf(b, sizeof(b), "%s/prompt", dir);
	CHECK(rename(a, b) == 0);
	advance(20);
}

static void remove_prompt(void)
{
	char b[160];

	snprintf(b, sizeof(b), "%s/prompt", dir);
	unlink(b);
	advance(20);
}

static int timers(void)
{
	int n = 0;

	for (lv_timer_t *t = lv_timer_get_next(NULL); t; t = lv_timer_get_next(t))
		n++;
	return n;
}

static void test_parse(void)
{
	struct prompt_data d;

	prompt_parse("title STEP 3/9\ntext COVER the sensor\nuntil 123456\nbogus x\n", 56, &d);
	CHECK(!strcmp(d.title, "STEP 3/9") && !strcmp(d.text, "COVER the sensor") && d.has_until && d.until == 123456);
	prompt_parse("text only\r\n", 11, &d);
	CHECK(!strcmp(d.text, "only") && d.title[0] == 0 && !d.has_until);
	prompt_parse("until abc\nuntil -5\n", 19, &d);
	CHECK(!d.has_until);
	prompt_parse("title caf\xc3\xa9\x01x\n", 14, &d);
	CHECK(!strcmp(d.title, "caf???x"));
	prompt_parse("", 0, &d);
	CHECK(d.title[0] == 0 && d.text[0] == 0 && !d.has_until);
	/* long text is cut, not overrun */
	{
		char big[400];

		memcpy(big, "text ", 5);
		memset(big + 5, 'a', 300);
		big[305] = '\n';
		prompt_parse(big, 306, &d);
		CHECK(strlen(d.text) == sizeof(d.text) - 1);
	}
}

static void test_fonts(void)
{
	const lv_font_t *f[] = { &chefkit_b500_64, &chefkit_bc600_60 };
	lv_font_glyph_dsc_t g;

	/* every printable ASCII byte a prompt may show has a glyph */
	for (int i = 0; i < 2; i++)
		for (int c = 0x20; c < 0x7f; c++)
			CHECK(lv_font_get_glyph_dsc(f[i], &g, (uint32_t)c, 0));
}

static void test_overlay(void)
{
	int base_timers;

	CHECK(prompt_start(dir, pf_now) == 0);
	prompt_set_trusted_uid(geteuid());
	CHECK(!prompt_visible());
	base_timers = timers();

	/* appears on a rename, without a countdown or a timer */
	publish("title STEP 2/9\ntext Hold the phone still in the room light\n");
	CHECK(prompt_visible() && !lv_obj_is_hidden(prompt_test_box()));
	CHECK(!strcmp(lv_label_get_text(prompt_test_title()), "STEP 2/9"));
	CHECK(!strcmp(lv_label_get_text(prompt_test_text()), "Hold the phone still in the room light"));
	CHECK(lv_obj_is_hidden(prompt_test_count()));
	CHECK(timers() == base_timers);			/* no timer without `until` */

	/* the card never takes touches: nothing in it is clickable, and the hit test on the top
	 * layer finds nothing at a point inside it, so the page below gets the click */
	CHECK(!lv_obj_is_clickable(prompt_test_box()));
	CHECK(!lv_obj_is_clickable(prompt_test_title()));
	CHECK(!lv_obj_is_clickable(prompt_test_text()));
	{
		lv_area_t a;
		lv_point_t p;
		lv_obj_t *below;

		lv_obj_update_layout(prompt_test_box());
		lv_obj_get_coords(prompt_test_box(), &a);
		p.x = (a.x1 + a.x2) / 2;
		p.y = (a.y1 + a.y2) / 2;
		CHECK(lv_indev_search_obj(lv_layer_top(), &p) == NULL);
		below = lv_button_create(lv_screen_active());
		lv_obj_set_pos(below, p.x - 50, p.y - 50);
		lv_obj_set_size(below, 100, 100);
		lv_obj_update_layout(lv_screen_active());
		CHECK(lv_indev_search_obj(lv_screen_active(), &p) == below);
		lv_obj_delete(below);
	}

	/* with `until`: whole-second countdown from the fake clock, one timer */
	{
		char b[96];

		snprintf(b, sizeof(b), "title STEP 3/9\ntext COVER\nuntil %lld\n", (long long)(t_now + 12000));
		publish(b);
	}
	CHECK(!lv_obj_is_hidden(prompt_test_count()) && timers() == base_timers + 1);
	CHECK(!strcmp(lv_label_get_text(prompt_test_count()), "12 s"));
	advance(1000);
	CHECK(!strcmp(lv_label_get_text(prompt_test_count()), "11 s"));
	advance(10000);
	CHECK(!strcmp(lv_label_get_text(prompt_test_count()), "1 s"));
	advance(2000);
	CHECK(!strcmp(lv_label_get_text(prompt_test_count()), "0 s"));
	advance(1500);
	CHECK(timers() == base_timers && !strcmp(lv_label_get_text(prompt_test_count()), "0 s"));	/* no ticking at 0 s */
	/* a new prompt replaces it, a prompt without `until` drops the timer */
	publish("title DONE\ntext Finished\n");
	CHECK(timers() == base_timers && !strcmp(lv_label_get_text(prompt_test_title()), "DONE"));

	/* removal hides it at once and nothing keeps running */
	{
		char b[96];

		snprintf(b, sizeof(b), "title X\ntext y\nuntil %lld\n", (long long)(t_now + 5000));
		publish(b);
	}
	CHECK(timers() == base_timers + 1);
	remove_prompt();
	CHECK(!prompt_visible() && lv_obj_is_hidden(prompt_test_box()) && timers() == base_timers);

	/* non-ASCII shows as '?' */
	publish("title caf\xc3\xa9\ntext na\xc3\xafve\n");
	CHECK(!strcmp(lv_label_get_text(prompt_test_title()), "caf??") && !strcmp(lv_label_get_text(prompt_test_text()), "na??ve"));
	remove_prompt();

	/* theme switch restyles it in place */
	publish("title A\ntext b\n");
	kit_theme_set(KIT_THEME_LIGHT);
	advance(20);
	CHECK(prompt_visible());
	kit_theme_set(KIT_THEME_DARK);
	remove_prompt();
}

static void test_untrusted(void)
{
	char link[160], target[160];
	char big[PROMPT_MAX_BYTES + 200];

	fakeloop_log_clear();
	/* group/other writable */
	write_file("prompt.tmp", "title BAD\ntext x\n", 0666);
	snprintf(link, sizeof(link), "%s/prompt.tmp", dir);
	snprintf(target, sizeof(target), "%s/prompt", dir);
	CHECK(rename(link, target) == 0);
	advance(20);
	CHECK(!prompt_visible() && fakeloop_log_count("prompt ignored") == 1);
	remove_prompt();
	/* a symlink */
	write_file("real", "title LINK\ntext x\n", 0600);
	snprintf(link, sizeof(link), "%s/real", dir);
	CHECK(symlink(link, target) == 0);
	advance(20);
	CHECK(!prompt_visible());
	remove_prompt();
	/* over 1 KiB */
	memset(big, 'a', sizeof(big) - 1);
	big[sizeof(big) - 1] = 0;
	memcpy(big, "text ", 5);
	write_file("prompt.tmp", big, 0600);
	snprintf(link, sizeof(link), "%s/prompt.tmp", dir);
	CHECK(rename(link, target) == 0);
	advance(20);
	CHECK(!prompt_visible());
	remove_prompt();
	/* a file of someone else (the uid check) */
	prompt_set_trusted_uid(geteuid() + 1);
	publish("title OTHER\ntext x\n");
	CHECK(!prompt_visible());
	prompt_set_trusted_uid(geteuid());
	publish("title OK\ntext x\n");
	CHECK(prompt_visible());
	remove_prompt();
}

int main(void)
{
	fakeloop_init(true);
	fakeloop_display(1080, 2246);
	kit_theme_init(KIT_THEME_DARK);
	snprintf(dir, sizeof(dir), "/tmp/prompt-test.XXXXXX");
	if (!mkdtemp(dir))
		return 1;
	test_parse();
	test_fonts();
	test_overlay();
	test_untrusted();
	prompt_stop();
	CHECK(fakeloop_watched() == 0);
	return check_report("test-prompt");
}
