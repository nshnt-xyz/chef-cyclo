/*
 * powerd's inputs: the power_supply sysfs attributes of one sample (read
 * against any root, so the tests use a fake tree), the policy's "powered"
 * and the power source, and the uevent filter. See main.c's header.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "powerd.h"

/* ------------------------------------------------------------- sample */

/*
 * Read one sysfs attribute into buf (trailing newline stripped). Returns
 * the length, or -errno (a qcom psy attribute whose getter fails reads as
 * -EINVAL, or as the literal text "-22" on some properties).
 */
static int read_attr(const char *root, const char *psy, const char *attr, char *buf, size_t len)
{
	char path[512];
	ssize_t n;
	int fd;

	if (snprintf(path, sizeof(path), "%s/%s/%s", root, psy, attr) >= (int)sizeof(path))
		return -ENAMETOOLONG;
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	n = read(fd, buf, len - 1);
	if (n < 0) {
		int err = errno;

		close(fd);
		return -err;
	}
	close(fd);
	buf[n] = '\0';
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' '))
		buf[--n] = '\0';
	return (int)n;
}

/* Integer attribute, POWERD_NA when missing/unreadable/not a number. With
 * m22_na, the literal -22 (-EINVAL printed by the qcom getters) is also NA;
 * temp keeps it, since -2.2 C is a real reading. */
static int read_int(const char *root, const char *psy, const char *attr, bool m22_na)
{
	char buf[64], *end;
	long v;

	if (read_attr(root, psy, attr, buf, sizeof(buf)) <= 0)
		return POWERD_NA;
	errno = 0;
	v = strtol(buf, &end, 10);
	if (errno || end == buf || *end != '\0' || v <= INT_MIN || v > INT_MAX)
		return POWERD_NA;
	if (m22_na && v == -22)
		return POWERD_NA;
	return (int)v;
}

/* String attribute; "" when missing. Commas (the CSV separator) and
 * control characters become ';' / ' '. */
static void read_str(const char *root, const char *psy, const char *attr, char *out, size_t len)
{
	char buf[128];
	size_t i, n;

	out[0] = '\0';
	if (read_attr(root, psy, attr, buf, sizeof(buf)) <= 0)
		return;
	if (strcmp(buf, "-22") == 0)
		return;
	n = strlen(buf);
	if (n >= len)
		n = len - 1;	/* truncate */
	memcpy(out, buf, n);
	out[n] = '\0';
	for (i = 0; out[i]; i++) {
		if (out[i] == ',')
			out[i] = ';';
		else if ((unsigned char)out[i] < 0x20 || out[i] == '=')
			out[i] = ' ';
	}
}

/* An `online` attribute: POWERD_ABSENT when the psy itself does not exist
 * (a supply that cannot be attached), POWERD_NA when it exists but the
 * value cannot be read (unknown, never "off"). */
static int read_online(const char *root, const char *psy)
{
	char path[512];
	struct stat st;

	snprintf(path, sizeof(path), "%s/%s", root, psy);
	if (stat(path, &st) < 0 && errno == ENOENT)
		return POWERD_ABSENT;
	return read_int(root, psy, "online", true);
}

void sample_read(const char *root, struct sample *s)
{
	read_str(root, "battery", "status", s->status, sizeof(s->status));
	read_str(root, "battery", "health", s->health, sizeof(s->health));
	read_str(root, "battery", "charge_type", s->charge_type, sizeof(s->charge_type));
	s->present = read_int(root, "battery", "present", true);
	s->capacity = read_int(root, "battery", "capacity", true);
	s->temp = read_int(root, "battery", "temp", false);
	s->voltage_uv = read_int(root, "battery", "voltage_now", true);
	/* bms, not battery: the battery psy follows the ABI sign (+ = charging) */
	s->current_ua = read_int(root, "bms", "current_now", true);
	s->profile_fcc_ua = read_int(root, "battery", "constant_charge_current_max", true);
	s->stl = read_int(root, "battery", "system_temp_level", true);
	s->nstl = read_int(root, "battery", "num_system_temp_levels", true);
	s->ocv_uv = read_int(root, "bms", "voltage_ocv", true);
	s->charge_full_uah = read_int(root, "bms", "charge_full", true);
	s->charge_counter_uah = read_int(root, "bms", "charge_counter", true);
	s->cycle_count = read_int(root, "bms", "cycle_count", true);
	s->soc_ready = read_int(root, "bms", "soc_reporting_ready", true);
	s->usb_online = read_online(root, "usb");
	read_str(root, "usb", "real_type", s->usb_type, sizeof(s->usb_type));
	s->usb_current_max_ua = read_int(root, "usb", "current_max", true);
	s->usb_input_ua = read_int(root, "usb", "input_current_now", true);
	s->usb_voltage_uv = read_int(root, "usb", "voltage_now", true);
	s->input_settled_ua = read_int(root, "main", "input_current_settled", true);
	read_str(root, "usb", "typec_mode", s->typec_mode, sizeof(s->typec_mode));
	s->pc_online = read_online(root, "pc_port");
	s->dc_online = read_online(root, "dc");
}

/* Any input known to be online. usb/online reads 0 for an SDP (or a PD
 * host data role) port: the kernel reports that input on pc_port/online
 * instead (qpnp-smb2.c usb/pc_port get_prop). */
bool supply_online(const struct sample *s)
{
	return s->usb_online > 0 || s->pc_online > 0 || s->dc_online > 0;
}

static bool known_off(int online)
{
	return online == 0 || online == POWERD_ABSENT;
}

/*
 * The low-battery policy's "powered", the complement of "known to be
 * discharging": battery/status reads exactly Discharging (smb-lib derives
 * it from raw input presence, so a charger that is attached but not
 * charging reads "Not charging") and usb, pc_port and dc are each known to
 * be offline or do not exist. Anything unreadable counts as powered, so an
 * unknown value can never lead to a low-battery shutdown.
 */
bool powered(const struct sample *s)
{
	return !(strcmp(s->status, "Discharging") == 0 && known_off(s->usb_online) &&
		 known_off(s->pc_online) && known_off(s->dc_online));
}

const char *source_name(const struct sample *s)
{
	if (s->usb_online > 0 || s->pc_online > 0)
		return "usb";
	if (s->dc_online > 0)
		return "dc";
	if (powered(s))
		return "unknown";
	return "battery";
}

/* ------------------------------------------------------------- uevent */

/*
 * A kernel uevent datagram is "ACTION@DEVPATH\0KEY=VALUE\0...". udev's
 * rebroadcasts start with "libudev" and are not ours. True when it is a
 * power_supply event.
 */
bool uevent_is_power_supply(const char *buf, size_t len)
{
	size_t off = 0;

	if (len == 0 || (len >= 7 && memcmp(buf, "libudev", 7) == 0))
		return false;
	if (!memchr(buf, '@', strnlen(buf, len)))
		return false;
	while (off < len) {
		size_t l = strnlen(buf + off, len - off);

		if (l == 22 && memcmp(buf + off, "SUBSYSTEM=power_supply", 22) == 0)
			return true;
		off += l + 1;
	}
	return false;
}
