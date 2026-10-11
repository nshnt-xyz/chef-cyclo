/*
 * clock.c - local time for the status bar. A CLOCK_REALTIME timerfd armed
 * for the next minute boundary with TFD_TIMER_CANCEL_ON_SET wakes once a
 * minute and also at once when the clock is set (chrony's first step after
 * the 1970 boot clock, chef-state's restore). The zone is a POSIX TZ
 * string (no tzdata).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "chefui.h"
#include "sys.h"
#include "sysstate.h"

static int clock_fd = -1;

static void arm(void)
{
	struct itimerspec its = { 0 };
	struct timespec now;

	clock_gettime(CLOCK_REALTIME, &now);
	its.it_value.tv_sec = (now.tv_sec / 60 + 1) * 60;
	if (timerfd_settime(clock_fd, TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET, &its, NULL))
		chefui_log("clock: timerfd_settime: %s", strerror(errno));
}

void sys_clock_refresh(void)
{
	char hhmm[sizeof(sys_state.clock.hhmm)], zone[sizeof(sys_state.clock.zone)];
	time_t t = time(NULL);
	struct tm tm;

	if (!localtime_r(&t, &tm)) {
		snprintf(hhmm, sizeof(hhmm), "--:--");
		zone[0] = 0;
	} else {
		strftime(hhmm, sizeof(hhmm), "%H:%M", &tm);
		if (!strftime(zone, sizeof(zone), "%Z", &tm))
			zone[0] = 0;
	}
	if (strcmp(hhmm, sys_state.clock.hhmm) || strcmp(zone, sys_state.clock.zone)) {
		memcpy(sys_state.clock.hhmm, hhmm, sizeof(hhmm));
		memcpy(sys_state.clock.zone, zone, sizeof(zone));
		sys_changed(SYS_CLOCK);
	}
}

static void readable(int fd, short revents, void *user)
{
	uint64_t n;

	(void)revents;
	(void)user;
	/* ECANCELED: the clock was set; either way re-read and re-arm */
	if (read(fd, &n, sizeof(n)) < 0 && errno != ECANCELED && errno != EAGAIN)
		chefui_log("clock: timerfd read: %s", strerror(errno));
	sys_clock_refresh();
	arm();
}

void sys_clock_set_tz(const char *tz)
{
	setenv("TZ", tz && *tz ? tz : "UTC0", 1);
	tzset();
	sys_clock_refresh();
}

int sys_clock_start(const char *tz)
{
	clock_fd = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK | TFD_CLOEXEC);
	if (clock_fd < 0) {
		int e = -errno;

		chefui_log("clock: timerfd_create: %s", strerror(errno));
		return e;
	}
	chefui_watch_fd(clock_fd, POLLIN, readable, NULL);
	sys_clock_set_tz(tz);
	arm();
	return 0;
}

void sys_clock_stop(void)
{
	if (clock_fd >= 0) {
		chefui_unwatch_fd(clock_fd);
		close(clock_fd);
		clock_fd = -1;
	}
}
