/*
 * timeview.h - the Time page's source model: pure parsers and formatters,
 * and the chrony client (timeview.c).
 */
#ifndef CHEF_TIMEVIEW_H
#define CHEF_TIMEVIEW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum sys_time_source {
	SYS_TIME_UNKNOWN,
	SYS_TIME_GPS,
	SYS_TIME_NETWORK,
	SYS_TIME_RESTORED,
	SYS_TIME_FLOOR,
	SYS_TIME_NONE,
};

struct sys_time_report {
	enum sys_time_source source;
	double reference_epoch;
};

/* Digits and meridiem are separate for the narrow status-bar ear. */
bool sys_time_format(int hour, int minute, bool clock_24h, char *digits,
		     size_t digits_size, char meridiem[3]);
void sys_time_full(int hour, int minute, bool clock_24h, char *out, size_t size);
bool sys_time_parse_tracking(const char *csv, size_t len, double query_epoch,
			     struct sys_time_report *out);
enum sys_time_source sys_time_parse_boot(const char *text, size_t len);
enum sys_time_source sys_time_read_boot(const char *path);
#ifdef CHEFUI_TEST
#include <sys/types.h>
void sys_timeview_test_executable(const char *path);
void sys_timeview_test_waitpid(pid_t (*fn)(pid_t, int *, int));	/* NULL: the real one */
unsigned sys_timeview_test_kills(void);			/* SIGKILLs sent */
pid_t sys_timeview_test_last_kill(void);
bool sys_timeview_test_reap_timer(void);
#endif

#endif
