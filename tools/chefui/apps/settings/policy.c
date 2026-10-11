/*
 * policy.c - screen policy decisions (see policy.h).
 */
#include "policy.h"

enum power_action policy_power_short(bool screen_on, int depth)
{
	return screen_on && depth > 0 ? POWER_BACK : POWER_DEFAULT;
}

void policy_activity(struct sleep_policy *p, int64_t now_ms)
{
	p->last_ms = now_ms;
}

void policy_set_sleep(struct sleep_policy *p, int sleep_ms, int64_t now_ms)
{
	p->sleep_ms = sleep_ms < 0 ? 0 : sleep_ms;
	p->last_ms = now_ms;	/* a new timeout counts from now */
}

bool policy_should_sleep(const struct sleep_policy *p, bool screen_on, int64_t now_ms)
{
	return screen_on && p->sleep_ms > 0 && now_ms - p->last_ms >= p->sleep_ms;
}

int64_t policy_next_check_ms(const struct sleep_policy *p, bool screen_on, int64_t now_ms)
{
	int64_t left;

	if (!screen_on || p->sleep_ms <= 0)
		return -1;
	left = p->last_ms + p->sleep_ms - now_ms;
	return left < 0 ? 0 : left;
}
