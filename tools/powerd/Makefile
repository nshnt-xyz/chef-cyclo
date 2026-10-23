# powerd: the battery daemon (main.c's header,
# docs/features/battery-and-charging.md): BatteryService low-battery
# warn/critical and over-temperature clean shutdown, batt_health-style
# power_supply CSV log (/data/v1/power or /run/chef/power) and state file
# under /run/chef/power, and the thermal-engine SS-BATT-BATT 44/42 C charge
# throttle on battery/system_temp_level. The supervised OpenRC service
# powerd (docs/features/services.md). Plain libc + <linux/netlink.h>.
# Sources: sample.c (sysfs reads, powered(), the uevent filter), policy.c
# (confirmations, alerts, throttle, poll cadence, one whole sample), log.c
# (state file, CSV format and rotation, the batched -l log and its flush),
# main.c (actions, the loop, options, `status` and `flush`), with the
# internal header powerd.h; the log lines go through ../common/chef_log.c,
# compiled in from source.
#
#   make                 host build: build/host/powerd
#   make test            host tests (build/host/tests/)
#   make device          the phone binary: build/device/powerd
#   make install-device DESTDIR=DIR   it into DIR/usr/bin
#
# tests/test_powerd.c links sample.c, policy.c and log.c against a fake
# sysfs tree with injected hooks; tests/test_powerd.sh runs the host
# binary end to end with -x so nothing powers off. tools/Makefile builds
# its own host copy (tools/build/host/powerd) from these sources for
# tools/tests/test_chef_state.py.

REPO_ROOT := ../..
CC ?= gcc
CFLAGS ?= -std=gnu11 -Wall -Wextra -Werror -O2 -g
LDFLAGS ?=
H := build/host
# No test reaches the host's syslog: chef_log() (../common/) finds no
# socket at this path, and the scripts' log helper
# (rootfs-overlay/usr/lib/chef/log.sh) no syslogd and no logger; the lines
# go to the kmsg path each test sets.
export CHEF_LOG_SOCKET := /nonexistent/chef-log-socket
export CHEF_LOG_SYSLOGD_PID := /nonexistent/syslogd.pid
export CHEF_LOG_LOGGER := /nonexistent/logger
D := build/device
COMMON := ../common/chef_log.c
COMMON_H := ../common/chef_log.h ../common/chef_paths.h
LIB_SRC := sample.c policy.c log.c $(COMMON)
SRC := $(LIB_SRC) main.c

.PHONY: all test clean device install-device

all: $(H)/powerd

include ../device.mk

$(H)/powerd: $(SRC) powerd.h $(COMMON_H)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -I.. -o $@ $(SRC) $(LDFLAGS)

$(H)/tests/test-powerd: tests/test_powerd.c $(LIB_SRC) powerd.h $(COMMON_H)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -I.. -I. -o $@ tests/test_powerd.c $(LIB_SRC) $(LDFLAGS)

$(D)/powerd: $(SRC) powerd.h $(COMMON_H)
	@mkdir -p $(@D)
	$(DEVICE_CC) $(DEVICE_CFLAGS) -I.. -o $@ $(SRC)

device: check-device-cc $(D)/powerd

install-device: check-destdir device
	install -d $(DESTDIR)/usr/bin
	install -m 755 $(D)/powerd $(DESTDIR)/usr/bin/

test: $(H)/tests/test-powerd $(H)/powerd
	$(H)/tests/test-powerd
	sh tests/test_powerd.sh

clean:
	rm -rf build
