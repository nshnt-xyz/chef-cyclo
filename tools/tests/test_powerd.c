/* Host tests for tools/powerd.c against a fake power_supply sysfs tree in a
 * temp directory: the attribute reader (numbers, the qcom -22/EINVAL
 * convention, missing files, CSV-unsafe strings), the uevent filter
 * (power_supply only, no libudev rebroadcasts), the low-battery policy
 * (warn/critical once, re-arm on power or at 20 %, never while powered),
 * shutdown confirmation (3 samples >= 5 s apart, reset on recovery or on
 * an unknown capacity/voltage, gated on present == 1 and
 * soc_reporting_ready == 1, the voltage leg only at or below critical,
 * never while a supply is online unless --shutdown-when-online,
 * over-temperature strictly above 68.0 C regardless of supply, fires once,
 * marker + state + CSV row written before the injected shutdown runs), the charge throttle (step up/down
 * every 5 s, clamp, hold band, reset on unplug, write failure disables
 * it), the state file, CSV header/rotation, and the poll cadence.
 * powerd.c is included with POWERD_NO_MAIN; hooks are injected, nothing
 * touches /sys, /dev/kmsg or powers off.
 * Build/run: see tools/Makefile ("make test").
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define POWERD_NO_MAIN
#include "../powerd.c"

static int g_failures;
static int g_tests;

#define CHECK(cond) do { \
	g_tests++; \
	if (!(cond)) { \
		g_failures++; \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	} \
} while (0)

/* ---- hooks ---- */

#define NLOG 64
static char g_log[NLOG][320];
static int g_nlog;

static void test_log(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(g_log[g_nlog % NLOG], sizeof(g_log[0]), fmt, ap);
	va_end(ap);
	g_nlog++;
}

/* Any log line since index `from` containing needle? */
static bool logged_since(int from, const char *needle)
{
	int i;

	for (i = from; i < g_nlog; i++)
		if (strstr(g_log[i % NLOG], needle))
			return true;
	return false;
}

static int count_since(int from, const char *needle)
{
	int i, n = 0;

	for (i = from; i < g_nlog; i++)
		if (strstr(g_log[i % NLOG], needle))
			n++;
	return n;
}

static int g_levels[64];
static int g_nlevels;
static int g_level_rc;

static int test_write_level(struct powerd *p, int level)
{
	(void)p;
	if (g_level_rc)
		return g_level_rc;
	g_levels[g_nlevels++ % 64] = level;
	return 0;
}

static int g_shutdowns;
static char g_state_at_shutdown[2048];

static void test_shutdown(struct powerd *p)
{
	char path[512];
	FILE *f;
	size_t n = 0;

	g_shutdowns++;
	/* the state file must already say why */
	snprintf(path, sizeof(path), "%s/state", p->dir);
	f = fopen(path, "r");
	if (f) {
		n = fread(g_state_at_shutdown, 1, sizeof(g_state_at_shutdown) - 1, f);
		fclose(f);
	}
	g_state_at_shutdown[n] = '\0';
}

static int g_buzzes;

static void test_buzz(struct powerd *p, int ms)
{
	(void)p;
	(void)ms;
	g_buzzes++;
}

/* ---- fake tree ---- */

static char g_root[256];
static char g_run[256];

static void put(const char *psy, const char *attr, const char *val)
{
	char path[512];
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", g_root, psy);
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/%s/%s", g_root, psy, attr);
	f = fopen(path, "w");
	if (!f) {
		perror(path);
		exit(2);
	}
	fprintf(f, "%s\n", val);
	fclose(f);
}

static void puti(const char *psy, const char *attr, int v)
{
	char buf[32];

	snprintf(buf, sizeof(buf), "%d", v);
	put(psy, attr, buf);
}

static void del(const char *psy, const char *attr)
{
	char path[512];

	snprintf(path, sizeof(path), "%s/%s/%s", g_root, psy, attr);
	unlink(path);
}

/* The stock survey's values (2026-09-26, charging from host CDP). */
static void tree_stock_charging(void)
{
	put("battery", "status", "Charging");
	put("battery", "health", "Good");
	put("battery", "charge_type", "Fast");
	puti("battery", "present", 1);
	puti("battery", "capacity", 79);
	puti("battery", "temp", 320);
	puti("battery", "voltage_now", 4106923);
	puti("battery", "current_now", 103027);
	puti("battery", "constant_charge_current_max", 3000000);
	puti("battery", "system_temp_level", 0);
	puti("battery", "num_system_temp_levels", 8);
	puti("bms", "voltage_ocv", 4102057);
	puti("bms", "charge_full", 5004000);
	puti("bms", "charge_counter", 3862092);
	puti("bms", "cycle_count", 0);
	puti("bms", "soc_reporting_ready", 1);
	puti("usb", "online", 1);
	put("usb", "real_type", "USB_CDP");
	puti("usb", "current_max", 1500000);
	puti("usb", "input_current_now", 144531);
	puti("usb", "voltage_now", 4355468);
	puti("main", "input_current_settled", 150000);
	put("usb", "typec_mode", "Source attached (default current)");
	puti("dc", "online", 0);
	puti("pc_port", "online", 0);
}

static void unplug(void)
{
	puti("usb", "online", 0);
	put("usb", "real_type", "Unknown");
	put("battery", "status", "Discharging");
	put("battery", "charge_type", "N/A");
}

static void plug(void)
{
	puti("usb", "online", 1);
	put("usb", "real_type", "USB_CDP");
	put("battery", "status", "Charging");
}

static void clear_run(void)
{
	char path[512];

	snprintf(path, sizeof(path), "%s/state", g_run);
	unlink(path);
	snprintf(path, sizeof(path), "%s/log.csv", g_run);
	unlink(path);
	snprintf(path, sizeof(path), "%s/log.csv.1", g_run);
	unlink(path);
	snprintf(path, sizeof(path), "%s/shutdown-pending", g_run);
	unlink(path);
}

static void fresh(struct powerd *p)
{
	powerd_init(p);
	p->sysfs = g_root;
	p->dir = g_run;
	p->vib = "/fake/vibrator";
	p->log = test_log;
	p->write_level = test_write_level;
	p->shutdown = test_shutdown;
	p->buzz = test_buzz;
	g_nlevels = 0;
	g_level_rc = 0;
	g_shutdowns = 0;
	g_buzzes = 0;
	g_state_at_shutdown[0] = '\0';
	clear_run();
	tree_stock_charging();
}

static char *slurp(const char *name)
{
	static char buf[1 << 16];
	char path[512];
	FILE *f;
	size_t n = 0;

	snprintf(path, sizeof(path), "%s/%s", g_run, name);
	f = fopen(path, "r");
	if (!f)
		return NULL;
	n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = '\0';
	return buf;
}

static bool has(const char *name, const char *needle)
{
	const char *s = slurp(name);

	return s && strstr(s, needle);
}

static int lines(const char *s)
{
	int n = 0;

	for (; s && *s; s++)
		n += *s == '\n';
	return n;
}

/* ---- tests ---- */

static void test_read(void)
{
	struct sample s;
	struct powerd p;

	fresh(&p);
	sample_read(g_root, &s);
	CHECK(strcmp(s.status, "Charging") == 0);
	CHECK(s.capacity == 79 && s.temp == 320 && s.voltage_uv == 4106923);
	CHECK(s.current_ua == 103027 && s.nstl == 8 && s.stl == 0);
	CHECK(s.ocv_uv == 4102057 && s.charge_full_uah == 5004000 && s.cycle_count == 0);
	CHECK(s.charge_counter_uah == 3862092);
	CHECK(s.soc_ready == 1 && s.usb_online == 1 && s.dc_online == 0);
	CHECK(strcmp(s.usb_type, "USB_CDP") == 0);
	CHECK(strcmp(s.typec_mode, "Source attached (default current)") == 0);
	CHECK(supply_online(&s) && powered(&s));
	CHECK(strcmp(source_name(&s), "usb") == 0);

	/* the qcom getters' -22 is "unknown", except for temp */
	puti("usb", "current_max", -22);
	puti("battery", "temp", -22);
	put("battery", "voltage_now", "garbage");
	put("usb", "real_type", "-22");
	put("usb", "typec_mode", "a,b=c");
	del("bms", "cycle_count");
	del("dc", "online");
	sample_read(g_root, &s);
	CHECK(s.usb_current_max_ua == POWERD_NA);
	CHECK(s.temp == -22);
	CHECK(s.voltage_uv == POWERD_NA);
	CHECK(s.usb_type[0] == '\0');
	CHECK(strcmp(s.typec_mode, "a;b c") == 0);
	CHECK(s.cycle_count == POWERD_NA && s.dc_online == POWERD_NA);

	/* dc alone powers it; nothing readable = unknown */
	puti("usb", "online", 0);
	puti("dc", "online", 1);
	sample_read(g_root, &s);
	CHECK(strcmp(source_name(&s), "dc") == 0 && supply_online(&s));
	del("usb", "online");
	del("dc", "online");
	put("battery", "status", "Discharging");
	sample_read(g_root, &s);
	/* unreadable online = unknown = powered, never "unplugged" */
	CHECK(strcmp(source_name(&s), "unknown") == 0 && powered(&s));
	/* status alone counts as powered for the low-battery policy */
	put("battery", "status", "Full");
	sample_read(g_root, &s);
	CHECK(!supply_online(&s) && powered(&s));
}

static void test_uevent(void)
{
	static const char ps[] = "change@/devices/soc/x/power_supply/battery\0ACTION=change\0"
				 "DEVPATH=/devices/soc/x/power_supply/battery\0SUBSYSTEM=power_supply\0"
				 "POWER_SUPPLY_NAME=battery\0POWER_SUPPLY_CAPACITY=79";
	static const char other[] = "add@/devices/virtual/net/lo\0ACTION=add\0SUBSYSTEM=net\0";
	static const char udev[] = "libudev\0\xfe\xed\xca\xfeSUBSYSTEM=power_supply\0";
	static const char prefix[] = "change@/x\0SUBSYSTEM=power_supply_foo\0";

	CHECK(uevent_is_power_supply(ps, sizeof(ps)));
	CHECK(!uevent_is_power_supply(other, sizeof(other)));
	CHECK(!uevent_is_power_supply(udev, sizeof(udev)));
	CHECK(!uevent_is_power_supply(prefix, sizeof(prefix)));
	CHECK(!uevent_is_power_supply("", 0));
	/* truncated before the key */
	CHECK(!uevent_is_power_supply(ps, 40));
}

static void test_low_battery(void)
{
	struct powerd p;
	int m;
	int64_t t = 0;

	fresh(&p);
	powerd_sample(&p, "start", t, 1.0);
	CHECK(logged_since(0, "start: Charging, 79 %"));
	CHECK(logged_since(0, "source usb USB_CDP"));

	/* powered: no warning even at 3 % */
	m = g_nlog;
	puti("battery", "capacity", 3);
	powerd_sample(&p, "poll", t += 60000, 2.0);
	CHECK(!logged_since(m, "LOW") && !logged_since(m, "CRITICAL"));
	CHECK(!p.warned && !p.crit);

	/* unplugged at 16 %: nothing; 15 %: warn once */
	m = g_nlog;
	puti("battery", "capacity", 16);
	unplug();
	powerd_sample(&p, "uevent", t += 60000, 3.0);
	CHECK(logged_since(m, "unplugged: on battery (16 %, 4.10 V)"));
	CHECK(logged_since(m, "status Charging -> Discharging"));
	CHECK(!logged_since(m, "LOW"));
	puti("battery", "capacity", 15);
	powerd_sample(&p, "poll", t += 60000, 4.0);
	CHECK(count_since(m, "LOW BATTERY 15 %") == 1);
	puti("battery", "capacity", 14);
	powerd_sample(&p, "poll", t += 60000, 5.0);
	CHECK(count_since(m, "LOW BATTERY") == 1);
	CHECK(strstr(slurp("state"), "alert=low\n") != NULL);

	/* critical once, with a buzz */
	puti("battery", "capacity", 5);
	powerd_sample(&p, "poll", t += 60000, 6.0);
	puti("battery", "capacity", 4);
	powerd_sample(&p, "poll", t += 60000, 7.0);
	CHECK(count_since(m, "CRITICAL BATTERY 5 %") == 1);
	CHECK(count_since(m, "CRITICAL") == 1);
	CHECK(g_buzzes == 1);
	CHECK(strstr(slurp("state"), "alert=critical\n") != NULL);

	/* no vibrator configured: no buzz */
	p.vib = "";
	p.crit = false;
	powerd_sample(&p, "poll", t += 60000, 7.5);
	CHECK(g_buzzes == 1);
	p.vib = "/fake/vibrator";

	/* 19 % does not re-arm; 20 % does */
	m = g_nlog;
	puti("battery", "capacity", 19);
	powerd_sample(&p, "poll", t += 60000, 8.0);
	CHECK(p.warned && p.crit);
	puti("battery", "capacity", 20);
	powerd_sample(&p, "poll", t += 60000, 9.0);
	CHECK(!p.warned && !p.crit);
	puti("battery", "capacity", 15);
	powerd_sample(&p, "poll", t += 60000, 10.0);
	CHECK(count_since(m, "LOW BATTERY 15 %") == 1);

	/* plugging in re-arms */
	m = g_nlog;
	plug();
	powerd_sample(&p, "uevent", t += 1000, 11.0);
	CHECK(!p.warned);
	CHECK(logged_since(m, "power source usb (USB_CDP, 1500 mA max, typec Source attached (default current))"));
	unplug();
	powerd_sample(&p, "uevent", t += 1000, 12.0);
	CHECK(count_since(m, "LOW BATTERY 15 %") == 1);

	/* unknown capacity: nothing */
	m = g_nlog;
	fresh(&p);
	unplug();
	del("battery", "capacity");
	powerd_sample(&p, "start", 0, 1.0);
	CHECK(!logged_since(m, "LOW") && !p.warned);
	CHECK(strstr(slurp("state"), "capacity=na\n") != NULL);
	CHECK(g_shutdowns == 0);
}

static void test_shutdown_empty(void)
{
	struct powerd p;
	char *st;
	int m;
	int64_t t = 0;

	/* capacity 0, unplugged: needs 3 samples >= 5 s apart */
	fresh(&p);
	powerd_sample(&p, "start", t, 1.0);
	unplug();
	puti("battery", "capacity", 0);
	puti("battery", "voltage_now", 3450000);
	m = g_nlog;
	powerd_sample(&p, "uevent", t += 1000, 2.0);
	CHECK(p.empty.count == 1 && g_shutdowns == 0);
	CHECK(logged_since(m, "battery empty (0 %, 3.45 V), confirming 1/3"));
	CHECK(strstr(slurp("state"), "alert=confirming\n") != NULL);
	/* a burst inside 5 s does not count */
	powerd_sample(&p, "uevent", t += 1000, 3.0);
	powerd_sample(&p, "uevent", t += 1000, 4.0);
	CHECK(p.empty.count == 1 && g_shutdowns == 0);
	CHECK(next_poll_ms(&p, &p.prev) == 5000);
	powerd_sample(&p, "tick", t += 4000, 8.0);	/* 6 s after the first */
	CHECK(p.empty.count == 2 && g_shutdowns == 0);
	powerd_sample(&p, "tick", t += 5000, 13.0);
	CHECK(p.empty.count == 3 && g_shutdowns == 1);
	CHECK(logged_since(m, "SHUTDOWN: battery empty: 0 %, 3.45 V (capacity; 3 samples)"));
	/* marker, state and log row were written before the hook ran */
	st = slurp("shutdown-pending");
	CHECK(st && strstr(st, "battery empty: 0 %") == st);
	CHECK(strstr(g_state_at_shutdown, "alert=shutdown\n") != NULL);
	CHECK(strstr(g_state_at_shutdown, "shutdown_reason=battery empty: 0 %") != NULL);
	st = slurp("log.csv");
	CHECK(st && strstr(st, ",tick,Discharging,battery,") && strstr(st, ",shutdown\n"));
	/* fires once */
	powerd_sample(&p, "tick", t += 5000, 18.0);
	powerd_sample(&p, "tick", t += 5000, 23.0);
	CHECK(g_shutdowns == 1);

	/* recovery resets the count */
	fresh(&p);
	unplug();
	puti("battery", "capacity", 0);
	powerd_sample(&p, "start", t = 0, 1.0);
	powerd_sample(&p, "tick", t += 5000, 6.0);
	CHECK(p.empty.count == 2);
	puti("battery", "capacity", 1);
	powerd_sample(&p, "tick", t += 5000, 11.0);
	CHECK(p.empty.count == 0);
	puti("battery", "capacity", 0);
	powerd_sample(&p, "tick", t += 5000, 16.0);
	powerd_sample(&p, "tick", t += 5000, 21.0);
	CHECK(g_shutdowns == 0 && p.empty.count == 2);
	powerd_sample(&p, "tick", t += 5000, 26.0);
	CHECK(g_shutdowns == 1);

	/* capacity 0 before the FG reports ready is not trusted, and neither
	 * is the voltage leg */
	fresh(&p);
	unplug();
	puti("battery", "capacity", 0);
	puti("battery", "voltage_now", 3000000);
	puti("bms", "soc_reporting_ready", 0);
	for (t = 0; t <= 30000; t += 5000)
		powerd_sample(&p, "tick", t, 1.0);
	CHECK(g_shutdowns == 0 && p.empty.count == 0);
	/* present unknown is not present == 1 */
	puti("bms", "soc_reporting_ready", 1);
	del("battery", "present");
	for (; t <= 60000; t += 5000)
		powerd_sample(&p, "tick", t, 1.0);
	CHECK(g_shutdowns == 0 && p.empty.count == 0);

	/* voltage leg: < 3300 mV only counts at or below the critical level */
	fresh(&p);
	unplug();
	puti("battery", "capacity", 30);
	puti("battery", "voltage_now", 3100000);	/* a sag under load */
	for (t = 0; t <= 30000; t += 5000)
		powerd_sample(&p, "tick", t, 1.0);
	CHECK(g_shutdowns == 0 && p.empty.count == 0);
	puti("battery", "capacity", 5);
	for (; t <= 60000; t += 5000)
		powerd_sample(&p, "tick", t, 1.0);
	CHECK(g_shutdowns == 1);
	CHECK(has("shutdown-pending", "battery empty: 5 %, 3.10 V (voltage; 3 samples)"));

	/* 3300 mV exactly is not empty */
	fresh(&p);
	unplug();
	puti("battery", "capacity", 3);
	puti("battery", "voltage_now", 3300000);
	for (t = 0; t <= 30000; t += 5000)
		powerd_sample(&p, "tick", t, 1.0);
	CHECK(g_shutdowns == 0);

	/* an unknown capacity or voltage resets the count, never reads as 0 */
	fresh(&p);
	unplug();
	puti("battery", "capacity", 0);
	powerd_sample(&p, "tick", t = 0, 1.0);
	powerd_sample(&p, "tick", t += 5000, 2.0);
	CHECK(p.empty.count == 2);
	puti("battery", "capacity", -22);
	powerd_sample(&p, "tick", t += 5000, 3.0);
	CHECK(p.empty.count == 0 && g_shutdowns == 0);
	puti("battery", "capacity", 0);
	powerd_sample(&p, "tick", t += 5000, 4.0);
	powerd_sample(&p, "tick", t += 5000, 5.0);
	put("battery", "voltage_now", "");
	powerd_sample(&p, "tick", t += 5000, 6.0);
	CHECK(p.empty.count == 0 && g_shutdowns == 0);

	/* never while a supply is online, even if status says Discharging */
	fresh(&p);
	puti("battery", "capacity", 0);
	puti("battery", "voltage_now", 3000000);
	put("battery", "status", "Discharging");
	for (t = 0; t <= 30000; t += 5000)
		powerd_sample(&p, "tick", t, 1.0);
	CHECK(g_shutdowns == 0 && p.empty.count == 0);
	/* ...nor with status Charging and online unreadable */
	fresh(&p);
	del("usb", "online");
	del("dc", "online");
	puti("battery", "capacity", 0);
	for (t = 0; t <= 30000; t += 5000)
		powerd_sample(&p, "tick", t, 1.0);
	CHECK(g_shutdowns == 0);

	/* absent battery: no action */
	fresh(&p);
	unplug();
	puti("battery", "present", 0);
	puti("battery", "capacity", 0);
	puti("battery", "voltage_now", 3000000);
	for (t = 0; t <= 30000; t += 5000)
		powerd_sample(&p, "tick", t, 1.0);
	CHECK(g_shutdowns == 0);

	/* --shutdown-when-online: the whole chain with USB attached */
	fresh(&p);
	p.cfg.act_when_online = true;
	p.cfg.warn_pct = 81;
	p.cfg.crit_pct = 80;
	p.cfg.empty_mv = 4200;
	m = g_nlog;
	for (t = 0; t <= 10000; t += 5000)
		powerd_sample(&p, "tick", t, 1.0);
	CHECK(logged_since(m, "LOW BATTERY 79 %") && logged_since(m, "CRITICAL BATTERY 79 %"));
	CHECK(g_shutdowns == 1);
	CHECK(has("shutdown-pending", "(voltage; 3 samples) [TEST: supply online]"));
}

/* SDP / host data role: usb/online is 0 and the input shows on
 * pc_port/online; "Not charging" is still an attached input. */
static void test_sdp_and_unknowns(void)
{
	struct powerd p;
	struct sample s;
	int64_t t;
	int m;

	fresh(&p);
	puti("usb", "online", 0);
	puti("pc_port", "online", 1);
	put("usb", "real_type", "USB");
	sample_read(g_root, &s);
	CHECK(supply_online(&s) && powered(&s));
	CHECK(strcmp(source_name(&s), "usb") == 0);
	m = g_nlog;
	puti("battery", "temp", 450);
	powerd_sample(&p, "start", 0, 1.0);
	powerd_sample(&p, "tick", 5000, 2.0);
	CHECK(logged_since(m, "start: Charging, 79 %, 4.10 V, battery 45.0 C, source usb USB"));
	CHECK(p.level == 2);		/* throttle sees the SDP input */
	CHECK(has("state", "source=usb\nonline=1\nusb_type=USB\n"));
	/* SDP unplug is a transition and resets the level */
	m = g_nlog;
	puti("pc_port", "online", 0);
	put("battery", "status", "Discharging");
	powerd_sample(&p, "uevent", 6000, 3.0);
	CHECK(logged_since(m, "unplugged: on battery") && p.level == 0);

	/* SDP attached but not charging, at 0 %: never a low-battery shutdown */
	fresh(&p);
	puti("usb", "online", 0);
	puti("pc_port", "online", 1);
	put("usb", "real_type", "USB");
	put("battery", "status", "Not charging");
	puti("battery", "capacity", 0);
	puti("battery", "voltage_now", 3100000);
	for (t = 0; t <= 30000; t += 5000)
		powerd_sample(&p, "tick", t, 1.0);
	CHECK(g_shutdowns == 0 && p.empty.count == 0 && !p.warned);
	/* "Not charging" alone (all inputs read 0) is still powered */
	puti("pc_port", "online", 0);
	for (; t <= 60000; t += 5000)
		powerd_sample(&p, "tick", t, 1.0);
	CHECK(g_shutdowns == 0 && p.empty.count == 0);

	/* status unreadable with every input 0: powered */
	fresh(&p);
	unplug();
	put("battery", "status", "");
	puti("battery", "capacity", 0);
	for (t = 0; t <= 30000; t += 5000)
		powerd_sample(&p, "tick", t, 1.0);
	CHECK(g_shutdowns == 0 && p.empty.count == 0);

	/* usb/online unreadable mid-confirmation: the count resets */
	fresh(&p);
	unplug();
	puti("battery", "capacity", 0);
	powerd_sample(&p, "tick", t = 0, 1.0);
	powerd_sample(&p, "tick", t += 5000, 2.0);
	CHECK(p.empty.count == 2);
	put("usb", "online", "garbage");
	powerd_sample(&p, "tick", t += 5000, 3.0);
	CHECK(p.empty.count == 0 && g_shutdowns == 0);
	puti("usb", "online", -22);
	for (t += 5000; t <= 60000; t += 5000)
		powerd_sample(&p, "tick", t, 4.0);
	CHECK(g_shutdowns == 0);

	/* an absent psy is "off", not unknown: pc_port and dc do not exist */
	fresh(&p);
	unplug();
	del("pc_port", "online");
	del("dc", "online");
	{
		char path[512];

		snprintf(path, sizeof(path), "%s/pc_port", g_root);
		rmdir(path);
		snprintf(path, sizeof(path), "%s/dc", g_root);
		rmdir(path);
	}
	puti("battery", "capacity", 0);
	sample_read(g_root, &s);
	CHECK(s.pc_online == POWERD_ABSENT && s.dc_online == POWERD_ABSENT && !powered(&s));
	CHECK(strcmp(source_name(&s), "battery") == 0);
	for (t = 0; t <= 10000; t += 5000)
		powerd_sample(&p, "tick", t, 1.0);
	CHECK(g_shutdowns == 1);
	CHECK(has("log.csv", ",na,na,0,0,shutdown\n"));	/* pc_port, dc absent -> na */
}

static void test_overtemp(void)
{
	struct powerd p;
	int64_t t;
	int m;

	/* on USB as well: over-temperature ignores the supply */
	fresh(&p);
	puti("battery", "temp", 681);
	m = g_nlog;
	for (t = 0; t <= 10000; t += 5000)
		powerd_sample(&p, "tick", t, 1.0);
	CHECK(g_shutdowns == 1);
	CHECK(logged_since(m, "over-temperature 68.1 C, confirming 1/3"));
	CHECK(logged_since(m, "SHUTDOWN: battery over-temperature 68.1 C > 68.0 C (3 samples)"));

	/* 68.0 C exactly never (strict, as AOSP); unknown temp resets */
	fresh(&p);
	puti("battery", "temp", 680);
	for (t = 0; t <= 30000; t += 5000)
		powerd_sample(&p, "tick", t, 1.0);
	CHECK(g_shutdowns == 0);
	fresh(&p);
	puti("battery", "temp", 700);
	powerd_sample(&p, "tick", t = 0, 1.0);
	powerd_sample(&p, "tick", t += 5000, 1.0);
	del("battery", "temp");
	powerd_sample(&p, "tick", t += 5000, 1.0);
	CHECK(p.hot.count == 0 && g_shutdowns == 0);
	fresh(&p);
	puti("battery", "temp", 679);
	for (t = 0; t <= 30000; t += 5000)
		powerd_sample(&p, "tick", t, 1.0);
	CHECK(g_shutdowns == 0);

	/* confirm count is configurable */
	fresh(&p);
	p.cfg.confirm_n = 1;
	puti("battery", "temp", 700);
	powerd_sample(&p, "tick", 0, 1.0);
	CHECK(g_shutdowns == 1);
}

static void test_throttle(void)
{
	struct powerd p;
	int64_t t = 0;
	int m, i;

	fresh(&p);
	powerd_sample(&p, "start", t, 1.0);
	CHECK(g_nlevels == 0 && p.level == 0);	/* main writes the startup 0 */

	/* 44.0 C: +1 per 5 s, not faster */
	puti("battery", "temp", 440);
	m = g_nlog;
	powerd_sample(&p, "tick", t += 5000, 2.0);
	CHECK(p.level == 1 && g_nlevels == 1 && g_levels[0] == 1);
	CHECK(logged_since(m, "charge throttle level 1 of 7 (battery 44.0 C, above set point)"));
	powerd_sample(&p, "uevent", t += 1000, 3.0);
	CHECK(p.level == 1);
	powerd_sample(&p, "tick", t += 4000, 4.0);
	CHECK(p.level == 2);
	CHECK(strstr(slurp("state"), "throttle_level=2\n") != NULL);
	/* clamp at num_system_temp_levels - 1 */
	for (i = 0; i < 10; i++)
		powerd_sample(&p, "tick", t += 5000, 5.0);
	CHECK(p.level == 7);
	/* hold band 42 < T < 44 */
	puti("battery", "temp", 430);
	powerd_sample(&p, "tick", t += 5000, 6.0);
	CHECK(p.level == 7);
	/* <= 42.0 C: -1 per 5 s */
	puti("battery", "temp", 420);
	powerd_sample(&p, "tick", t += 5000, 7.0);
	CHECK(p.level == 6);
	powerd_sample(&p, "tick", t += 5000, 8.0);
	CHECK(p.level == 5);
	/* unplug: straight to 0 */
	m = g_nlog;
	puti("battery", "temp", 450);
	unplug();
	powerd_sample(&p, "uevent", t += 100, 9.0);
	CHECK(p.level == 0 && g_levels[(g_nlevels - 1) % 64] == 0);
	CHECK(logged_since(m, "charge throttle level 0 of 7 (battery 45.0 C, unplugged)"));
	/* hot but unplugged: nothing written */
	i = g_nlevels;
	powerd_sample(&p, "poll", t += 60000, 10.0);
	CHECK(g_nlevels == i && p.level == 0);
	CHECK(next_poll_ms(&p, &p.prev) == 10000);	/* hot: 10 s */

	/* no levels table: throttle never writes */
	fresh(&p);
	puti("battery", "num_system_temp_levels", 0);
	puti("battery", "temp", 500);
	for (t = 0; t <= 20000; t += 5000)
		powerd_sample(&p, "tick", t, 1.0);
	CHECK(g_nlevels == 0);

	/* a failed write disables the throttle, logged once */
	fresh(&p);
	g_level_rc = -EINVAL;
	puti("battery", "temp", 500);
	m = g_nlog;
	for (t = 0; t <= 20000; t += 5000)
		powerd_sample(&p, "tick", t, 1.0);
	CHECK(count_since(m, "charge throttle disabled") == 1 && p.level == 0 && p.level_broken);

	/* --no-throttle */
	fresh(&p);
	p.cfg.throttle = false;
	puti("battery", "temp", 500);
	for (t = 0; t <= 20000; t += 5000)
		powerd_sample(&p, "tick", t, 1.0);
	CHECK(g_nlevels == 0);

	/* a test set point */
	fresh(&p);
	p.cfg.thr_set_dc = 300;
	p.cfg.thr_clr_dc = 290;
	powerd_sample(&p, "tick", 0, 1.0);
	CHECK(p.level == 1);
}

static void test_outputs(void)
{
	struct powerd p;
	char *st, *csv;
	char path[512];
	struct stat sb;
	int64_t t;
	int n;

	fresh(&p);
	snprintf(p.overrides, sizeof(p.overrides), "warn=50");
	powerd_sample(&p, "start", 0, 12.5);
	st = slurp("state");
	CHECK(st != NULL);
	CHECK(strstr(st, "uptime=12.5\nstatus=Charging\nsource=usb\nonline=1\nusb_type=USB_CDP\n"
			 "capacity=79\nvoltage_mv=4106\ncurrent_ma=103\ntemp_c=32.0\n") == st);
	CHECK(strstr(st, "ocv_mv=4102\ncharge_full_uah=5004000\ncharge_counter_uah=3862092\ncycle_count=0\nsoc_ready=1\n"));
	CHECK(strstr(st, "usb_current_max_ma=1500\nusb_input_ma=144\nusb_voltage_mv=4355\ninput_settled_ma=150\nprofile_fcc_ma=3000\n"));
	CHECK(strstr(st, "throttle_level=0\nsystem_temp_level=0\nnum_system_temp_levels=8\n"));
	CHECK(strstr(st, "alert=none\nshutdown_reason=\noverrides=warn=50\n"));
	snprintf(path, sizeof(path), "%s/.state.tmp", g_run);
	CHECK(stat(path, &sb) < 0);	/* renamed, not left behind */

	csv = slurp("log.csv");
	CHECK(csv && strncmp(csv, csv_header, sizeof(csv_header) - 1) == 0);
	CHECK(lines(csv) == 2);
	CHECK(strstr(csv, "\n12.5,") && strstr(csv, ",start,Charging,usb,USB_CDP,79,4106,103,32.0,Good,Fast,4102,5004000,3862092,0,1,1500,144,4355,150,3000,Source attached (default current),0,0,0,0,none\n"));

	/* throttle ticks with nothing new: no row; a change or 60 s: a row */
	powerd_sample(&p, "tick", 5000, 13.0);
	powerd_sample(&p, "tick", 10000, 14.0);
	CHECK(lines(slurp("log.csv")) == 2);
	puti("battery", "capacity", 80);
	powerd_sample(&p, "tick", 15000, 15.0);
	CHECK(lines(slurp("log.csv")) == 3);
	powerd_sample(&p, "tick", 75000, 16.0);
	CHECK(lines(slurp("log.csv")) == 4);
	powerd_sample(&p, "poll", 76000, 17.0);	/* polls and uevents always */
	powerd_sample(&p, "uevent", 77000, 18.0);
	CHECK(lines(slurp("log.csv")) == 6);

	/* rotation: bounded, one generation kept, each file starts with a header */
	fresh(&p);
	p.log_max = 2048;
	for (t = 0, n = 0; n < 40; n++, t += 60000)
		powerd_sample(&p, "poll", t, (double)n);
	snprintf(path, sizeof(path), "%s/log.csv", g_run);
	CHECK(stat(path, &sb) == 0 && sb.st_size <= 2048);
	csv = slurp("log.csv");
	CHECK(csv && strncmp(csv, csv_header, sizeof(csv_header) - 1) == 0);
	snprintf(path, sizeof(path), "%s/log.csv.1", g_run);
	CHECK(stat(path, &sb) == 0 && sb.st_size <= 2048);
	csv = slurp("log.csv.1");
	CHECK(csv && strncmp(csv, csv_header, sizeof(csv_header) - 1) == 0);
	csv = slurp("log.csv");
	CHECK(strstr(csv, "\n39.0,") != NULL);	/* newest row in the live file */
}

static void test_cadence(void)
{
	struct powerd p;
	struct sample s;

	fresh(&p);
	sample_read(g_root, &s);
	CHECK(next_poll_ms(&p, &s) == 5000);	/* online: throttle cadence */
	p.cfg.throttle = false;
	CHECK(next_poll_ms(&p, &s) == 60000);
	p.cfg.throttle = true;
	unplug();
	sample_read(g_root, &s);
	CHECK(next_poll_ms(&p, &s) == 60000);
	s.capacity = 15;
	CHECK(next_poll_ms(&p, &s) == 10000);
	s.capacity = 50;
	s.temp = 440;
	CHECK(next_poll_ms(&p, &s) == 10000);
	s.temp = 300;
	p.hot.count = 1;
	CHECK(next_poll_ms(&p, &s) == 5000);
}

int main(void)
{
	char tmpl[] = "/tmp/test-powerd.XXXXXX";
	char cmd[600];

	if (!mkdtemp(tmpl)) {
		perror("mkdtemp");
		return 2;
	}
	snprintf(g_root, sizeof(g_root), "%s/sys", tmpl);
	snprintf(g_run, sizeof(g_run), "%s/run", tmpl);
	mkdir(g_root, 0755);
	mkdir(g_run, 0755);

	test_read();
	test_uevent();
	test_low_battery();
	test_shutdown_empty();
	test_sdp_and_unknowns();
	test_overtemp();
	test_throttle();
	test_outputs();
	test_cadence();

	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpl);
	if (system(cmd) != 0)
		fprintf(stderr, "could not remove %s\n", tmpl);
	printf("test-powerd: %d/%d checks passed\n", g_tests - g_failures, g_tests);
	return g_failures ? 1 : 0;
}
