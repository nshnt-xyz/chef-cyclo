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

/* English names: the date must not depend on a C library locale. */
static const char *const weekdays[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday",
	"Friday", "Saturday" };
static const char *const months[] = { "January", "February", "March", "April", "May", "June",
	"July", "August", "September", "October", "November", "December" };

void sys_clock_date_text(const struct tm *tm, char *out, size_t size)
{
	snprintf(out, size, "%s %d %s %d", weekdays[tm->tm_wday], tm->tm_mday, months[tm->tm_mon],
		 tm->tm_year + 1900);
}

/* Local fields of an epoch under the current TZ (UI thread only). */
void sys_clock_fields(time_t t, struct sys_state *out)
{
	struct tm tm;

	if (!localtime_r(&t, &tm) || tm.tm_wday < 0 || tm.tm_wday > 6 || tm.tm_mon < 0 ||
	    tm.tm_mon > 11) {
		snprintf(out->clock.hhmm, sizeof(out->clock.hhmm), "--:--");
		out->clock.zone[0] = 0;
		out->clock.date[0] = 0;
		out->clock.hour = out->clock.minute = -1;
		out->clock.valid = false;
		return;
	}
	strftime(out->clock.hhmm, sizeof(out->clock.hhmm), "%H:%M", &tm);
	if (!strftime(out->clock.zone, sizeof(out->clock.zone), "%Z", &tm))
		out->clock.zone[0] = 0;
	sys_clock_date_text(&tm, out->clock.date, sizeof(out->clock.date));
	out->clock.hour = tm.tm_hour;
	out->clock.minute = tm.tm_min;
	out->clock.valid = true;
	/* the offset in minutes east of UTC, for the Time page */
	out->clock.utc_offset_min = (int)(tm.tm_gmtoff / 60);
}

void sys_clock_refresh(void)
{
	struct sys_state now;

	memset(&now, 0, sizeof(now));
	sys_clock_fields(time(NULL), &now);
	if (strcmp(now.clock.hhmm, sys_state.clock.hhmm) ||
	    strcmp(now.clock.zone, sys_state.clock.zone) ||
	    strcmp(now.clock.date, sys_state.clock.date) ||
	    now.clock.hour != sys_state.clock.hour || now.clock.minute != sys_state.clock.minute ||
	    now.clock.valid != sys_state.clock.valid ||
	    now.clock.utc_offset_min != sys_state.clock.utc_offset_min) {
		sys_state.clock = now.clock;
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

void sys_clock_use_tz(const char *tz)
{
	setenv("TZ", tz && *tz ? tz : "UTC0", 1);
	tzset();
}

void sys_clock_set_tz(const char *tz)
{
	sys_clock_use_tz(tz);
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
