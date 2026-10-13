/*
 * pages.h - the page interface inside chef-settings: each page kind has
 * ops (page_*.c), the stack in app.c builds, notifies and deletes them.
 *
 * A page is "visible" while it is the top of the stack and the screen is
 * on; on_visible tells it when that changes (after the stack changed), so
 * a page can hold resources (scans, leases) only while it is seen.
 */
#ifndef CHEF_SETTINGS_PAGES_H
#define CHEF_SETTINGS_PAGES_H

#include <stdbool.h>

#include "app.h"
#include "widgets.h"

struct page_ops {
	void (*build)(struct page *p, const void *arg);
	void (*destroy)(struct page *p);		/* free p->priv (may be NULL) */
	void (*on_state)(struct page *p, unsigned what);	/* SYS_* bits */
	void (*on_visible)(struct page *p, bool visible);
	void (*on_snapshot)(struct page *p, bool begin);
	void (*on_settings)(struct page *p);		/* app.set replaced */
};

struct page {
	enum app_page kind;
	const struct page_ops *ops;
	struct kit_page kp;
	void *priv;
};

extern const struct page_ops page_display_ops, page_wifi_ops, page_wifi_password_ops,
	page_wifi_hidden_ops, page_wifi_details_ops, page_gps_ops, page_gps_sats_ops;

/* Shared helpers (app.c). */
void page_help_text(lv_obj_t *parent, const char *s);
bool app_page_visible(const struct page *p);

#endif /* CHEF_SETTINGS_PAGES_H */
