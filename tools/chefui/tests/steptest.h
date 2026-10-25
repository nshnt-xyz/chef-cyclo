/*
 * steptest.h - what test_bootsteps.c and test_shutsteps.c share: the
 * temporary OpenRC and run trees, the injected clocks, the captured log
 * lines and the shell and link helpers. Include after check.h and the
 * tracker's header; each test file keeps only its own init and rules.
 */
#ifndef CHEFUI_TEST_STEPTEST_H
#define CHEFUI_TEST_STEPTEST_H

#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char root[128], openrc[160], run[160];
static int64_t g_now, g_boot;
static char g_log[32][160];
static int g_nlog;

static int64_t clk(void) { return g_now; }
static int64_t boot_clk(void) { return g_boot; }

static void log_fn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void log_fn(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	if (g_nlog < 32)
		vsnprintf(g_log[g_nlog++], sizeof(g_log[0]), fmt, ap);
	va_end(ap);
}

static void sh(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void sh(const char *fmt, ...)
{
	char cmd[512];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(cmd, sizeof(cmd), fmt, ap);
	va_end(ap);
	if (system(cmd))
		abort();
}

/* A fresh tree: the OpenRC root with the NULL-terminated SUBDIRS in it and
 * the run directory, or only the empty root when SUBDIRS is NULL. The
 * clocks and the log start over (loop clock 1000 ms, boot clock BOOT_MS). */
static void steptest_tree(const char *const *subdirs, int64_t boot_ms)
{
	snprintf(root, sizeof(root), "/tmp/steps-test-XXXXXX");
	if (!mkdtemp(root))
		abort();
	snprintf(openrc, sizeof(openrc), "%s/openrc", root);
	snprintf(run, sizeof(run), "%s/chef", root);
	sh("mkdir -p %s", root);
	if (subdirs) {
		sh("mkdir -p %s", run);
		for (; *subdirs; subdirs++)
			sh("mkdir -p %s/%s", openrc, *subdirs);
	}
	g_now = 1000;
	g_boot = boot_ms;
	g_nlog = 0;
}

static void svc(const char *dir, const char *name)
{
	sh("ln -s /nonexistent %s/%s/%s", openrc, dir, name);
}

static void unsvc(const char *dir, const char *name)
{
	sh("rm -f %s/%s/%s", openrc, dir, name);
}

/* FD readable within a second (the tracker's inotify fd). */
static bool steptest_readable(int fd)
{
	struct pollfd p = { .fd = fd, .events = POLLIN };

	return poll(&p, 1, 1000) == 1;
}

static bool logged(const char *s)
{
	int i;

	for (i = 0; i < g_nlog; i++)
		if (!strcmp(g_log[i], s))
			return true;
	return false;
}

#endif
