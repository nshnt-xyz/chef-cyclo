/*
 * chefui_internal.h - what chefui.c shares with the backends
 * (backend_fb.c on the phone, backend_sdl.c on the PC) and the tests.
 * Not for applications.
 */
#ifndef CHEFUI_INTERNAL_H
#define CHEFUI_INTERNAL_H

#include <poll.h>
#include <stdint.h>

#include "chefui.h"

/* One-second counters for CHEFUI_STATS; the backends add to them. */
struct cu_stats {
	unsigned frames;
	unsigned long px;		/* pixels copied into fb pages */
	double copy_ms;
	double pan_ms, pan_max_ms;
	unsigned wakeups;		/* poll() returns */
};
extern struct cu_stats cu_stats;
extern unsigned long cu_total_wakeups;	/* since start (tests) */

struct cu_backend {
	const char *name;
	enum chefui_backend kind;
	bool uses_buttond;
	/* Create the display (and pointer indev). 0 or -errno. */
	int (*init)(const struct chefui_config *cfg, lv_display_t **disp, lv_indev_t **pointer);
	int (*screen_set)(bool on);
	bool (*screen_is_on)(void);
	int (*brightness_set)(int level);
	int (*brightness_get)(void);
	int (*handoff_exec)(const char *path, char *const argv[], bool dark);
	void (*shutdown)(void);
	/* poll() integration: add fds (returns how many), adjust the
	 * timeout, then handle the results (called after every poll, also
	 * on timeout, with the same slice). */
	int (*add_pollfds)(struct pollfd *pfd, int max);
	int (*timeout_ms)(int timeout);
	void (*handle_pollfds)(const struct pollfd *pfd, int n);
};

/* NULL-terminated, defined per build (backends_device.c / backends_host.c). */
extern const struct cu_backend *const cu_backends[];

/* chefui.c services for the backends */
void cu_dispatch_gesture(const char *gesture);	/* app first, then the default */
void cu_notify_touch(const struct chefui_contact *c, int n);
void cu_notify_screen(bool on);
uint32_t cu_refresh_period(void);
int64_t cu_now_ms(void);

/* Test hooks: alternative paths, set before chefui_init(). NULL = keep. */
struct cu_test_paths {
	const char *fb, *lock, *flag, *backlight, *modes;
	const char *input_dir;		/* instead of /dev/input */
	const char *buttond_sock;
	const char *kmsg;		/* instead of /dev/kmsg */
};
void cu_set_test_paths(const struct cu_test_paths *p);
extern struct cu_test_paths cu_paths;

#endif /* CHEFUI_INTERNAL_H */
