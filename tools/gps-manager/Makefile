# gps-manager: the GNSS demand manager (main.c's header,
# docs/features/gps.md). Leases on /run/chef/gps-manager.sock drive the LOC
# CID/session, gpsd and the follower | nmea-broker pipeline on top of
# modem-up's shared modem, and stop them after the last release. Never
# touches modem-up. Started from inittab (respawn). Plain libc. Sources:
# core.c (state machine), server.c (lease server), owner.c (owner check,
# state file), main.c (process shell), cli.c (command-line client), with
# the internal header gps-manager.h; the socket, its JSON lines and the
# log come from ../common/ (linesrv, json, chef_log), compiled in from
# source.
#
#   make                 host build: build/host/gps-manager
#   make test            host tests (build/host/tests/)
#   make device          the phone binary: build/device/gps-manager
#   make install-device DESTDIR=DIR   it into DIR/usr/bin
#
# tests/test_gps-manager.c links core.c, server.c and owner.c and drives
# them through fake operations, also under ASan/UBSan;
# tests/test_gps-manager.sh runs the binary with stub qmicli/gpsd, the
# real nmea-broker, kill -9 and a respawn. nmea-broker stays in tools/
# (built and packed there); the shell test's host copy is compiled here
# into build/host/ from ../nmea-broker.c, so this directory never builds
# into tools/build/ (the root `make test` runs both suites in parallel).

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
SAN := -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all
COMMON := ../common/chef_log.c ../common/linesrv.c ../common/json.c
COMMON_H := ../common/chef_log.h ../common/linesrv.h ../common/json.h ../common/chef_paths.h
LIB_SRC := core.c server.c owner.c $(COMMON)
SRC := $(LIB_SRC) main.c cli.c

.PHONY: all test clean device install-device

all: $(H)/gps-manager

include ../device.mk

$(H)/gps-manager: $(SRC) gps-manager.h $(COMMON_H)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -I.. -o $@ $(SRC) $(LDFLAGS)

$(H)/nmea-broker: ../nmea-broker.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(H)/tests/test-gps-manager: tests/test_gps-manager.c $(LIB_SRC) gps-manager.h $(COMMON_H)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -I.. -o $@ tests/test_gps-manager.c $(LIB_SRC) $(LDFLAGS)

$(H)/tests/test-gps-manager-san: tests/test_gps-manager.c $(LIB_SRC) gps-manager.h $(COMMON_H)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(SAN) -I.. -o $@ tests/test_gps-manager.c $(LIB_SRC) $(LDFLAGS)

$(D)/gps-manager: $(SRC) gps-manager.h $(COMMON_H)
	@mkdir -p $(@D)
	$(DEVICE_CC) $(DEVICE_CFLAGS) -I.. -o $@ $(SRC)

device: check-device-cc $(D)/gps-manager

install-device: check-destdir device
	install -d $(DESTDIR)/usr/bin
	install -m 755 $(D)/gps-manager $(DESTDIR)/usr/bin/

test: $(H)/tests/test-gps-manager $(H)/tests/test-gps-manager-san $(H)/gps-manager $(H)/nmea-broker
	$(H)/tests/test-gps-manager
	$(H)/tests/test-gps-manager-san
	sh tests/test_gps-manager.sh

clean:
	rm -rf build
