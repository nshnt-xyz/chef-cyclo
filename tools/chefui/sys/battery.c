/*
 * battery.c - battery level and supply state from sysfs power_supply.
 *
 * capacity and status come from power_supply/battery; "powered" is the
 * opposite of powerd's "unpowered" (docs/features/battery-and-charging.md):
 * status exactly Discharging and usb, pc_port and dc each offline or
 * absent. Anything unreadable counts as powered (no warn colour on a
 * guess). Re-read on power_supply uevents (kernel netlink, coalesced over
 * 200 ms) and every 60 s as a fallback.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <linux/netlink.h>

#include "chefui.h"
#include "sys.h"
#include "sysstate.h"

#define BATT_POLL_MS 60000
#define BATT_COALESCE_MS 200

static struct {
	char root[200];
	int fd;
	lv_timer_t *poll, *soon;
} B = { .fd = -1 };

/* Small sysfs attribute, trailing newline stripped. 0 or -errno. */
static int read_attr(const char *dev, const char *attr, char *buf, size_t size)
{
	char path[300];
	ssize_t n;
	int fd;

	snprintf(path, sizeof(path), "%s/%s/%s", B.root, dev, attr);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	n = read(fd, buf, size - 1);
	if (n < 0) {
		int e = -errno;

		close(fd);
		return e;
	}
	close(fd);
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' '))
		n--;
	buf[n] = 0;
	return 0;
}

/* 0 offline or absent, 1 online, -1 unreadable/garbage */
static int online(const char *dev)
{
	char b[16];
	int r = read_attr(dev, "online", b, sizeof(b));

	if (r == -ENOENT)
		return 0;
	if (r)
		return -1;
	return !strcmp(b, "0") ? 0 : !strcmp(b, "1") ? 1 : -1;
}

void sys_battery_refresh(void)
{
	char cap[16], st[32];
	bool known = false, powered = true;
	int c = 0;

	if (!read_attr("battery", "capacity", cap, sizeof(cap))) {
		char *end;
		long v = strtol(cap, &end, 10);

		if (*cap && !*end && v >= 0 && v <= 100) {
			known = true;
			c = (int)v;
		}
	}
	if (!read_attr("battery", "status", st, sizeof(st)) && !strcmp(st, "Discharging") &&
	    online("usb") == 0 && online("pc_port") == 0 && online("dc") == 0)
		powered = false;
	if (known != sys_state.batt.known || c != sys_state.batt.capacity ||
	    powered != sys_state.batt.powered) {
		sys_state.batt.known = known;
		sys_state.batt.capacity = c;
		sys_state.batt.powered = powered;
		sys_changed(SYS_BATT);
	}
}

static void soon_fired(lv_timer_t *t)
{
	(void)t;
	B.soon = NULL;
	sys_battery_refresh();
}

static void poll_fired(lv_timer_t *t)
{
	(void)t;
	sys_battery_refresh();
}

bool sys_uevent_is_power_supply(const char *msg, size_t len)
{
	size_t i = 0;

	/* "ACTION@DEVPATH\0KEY=VALUE\0..." */
	while (i < len) {
		const char *s = msg + i;
		size_t n = strnlen(s, len - i);

		if (n == sizeof("SUBSYSTEM=power_supply") - 1 && !memcmp(s, "SUBSYSTEM=power_supply", n))
			return true;
		i += n + 1;
	}
	return false;
}

static void uevent_readable(int fd, short revents, void *user)
{
	char buf[4096];
	bool hit = false;
	int i;

	(void)user;
	/* bounded: at most 64 messages per wakeup, the rest next time */
	for (i = 0; i < 64; i++) {
		ssize_t n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		if (sys_uevent_is_power_supply(buf, (size_t)n))
			hit = true;
	}
	if (revents & (POLLERR | POLLNVAL)) {
		/* ENOBUFS (overrun) shows as an error read; just re-read state */
		hit = true;
	}
	if (hit && !B.soon) {
		B.soon = lv_timer_create(soon_fired, BATT_COALESCE_MS, NULL);
		lv_timer_set_repeat_count(B.soon, 1);
	}
}

static int uevent_open(void)
{
	struct sockaddr_nl sa = { .nl_family = AF_NETLINK, .nl_groups = 1 };
	int fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);

	if (fd < 0)
		return -errno;
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa))) {
		int e = -errno;

		close(fd);
		return e;
	}
	return fd;
}

int sys_battery_start(const char *ps_root, int uevent_fd)
{
	snprintf(B.root, sizeof(B.root), "%s", ps_root ? ps_root : "/sys/class/power_supply");
	B.fd = uevent_fd >= 0 ? uevent_fd : uevent_open();
	if (B.fd < 0)
		chefui_log("battery: uevent socket: %s; polling every %d s", strerror(-B.fd),
			   BATT_POLL_MS / 1000);
	else
		chefui_watch_fd(B.fd, POLLIN, uevent_readable, NULL);
	B.poll = lv_timer_create(poll_fired, BATT_POLL_MS, NULL);
	sys_battery_refresh();
	return 0;
}

void sys_battery_stop(void)
{
	if (B.fd >= 0) {
		chefui_unwatch_fd(B.fd);
		close(B.fd);
		B.fd = -1;
	}
	if (B.poll)
		lv_timer_delete(B.poll);
	if (B.soon)
		lv_timer_delete(B.soon);
	B.poll = B.soon = NULL;
}
