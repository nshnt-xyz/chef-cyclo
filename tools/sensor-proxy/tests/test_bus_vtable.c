/* Host test for tools/sensor-proxy/bus.c's vtables: every method of both
 * objects (six on net.hadess.SensorProxy, two on .Compass) carries
 * SD_BUS_VTABLE_UNPRIVILEGED. Without it sd-bus refuses a caller with
 * another uid than the daemon's, which the D-Bus tests never are (they run
 * the daemon as the developer). bus.c is included for its static arrays.
 * Build/run: see tools/sensor-proxy/Makefile ("make test").
 */
#include "../bus.c"

static int g_failures;
static int g_tests;

#define CHECK(cond) do { \
	g_tests++; \
	if (!(cond)) { \
		g_failures++; \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	} \
} while (0)

/* Walk one vtable from START to END: the number of methods, each checked. */
static int methods(const sd_bus_vtable *v, const char *what)
{
	int n = 0;

	CHECK(v->type == _SD_BUS_VTABLE_START);
	for (v++; v->type != _SD_BUS_VTABLE_END; v++) {
		if (v->type != _SD_BUS_VTABLE_METHOD)
			continue;
		n++;
		g_tests++;
		if (!(v->flags & SD_BUS_VTABLE_UNPRIVILEGED)) {
			g_failures++;
			fprintf(stderr, "FAIL %s: %s is not SD_BUS_VTABLE_UNPRIVILEGED\n", what, v->x.method.member);
		}
	}
	return n;
}

int main(void)
{
	CHECK(methods(vtable, IFACE) == 6);
	CHECK(methods(compass_vtable, COMPASS_IFACE) == 2);
	printf("test-bus-vtable: %d/%d checks passed\n", g_tests - g_failures, g_tests);
	return g_failures ? 1 : 0;
}
