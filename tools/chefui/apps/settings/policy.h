/*
 * policy.h - the system UI's screen policy, as pure decisions (no LVGL,
 * no clock): what a short Power press does, and when menus auto-sleep.
 *
 *  - Power short with the screen off: the platform default (wake).
 *  - Power short on the hub: the platform default (screen off).
 *  - Power short on any other page: Back (handled, the screen stays on).
 *  - Auto-sleep: any touch, handled button or wake restarts the
 *    inactivity time; after sleep_ms of none the screen goes off.
 *    sleep_ms 0 = Never. Waking shows the page that was open.
 */
#ifndef CHEF_POLICY_H
#define CHEF_POLICY_H

#include <stdbool.h>
#include <stdint.h>

enum power_action { POWER_DEFAULT, POWER_BACK };

enum power_action policy_power_short(bool screen_on, int depth);

struct sleep_policy {
	int sleep_ms;		/* 0 = never */
	int64_t last_ms;	/* last activity */
};

void policy_activity(struct sleep_policy *p, int64_t now_ms);
void policy_set_sleep(struct sleep_policy *p, int sleep_ms, int64_t now_ms);
/* Screen on and idle for sleep_ms: turn it off. */
bool policy_should_sleep(const struct sleep_policy *p, bool screen_on, int64_t now_ms);
/* ms until the next check is due (>= 0), or -1 when no check is needed. */
int64_t policy_next_check_ms(const struct sleep_policy *p, bool screen_on, int64_t now_ms);

#endif /* CHEF_POLICY_H */
