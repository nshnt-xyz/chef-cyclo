/*
 * testbase.h - what the step tracker tests (steptest.h) and the cover tests
 * (covertest.h) both need: the temporary OpenRC and run trees, the shell
 * and link helpers and the captured log lines.
 */
#ifndef CHEFUI_TEST_TESTBASE_H
#define CHEFUI_TEST_TESTBASE_H

#define _GNU_SOURCE
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char root[128], openrc[160], run[160];
static char g_log[32][160];
static int g_nlog;

static inline void log_fn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static inline void log_fn(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	if (g_nlog < 32)
		vsnprintf(g_log[g_nlog++], sizeof(g_log[0]), fmt, ap);
	va_end(ap);
}

/* A captured line is exactly S / contains SUB. */
static inline bool log_is(const char *s)
{
	for (int i = 0; i < g_nlog; i++)
		if (!strcmp(g_log[i], s))
			return true;
	return false;
}

static inline bool log_has(const char *sub)
{
	for (int i = 0; i < g_nlog; i++)
		if (strstr(g_log[i], sub))
			return true;
	return false;
}

static inline void sh(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static inline void sh(const char *fmt, ...)
{
	char cmd[512];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(cmd, sizeof(cmd), fmt, ap);
	va_end(ap);
	if (system(cmd))
		abort();
}

/* A fresh temporary tree named NAME: its root, the openrc and run paths
 * under it (the callers make what they need); the log starts over. */
static inline void testbase_tree(const char *name)
{
	snprintf(root, sizeof(root), "/tmp/%s-test-XXXXXX", name);
	if (!mkdtemp(root))
		abort();
	snprintf(openrc, sizeof(openrc), "%s/openrc", root);
	snprintf(run, sizeof(run), "%s/chef", root);
	g_nlog = 0;
}

static inline void svc(const char *dir, const char *name)
{
	sh("ln -s /nonexistent %s/%s/%s", openrc, dir, name);
}

static inline void unsvc(const char *dir, const char *name)
{
	sh("rm -f %s/%s/%s", openrc, dir, name);
}

#endif
