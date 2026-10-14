/*
 * storage.c - free space on /data for the hub's Storage row. The
 * ownership check and statvfs run on the settings persistence worker
 * (settings.c), never on the UI thread: a hung eMMC must not stall a tap.
 * Requested on hub entry; the result lands in sys_state.storage.
 */
#include "settings.h"
#include "sys.h"
#include "sysstate.h"
#include "storage_usage.h"
#include <time.h>

static bool usage_visible;

static int64_t mono_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* The usage result is the newest sample (it runs after the STAT on the single
 * worker queue and samples capacity again at its end), so its capacity
 * replaces the cheap one and the page stays coherent. A failure or lost
 * ownership leaves no snapshot; nothing is requested again from here (the
 * page shows the honest state from the copied cheap fields). */
static void usage_done(const struct sys_usage_result *u, void *user)
{
	(void)user;
	sys_state.storage.known = u->known;
	sys_state.storage.ours = u->ours;
	sys_state.storage.total_bytes = u->total_bytes;
	sys_state.storage.free_bytes = u->free_bytes;
	sys_state.storage.root_dev = u->root_dev;
	sys_state.storage.root_ino = u->root_ino;
	if (u->known && u->ours) {
		sys_state.storage.usage_known = true;
		sys_state.storage.usage_complete = u->complete;
		sys_state.storage.usage_maps = u->maps_bytes;
		sys_state.storage.usage_rides = u->rides_bytes;
		sys_state.storage.usage_routes = u->routes_bytes;
		sys_state.storage.usage_other = u->other_bytes;
		sys_state.storage.usage_total_bytes = u->total_bytes;
		sys_state.storage.usage_free_bytes = u->free_bytes;
		sys_state.storage.usage_root_dev = u->root_dev;
		sys_state.storage.usage_root_ino = u->root_ino;
		sys_state.storage.usage_mount_id = u->mount_id;
		for (int i = 0; i < 3; i++)
			sys_state.storage.usage_cat[i] = u->cat_state[i];
		sys_state.storage.usage_updated_mono_ms = u->sampled_mono_ms;
	} else {
		sys_state.storage.usage_known = false;
		sys_state.storage.usage_complete = false;
	}
	sys_changed(SYS_STORAGE);
}

static void stat_done(const struct ui_store_stat_result *s, void *user)
{
	bool changed, invalid;
	(void)user;
	invalid = sys_state.storage.usage_known &&
		(!s->known || !s->ours || s->total_bytes != sys_state.storage.usage_total_bytes ||
		 s->free_bytes != sys_state.storage.usage_free_bytes ||
		 s->root_dev != sys_state.storage.usage_root_dev ||
		 s->root_ino != sys_state.storage.usage_root_ino);
	changed = s->known != sys_state.storage.known || s->ours != sys_state.storage.ours ||
		s->free_bytes != sys_state.storage.free_bytes ||
		s->total_bytes != sys_state.storage.total_bytes ||
		s->root_dev != sys_state.storage.root_dev || s->root_ino != sys_state.storage.root_ino;
	sys_state.storage.known = s->known;
	sys_state.storage.ours = s->ours;
	sys_state.storage.free_bytes = s->free_bytes;
	sys_state.storage.total_bytes = s->total_bytes;
	sys_state.storage.root_dev = s->root_dev;
	sys_state.storage.root_ino = s->root_ino;
	if (invalid) {
		sys_state.storage.usage_known = false;
		sys_state.storage.usage_complete = false;
	}
	if (changed || invalid)
		sys_changed(SYS_STORAGE);
	if (usage_visible && (!sys_state.storage.usage_known ||
	    mono_ms() - sys_state.storage.usage_updated_mono_ms >= 30000))
		ui_store_usage();
}

#ifdef CHEFUI_TEST
void sys_storage_test_apply_stat(const struct ui_store_stat_result *stat)
{
	stat_done(stat, NULL);
}

void sys_storage_test_apply_usage(const struct sys_usage_result *usage)
{
	usage_done(usage, NULL);
}
#endif

void sys_storage_refresh(void)
{
	ui_store_on_stat(stat_done, NULL);
	ui_store_stat();
}

void sys_storage_usage_want(bool visible)
{
	usage_visible = visible;
	if (visible) {
		sys_storage_refresh();
	} else {
		ui_store_usage_cancel();
	}
}

void sys_storage_usage_init(void)
{
	ui_store_on_usage(usage_done, NULL);
}
