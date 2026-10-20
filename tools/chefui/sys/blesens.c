/*
 * blesens.c - the Bluetooth pages' view of blesensd (see blesens.h): the
 * chefui wrapper of blesclient.h, whose connection runs on the chefui loop
 * through lcwatch. ble_view is a copy of the client's view, refreshed on
 * every change.
 */
#include <errno.h>
#include <string.h>

#include "blesens.h"
#include "chefui.h"
#include "lcwatch.h"
#include "sysstate.h"

struct ble_view ble_view;

static struct {
	struct blesclient bc;
	struct lcwatch w;
	bool running;
	bool want_hold, want_scan;	/* what the pages want, also before a start */
} B;

static void changed(struct blesclient *bc, void *ctx)
{
	(void)ctx;
	ble_view = bc->view;
	sys_changed(SYS_BLE);
}

int sys_ble_start(const char *sock)
{
	struct blesclient_config cfg = { .path = sock, .changed = changed, .log = chefui_log };
	int64_t now = lcwatch_now();

	if (B.running) {
		blesclient_stop(&B.bc, now);
		lcwatch_fini(&B.w);
	}
	blesclient_init(&B.bc, &cfg);
	lcwatch_init(&B.w, &B.bc.lc, NULL, NULL, NULL);
	memset(&ble_view, 0, sizeof(ble_view));
	B.running = true;
	blesclient_want(&B.bc, B.want_hold, B.want_scan, now);
	blesclient_start(&B.bc, now);
	lcwatch_sync(&B.w);
	return 0;
}

void sys_ble_stop(void)
{
	if (B.running) {
		B.running = false;
		blesclient_stop(&B.bc, lcwatch_now());
		lcwatch_fini(&B.w);
	}
	memset(&ble_view, 0, sizeof(ble_view));
	sys_changed(SYS_BLE);
}

void sys_ble_want(bool hold, bool scan)
{
	B.want_hold = hold;
	B.want_scan = scan && hold;
	if (!B.running)
		return;
	blesclient_want(&B.bc, hold, scan, lcwatch_now());
	lcwatch_sync(&B.w);
}

void sys_ble_wants(bool *hold, bool *scan)
{
	*hold = B.want_hold;
	*scan = B.want_scan;
}

int sys_ble_pair(const struct ble_found *f)
{
	int rc;

	if (!B.running)
		return -ENOTCONN;
	rc = blesclient_pair(&B.bc, f, lcwatch_now());
	lcwatch_sync(&B.w);
	return rc;
}

int sys_ble_cancel(void)
{
	int rc;

	if (!B.running)
		return 0;
	rc = blesclient_cancel(&B.bc, lcwatch_now());
	lcwatch_sync(&B.w);
	return rc;
}

void sys_ble_pair_clear(void)
{
	if (B.running)
		blesclient_pair_clear(&B.bc);
}

int sys_ble_forget(const char *addr)
{
	int rc;

	if (!B.running)
		return -ENOTCONN;
	rc = blesclient_forget(&B.bc, addr, lcwatch_now());
	lcwatch_sync(&B.w);
	return rc;
}
