/*
 * buttons.h - buttond client (/run/chef/buttond/buttond.sock, protocol in
 * tools/buttond/buttond.c's header) and gesture dispatch. No LVGL: covered
 * by tests/test_buttons.c over a real AF_UNIX listener.
 *
 * On connect every configured gesture is claimed ("claim G\n");
 * power.short is always among them, power.long only if the application
 * asks (buttond's power-off default must keep working), and the
 * Power+VolDown chord is never claimed. buttond's lines are JSON objects
 * (proto 1, docs/features/buttons-and-power-off.md), read across partial
 * reads with ../common/json.h: {"type":"gesture","gesture":"G"} is
 * dispatched, {"type":"key","button":"power","state":"down|held|up"}
 * goes to the key callback (no default; dropped when none is set, and an
 * unknown state or a missing button is ignored without a log), "error"
 * lines are logged, a hello with another proto is logged once, and "ok",
 * "status" and unknown types or members are ignored. When buttond goes
 * away the client reconnects with a 1 s backoff and claims again.
 *
 * Dispatch: the application's callback sees every gesture first; when it
 * does not report the gesture handled, power.short falls back to the
 * default action (the screen toggle).
 */
#ifndef CHEFUI_BUTTONS_H
#define CHEFUI_BUTTONS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CU_BTN_MAX_CLAIMS  12
#define CU_BTN_NAME_MAX    24
#define CU_BTN_RETRY_MS    1000

typedef bool (*cu_btn_app_cb)(const char *gesture, void *user);
enum cu_btn_key_state { CU_BTN_KEY_DOWN, CU_BTN_KEY_HELD, CU_BTN_KEY_UP };
typedef void (*cu_btn_key_cb)(const char *button, enum cu_btn_key_state state, void *user);
typedef void (*cu_btn_default_cb)(const char *gesture, void *user);
typedef void (*cu_btn_log_fn)(const char *fmt, ...);

struct cu_buttons {
	char sock_path[108];
	char claims[CU_BTN_MAX_CLAIMS][CU_BTN_NAME_MAX];
	int nclaims;
	int fd;				/* -1 while disconnected */
	char in[256];
	size_t inlen;
	bool discarding;		/* the rest of an over-long line */
	int64_t next_try_ms;		/* reconnect deadline, CLOCK_MONOTONIC */
	unsigned long connects;
	unsigned long closes;		/* cu_buttons_close() calls */
	bool proto_warned;		/* an unknown proto was logged */

	cu_btn_app_cb app_cb;
	void *app_user;
	cu_btn_key_cb key_cb;
	void *key_user;
	cu_btn_default_cb default_cb;
	void *default_user;
	cu_btn_log_fn log;
};

/* sock_path NULL = CHEF_BUTTOND_SOCK; extra: NULL-terminated gesture names
 * (power.short is added if absent; "power+voldown" is refused). */
void cu_buttons_init(struct cu_buttons *b, const char *sock_path, const char *const *extra,
		     cu_btn_log_fn log);

/* Try to connect now and send the claims; on failure the next try is due
 * CU_BTN_RETRY_MS later. 0 or -errno. */
int cu_buttons_connect(struct cu_buttons *b, int64_t now_ms);

/* The fd is readable (or hung up): read, parse, dispatch. On EOF/error
 * the connection is dropped and a reconnect scheduled. */
void cu_buttons_input(struct cu_buttons *b, int64_t now_ms);

/* Bytes from buttond: complete lines are handled, the rest is kept. */
void cu_buttons_feed(struct cu_buttons *b, const char *data, size_t n);

/* Deliver one gesture: application first, then the default for
 * power.short. Returns true when the application handled it. */
bool cu_buttons_dispatch(struct cu_buttons *b, const char *gesture);

/* poll() timeout contribution: -1 while connected, else ms to the retry. */
int cu_buttons_timeout_ms(const struct cu_buttons *b, int64_t now_ms);

/* Reconnect if disconnected and the retry is due. */
void cu_buttons_tick(struct cu_buttons *b, int64_t now_ms);

void cu_buttons_close(struct cu_buttons *b);

#endif /* CHEFUI_BUTTONS_H */
