/*
 * storage_usage.h - the Storage page's category scan (storage_usage.c).
 * Synchronous and worker-only: it runs on ui-store's thread, never on the UI
 * thread, and shares no state with it.
 */
#ifndef CHEF_STORAGE_USAGE_H
#define CHEF_STORAGE_USAGE_H

#include <stdbool.h>
#include <stdint.h>

/* What the scan knows of one category. */
enum sys_usage_cat {
	USAGE_CAT_UNSCANNED,	/* never reached (a bound stopped the scan, or unreadable root) */
	USAGE_CAT_PARTIAL,	/* started, but a bound or an error cut it short */
	USAGE_CAT_EXACT,	/* read to the end (absent counts as an exact zero) */
};

struct sys_usage_result {
	bool known, ours, complete;
	enum sys_usage_cat cat_state[3];	/* maps, rides, routes */
	uint64_t total_bytes, free_bytes;
	uint64_t maps_bytes, rides_bytes, routes_bytes, other_bytes;
	uint64_t root_dev, root_ino, mount_id;
	int64_t sampled_mono_ms;
};

/* Synchronous scanner. Call only from ui-store's worker thread. mountpoint
 * NULL checks with ui_mountpoint() (st_dev against the parent). */
void sys_storage_usage_scan(const char *data,
			    bool (*mountpoint)(const char *path),
			    struct sys_usage_result *out);
struct sys_usage_limits {
	unsigned max_depth, max_entries, max_keys;
	int64_t budget_ns;
	uint64_t mismatch_ino; /* force this fixture inode onto another mount ID */
	/* called with each entry's name before it is opened (a fixture removes it) */
	void (*before_open)(const char *name, void *user);
	void *user;
};
#ifdef CHEFUI_TEST
void sys_storage_usage_scan_limited(const char *data,
				    bool (*mountpoint)(const char *path),
				    const struct sys_usage_limits *limits,
				    struct sys_usage_result *out);
#endif

#endif
