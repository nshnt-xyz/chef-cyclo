/*
 * deviceinfo.c - the About page's facts: the image metadata
 * (/etc/chef/image-info), the boot serial from /proc/cmdline, the Bluetooth
 * and Wi-Fi addresses, kernel and uptime. The bounded parsers and
 * sys_device_info_collect() are pure and run on the settings worker thread
 * (reads of regular files and sysfs only, never blocking the UI); the model
 * callback and the want() request run on the UI thread. sysfs attributes
 * report st_size 4096, so they are read by length, not by size.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/utsname.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "deviceinfo.h"
#include "settings.h"
#include "sys.h"
#include "sysstate.h"

#define INFO_MAX 1024
#define EPOCH_MIN 1767225600ULL
#define EPOCH_MAX 4102444800ULL

bool sys_image_info_parse(const char *text, size_t len, struct sys_image_info *out)
{
	char copy[INFO_MAX + 1], *line[6], *p;
	struct sys_image_info v = { 0 };
	const char *key[] = { "name", "version", "commit", "dirty", "epoch" };
	uint64_t epoch = 0;

	if (!text || !out || !len || len > INFO_MAX || text[len - 1] != '\n' ||
	    memchr(text, 0, len))
		return false;
	memcpy(copy, text, len);
	copy[len] = 0;
	if (strchr(copy, '\r') || strchr(copy, '\t'))
		return false;
	p = copy;
	for (int i = 0; i < 6; i++) {
		line[i] = p;
		char *nl = strchr(p, '\n');
		if (!nl)
			return false;
		*nl = 0;
		p = nl + 1;
	}
	if (*p || strcmp(line[0], "chef-image-info 1"))
		return false;
	for (int i = 0; i < 5; i++) {
		char *space = strchr(line[i + 1], ' ');
		if (!space || space == line[i + 1] || !space[1] || strchr(space + 1, ' '))
			return false;
		*space++ = 0;
		if (strcmp(line[i + 1], key[i]))
			return false;
		if (i == 0) {
			if (strcmp(space, "chef-cyclo"))
				return false;
			snprintf(v.name, sizeof(v.name), "%s", space);
		} else if (i == 1) {
			if (strcmp(space, "dev"))
				return false;
			snprintf(v.version, sizeof(v.version), "%s", space);
		} else if (i == 2) {
			if (strlen(space) != 40)
				return false;
			for (int j = 0; j < 40; j++)
				if (!(space[j] >= '0' && space[j] <= '9') &&
				    !(space[j] >= 'a' && space[j] <= 'f')) return false;
			snprintf(v.commit, sizeof(v.commit), "%s", space);
		} else if (i == 3) {
			if (strcmp(space, "0") && strcmp(space, "1"))
				return false;
			v.dirty = space[0] == '1';
		} else {
			if (!*space || strlen(space) > 12 || (space[0] == '0' && space[1]))
				return false;
			for (const char *q = space; *q; q++) {
				if (!isdigit((unsigned char)*q))
					return false;
				epoch = epoch * 10 + (unsigned)(*q - '0');
				if (epoch > EPOCH_MAX)
					return false;
			}
			if (epoch < EPOCH_MIN || epoch >= EPOCH_MAX)
				return false;
			v.epoch = epoch;
		}
	}
	*out = v;
	return true;
}

bool sys_device_serial_parse(const char *cmdline, size_t len, char *out, size_t out_size)
{
	const char key[] = "androidboot.serialno=";
	char value[64];
	int found = 0;
	size_t n = 0;

	if (!cmdline || !out || !out_size || len > 8192 || memchr(cmdline, 0, len))
		return false;
	for (size_t i = 0; i < len;) {
		while (i < len && isspace((unsigned char)cmdline[i]))
			i++;
		size_t start = i;
		while (i < len && !isspace((unsigned char)cmdline[i]))
			i++;
		size_t width = i - start;
		if (width >= sizeof(key) - 1 && !memcmp(cmdline + start, key, sizeof(key) - 1)) {
			found++;
			n = width - (sizeof(key) - 1);
			if (!n || n >= sizeof(value))
				return false;
			memcpy(value, cmdline + start + sizeof(key) - 1, n);
			value[n] = 0;
			for (size_t j = 0; j < n; j++)
				if ((unsigned char)value[j] < 33 || (unsigned char)value[j] > 126)
					return false;
		}
	}
	if (found != 1 || n + 1 > out_size)
		return false;
	memcpy(out, value, n + 1);
	return true;
}

bool sys_device_mac_parse(const char *text, size_t len, char out[18])
{
	static const char hex[] = "0123456789ABCDEF";
	char value[18];
	size_t n;
	if (!text || !out)
		return false;
	n = len;
	while (n && (text[n - 1] == '\n' || text[n - 1] == '\r'))
		n--;
	if (n != 17)
		return false;
	for (int i = 0; i < 17; i++) {
		if (i % 3 == 2) {
			if (text[i] != ':')
				return false;
			value[i] = ':';
		} else {
			const char *p = strchr(hex, toupper((unsigned char)text[i]));
			if (!p)
				return false;
			value[i] = *p;
		}
	}
	value[17] = 0;
	memcpy(out, value, sizeof(value));
	return true;
}

static int bounded_read(const char *path, char *buf, size_t cap, bool proc)
{
	struct stat st;
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	size_t used = 0;
	if (fd < 0)
		return -errno;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || (!proc && st.st_size >= (off_t)cap)) {
		close(fd);
		return -EINVAL;
	}
	while (used < cap) {
		ssize_t n = read(fd, buf + used, cap - used);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0) {
			int e = errno;
			close(fd);
			return -e;
		}
		if (!n)
			break;
		used += (size_t)n;
	}
	if (used == cap) {
		close(fd);
		return -EFBIG;
	}
	buf[used] = 0;
	close(fd);
	return (int)used;
}

int sys_device_info_collect(const struct sys_device_info_request *request,
			   struct sys_device_info *out)
{
	struct sys_device_info v = { 0 };
	struct sys_image_info image;
	struct utsname u;
	struct timespec boot, mono;
	char buf[8193];
	int n;
	const char *metadata_path, *cmdline_path, *bluetooth_path, *wifi_path;

	if (!request || !out || request->width <= 0 || request->height <= 0)
		return -EINVAL;
	snprintf(v.image_name, sizeof(v.image_name), "chef-cyclo");
	snprintf(v.version, sizeof(v.version), "Build information unavailable");
	snprintf(v.lvgl, sizeof(v.lvgl), "%s", request->lvgl);
	snprintf(v.resolution, sizeof(v.resolution), "%d x %d", request->width, request->height);
	metadata_path = request->metadata_path[0] ? request->metadata_path : "/etc/chef/image-info";
	cmdline_path = request->cmdline_path[0] ? request->cmdline_path : "/proc/cmdline";
	bluetooth_path = request->bluetooth_path[0] ? request->bluetooth_path :
		"/sys/class/bluetooth/hci0/address";
	wifi_path = request->wifi_path[0] ? request->wifi_path : "/sys/class/net/wlan0/address";
	if (!uname(&u))
		snprintf(v.kernel, sizeof(v.kernel), "Linux %s", u.release);
	n = bounded_read(metadata_path, buf, 1025, false);
	if (n > 0 && sys_image_info_parse(buf, (size_t)n, &image)) {
		struct tm tm;
		time_t epoch = (time_t)image.epoch;
		v.metadata_known = true;
		snprintf(v.image_name, sizeof(v.image_name), "%s", image.name);
		snprintf(v.version, sizeof(v.version), "%s", image.version);
		snprintf(v.commit, sizeof(v.commit), "%s", image.commit);
		v.dirty = image.dirty;
		if (gmtime_r(&epoch, &tm))
			strftime(v.build_date, sizeof(v.build_date), "%Y-%m-%d UTC", &tm);
	}
	n = bounded_read(cmdline_path, buf, 8193, true);
	if (n >= 0)
		v.serial_known = sys_device_serial_parse(buf, (size_t)n, v.serial, sizeof(v.serial));
	n = bounded_read(bluetooth_path, buf, 64, true);	/* sysfs: st_size is 4096 */
	if (n >= 0)
		v.bluetooth_known = sys_device_mac_parse(buf, (size_t)n, v.bluetooth);
	n = bounded_read(wifi_path, buf, 64, true);
	if (n >= 0)
		v.wifi_known = sys_device_mac_parse(buf, (size_t)n, v.wifi);
	if (!clock_gettime(CLOCK_BOOTTIME, &boot))
		v.uptime_s = (uint64_t)boot.tv_sec;
	if (!clock_gettime(CLOCK_MONOTONIC, &mono))
		v.sampled_mono_ms = (int64_t)mono.tv_sec * 1000 + mono.tv_nsec / 1000000;
	*out = v;
	return 0;
}

/* ---- model: the worker's result lands in sys_state.device ---- */

static void info_done(const struct sys_device_info *info, void *user)
{
	(void)user;
	sys_state.device = *info;
	sys_changed(SYS_DEVICE_INFO);
}

void sys_device_info_init(void)
{
	ui_store_on_info(info_done, NULL);
}

void sys_device_info_want(bool visible, int width, int height, const char *lvgl)
{
	struct sys_device_info_request r;

	if (!visible) {
		ui_store_info_cancel();
		return;
	}
	memset(&r, 0, sizeof(r));
	r.width = width;
	r.height = height;
	snprintf(r.lvgl, sizeof(r.lvgl), "%s", lvgl ? lvgl : "");
	sys_device_info_init();
	ui_store_info(&r);
}
