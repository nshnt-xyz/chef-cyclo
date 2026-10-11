/*
 * keyboard.c - the text field and on-screen keyboard (see widgets.h),
 * from the round-1 password page (P17): keys 168 px high with a 24 px
 * radius and an inset 3 px line, 12 px apart in rows 14 px apart, on the
 * surface colour under a 3 px rule.
 *
 * The field never copies its text: the label points at the caller's
 * buffer (or at a row of bullets), so a passphrase exists in one place
 * the caller wipes. Every printable ASCII character has a key: letters
 * (one-shot shift), 123 and #+= layers.
 */
#include <string.h>

#include "fonts.h"
#include "icons.h"
#include "widgets.h"

#define BULLET "\xE2\x80\xA2"	/* U+2022, 3 bytes */

/* ------------------------------------------------------------ text field */

static void field_show(struct kit_field *f)
{
	bool dots = f->hidden;
	int32_t inner_w, room;

	if (dots) {
		size_t i;

		for (i = 0; i < f->len && i < 64; i++)
			memcpy(f->dots + 3 * i, BULLET, 3);
		f->dots[3 * i] = 0;
	}
	lv_label_set_text_static(f->label, dots ? f->dots : f->buf);
	lv_obj_set_hidden(f->cursor, !f->focused);
	/* keep the end (and the cursor) in view */
	lv_obj_update_layout(f->obj);
	inner_w = lv_obj_get_width(f->inner);
	room = lv_obj_get_content_width(f->obj);
	lv_obj_set_x(f->inner, inner_w > room ? room - inner_w : 0);
}

void kit_field_create(struct kit_field *f, lv_obj_t *parent, char *buf, size_t cap)
{
	memset(f, 0, sizeof(*f));
	f->buf = buf;
	f->cap = cap > 64 ? 64 : cap;
	f->len = strnlen(buf, f->cap - 1);
	buf[f->len] = 0;
	/* height:192px border-box; border 3 acc (line2 when not focused);
	 * radius 36; surf; padding 0 40; 72 px, letter-spacing .1em */
	f->obj = kit_box(parent);
	lv_obj_set_height(f->obj, 192);
	lv_obj_add_style(f->obj, &kit_st.field, 0);
	lv_obj_add_style(f->obj, &kit_st.field_idle, KIT_STATE_OFF);
	lv_obj_set_style_pad_hor(f->obj, 40, 0);
	lv_obj_set_clickable(f->obj, true);
	lv_obj_set_event_bubble(f->obj, false);
	/* LVGL clips children to the parent's box, padding included: a box
	 * over the content area clips text scrolled left */
	f->inner = kit_box(f->obj);
	lv_obj_set_size(f->inner, lv_pct(100), lv_pct(100));
	f->inner = kit_box(f->inner);
	lv_obj_set_size(f->inner, LV_SIZE_CONTENT, lv_pct(100));
	lv_obj_set_flex_flow(f->inner, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(f->inner, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(f->inner, 6, 0);
	f->label = kit_text(f->inner, &chefkit_b400_72, 0, KIT_INK_FG, "");
	lv_obj_set_style_text_letter_space(f->label, 7, 0);
	/* the cursor: 6 x 96, accent */
	f->cursor = kit_box(f->inner);
	lv_obj_set_size(f->cursor, 6, 96);
	lv_obj_add_style(f->cursor, &kit_st.caret, 0);
	f->focused = true;
	field_show(f);
}

void kit_field_set_hidden(struct kit_field *f, bool hidden)
{
	f->hidden = hidden;
	field_show(f);
}

void kit_field_set_focus(struct kit_field *f, bool focused)
{
	f->focused = focused;
	lv_obj_set_state(f->obj, KIT_STATE_OFF, !focused);
	field_show(f);
}

bool kit_field_put(struct kit_field *f, char c)
{
	if (f->len + 1 >= f->cap || c < 0x20 || c > 0x7E)
		return false;
	f->buf[f->len++] = c;
	f->buf[f->len] = 0;
	field_show(f);
	return true;
}

void kit_field_backspace(struct kit_field *f)
{
	if (!f->len)
		return;
	f->buf[--f->len] = 0;
	field_show(f);
}

void kit_field_clear(struct kit_field *f)
{
	/* volatile: a wipe the compiler keeps */
	volatile char *v = f->buf;

	for (size_t i = 0; i < f->cap; i++)
		v[i] = 0;
	for (size_t i = 0; i < sizeof(f->dots); i++)
		((volatile char *)f->dots)[i] = 0;
	f->len = 0;
	field_show(f);
}

void kit_field_mask(struct kit_field *f, bool mask)
{
	bool hidden = f->hidden;

	f->hidden = mask || hidden;
	field_show(f);
	f->hidden = hidden;
	if (!mask)
		field_show(f);
}

/* -------------------------------------------------------------- keyboard */

enum {
	K_SHIFT = 1,		/* lower <-> upper */
	K_NUM,			/* to 123 */
	K_SYM,			/* to #+= */
	K_ABC,			/* to letters */
};

struct key {
	const char *label;	/* text, or a glyph when font is an icon font */
	int code;		/* character, KIT_KEY_* or K_* */
	unsigned char grow;	/* CSS flex x 10 */
};

#define ROW_MAX 11

static const char *const letters[2][3] = {
	{ "qwertyuiop", "asdfghjkl", "zxcvbnm" },
	{ "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM" },
};
static const char num1[] = "1234567890", num2[] = "-/:;()$&@\"", num3[] = ".,?!'";
static const char sym1[] = "[]{}#%^*+=", sym2[] = "_\\|~<>`";

static void key_event(lv_event_t *e)
{
	lv_obj_t *key = lv_event_get_current_target(e);
	struct kit_kb *kb = lv_event_get_user_data(e);
	int code = (int)(intptr_t)lv_obj_get_user_data(key);
	lv_event_code_t ev = lv_event_get_code(e);

	if (ev == LV_EVENT_LONG_PRESSED_REPEAT && code != KIT_KEY_BACKSPACE)
		return;
	switch (code) {
	case K_SHIFT:
		kit_kb_set_layer(kb, kb->layer == KIT_KB_UPPER ? KIT_KB_LOWER : KIT_KB_UPPER);
		return;
	case K_NUM:
		kit_kb_set_layer(kb, KIT_KB_NUM);
		return;
	case K_SYM:
		kit_kb_set_layer(kb, KIT_KB_SYM);
		return;
	case K_ABC:
		kit_kb_set_layer(kb, KIT_KB_LOWER);
		return;
	default:
		break;
	}
	if (kb->on_key)
		kb->on_key(code, kb->user);
	if (kb->layer == KIT_KB_UPPER && code != KIT_KEY_BACKSPACE && code != KIT_KEY_ACTION)
		kit_kb_set_layer(kb, KIT_KB_LOWER);	/* one-shot shift */
}

static lv_obj_t *key_create(struct kit_kb *kb, lv_obj_t *row, const struct key *k, bool glyph,
			    lv_style_t *variant, const lv_font_t *font, enum kit_ink ink)
{
	lv_obj_t *key = kit_box(row), *l;

	lv_obj_set_height(key, KIT_KEY_H);
	lv_obj_set_flex_grow(key, k->grow);
	lv_obj_add_style(key, &kit_st.key, 0);
	if (variant)
		lv_obj_add_style(key, variant, 0);
	lv_obj_add_style(key, &kit_st.key_pressed, LV_STATE_PRESSED);
	lv_obj_set_flex_flow(key, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(key, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_clickable(key, true);
	lv_obj_set_event_bubble(key, false);
	lv_obj_set_user_data(key, (void *)(intptr_t)k->code);
	lv_obj_add_event_cb(key, key_event, LV_EVENT_CLICKED, kb);
	if (k->code == KIT_KEY_BACKSPACE)
		lv_obj_add_event_cb(key, key_event, LV_EVENT_LONG_PRESSED_REPEAT, kb);
	if (glyph)
		l = kit_icon(key, font, 64, ink, k->label);
	else
		l = kit_text(key, font, 0, ink, k->label);
	(void)l;
	return key;
}

static lv_obj_t *row_create(lv_obj_t *layer)
{
	lv_obj_t *row = kit_box(layer);

	lv_obj_set_size(row, lv_pct(100), KIT_KEY_H);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(row, 12, 0);
	return row;
}

static void chars_row(struct kit_kb *kb, lv_obj_t *row, const char *chars)
{
	static char one[128][2];	/* "x" for every ASCII character */

	for (const char *c = chars; *c; c++) {
		struct key k = { one[(unsigned char)*c], *c, 10 };

		one[(unsigned char)*c][0] = *c;
		key_create(kb, row, &k, false, NULL, &chefkit_b500_64, KIT_INK_FG);
	}
}

static void build_layer(struct kit_kb *kb, enum kit_kb_layer layer)
{
	lv_obj_t *lay = kit_box(kb->obj), *row;
	bool abc = layer == KIT_KB_LOWER || layer == KIT_KB_UPPER;
	const struct key del = { KIT_IC_DELETE, KIT_KEY_BACKSPACE, 15 };
	const struct key shift = { KIT_IC_ARROW_BIG_UP, K_SHIFT, 15 };
	const struct key to_num = { "123", K_NUM, 16 }, to_sym = { "#+=", K_SYM, 15 };
	const struct key to_num3 = { "123", K_NUM, 15 }, to_abc = { "ABC", K_ABC, 16 };
	const struct key space = { "space", ' ', 44 };
	struct key action = { kb->action, KIT_KEY_ACTION, 24 };

	kb->layers[layer] = lay;
	lv_obj_set_size(lay, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(lay, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(lay, 14, 0);

	row = row_create(lay);
	chars_row(kb, row, abc ? letters[layer][0] : layer == KIT_KB_NUM ? num1 : sym1);
	row = row_create(lay);
	chars_row(kb, row, abc ? letters[layer][1] : layer == KIT_KB_NUM ? num2 : sym2);
	row = row_create(lay);
	if (abc)
		key_create(kb, row, &shift, true, &kit_st.key_fn, &chefkit_icon_64,
			   layer == KIT_KB_UPPER ? KIT_INK_ACCT : KIT_INK_FG);
	else
		key_create(kb, row, layer == KIT_KB_NUM ? &to_sym : &to_num3, false, &kit_st.key_fn,
			   &chefkit_b500_52, KIT_INK_FG);
	chars_row(kb, row, abc ? letters[layer][2] : num3);
	key_create(kb, row, &del, true, &kit_st.key_fn, &chefkit_icon_64, KIT_INK_FG);
	row = row_create(lay);
	key_create(kb, row, abc ? &to_num : &to_abc, false, &kit_st.key_fn, &chefkit_b500_52,
		   KIT_INK_FG);
	key_create(kb, row, &space, false, NULL, &chefkit_b500_48, KIT_INK_MUT);
	key_create(kb, row, &action, false, &kit_st.key_join, &chefkit_b500_60, KIT_INK_ONACC);
}

void kit_kb_create(struct kit_kb *kb, lv_obj_t *parent, const char *action,
		   void (*on_key)(int key, void *user), void *user)
{
	memset(kb, 0, sizeof(*kb));
	kb->action = action;
	kb->on_key = on_key;
	kb->user = user;
	/* surf; border-top 3 line; padding 20 12 60 */
	kb->obj = kit_box(parent);
	lv_obj_set_size(kb->obj, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_add_style(kb->obj, &kit_st.kb, 0);
	lv_obj_set_style_pad_top(kb->obj, 20, 0);
	lv_obj_set_style_pad_hor(kb->obj, 12, 0);
	lv_obj_set_style_pad_bottom(kb->obj, 60, 0);
	for (int l = KIT_KB_LOWER; l <= KIT_KB_SYM; l++)
		build_layer(kb, (enum kit_kb_layer)l);
	kit_kb_set_layer(kb, KIT_KB_LOWER);
}

void kit_kb_set_layer(struct kit_kb *kb, enum kit_kb_layer layer)
{
	kb->layer = layer;
	for (int l = KIT_KB_LOWER; l <= KIT_KB_SYM; l++)
		lv_obj_set_hidden(kb->layers[l], l != (int)layer);
}

static lv_obj_t *find_key(lv_obj_t *layer, int code)
{
	for (uint32_t r = 0; r < lv_obj_get_child_count(layer); r++) {
		lv_obj_t *row = lv_obj_get_child(layer, (int32_t)r);

		for (uint32_t k = 0; k < lv_obj_get_child_count(row); k++) {
			lv_obj_t *key = lv_obj_get_child(row, (int32_t)k);

			if ((int)(intptr_t)lv_obj_get_user_data(key) == code)
				return key;
		}
	}
	return NULL;
}

bool kit_kb_type(struct kit_kb *kb, int c)
{
	enum kit_kb_layer want;
	lv_obj_t *key;

	if (c >= 'a' && c <= 'z')
		want = KIT_KB_LOWER;
	else if (c >= 'A' && c <= 'Z')
		want = KIT_KB_UPPER;
	else if (c == ' ' || c == KIT_KEY_BACKSPACE || c == KIT_KEY_ACTION ||
		 (c && (strchr(num1, c) || strchr(num2, c) || strchr(num3, c))))
		want = (c == ' ' || c == KIT_KEY_BACKSPACE || c == KIT_KEY_ACTION) ? kb->layer
			: KIT_KB_NUM;
	else if (c && (strchr(sym1, c) || strchr(sym2, c)))
		want = KIT_KB_SYM;
	else
		return false;
	/* walk there with the layer keys, as a finger would */
	if (want != kb->layer) {
		if (want == KIT_KB_UPPER && kb->layer != KIT_KB_LOWER)
			lv_obj_send_event(find_key(kb->layers[kb->layer], K_ABC), LV_EVENT_CLICKED, NULL);
		if (want == KIT_KB_UPPER)
			lv_obj_send_event(find_key(kb->layers[kb->layer], K_SHIFT), LV_EVENT_CLICKED, NULL);
		else if (want == KIT_KB_LOWER)
			lv_obj_send_event(find_key(kb->layers[kb->layer],
						   kb->layer == KIT_KB_UPPER ? K_SHIFT : K_ABC),
					  LV_EVENT_CLICKED, NULL);
		else if (want == KIT_KB_NUM)
			lv_obj_send_event(find_key(kb->layers[kb->layer], K_NUM), LV_EVENT_CLICKED, NULL);
		else if (kb->layer == KIT_KB_NUM)
			lv_obj_send_event(find_key(kb->layers[kb->layer], K_SYM), LV_EVENT_CLICKED, NULL);
		else {
			if (kb->layer != KIT_KB_NUM)
				lv_obj_send_event(find_key(kb->layers[kb->layer], K_NUM),
						  LV_EVENT_CLICKED, NULL);
			lv_obj_send_event(find_key(kb->layers[kb->layer], K_SYM), LV_EVENT_CLICKED, NULL);
		}
	}
	key = find_key(kb->layers[kb->layer], c);
	if (!key)
		return false;
	lv_obj_send_event(key, LV_EVENT_CLICKED, NULL);
	return true;
}
