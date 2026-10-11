/*
 * test_wifi.c - wifi.c (the Wi-Fi pages' NetworkManager client) against
 * tests/fake_nm.py on a private dbus-daemon, never the system bus:
 * DBUS_SYSTEM_BUS_ADDRESS points both at it.
 *
 *   test-wifi tests/fake_nm.py
 *
 * Covered: the view while watched (APs grouped by SSID, saved profiles
 * marked, the connection's band, signal, IPv4 and gateway, no internet
 * claim without NetworkManager's check), the first scan and the scanning
 * indicator, joins (a new WPA2 network: profile, key-mgmt and passphrase
 * as NetworkManager received them, the caller's buffer wiped; a wrong
 * passphrase: BAD_PASSWORD and the new profile deleted; an open network;
 * a hidden one; a saved profile, kept when it fails), Forget, the
 * WirelessEnabled switch, no calls while not watched, and NO_AUTO_START
 * on every call.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/wait.h>
#include <unistd.h>

#include "check.h"
#include "fakeloop.h"
#include "sys.h"
#include "sysstate.h"
#include "wifi.h"

static char tmp[256], addr[320];

/* getrandom() as wifi.c calls it: it must never wait for the pool, and
 * with the pool not ready the UUID comes from /dev/urandom */
static bool entropy_not_ready;
static int blocking_getrandom;
ssize_t __real_getrandom(void *buf, size_t n, unsigned flags);
ssize_t __wrap_getrandom(void *buf, size_t n, unsigned flags)
{
	if (!(flags & GRND_NONBLOCK))
		blocking_getrandom++;
	if (entropy_not_ready) {
		errno = EAGAIN;
		return -1;
	}
	return __real_getrandom(buf, n, flags);
}
static pid_t daemon_pid, fake_pid;
static int cmd_w = -1, out_r = -1;
static const char *fake_py;

static void start_daemon(void)
{
	char conf[300], sock[300];
	FILE *f;

	snprintf(conf, sizeof(conf), "%s/bus.conf", tmp);
	snprintf(sock, sizeof(sock), "%s/bus", tmp);
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

		dup2(null, 1);	/* never hold the test's output open */
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

/* Also on a crash or a failed check's exit: no daemon left behind. */
static void cleanup(void)
{
	stop_pid(&fake_pid);
	stop_pid(&daemon_pid);
}

static void on_fatal(int sig)
{
	if (fake_pid > 0)
		kill(fake_pid, SIGTERM);
	if (daemon_pid > 0)
		kill(daemon_pid, SIGTERM);
	signal(sig, SIG_DFL);
	raise(sig);
}

/* A line from a fake, running the loop meanwhile (the fake may wait on
 * our replies). */
static bool readline_fd(int fd, char *buf, size_t size, int ms)
{
	size_t n = 0;
	int waited = 0;

	while (n < size - 1 && waited < ms) {
		struct pollfd p = { fd, POLLIN, 0 };

		if (poll(&p, 1, 0) <= 0) {
			fakeloop_run_for(10);
			waited += 10;
			continue;
		}
		if (read(fd, buf + n, 1) != 1)
			return false;
		if (buf[n] == '\n') {
			buf[n] = 0;
			return true;
		}
		n++;
	}
	return false;
}

/* init: commands the fake runs before it takes NetworkManager's name */
static bool readline_out(char *buf, size_t size, int ms)
{
	return readline_fd(out_r, buf, size, ms);
}

static void start_fake_with(const char *init1, const char *init2)
{
	int c[2], o[2];
	char line[64];

	CHECK(pipe2(c, O_CLOEXEC) == 0 && pipe2(o, O_CLOEXEC) == 0);
	fake_pid = fork();
	if (fake_pid == 0) {
		dup2(c[0], 0);
		dup2(o[1], 1);
		execlp("python3", "python3", "-I", fake_py, init1, init2, (char *)NULL);
		_exit(127);
	}
	close(c[0]);
	close(o[1]);
	cmd_w = c[1];
	out_r = o[0];
	CHECK(readline_out(line, sizeof(line), 10000) && !strcmp(line, "ready"));
}

static void start_fake(void)
{
	start_fake_with(NULL, NULL);
}

static void fake(const char *cmd)
{
	CHECK(dprintf(cmd_w, "%s\n", cmd) > 0);
}

/* The fake's JSON dump (one line). */
static char dump_buf[4096];

static const char *dump(void)
{
	fake("dump");
	if (!readline_out(dump_buf, sizeof(dump_buf), 3000))
		dump_buf[0] = 0;
	return dump_buf;
}

/* "key": N from the dump */
static long dump_num(const char *key)
{
	char pat[64];
	const char *p;

	snprintf(pat, sizeof(pat), "\"%s\": ", key);
	p = strstr(dump(), pat);
	return p ? strtol(p + strlen(pat), NULL, 10) : -1;
}

/* The fake's counter reaching min (the request may still be in flight). */
static bool wait_num(const char *key, long min, int ms)
{
	for (int t = 0; t < ms; t += 50) {
		if (dump_num(key) >= min)
			return true;
		fakeloop_run_for(50);
	}
	return false;
}

static bool has(const char *needle)
{
	return strstr(dump(), needle) != NULL;
}

static bool view_ready(void *arg)
{
	(void)arg;
	return wifi_view.n >= 3 && wifi_view.cur.valid && wifi_view.cur.ip4[0];
}

static bool join_is(void *arg)
{
	return wifi_view.join.st == *(enum wifi_join *)arg;
}

static bool not_scanning(void *arg)
{
	(void)arg;
	return !wifi_view.scanning;
}

static bool active_is(void *arg)
{
	return wifi_view.cur.valid && !strcmp(wifi_view.cur.text, arg) && wifi_view.cur.ip4[0];
}

static bool inactive(void *arg)
{
	(void)arg;
	return !wifi_view.cur.valid;
}

static bool empty_view(void *arg)
{
	(void)arg;
	return wifi_view.n == 0 && !wifi_view.cur.valid;
}

static bool no_cleanup(void *arg)
{
	(void)arg;
	return !sys_wifi_cleanup_pending();
}

static bool wireless_off(void *arg)
{
	(void)arg;
	return sys_state.wifi.known && !sys_state.wifi.enabled;
}

static const struct wifi_net *find(const char *text)
{
	for (int i = 0; i < wifi_view.n; i++)
		if (!strcmp(wifi_view.nets[i].text, text))
			return &wifi_view.nets[i];
	return NULL;
}

static bool listed(void *arg)
{
	return find(arg) != NULL;
}

static bool join_and_wait(enum wifi_join want, int ms)
{
	return fakeloop_run_until(join_is, &want, ms);
}

static struct wifi_ssid ssid_of(const char *s)
{
	struct wifi_ssid ss = { .len = (int)strlen(s) };

	memcpy(ss.b, s, (size_t)ss.len);
	return ss;
}

int main(int argc, char **argv)
{
	struct wifi_net n;
	struct wifi_ssid ss;
	char psk[WIFI_PSK_MAX + 1], cmd[300];
	long calls;

	if (argc != 2)
		return 2;
	fake_py = argv[1];
	signal(SIGPIPE, SIG_IGN);
	atexit(cleanup);
	signal(SIGSEGV, on_fatal);
	signal(SIGABRT, on_fatal);
	signal(SIGTERM, on_fatal);
	snprintf(tmp, sizeof(tmp), "/tmp/chef-wifi-test.XXXXXX");
	if (!mkdtemp(tmp))
		return 1;
	snprintf(addr, sizeof(addr), "unix:path=%s/bus", tmp);
	setenv("DBUS_SYSTEM_BUS_ADDRESS", addr, 1);
	fakeloop_init(false);
	start_daemon();
	start_fake();
	/* the radio environment: two Cafe APs (5 GHz stronger), a saved
	 * Garage, an open network, an enterprise one, a hidden one, HomeNet */
	fake("ap 1 Cafe_Velo wpa2 2437 40 ridefast2");
	fake("ap 2 Cafe_Velo wpa2 5180 70 ridefast2");
	fake("ap 3 Garage wpa2 2412 50 garagepass");
	fake("ap 4 OpenRide open 2462 30 -");
	fake("ap 5 Corp eap 5745 60 -");
	fake("ap 6 HomeNet mixed 5180 82 homepass1");
	fake("ap 7 Back_Room wpa3 2412 20 backroom1");
	fake("saved Garage garagepass");
	fake("saved HomeNet homepass1");

	sys_netbus_start();
	sys_wifi_init();

	/* not watched: no calls from wifi.c */
	fakeloop_run_for(500);
	calls = dump_num("calls");
	fakeloop_run_for(500);
	CHECK(dump_num("calls") == calls);

	/* connect HomeNet first (a saved profile), then look */
	sys_wifi_watch(true, true);
	CHECK(fakeloop_run_until(listed, "HomeNet", 5000));
	CHECK(wait_num("scans", 1, 3000));
	n = *find("HomeNet");
	CHECK(n.known && n.sec == WIFI_SEC_WPA2_WPA3);
	CHECK(sys_wifi_join(&n) == 0 && wifi_view.join.st == WIFI_JOIN_BUSY);
	CHECK(join_and_wait(WIFI_JOIN_DONE, 5000));
	CHECK(dump_num("activate") == 1 && dump_num("add_activate") == 0);
	sys_wifi_join_ack();
	CHECK(wifi_view.join.st == WIFI_JOIN_IDLE);
	CHECK(fakeloop_run_until(active_is, "HomeNet", 5000));

	/* the view: grouped, the active SSID out of the list, saved marked */
	CHECK(fakeloop_run_until(view_ready, NULL, 3000));
	CHECK(wifi_view.n == 5);			/* Cafe, Corp, Garage, OpenRide, Back Room */
	CHECK(!find("HomeNet"));
	CHECK(find("Cafe Velo") && find("Cafe Velo")->strength == 70 &&
	      find("Cafe Velo")->freq == 5180 && !find("Cafe Velo")->known);
	CHECK(find("Garage") && find("Garage")->known);
	CHECK(find("Corp") && find("Corp")->sec == WIFI_SEC_ENTERPRISE);
	CHECK(find("Back Room") && find("Back Room")->sec == WIFI_SEC_WPA3);
	CHECK(!strcmp(wifi_view.nets[0].text, "Cafe Velo"));	/* strongest first */
	CHECK(wifi_view.cur.ap_known && wifi_view.cur.freq == 5180 && wifi_view.cur.strength == 82);
	CHECK(!strcmp(wifi_view.cur.ip4, "192.168.7.20") && wifi_view.cur.prefix == 24 &&
	      !strcmp(wifi_view.cur.gw, "192.168.7.1"));
	CHECK(wifi_view.cur.dev_state == SYS_NM_DEVICE_ACTIVATED);
	CHECK(!wifi_view.cur.connectivity_known);	/* the check is off */
	CHECK(!strncmp(wifi_view.cur.conn, "/org/freedesktop/NetworkManager/Settings/", 41));
	/* the scan finished: LastScan changed */
	CHECK(fakeloop_run_until(not_scanning, NULL, 3000));

	/* a new WPA2 network with the right passphrase */
	n = *find("Cafe Velo");
	snprintf(psk, sizeof(psk), "ridefast2");
	CHECK(sys_wifi_join_new(&n.ssid, n.sec, psk, false, n.ap) == 0);
	CHECK(!memcmp(psk, "\0\0\0\0\0\0\0\0\0\0", 10));	/* wiped at once */
	CHECK(join_and_wait(WIFI_JOIN_DONE, 5000));
	CHECK(has("\"key_mgmt\": \"wpa-psk\"") && has("\"psk\": \"ridefast2\""));
	CHECK(has("\"ssid\": \"Cafe Velo\"") && has("\"hidden\": false"));
	CHECK(has("\"specific\": \"/org/freedesktop/NetworkManager/AccessPoint/2\""));
	CHECK(has("\"profiles\": [\"Cafe Velo\", \"Garage\", \"HomeNet\"]"));
	sys_wifi_join_ack();
	CHECK(fakeloop_run_until(active_is, "Cafe Velo", 5000));
	/* now the list has HomeNet again, saved; Cafe is the card */
	CHECK(fakeloop_run_until(listed, "HomeNet", 5000));
	CHECK(find("HomeNet")->known && !find("Cafe Velo"));

	/* a wrong passphrase: refused, and the profile that failed is gone */
	n = *find("Garage");
	snprintf(psk, sizeof(psk), "wrongpass");
	n.known = false;
	CHECK(sys_wifi_join_new(&n.ssid, n.sec, psk, false, n.ap) == 0);
	CHECK(join_and_wait(WIFI_JOIN_BAD_PASSWORD, 5000));
	fakeloop_run_for(300);
	CHECK(has("\"profiles\": [\"Cafe Velo\", \"Garage\", \"HomeNet\"]"));	/* new one deleted */
	CHECK(dump_num("deletes") == 1);
	sys_wifi_join_ack();

	/* a join while one runs is refused and leaves the running one alone,
	 * for a new and for a saved network */
	CHECK(fakeloop_run_until(listed, "Cafe Velo", 5000));
	n = *find("Cafe Velo");
	CHECK(sys_wifi_join(&n) == 0);		/* saved since the join above */
	snprintf(psk, sizeof(psk), "otherpass1");
	CHECK(sys_wifi_join_new(&n.ssid, n.sec, psk, false, n.ap) == -EBUSY);
	CHECK(psk[0] == 0 && wifi_view.join.st == WIFI_JOIN_BUSY);
	CHECK(sys_wifi_join(&n) == -EBUSY && wifi_view.join.st == WIFI_JOIN_BUSY);
	CHECK(join_and_wait(WIFI_JOIN_DONE, 5000));
	sys_wifi_join_ack();

	/* the activation ends before NetworkManager answers the call: success,
	 * then a refused passphrase (its new profile still deleted) */
	CHECK(fakeloop_run_until(listed, "HomeNet", 5000));
	fake("sync-next");
	CHECK(sys_wifi_join(find("HomeNet")) == 0);
	CHECK(join_and_wait(WIFI_JOIN_DONE, 5000));
	sys_wifi_join_ack();
	CHECK(fakeloop_run_until(active_is, "HomeNet", 5000));
	CHECK(fakeloop_run_until(listed, "Back Room", 5000));
	n = *find("Back Room");
	fake("sync-next");
	snprintf(psk, sizeof(psk), "notbackroom");
	CHECK(sys_wifi_join_new(&n.ssid, n.sec, psk, false, n.ap) == 0);
	CHECK(join_and_wait(WIFI_JOIN_BAD_PASSWORD, 5000));
	sys_wifi_join_ack();
	CHECK(fakeloop_run_until(no_cleanup, NULL, 3000));
	CHECK(!has("\"Back Room="));		/* no such profile left */

	/* a Delete that fails is looked up again (3 times at once), then
	 * retried until NetworkManager confirms */
	fake("delete-fails 4");
	snprintf(psk, sizeof(psk), "notbackroom");
	CHECK(sys_wifi_join_new(&n.ssid, n.sec, psk, false, n.ap) == 0);
	CHECK(join_and_wait(WIFI_JOIN_BAD_PASSWORD, 5000));
	sys_wifi_join_ack();
	CHECK(wait_num("delete_errors", 4, 3000));
	CHECK(sys_wifi_cleanup_pending() && has("\"Back Room="));
	CHECK(fakeloop_run_until(no_cleanup, NULL, 8000));	/* the 5 s retry */
	CHECK(!has("\"Back Room="));

	/* the entropy pool not ready: the UUID still comes, without waiting */
	entropy_not_ready = true;
	snprintf(psk, sizeof(psk), "notbackroom");
	CHECK(sys_wifi_join_new(&n.ssid, n.sec, psk, false, n.ap) == 0);
	CHECK(join_and_wait(WIFI_JOIN_BAD_PASSWORD, 5000));
	sys_wifi_join_ack();
	entropy_not_ready = false;
	{
		const char *u = strstr(dump(), "\"uuid\": \"");

		u = u ? u + 9 : NULL;			/* the value */
		CHECK(u && strlen(u) > WIFI_UUID_LEN && u[8] == '-' && u[14] == '4' &&
		      u[WIFI_UUID_LEN] == '"');
	}
	CHECK(blocking_getrandom == 0);
	CHECK(fakeloop_run_until(no_cleanup, NULL, 3000));

	/* a full delete queue: never evicted; a new profile or a Forget is
	 * refused instead, and everything is deleted once NM lets it */
	fake("delete-fails 1000");
	for (int i = 0; i < 4; i++) {
		snprintf(psk, sizeof(psk), "notbackroom");
		CHECK(sys_wifi_join_new(&n.ssid, n.sec, psk, false, n.ap) == 0);
		CHECK(join_and_wait(WIFI_JOIN_BAD_PASSWORD, 5000));
		sys_wifi_join_ack();
	}
	snprintf(psk, sizeof(psk), "notbackroom");
	CHECK(sys_wifi_join_new(&n.ssid, n.sec, psk, false, n.ap) == -ENOSPC);
	CHECK(psk[0] == 0 && wifi_view.join.st == WIFI_JOIN_IDLE);
	CHECK(sys_wifi_forget("00000000-0000-4000-8000-00000000abcd") == -ENOSPC);
	fakeloop_run_for(500);
	fake("delete-fails 0");
	CHECK(fakeloop_run_until(no_cleanup, NULL, 8000));
	CHECK(!has("\"Back Room="));

	/* a busy join refuses another; a bad passphrase never reaches NM */
	snprintf(psk, sizeof(psk), "short");
	CHECK(sys_wifi_join_new(&n.ssid, WIFI_SEC_WPA2, psk, false, n.ap) == -EINVAL);
	CHECK(psk[0] == 0);
	snprintf(psk, sizeof(psk), "bad\001char");
	CHECK(sys_wifi_join_new(&n.ssid, WIFI_SEC_WPA2, psk, false, n.ap) == -EINVAL);
	ss = ssid_of("Corp");
	psk[0] = 0;
	CHECK(sys_wifi_join_new(&ss, WIFI_SEC_ENTERPRISE, psk, false, NULL) == -EINVAL);

	/* an open network: no security section */
	CHECK(fakeloop_run_until(listed, "OpenRide", 5000));
	n = *find("OpenRide");
	CHECK(sys_wifi_join(&n) == 0);
	CHECK(join_and_wait(WIFI_JOIN_DONE, 5000));
	CHECK(has("\"key_mgmt\": \"\"") && has("\"psk\": \"\""));
	sys_wifi_join_ack();

	/* a hidden WPA3 network: hidden flag, SAE, no specific AP */
	ss = ssid_of("Back Room");
	snprintf(psk, sizeof(psk), "backroom1");
	CHECK(sys_wifi_join_new(&ss, WIFI_SEC_WPA3, psk, true, NULL) == 0);
	CHECK(join_and_wait(WIFI_JOIN_DONE, 5000));
	CHECK(has("\"hidden\": true") && has("\"key_mgmt\": \"sae\"") && has("\"specific\": \"/\""));
	sys_wifi_join_ack();

	/* a saved profile that fails is kept */
	CHECK(fakeloop_run_until(listed, "Garage", 5000));
	n = *find("Garage");
	fake("fail-next");
	CHECK(sys_wifi_join(&n) == 0);
	CHECK(join_and_wait(WIFI_JOIN_FAILED, 5000));
	CHECK(has("\"Garage\""));
	sys_wifi_join_ack();

	/* Forget the active profile */
	CHECK(fakeloop_run_until(listed, "HomeNet", 5000));
	CHECK(sys_wifi_join(find("HomeNet")) == 0);
	CHECK(join_and_wait(WIFI_JOIN_DONE, 5000));
	sys_wifi_join_ack();
	CHECK(fakeloop_run_until(active_is, "HomeNet", 5000));
	CHECK(wifi_view.cur.uuid[0] != 0);
	CHECK(sys_wifi_forget(wifi_view.cur.uuid) == 0);
	CHECK(fakeloop_run_until(inactive, NULL, 5000));
	CHECK(!has("\"HomeNet\""));
	CHECK(fakeloop_run_until(no_cleanup, NULL, 3000));
	CHECK(sys_wifi_forget("bad") == -EINVAL);

	/* the switch */
	CHECK(sys_wifi_set_enabled(false) == 0);
	CHECK(fakeloop_run_until(wireless_off, NULL, 3000));
	CHECK(has("\"wireless\": false"));
	CHECK(sys_wifi_set_enabled(true) == 0);
	fakeloop_run_for(300);
	CHECK(has("\"wireless\": true"));

	/* NetworkManager restarts while watched: the view empties and comes
	 * back when the name has an owner again */
	CHECK(fakeloop_run_until(listed, "Garage", 5000));
	close(cmd_w);
	close(out_r);
	stop_pid(&fake_pid);
	CHECK(fakeloop_run_until(empty_view, NULL, 5000));
	start_fake();
	fake("ap 3 Garage wpa2 2412 50 garagepass");
	CHECK(fakeloop_run_until(listed, "Garage", 5000));

	/* deletes keep failing, NetworkManager restarts: the profile is found
	 * again by its UUID, not by the object path, which the restarted NM
	 * gives to another profile */
	{
		char uuid[64] = "", init[160];
		const char *p, *q;

		fake("ap 7 Back_Room wpa3 2412 20 backroom1");
		CHECK(fakeloop_run_until(listed, "Back Room", 5000));
		n = *find("Back Room");
		fake("delete-fails 100");
		snprintf(psk, sizeof(psk), "notbackroom");
		CHECK(sys_wifi_join_new(&n.ssid, n.sec, psk, false, n.ap) == 0);
		CHECK(join_and_wait(WIFI_JOIN_BAD_PASSWORD, 5000));
		sys_wifi_join_ack();
		CHECK(wait_num("delete_errors", 1, 3000) && sys_wifi_cleanup_pending());
		p = strstr(dump(), "Back Room=");
		if (p && (q = strchr(p + 10, '='))) {
			memcpy(uuid, p + 10, (size_t)(q - p - 10) < sizeof(uuid) - 1 ? (size_t)(q - p - 10) : 0);
		}
		CHECK(strlen(uuid) == WIFI_UUID_LEN);
		close(cmd_w);
		close(out_r);
		stop_pid(&fake_pid);
		/* restarted: Garage first takes the first path, ours another */
		snprintf(init, sizeof(init), "saved Back_Room x %s", uuid);
		start_fake_with("saved Garage garagepass", init);
		fake("ap 3 Garage wpa2 2412 50 garagepass");
		CHECK(fakeloop_run_until(no_cleanup, NULL, 5000));
		CHECK(has("\"Garage=") && !has("\"Back Room="));
		CHECK(has("\"paths\": [\"/org/freedesktop/NetworkManager/Settings/11\"]"));
	}

	/* NetworkManager changes owner between a lookup and its answer: the
	 * answer (a path of the old instance) is dropped, the new one is asked
	 * again; Garage, which the new instance has at that old path, stays.
	 * Every Delete went to the unique name that answered the lookup. */
	{
		static const char u2[] = "00000000-0000-4000-8000-0000000000b2";
		char init[160], line[64];
		pid_t a_pid;
		int a_cmd, a_out;

		CHECK(dump_num("delete_wellknown") == 0 && dump_num("delete_unique") >= 1);
		close(cmd_w);
		close(out_r);
		stop_pid(&fake_pid);
		snprintf(init, sizeof(init), "saved Back_Room x %s", u2);
		start_fake_with(init, "swap-on-lookup");	/* u2 at /Settings/11 */
		CHECK(sys_wifi_forget(u2) == 0);
		CHECK(readline_out(line, sizeof(line), 5000) && !strcmp(line, "released"));
		a_pid = fake_pid;
		a_cmd = cmd_w;
		a_out = out_r;
		fake_pid = 0;
		start_fake_with("saved Garage garagepass", init);	/* Garage at /11 */
		CHECK(dprintf(a_cmd, "reply\n") > 0);
		CHECK(fakeloop_run_until(no_cleanup, NULL, 5000));
		CHECK(has("\"Garage=") && !has("\"Back Room="));
		CHECK(dump_num("delete_wellknown") == 0 && dump_num("delete_unique") == 1);
		close(a_cmd);
		close(a_out);
		stop_pid(&a_pid);
		fake("ap 3 Garage wpa2 2412 50 garagepass");
	}

	/* not watched: AP noise causes no calls */
	sys_wifi_watch(false, false);
	fakeloop_run_for(300);
	calls = dump_num("calls");
	for (int i = 0; i < 10; i++) {
		snprintf(cmd, sizeof(cmd), "strength 3 %d", 20 + i);
		fake(cmd);
	}
	fakeloop_run_for(2000);
	CHECK(dump_num("calls") == calls);
	CHECK(dump_num("autostart") == 0);		/* every call had NO_AUTO_START */

	sys_netbus_stop();
	CHECK(fakeloop_watched() == 0);
	close(cmd_w);
	close(out_r);
	stop_pid(&fake_pid);
	stop_pid(&daemon_pid);
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmp);
	if (system(cmd))
		fprintf(stderr, "cleanup failed\n");
	return check_report("test-wifi");
}
