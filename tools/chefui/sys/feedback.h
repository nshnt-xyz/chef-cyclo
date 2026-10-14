/*
 * feedback.h - UI haptics (feedback.c): a pulse on the vibrator from a
 * dedicated worker, requested without waiting from the UI thread.
 */
#ifndef CHEF_FEEDBACK_H
#define CHEF_FEEDBACK_H

#include <stdbool.h>

int sys_feedback_start(const char *sysfs_path, bool enabled);
void sys_feedback_enabled(bool enabled);
void sys_feedback_screen_allowed(bool allowed);
bool sys_feedback_request(void);
void sys_feedback_stop(void);
#ifdef CHEFUI_TEST
bool sys_feedback_test_signals_blocked(void);
unsigned sys_feedback_test_calls(void);	/* requests asked for, accepted or not */
#endif

#endif
