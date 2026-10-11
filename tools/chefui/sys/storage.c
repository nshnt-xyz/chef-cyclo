/*
 * storage.c - free space on /data for the hub's Storage row. The
 * ownership check and statvfs run on the settings persistence worker
 * (settings.c), never on the UI thread: a hung eMMC must not stall a tap.
 * Requested on hub entry; the result lands in sys_state.storage.
 */
#include "settings.h"
#include "sys.h"
#include "sysstate.h"

static void stat_done(bool known, bool ours, uint64_t free_bytes, void *user)
{
	(void)user;
	if (known != sys_state.storage.known || ours != sys_state.storage.ours ||
	    free_bytes != sys_state.storage.free_bytes) {
		sys_state.storage.known = known;
		sys_state.storage.ours = ours;
		sys_state.storage.free_bytes = free_bytes;
		sys_changed(SYS_STORAGE);
	}
}

void sys_storage_refresh(void)
{
	ui_store_on_stat(stat_done, NULL);
	ui_store_stat();
}
