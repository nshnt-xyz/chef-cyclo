/* check.h - the CHECK() counter the tools/ host tests use. */
#ifndef CHEFUI_TEST_CHECK_H
#define CHEFUI_TEST_CHECK_H

#include <stdio.h>

static int g_failures;
static int g_tests;

#define CHECK(cond) do { \
	g_tests++; \
	if (!(cond)) { \
		g_failures++; \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	} \
} while (0)

static int check_report(const char *name)
{
	if (g_failures) {
		fprintf(stderr, "%s: %d/%d checks FAILED\n", name, g_failures, g_tests);
		return 1;
	}
	printf("%s: %d/%d checks passed\n", name, g_tests, g_tests);
	return 0;
}

#endif
