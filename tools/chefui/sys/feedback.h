/*
 * feedback.h - UI haptics (feedback.c): a pulse on the vibrator from a
 * dedicated worker, requested without waiting from the UI thread.
 */
#ifndef CHEF_FEEDBACK_H
#define CHEF_FEEDBACK_H

#include <stdbool.h>
#include <stdint.h>

int sys_feedback_start(const char *sysfs_path, bool enabled);
void sys_feedback_enabled(bool enabled);
void sys_feedback_screen_allowed(bool allowed);
bool sys_feedback_request(void);
void sys_feedback_stop(void);
#ifdef CHEFUI_TEST
bool sys_feedback_test_signals_blocked(void);
unsigned sys_feedback_test_calls(void);	/* requests asked for, accepted or not */
uint64_t sys_feedback_test_generation(void);
bool sys_feedback_test_busy(void);	/* the current worker holds a record */
unsigned sys_feedback_test_live_workers(void);	/* started workers not yet exited */
unsigned sys_feedback_test_opens(void);	/* write_pulse() opens begun */
void sys_feedback_test_reset_rate(void);	/* clear the 100 ms request gap */
#endif

#endif
