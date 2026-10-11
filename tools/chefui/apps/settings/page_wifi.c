/*
 * page_wifi.c - the Wi-Fi pages: the switch and network list (P15 off,
 * P16 on), the password page (P17), a hidden-network page and the
 * connection details (P18). State comes from wifi_view (wifi.h), which
 * is only kept current, and scanned, while one of these pages is seen:
 * every page calls wifi_sync() when its visibility changes.
 *
 * Left out by decision (docs/next-steps/system-ui-2-handoff.md): the
 * Sync sections (ride upload, maps, routes), "internet OK" unless
 * NetworkManager's connectivity check runs, and the signal in dBm.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "fonts.h"
#include "icons.h"
#include "pages.h"
#include "sysstate.h"
#include "wifi.h"

#define DOT " \xC2\xB7 "	/* " · " */
#define ELLIPSIS "\xE2\x80\xA6"	/* "…" */
#define NONE "\xE2\x80\x93"	/* "–": no value */
#define FORGET_ARM_MS 4000

static bool is_wifi_page(enum app_page k)
{
	return k == APP_PAGE_WIFI || k == APP_PAGE_WIFI_PASSWORD || k == APP_PAGE_WIFI_HIDDEN ||
	       k == APP_PAGE_WIFI_DETAILS;
}

/* NetworkManager is followed while a Wi-Fi page is seen; the list scans. */
static void wifi_sync(void)
{
	struct page *top = app_top_page();
	bool watch = app.pf.screen_is_on() && is_wifi_page(top->kind);

	sys_wifi_watch(watch, watch && top->kind == APP_PAGE_WIFI && sys_wifi_on(&sys_state));
}

static const char *signal_glyph(int strength)
{
	int l = wifi_signal_level(strength);

	return l == 2 ? KIT_IC_WIFI_HIGH : l == 1 ? KIT_IC_WIFI_LOW : KIT_IC_WIFI_ZERO;
}

/* A join's outcome as a line ("" while there is none). */
static void join_text(char *buf, size_t n)
{
	const char *t = wifi_view.join.text;

	switch (wifi_view.join.st) {
	case WIFI_JOIN_BUSY: snprintf(buf, n, "Joining %s" ELLIPSIS, t); break;
	case WIFI_JOIN_BAD_PASSWORD: snprintf(buf, n, "Wrong password for %s", t); break;
	case WIFI_JOIN_FAILED: snprintf(buf, n, "Could not join %s", t); break;
	default: buf[0] = 0; break;
	}
}

/* 40 px help line under a control (padding-top 28), muted or warn. */
static lv_obj_t *help_line(lv_obj_t *parent, const char *s)
{
	lv_obj_t *l = kit_text(parent, &chefkit_b400_40, 0, KIT_INK_MUT, s);
	lv_obj_t *box = lv_obj_get_parent(l);

	lv_obj_set_style_pad_top(box, 28, 0);
	lv_obj_set_height(box, 28 + kit_css_line(&chefkit_b400_40));
	kit_text_single_line(l);
	return l;
}

static void help_set(lv_obj_t *l, const char *s, bool warn)
{
	if (strcmp(lv_label_get_text(l), s))
		lv_label_set_text(l, s);
	kit_set_ink(l, warn ? KIT_INK_WARN : KIT_INK_MUT);
}

/* ================================================================ list */

struct wifi_page {
	struct page *p;
	struct kit_master master;
	struct kit_empty empty;
	lv_obj_t *list, *conn_section;
	struct kit_card card;
	struct kit_section_action avail;
	struct kit_row rows[WIFI_NETS_MAX];
	int n_rows;			/* rows built (some may be hidden) */
	struct wifi_net shown[WIFI_NETS_MAX];
	int n_shown;
	lv_obj_t *none;			/* "No networks found" (its line box) */
	struct kit_row hidden;
	char result[WIFI_SSID_TEXT + 40];	/* the last join outcome */
};

static void list_refresh(struct wifi_page *w);

static void master_changed(lv_event_t *e)
{
	struct wifi_page *w = lv_event_get_user_data(e);
	bool on = kit_switch_get(w->master.sw);

	w->result[0] = 0;
	if (sys_wifi_set_enabled(on) < 0)
		kit_switch_set(w->master.sw, !on);	/* no NetworkManager to ask */
}

static void card_clicked(lv_event_t *e)
{
	(void)e;
	if (wifi_view.cur.valid && wifi_view.cur.uuid[0])
		app_open(APP_PAGE_WIFI_DETAILS, wifi_view.cur.text);
}

static void net_clicked(lv_event_t *e)
{
	struct wifi_page *w = lv_event_get_user_data(e);
	int i = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
	struct wifi_net n;

	if (i < 0 || i >= w->n_shown || wifi_view.join.st == WIFI_JOIN_BUSY)
		return;
	n = w->shown[i];
	w->result[0] = 0;
	if (!wifi_sec_joinable(n.sec)) {
		snprintf(w->result, sizeof(w->result), "%s networks are not supported",
			 wifi_sec_short(n.sec));
	} else if (n.known || n.sec == WIFI_SEC_OPEN) {
		if (sys_wifi_join(&n) < 0)
			snprintf(w->result, sizeof(w->result), "Could not join %s", n.text);
	} else {
		app_open_arg(APP_PAGE_WIFI_PASSWORD, n.text, &n);
		return;
	}
	list_refresh(w);
}

static void hidden_clicked(lv_event_t *e)
{
	(void)e;
	if (wifi_view.join.st != WIFI_JOIN_BUSY)
		app_open(APP_PAGE_WIFI_HIDDEN, "Hidden network");
}

static void rows_refresh(struct wifi_page *w)
{
	char sub[96];
	int i;

	w->n_shown = wifi_view.n;
	memcpy(w->shown, wifi_view.nets, (size_t)w->n_shown * sizeof(w->shown[0]));
	for (i = 0; i < w->n_shown; i++) {
		const struct wifi_net *n = &w->shown[i];

		wifi_net_subtitle(n, sub, sizeof(sub));
		if (i == w->n_rows) {
			struct kit_row *r = &w->rows[i];

			kit_list_row_create(r, w->list, signal_glyph(n->strength), n->text, sub,
					    &chefkit_icon_56, NULL, net_clicked, w);
			lv_obj_set_user_data(r->obj, (void *)(intptr_t)i);
			/* before "No networks found" and "Add hidden network" */
			lv_obj_move_to_index(r->obj, lv_obj_get_index(w->none));
			w->n_rows++;
		}
		kit_row_set(&w->rows[i], signal_glyph(n->strength), n->text, sub,
			    n->sec == WIFI_SEC_OPEN ? NULL : KIT_IC_LOCK);
		lv_obj_set_hidden(w->rows[i].obj, false);
	}
	for (; i < w->n_rows; i++)
		lv_obj_set_hidden(w->rows[i].obj, true);
}

static void list_refresh(struct wifi_page *w)
{
	const struct sys_state *s = &sys_state;
	bool on = sys_wifi_on(s);
	char sub[WIFI_SSID_TEXT + 40], card_sub[96];

	/* a join outcome stays until the next action on this page */
	if (wifi_view.join.st == WIFI_JOIN_BUSY) {
		join_text(sub, sizeof(sub));
	} else if (wifi_view.join.st != WIFI_JOIN_IDLE && app_page_visible(w->p)) {
		join_text(w->result, sizeof(w->result));
		if (wifi_view.join.st == WIFI_JOIN_DONE)
			w->result[0] = 0;
		sys_wifi_join_ack();
	}
	if (wifi_view.join.st != WIFI_JOIN_BUSY)
		snprintf(sub, sizeof(sub), "%s", w->result[0] ? w->result
			 : !s->wifi.known ? "Unavailable" : !s->wifi.enabled ? "Off"
			 : !s->wifi.device ? "No device" : "On");
	kit_master_set(&w->master, s->wifi.known && s->wifi.enabled, sub);
	lv_obj_set_disabled(w->master.sw, !s->wifi.known);

	lv_obj_set_hidden(w->empty.obj, on);
	lv_obj_set_hidden(w->list, !on);
	if (!on) {
		if (!s->wifi.known)
			kit_empty_set(&w->empty, KIT_IC_WIFI_OFF, "Wi-Fi unavailable",
				      "NetworkManager is not running.");
		else if (!s->wifi.enabled)
			kit_empty_set(&w->empty, KIT_IC_WIFI_OFF, "Wi-Fi is off",
				      "Turn it on to see networks nearby.");
		else
			kit_empty_set(&w->empty, KIT_IC_WIFI_OFF, "No Wi-Fi device",
				      "The Wi-Fi chip is not ready yet.");
		return;
	}
	lv_obj_set_hidden(w->conn_section, !wifi_view.cur.valid);
	lv_obj_set_hidden(w->card.obj, !wifi_view.cur.valid);
	if (wifi_view.cur.valid) {
		wifi_active_subtitle(&wifi_view.cur, card_sub, sizeof(card_sub));
		kit_card_set(&w->card, KIT_IC_WIFI, wifi_view.cur.text, card_sub);
	}
	kit_section_action_show(&w->avail, wifi_view.scanning);
	rows_refresh(w);
	lv_obj_set_hidden(w->none, wifi_view.n > 0 || wifi_view.scanning);
}

static void wifi_build(struct page *p, const void *arg)
{
	struct wifi_page *w = calloc(1, sizeof(*w));
	lv_obj_t *body = p->kp.body;

	(void)arg;
	p->priv = w;
	if (!w)
		return;
	w->p = p;
	kit_page_scrollable(&p->kp);
	kit_master_create(&w->master, body, "Wi-Fi", master_changed, w);
	kit_empty_create(&w->empty, body, KIT_IC_WIFI_OFF, "", "");

	w->list = kit_box(body);
	lv_obj_set_size(w->list, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(w->list, LV_FLEX_FLOW_COLUMN);
	w->conn_section = lv_obj_get_parent(kit_section(w->list, "CONNECTED", 64, 28));
	kit_card_create(&w->card, w->list, KIT_IC_WIFI, card_clicked, w);
	kit_section_action_create(&w->avail, w->list, "AVAILABLE", 72, 8, KIT_IC_REFRESH_CW,
				  "Scanning");
	w->none = lv_obj_get_parent(help_line(w->list, "No networks found"));
	kit_action_row_create(&w->hidden, w->list, KIT_IC_PLUS, "Add hidden network",
			      hidden_clicked, w);
	list_refresh(w);
}

static void wifi_destroy(struct page *p)
{
	free(p->priv);
}

static void wifi_state(struct page *p, unsigned what)
{
	if (!p->priv || !(what & (SYS_WIFI | SYS_WIFI_VIEW)))
		return;
	if ((what & SYS_WIFI) && app_page_visible(p))
		wifi_sync();		/* scanning follows the switch */
	list_refresh(p->priv);
}

static void wifi_visible(struct page *p, bool visible)
{
	wifi_sync();
	if (visible && p->priv)
		list_refresh(p->priv);
}

const struct page_ops page_wifi_ops = {
	.build = wifi_build,
	.destroy = wifi_destroy,
	.on_state = wifi_state,
	.on_visible = wifi_visible,
};

/* ====================================================== password entry */

/* The password page and the hidden-network page share this. */
struct entry_page {
	struct page *p;
	struct wifi_net net;		/* password page: the network */
	bool hidden_net;		/* hidden page */
	char psk[WIFI_PSK_MAX + 1];
	char ssid[WIFI_SSID_MAX + 1];
	struct kit_field pass, name;
	struct kit_field *focus;
	lv_obj_t *eye, *help, *pass_section, *pass_row;
	struct kit_seg sec;
	struct kit_kb kb;
	bool joining;
};

static const enum wifi_sec hidden_secs[] = { WIFI_SEC_OPEN, WIFI_SEC_WPA2, WIFI_SEC_WPA3 };

static enum wifi_sec entry_sec(const struct entry_page *ep)
{
	if (!ep->hidden_net)
		return ep->net.sec;
	return ep->sec.active >= 0 && ep->sec.active < 3 ? hidden_secs[ep->sec.active] : WIFI_SEC_WPA2;
}

static void entry_join(struct entry_page *ep)
{
	enum wifi_sec sec = entry_sec(ep);
	size_t len = ep->pass.len;
	int r;

	if (ep->hidden_net && !ep->name.len) {
		help_set(ep->help, "Enter the network name.", true);
		return;
	}
	if (wifi_sec_needs_psk(sec) && (len < WIFI_PSK_MIN || len > WIFI_PSK_MAX)) {
		help_set(ep->help, "Use 8 to 63 characters.", true);
		return;
	}
	if (ep->hidden_net) {
		struct wifi_ssid ssid = { .len = (int)ep->name.len };

		memcpy(ssid.b, ep->ssid, ep->name.len);
		if (!wifi_sec_needs_psk(sec))
			kit_field_clear(&ep->pass);
		r = sys_wifi_join_new(&ssid, sec, ep->psk, true, NULL);
	} else {
		r = sys_wifi_join_new(&ep->net.ssid, sec, ep->psk, false, ep->net.ap);
	}
	kit_field_clear(&ep->pass);		/* the passphrase is gone from here */
	if (r < 0) {
		sys_wifi_join_ack();
		help_set(ep->help, "Could not start joining. Try again.", true);
		return;
	}
	ep->joining = true;
	help_set(ep->help, "Joining" ELLIPSIS, false);
}

static void entry_key(int key, void *user)
{
	struct entry_page *ep = user;

	if (ep->joining)
		return;
	if (key == KIT_KEY_ACTION)
		entry_join(ep);
	else if (key == KIT_KEY_BACKSPACE)
		kit_field_backspace(ep->focus);
	else
		kit_field_put(ep->focus, (char)key);
}

static void eye_clicked(lv_event_t *e)
{
	struct entry_page *ep = lv_event_get_user_data(e);
	bool hidden = !ep->pass.hidden;

	kit_field_set_hidden(&ep->pass, hidden);
	/* the button offers the other state: eye shows, eye-off hides */
	lv_label_set_text(ep->eye, hidden ? KIT_IC_EYE : KIT_IC_EYE_OFF);
}

static void field_clicked(lv_event_t *e)
{
	struct entry_page *ep = lv_event_get_user_data(e);
	struct kit_field *f = lv_event_get_current_target(e) == ep->name.obj ? &ep->name : &ep->pass;

	if (f == ep->focus)
		return;
	ep->focus = f;
	kit_field_set_focus(&ep->name, f == &ep->name);
	kit_field_set_focus(&ep->pass, f == &ep->pass);
}

/* Field and eye button side by side (gap 24). */
static lv_obj_t *pass_row(struct entry_page *ep, lv_obj_t *parent)
{
	lv_obj_t *row = kit_box(parent), *eye;

	lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(row, 24, 0);
	kit_field_create(&ep->pass, row, ep->psk, sizeof(ep->psk));
	lv_obj_set_flex_grow(ep->pass.obj, 1);
	lv_obj_add_event_cb(ep->pass.obj, field_clicked, LV_EVENT_CLICKED, ep);
	/* 192 x 192, border 3 line2, radius 36, 72 px glyph */
	eye = kit_box(row);
	lv_obj_set_size(eye, 192, 192);
	lv_obj_add_style(eye, &kit_st.outline, 0);
	lv_obj_set_clickable(eye, true);
	lv_obj_set_event_bubble(eye, false);
	lv_obj_add_event_cb(eye, eye_clicked, LV_EVENT_CLICKED, ep);
	ep->eye = kit_icon(eye, &chefkit_icon_72, 186, KIT_INK_FG, KIT_IC_EYE_OFF);
	return row;
}

static void entry_section(lv_obj_t *parent, const char *text, int pad_top)
{
	kit_section(parent, text, pad_top, 24);
}

static void password_build(struct page *p, const void *arg)
{
	struct entry_page *ep = calloc(1, sizeof(*ep));
	char label[64];

	p->priv = ep;
	if (!ep)
		return;
	ep->p = p;
	if (arg)
		ep->net = *(const struct wifi_net *)arg;
	kit_page_set_back_glyph(&p->kp, KIT_IC_X);
	kit_page_title_fit(&p->kp);
	snprintf(label, sizeof(label), "PASSWORD" DOT "%s", wifi_sec_short(ep->net.sec));
	entry_section(p->kp.body, label, 72);
	pass_row(ep, p->kp.body);
	ep->focus = &ep->pass;
	ep->help = help_line(p->kp.body, "Saved on device. Never shown in logs.");
	kit_kb_create(&ep->kb, p->kp.obj, "Join", entry_key, ep);
}

static void sec_changed(lv_event_t *e)
{
	struct entry_page *ep = lv_event_get_user_data(e);
	bool psk = wifi_sec_needs_psk(entry_sec(ep));

	lv_obj_set_hidden(ep->pass_section, !psk);
	lv_obj_set_hidden(ep->pass_row, !psk);
	if (!psk && ep->focus == &ep->pass) {
		ep->focus = &ep->name;
		kit_field_set_focus(&ep->name, true);
		kit_field_set_focus(&ep->pass, false);
	}
}

static void hidden_build(struct page *p, const void *arg)
{
	static const char *const labels[] = { "Open", "WPA2", "WPA3" };
	struct entry_page *ep = calloc(1, sizeof(*ep));
	lv_obj_t *body = p->kp.body;

	(void)arg;
	p->priv = ep;
	if (!ep)
		return;
	ep->p = p;
	ep->hidden_net = true;
	kit_page_set_back_glyph(&p->kp, KIT_IC_X);
	entry_section(body, "NETWORK NAME", 72);
	kit_field_create(&ep->name, body, ep->ssid, sizeof(ep->ssid));
	lv_obj_set_width(ep->name.obj, lv_pct(100));
	lv_obj_add_event_cb(ep->name.obj, field_clicked, LV_EVENT_CLICKED, ep);
	entry_section(body, "SECURITY", 56);
	kit_seg_create(&ep->sec, body, 3, labels, NULL);
	kit_seg_set_active(&ep->sec, 1);
	lv_obj_add_event_cb(ep->sec.obj, sec_changed, LV_EVENT_VALUE_CHANGED, ep);
	ep->pass_section = lv_obj_get_parent(kit_section(body, "PASSWORD", 56, 24));
	ep->pass_row = pass_row(ep, body);
	ep->focus = &ep->name;
	kit_field_set_focus(&ep->pass, false);
	ep->help = help_line(body, "Saved on device. Never shown in logs.");
	kit_kb_create(&ep->kb, p->kp.obj, "Join", entry_key, ep);
}

static void entry_destroy(struct page *p)
{
	struct entry_page *ep = p->priv;

	if (ep) {
		wifi_wipe(ep->psk, sizeof(ep->psk));
		wifi_wipe(ep->pass.dots, sizeof(ep->pass.dots));
		free(ep);
	}
}

static void entry_state(struct page *p, unsigned what)
{
	struct entry_page *ep = p->priv;
	enum wifi_join st = wifi_view.join.st;

	if (!ep || !(what & SYS_WIFI_VIEW) || !ep->joining || st == WIFI_JOIN_BUSY)
		return;
	ep->joining = false;
	sys_wifi_join_ack();
	if (st == WIFI_JOIN_DONE)
		app_back();		/* the list shows the connection */
	else if (st == WIFI_JOIN_BAD_PASSWORD)
		help_set(ep->help, "Wrong password. Try again.", true);
	else
		help_set(ep->help, "Could not join. Try again.", true);
}

static void entry_visible(struct page *p, bool visible)
{
	(void)p;
	(void)visible;
	wifi_sync();
}

static void entry_snapshot(struct page *p, bool begin)
{
	struct entry_page *ep = p->priv;

	if (ep)
		kit_field_mask(&ep->pass, begin);
}

const struct page_ops page_wifi_password_ops = {
	.build = password_build,
	.destroy = entry_destroy,
	.on_state = entry_state,
	.on_visible = entry_visible,
	.on_snapshot = entry_snapshot,
};

const struct page_ops page_wifi_hidden_ops = {
	.build = hidden_build,
	.destroy = entry_destroy,
	.on_state = entry_state,
	.on_visible = entry_visible,
	.on_snapshot = entry_snapshot,
};

/* Type into the open entry page (host captures and tests). */
bool page_wifi_type(const char *text)
{
	struct page *p = app_top_page();
	struct entry_page *ep;

	if (p->kind != APP_PAGE_WIFI_PASSWORD && p->kind != APP_PAGE_WIFI_HIDDEN)
		return false;
	ep = p->priv;
	for (; *text; text++)
		if (!kit_kb_type(&ep->kb, (unsigned char)*text))
			return false;
	kit_kb_set_layer(&ep->kb, KIT_KB_LOWER);	/* as the design shows it */
	return true;
}

/* ============================================================= details */

struct details_page {
	struct page *p;
	char uuid[WIFI_UUID_LEN + 1];	/* the profile this page is for */
	struct kit_steps steps;
	lv_obj_t *signal, *band, *security, *ip, *gw;
	lv_obj_t *forget;		/* the button's label */
	lv_timer_t *disarm;
};

static void details_refresh(struct details_page *d)
{
	const struct wifi_active *c = &wifi_view.cur;
	char b[64];

	kit_steps_set(&d->steps, 0, c->dev_state >= 70);	/* IP_CONFIG and on: joined */
	kit_steps_set(&d->steps, 1, c->ip4[0] != 0);
	/* the Internet step only while NetworkManager checks connectivity */
	kit_steps_show(&d->steps, 2, c->connectivity_known);
	kit_steps_set(&d->steps, 2, c->connectivity_known && c->connectivity == 4);
	if (c->ap_known)
		snprintf(b, sizeof(b), "%d%%" DOT "%s", c->strength, wifi_signal_word(c->strength));
	else
		snprintf(b, sizeof(b), NONE);
	kit_kv_set(d->signal, b);
	if (c->ap_known && *wifi_band(c->freq))
		snprintf(b, sizeof(b), "%s" DOT "channel %d", wifi_band(c->freq), wifi_channel(c->freq));
	else
		snprintf(b, sizeof(b), NONE);
	kit_kv_set(d->band, b);
	kit_kv_set(d->security, c->ap_known ? wifi_sec_long(c->sec) : NONE);
	kit_kv_set(d->ip, c->ip4[0] ? c->ip4 : NONE);
	kit_kv_set(d->gw, c->gw[0] ? c->gw : NONE);
}

static void disarm_fired(lv_timer_t *t)
{
	struct details_page *d = lv_timer_get_user_data(t);

	d->disarm = NULL;
	lv_label_set_text(d->forget, "Forget network");
}

static void forget_clicked(lv_event_t *e)
{
	struct details_page *d = lv_event_get_user_data(e);

	/* two taps: forgetting drops the saved password */
	if (!d->disarm) {
		lv_label_set_text(d->forget, "Tap again to forget");
		d->disarm = lv_timer_create(disarm_fired, FORGET_ARM_MS, d);
		lv_timer_set_repeat_count(d->disarm, 1);
		return;
	}
	lv_timer_delete(d->disarm);
	d->disarm = NULL;
	/* the profile this page was opened for, whatever is active now */
	if (sys_wifi_forget(d->uuid) < 0) {
		lv_label_set_text(d->forget, "Could not forget");
		return;
	}
	app_back();
}

static void details_build(struct page *p, const void *arg)
{
	static const char *const steps[] = { "Joined", "IP address", "Internet" };
	struct details_page *d = calloc(1, sizeof(*d));
	lv_obj_t *body = p->kp.body, *btn, *gap;

	(void)arg;
	p->priv = d;
	if (!d)
		return;
	d->p = p;
	snprintf(d->uuid, sizeof(d->uuid), "%s", wifi_view.cur.uuid);
	kit_page_title_fit(&p->kp);
	kit_steps_create(&d->steps, body, 72, 3, steps);
	kit_section(body, "CONNECTION", 52, 8);
	d->signal = kit_kv_row(body, "Signal", "");
	d->band = kit_kv_row(body, "Band", "");
	d->security = kit_kv_row(body, "Security", "");
	d->ip = kit_kv_row(body, "IP address", "");
	d->gw = kit_kv_row(body, "Gateway", "");
	kit_spacer(body);
	btn = kit_button_create(body, KIT_IC_TRASH_2, "Forget network", forget_clicked, d);
	d->forget = lv_obj_get_child(lv_obj_get_child(btn, 1), 0);
	gap = kit_box(body);
	lv_obj_set_size(gap, 1, 72);
	details_refresh(d);
}

static void details_destroy(struct page *p)
{
	struct details_page *d = p->priv;

	if (d && d->disarm)
		lv_timer_delete(d->disarm);
	free(d);
}

static void details_state(struct page *p, unsigned what)
{
	struct details_page *d = p->priv;

	if (!d || !(what & SYS_WIFI_VIEW))
		return;
	/* disconnected, forgotten or another network now: this page is about
	 * a connection that is gone, so it closes (Forget never reaches a
	 * replacement: it names this page's profile) */
	if (!wifi_view.cur.valid || strcmp(wifi_view.cur.uuid, d->uuid)) {
		if (d->disarm) {
			lv_timer_delete(d->disarm);
			d->disarm = NULL;
		}
		if (app_top_page() == p)
			app_back();
		return;
	}
	details_refresh(d);
}

const struct page_ops page_wifi_details_ops = {
	.build = details_build,
	.destroy = details_destroy,
	.on_state = details_state,
	.on_visible = entry_visible,
};
