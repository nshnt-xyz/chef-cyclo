/*
 * deviceinfo.h - the About page's device facts (deviceinfo.c): bounded
 * parsers and the worker-side collector.
 */
#ifndef CHEF_DEVICEINFO_H
#define CHEF_DEVICEINFO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct sys_image_info {
	char name[32], version[32], commit[41];
	bool dirty;
	uint64_t epoch;
};

struct sys_device_info {
	bool metadata_known;
	char image_name[32], version[32], commit[41], build_date[32];
	bool dirty;
	char lvgl[24], kernel[128], resolution[32], serial[64], bluetooth[18], wifi[18];
	bool serial_known, bluetooth_known, wifi_known;
	uint64_t uptime_s;
	int64_t sampled_mono_ms;
};

struct sys_device_info_request {
	int width, height;
	char lvgl[24];
	char metadata_path[256], cmdline_path[256];
	char bluetooth_path[256], wifi_path[256];
};

bool sys_image_info_parse(const char *text, size_t len, struct sys_image_info *out);
bool sys_device_serial_parse(const char *cmdline, size_t len, char *out, size_t out_size);
bool sys_device_mac_parse(const char *text, size_t len, char out[18]);
int sys_device_info_collect(const struct sys_device_info_request *request,
			   struct sys_device_info *out);

#endif
