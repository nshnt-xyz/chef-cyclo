/*
 * steptest.h - what test_bootsteps.c and test_shutsteps.c share: the
 * temporary OpenRC and run trees, the injected clocks, the captured log
 * lines and the shell and link helpers. Include after check.h and the
 * tracker's header; each test file keeps only its own init and rules.
 */
#ifndef CHEFUI_TEST_STEPTEST_H
#define CHEFUI_TEST_STEPTEST_H

#include <poll.h>
#include <stdint.h>

#include "testbase.h"

static int64_t g_now, g_boot;

static int64_t clk(void) { return g_now; }
static int64_t boot_clk(void) { return g_boot; }

/* A fresh tree: the OpenRC root with the NULL-terminated SUBDIRS in it and
 * the run directory, or only the empty root when SUBDIRS is NULL. The
 * clocks and the log start over (loop clock 1000 ms, boot clock BOOT_MS). */
static void steptest_tree(const char *const *subdirs, int64_t boot_ms)
{
	testbase_tree("steps");
	sh("mkdir -p %s", root);
	if (subdirs) {
		sh("mkdir -p %s", run);
		for (; *subdirs; subdirs++)
			sh("mkdir -p %s/%s", openrc, *subdirs);
	}
	g_now = 1000;
	g_boot = boot_ms;
}

/* FD readable within a second (the tracker's inotify fd). */
static bool steptest_readable(int fd)
{
	struct pollfd p = { .fd = fd, .events = POLLIN };

	return poll(&p, 1, 1000) == 1;
}

static bool logged(const char *s)
{
	return log_is(s);
}

#endif
