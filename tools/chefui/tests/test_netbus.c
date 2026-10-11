/*
 * test_netbus.c - netbus.c (NetworkManager/BlueZ over sd-bus) against a
 * fake NetworkManager and BlueZ on a private dbus-daemon, never the
 * system bus: DBUS_SYSTEM_BUS_ADDRESS points sd_bus_open_system() at it.
 *
 * The fakes run in a child process (blocking sd-bus is fine there) driven
 * by text commands on a pipe. Covered: initial enumeration, property
 * changes by signal, wlan0 appearing late, services leaving and coming
 * back (NameOwnerChanged), the bus daemon restarting (reconnect with
 * backoff), RSSI noise not triggering re-reads, no polling while idle, and
 * every call carrying NO_AUTO_START. Needs libsystemd and dbus-daemon.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <systemd/sd-bus.h>

#include "check.h"
#include "fakeloop.h"
#include "sys.h"
#include "sysstate.h"

#define NM "org.freedesktop.NetworkManager"
#define NM_PATH "/org/freedesktop/NetworkManager"
#define DEV_PATH NM_PATH "/Devices/3"
#define AC_PATH NM_PATH "/ActiveConnection/1"

static char tmp[256], addr[320];

/* ================================================================ fake */

static struct {
	sd_bus *bus;
	int wireless, dev;
	uint32_t state;
	char active[64], id[64];
	int powered;
	int conn[3];			/* dev_AA, dev_BB, dev_CC */
	bool cc_added;
	unsigned objects_calls, gets, autostart_violations;
} F;

/* A call from netbus that would let dbus-daemon activate a service.
 * (Getters also run for our own PropertiesChanged: no message then.) */
static void check_autostart(sd_bus_message *m)
{
	if (m && sd_bus_message_get_type(m, &(uint8_t){ 0 }) >= 0 &&
	    sd_bus_message_is_method_call(m, NULL, NULL) && sd_bus_message_get_auto_start(m) != 0)
		F.autostart_violations++;
}

static int get_b(sd_bus *b, const char *path, const char *iface, const char *prop,
		 sd_bus_message *reply, void *ud, sd_bus_error *e)
{
	(void)b; (void)path; (void)iface; (void)prop; (void)e;
	check_autostart(sd_bus_get_current_message(b));
	F.gets++;
	return sd_bus_message_append(reply, "b", *(int *)ud);
}

static int get_u(sd_bus *b, const char *path, const char *iface, const char *prop,
		 sd_bus_message *reply, void *ud, sd_bus_error *e)
{
	(void)path; (void)iface; (void)prop; (void)e; (void)ud;
	check_autostart(sd_bus_get_current_message(b));
	F.gets++;
	return sd_bus_message_append(reply, "u", F.state);
}

static int get_active(sd_bus *b, const char *path, const char *iface, const char *prop,
		      sd_bus_message *reply, void *ud, sd_bus_error *e)
{
	(void)path; (void)iface; (void)prop; (void)e; (void)ud;
	check_autostart(sd_bus_get_current_message(b));
	F.gets++;
	return sd_bus_message_append(reply, "o", F.active);
}

static int get_id(sd_bus *b, const char *path, const char *iface, const char *prop,
		  sd_bus_message *reply, void *ud, sd_bus_error *e)
{
	(void)path; (void)iface; (void)prop; (void)e; (void)ud;
	check_autostart(sd_bus_get_current_message(b));
	F.gets++;
	return sd_bus_message_append(reply, "s", F.id);
}

static int get_device(sd_bus_message *m, void *ud, sd_bus_error *e)
{
	const char *ifc;

	(void)ud;
	check_autostart(m);
	if (sd_bus_message_read(m, "s", &ifc) < 0)
		return -EINVAL;
	if (!F.dev || strcmp(ifc, "wlan0"))
		return sd_bus_error_set(e, NM ".UnknownDevice", "No such device");
	return sd_bus_reply_method_return(m, "o", DEV_PATH);
}

static const sd_bus_vtable nm_vt[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_PROPERTY("WirelessEnabled", "b", get_b, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_METHOD("GetDeviceByIpIface", "s", "o", get_device, 0),
	SD_BUS_SIGNAL("DeviceAdded", "o", 0),
	SD_BUS_SIGNAL("DeviceRemoved", "o", 0),
	SD_BUS_VTABLE_END
};

static const sd_bus_vtable dev_vt[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_PROPERTY("State", "u", get_u, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_PROPERTY("ActiveConnection", "o", get_active, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_VTABLE_END
};

static const sd_bus_vtable ac_vt[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_PROPERTY("Id", "s", get_id, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_VTABLE_END
};

static int get_rssi(sd_bus *b, const char *path, const char *iface, const char *prop,
		    sd_bus_message *reply, void *ud, sd_bus_error *e)
{
	(void)b; (void)path; (void)iface; (void)prop; (void)e; (void)ud;
	return sd_bus_message_append(reply, "n", -60);
}

static const sd_bus_vtable adapter_vt[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_PROPERTY("Powered", "b", get_b, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_VTABLE_END
};

static const sd_bus_vtable device_vt[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_PROPERTY("RSSI", "n", get_rssi, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_PROPERTY("Connected", "b", get_b, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_VTABLE_END
};

static int count_objects(sd_bus_message *m, void *ud, sd_bus_error *e)
{
	(void)ud; (void)e;
	if (!strcmp(sd_bus_message_get_member(m), "GetManagedObjects")) {
		F.objects_calls++;
		check_autostart(m);
	}
	return 0;	/* let the object manager answer */
}

static void fake_cmd(char *c, int out)
{
	char a[64] = "", b[64] = "";
	int v = 0;

	if (sscanf(c, "wireless %d", &v) == 1) {
		F.wireless = v;
		sd_bus_emit_properties_changed(F.bus, NM_PATH, NM, "WirelessEnabled", NULL);
	} else if (sscanf(c, "dev %d", &v) == 1) {
		F.dev = v;
		sd_bus_emit_signal(F.bus, NM_PATH, NM, v ? "DeviceAdded" : "DeviceRemoved", "o", DEV_PATH);
	} else if (sscanf(c, "state %d", &v) == 1) {
		F.state = (uint32_t)v;
		sd_bus_emit_properties_changed(F.bus, DEV_PATH, NM ".Device", "State", NULL);
	} else if (sscanf(c, "active %63s %63s", a, b) >= 1) {
		snprintf(F.active, sizeof(F.active), "%s", a);
		snprintf(F.id, sizeof(F.id), "%s", b);
		sd_bus_emit_properties_changed(F.bus, DEV_PATH, NM ".Device", "ActiveConnection", NULL);
	} else if (!strcmp(c, "nm-down")) {
		sd_bus_release_name(F.bus, NM);
	} else if (!strcmp(c, "nm-up")) {
		sd_bus_request_name(F.bus, NM, 0);
	} else if (!strcmp(c, "bz-down")) {
		sd_bus_release_name(F.bus, "org.bluez");
	} else if (!strcmp(c, "bz-up")) {
		sd_bus_request_name(F.bus, "org.bluez", 0);
	} else if (sscanf(c, "powered %d", &v) == 1) {
		F.powered = v;
		sd_bus_emit_properties_changed(F.bus, "/org/bluez/hci0", "org.bluez.Adapter1", "Powered", NULL);
	} else if (sscanf(c, "connect %d %d", &v, &F.conn[2]) == 2) {
		const char *p = v == 0 ? "/org/bluez/hci0/dev_AA" : v == 1 ? "/org/bluez/hci0/dev_BB"
			: "/org/bluez/hci0/dev_CC";

		F.conn[v] = F.conn[2];
		F.conn[2] = v == 2 ? F.conn[2] : F.conn[2];
		sd_bus_emit_properties_changed(F.bus, p, "org.bluez.Device1", "Connected", NULL);
	} else if (!strcmp(c, "rssi")) {
		sd_bus_emit_properties_changed(F.bus, "/org/bluez/hci0/dev_AA", "org.bluez.Device1", "RSSI", NULL);
	} else if (!strcmp(c, "add-cc")) {
		F.conn[2] = 1;
		sd_bus_add_object_vtable(F.bus, NULL, "/org/bluez/hci0/dev_CC", "org.bluez.Device1",
					 device_vt, &F.conn[2]);
		sd_bus_emit_interfaces_added(F.bus, "/org/bluez/hci0/dev_CC", "org.bluez.Device1", NULL);
	} else if (!strcmp(c, "count")) {
		dprintf(out, "%u %u %u\n", F.objects_calls, F.gets, F.autostart_violations);
	}
	sd_bus_flush(F.bus);
}

static void fake_main(int cmd_fd, int out_fd)
{
	char buf[256];
	size_t n = 0;

	memset(&F, 0, sizeof(F));
	F.wireless = 1;
	F.dev = 1;
	F.state = 100;
	snprintf(F.active, sizeof(F.active), AC_PATH);
	snprintf(F.id, sizeof(F.id), "HomeNet");
	F.powered = 1;
	F.conn[0] = 1;
	F.conn[1] = 0;
	if (sd_bus_open_system(&F.bus) < 0)
		_exit(10);
	sd_bus_add_object_vtable(F.bus, NULL, NM_PATH, NM, nm_vt, &F.wireless);
	sd_bus_add_object_vtable(F.bus, NULL, DEV_PATH, NM ".Device", dev_vt, NULL);
	sd_bus_add_object_vtable(F.bus, NULL, AC_PATH, NM ".Connection.Active", ac_vt, NULL);
	sd_bus_add_object_manager(F.bus, NULL, "/");
	sd_bus_add_filter(F.bus, NULL, count_objects, NULL);
	sd_bus_add_object_vtable(F.bus, NULL, "/org/bluez/hci0", "org.bluez.Adapter1", adapter_vt, &F.powered);
	sd_bus_add_object_vtable(F.bus, NULL, "/org/bluez/hci0/dev_AA", "org.bluez.Device1", device_vt, &F.conn[0]);
	sd_bus_add_object_vtable(F.bus, NULL, "/org/bluez/hci0/dev_BB", "org.bluez.Device1", device_vt, &F.conn[1]);
	if (sd_bus_request_name(F.bus, NM, 0) < 0 || sd_bus_request_name(F.bus, "org.bluez", 0) < 0)
		_exit(11);
	dprintf(out_fd, "ready\n");
	for (;;) {
		struct pollfd p[2] = { { sd_bus_get_fd(F.bus), (short)sd_bus_get_events(F.bus), 0 },
				       { cmd_fd, POLLIN, 0 } };

		while (sd_bus_process(F.bus, NULL) > 0)
			;
		p[0].events = (short)sd_bus_get_events(F.bus);
		if (poll(p, 2, 100) < 0)
			continue;
		if (p[1].revents) {
			ssize_t r = read(cmd_fd, buf + n, sizeof(buf) - 1 - n);
			char *nl;

			if (r <= 0)
				_exit(0);
			n += (size_t)r;
			buf[n] = 0;
			while ((nl = strchr(buf, '\n'))) {
				*nl = 0;
				fake_cmd(buf, out_fd);
				n -= (size_t)(nl + 1 - buf);
				memmove(buf, nl + 1, n + 1);
			}
		}
		if (p[0].revents & (POLLHUP | POLLERR))
			_exit(0);
	}
}

/* ============================================================== parent */

static pid_t daemon_pid, fake_pid;
static int cmd_w = -1, out_r = -1;

static void start_daemon(void)
{
	char conf[300], sock[300];
	FILE *f;

	snprintf(conf, sizeof(conf), "%s/bus.conf", tmp);
	snprintf(sock, sizeof(sock), "%s/bus", tmp);
	unlink(sock);
	f = fopen(conf, "w");
	fprintf(f, "<!DOCTYPE busconfig PUBLIC \"-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN\"\n"
		   " \"http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd\">\n"
		   "<busconfig><type>session</type><listen>unix:path=%s</listen>\n"
		   "<policy context=\"default\"><allow send_destination=\"*\" eavesdrop=\"true\"/>"
		   "<allow eavesdrop=\"true\"/><allow own=\"*\"/></policy></busconfig>\n", sock);
	fclose(f);
	daemon_pid = fork();
	if (daemon_pid == 0) {
		int null = open("/dev/null", O_WRONLY);

		dup2(null, 2);
		execlp("dbus-daemon", "dbus-daemon", "--nofork", "--config-file", conf, (char *)NULL);
		_exit(127);
	}
	for (int i = 0; i < 200 && access(sock, F_OK); i++)
		usleep(10000);
	CHECK(access(sock, F_OK) == 0);
}

static void stop_pid(pid_t *p)
{
	if (*p > 0) {
		kill(*p, SIGTERM);
		waitpid(*p, NULL, 0);
		*p = 0;
	}
}

static bool readline_out(char *buf, size_t size, int ms)
{
	size_t n = 0;

	while (n < size - 1) {
		struct pollfd p = { out_r, POLLIN, 0 };

		if (poll(&p, 1, ms) <= 0 || read(out_r, buf + n, 1) != 1)
			return false;
		if (buf[n] == '\n') {
			buf[n] = 0;
			return true;
		}
		n++;
	}
	return false;
}

static void start_fake(void)
{
	int c[2], o[2];
	char line[64];

	CHECK(pipe2(c, O_CLOEXEC) == 0 && pipe2(o, O_CLOEXEC) == 0);
	fake_pid = fork();
	if (fake_pid == 0) {
		close(c[1]);
		close(o[0]);
		fake_main(c[0], o[1]);
		_exit(0);
	}
	close(c[0]);
	close(o[1]);
	cmd_w = c[1];
	out_r = o[0];
	CHECK(readline_out(line, sizeof(line), 3000) && !strcmp(line, "ready"));
}

static void stop_fake(void)
{
	close(cmd_w);
	close(out_r);
	cmd_w = out_r = -1;
	stop_pid(&fake_pid);
}

static void fake(const char *cmd)
{
	CHECK(dprintf(cmd_w, "%s\n", cmd) > 0);
}

static void counts(unsigned *objects, unsigned *gets, unsigned *violations)
{
	char line[64];

	fake("count");
	*objects = *gets = *violations = ~0u;
	if (readline_out(line, sizeof(line), 2000))
		sscanf(line, "%u %u %u", objects, gets, violations);
}

static bool wifi_is(void *arg)
{
	const char *want = arg;
	char now[96];

	snprintf(now, sizeof(now), "%d%d%d %u %s", sys_state.wifi.known, sys_state.wifi.enabled,
		 sys_state.wifi.device, sys_state.wifi.dev_state, sys_state.wifi.ssid);
	return !strcmp(now, want);
}

static bool bt_is(void *arg)
{
	const char *want = arg;
	char now[32];

	snprintf(now, sizeof(now), "%d%d%d %d", sys_state.bt.known, sys_state.bt.adapter,
		 sys_state.bt.powered, sys_state.bt.connected);
	return !strcmp(now, want);
}

#define WAIT_WIFI(s, ms) CHECK(fakeloop_run_until(wifi_is, (void *)(s), ms))
#define WAIT_BT(s, ms) CHECK(fakeloop_run_until(bt_is, (void *)(s), ms))

int main(void)
{
	unsigned o1, g1, v1, o2, g2, v2;
	char cmd[300];

	signal(SIGPIPE, SIG_IGN);
	snprintf(tmp, sizeof(tmp), "/tmp/chef-netbus-test.XXXXXX");
	if (!mkdtemp(tmp))
		return 1;
	snprintf(addr, sizeof(addr), "unix:path=%s/bus", tmp);
	setenv("DBUS_SYSTEM_BUS_ADDRESS", addr, 1);
	fakeloop_init(false);

	/* no bus yet: unknown, and retrying quietly */
	sys_netbus_start();
	fakeloop_run_for(300);
	CHECK(!sys_state.wifi.known && !sys_state.bt.known);
	CHECK(fakeloop_log_count("unavailable") == 1);

	/* the bus and the services appear: initial enumeration */
	start_daemon();
	start_fake();
	WAIT_WIFI("111 100 HomeNet", 4000);
	WAIT_BT("111 1", 2000);
	CHECK(fakeloop_log_count("connected") >= 1);

	/* property changes by signal */
	fake("wireless 0");
	WAIT_WIFI("101 100 HomeNet", 1000);	/* off (the UI shows Off, not the SSID) */
	fake("wireless 1");
	WAIT_WIFI("111 100 HomeNet", 1000);
	fake("state 30");
	WAIT_WIFI("111 30 ", 1000);
	fake("active / -");
	fake("state 100");
	WAIT_WIFI("111 100 ", 1000);
	fake("active " AC_PATH " HomeNet");
	WAIT_WIFI("111 100 HomeNet", 1000);
	/* wlan0 goes and comes back */
	fake("dev 0");
	WAIT_WIFI("110 0 ", 1000);
	fake("dev 1");
	WAIT_WIFI("111 100 HomeNet", 1000);

	fake("connect 1 1");
	WAIT_BT("111 2", 1000);
	fake("powered 0");
	WAIT_BT("110 0", 1000);
	fake("powered 1");
	WAIT_BT("111 2", 1000);
	fake("add-cc");
	WAIT_BT("111 3", 1000);

	/* RSSI noise and idle time cause no re-reads */
	fakeloop_run_for(300);
	counts(&o1, &g1, &v1);
	for (int i = 0; i < 20; i++)
		fake("rssi");
	fakeloop_run_for(1500);
	counts(&o2, &g2, &v2);
	CHECK(o1 == o2 && g1 == g2);
	CHECK(v2 == 0);			/* every call had NO_AUTO_START */

	/* services leave and come back */
	fake("nm-down");
	WAIT_WIFI("000 0 ", 1000);
	fake("nm-up");
	WAIT_WIFI("111 100 HomeNet", 1000);
	fake("bz-down");
	WAIT_BT("000 0", 1000);
	fake("bz-up");
	WAIT_BT("111 3", 1000);

	/* the bus daemon restarts: lost, reconnect with backoff */
	fakeloop_log_clear();
	stop_fake();
	stop_pid(&daemon_pid);
	WAIT_WIFI("000 0 ", 2000);
	WAIT_BT("000 0", 1000);
	CHECK(fakeloop_log_count("lost") + fakeloop_log_count("unavailable") >= 1);
	fakeloop_run_for(1500);
	start_daemon();
	start_fake();
	WAIT_WIFI("111 100 HomeNet", 12000);
	WAIT_BT("111 1", 2000);

	sys_netbus_stop();
	CHECK(fakeloop_watched() == 0);
	stop_fake();
	stop_pid(&daemon_pid);
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmp);
	if (system(cmd))
		fprintf(stderr, "cleanup failed\n");
	return check_report("test-netbus");
}
