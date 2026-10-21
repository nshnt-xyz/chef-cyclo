# buttond: the side-button gesture daemon (buttond.c), see buttond.c's file
# header and docs/features/buttons-and-power-off.md. It grabs the
# qpnp_pon/gpio-keys evdev nodes, turns presses into
# short/double/long/chord gestures, publishes them on /run/chef/buttond.sock
# to claimants, by default toggles the screen-off flag /run/chef/screen-off
# on an unclaimed power.short and powers off cleanly (sync + busybox
# `poweroff`) when power is held 3 s and released. Started from inittab
# (respawn). Only libc's <linux/input.h> (device discovery through the
# header-only ../evdev.h); the socket, its JSON lines and the log come from
# ../common/ (linesrv, json, chef_log), compiled in from source.
# tools/tests/test_fblog.c (tools/Makefile) also includes buttond.c, for
# the screen-off flag protocol.
#
#   make                 host build: build/host/buttond
#   make test            host tests (build/host/tests/)
#   make device          the phone binary: build/device/buttond
#   make install-device DESTDIR=DIR   it into DIR/usr/bin
#
# The test includes buttond.c with BUTTOND_NO_MAIN and drives the server
# over socketpairs with an injected log/kill and a temp flag file, also
# under ASan/UBSan.

REPO_ROOT := ../..
CC ?= cc
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
SAN := -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all
COMMON := ../common/chef_log.c ../common/linesrv.c ../common/json.c
COMMON_H := ../common/chef_log.h ../common/linesrv.h ../common/json.h ../common/chef_paths.h

.PHONY: all test clean device install-device

all: $(H)/buttond

include ../device.mk

$(H)/buttond: buttond.c ../evdev.h $(COMMON) $(COMMON_H)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -I.. -o $@ buttond.c $(COMMON) $(LDFLAGS)

$(H)/tests/test-buttond: tests/test_buttond.c buttond.c ../evdev.h $(COMMON) $(COMMON_H)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -I.. -o $@ tests/test_buttond.c $(COMMON) $(LDFLAGS)

$(H)/tests/test-buttond-san: tests/test_buttond.c buttond.c ../evdev.h $(COMMON) $(COMMON_H)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(SAN) -I.. -o $@ tests/test_buttond.c $(COMMON) $(LDFLAGS)

$(D)/buttond: buttond.c ../evdev.h $(COMMON) $(COMMON_H)
	@mkdir -p $(@D)
	$(DEVICE_CC) $(DEVICE_CFLAGS) -I.. -o $@ buttond.c $(COMMON)

device: check-device-cc $(D)/buttond

install-device: check-destdir device
	install -d $(DESTDIR)/usr/bin
	install -m 755 $(D)/buttond $(DESTDIR)/usr/bin/

test: $(H)/buttond $(H)/tests/test-buttond $(H)/tests/test-buttond-san
	$(H)/tests/test-buttond
	$(H)/tests/test-buttond-san

clean:
	rm -rf build
