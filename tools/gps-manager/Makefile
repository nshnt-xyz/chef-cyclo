# gps-manager: the GNSS demand manager (gps-manager.c, docs/features/gps.md).
# Leases on /run/gps-manager.sock drive the LOC CID/session, gpsd and the
# follower | nmea-broker pipeline on top of gps-up's shared modem, and stop
# them after the last release. Never touches gps-up. Started from inittab
# (respawn). Plain libc.
#
#   make                 host build: build/host/gps-manager
#   make test            host tests (build/host/tests/)
#   make device          the phone binary: build/device/gps-manager
#   make install-device DESTDIR=DIR   it into DIR/usr/bin
#
# tests/test_gps-manager.c includes gps-manager.c with GPSMGR_NO_MAIN and
# drives the core through fake operations; tests/test_gps-manager.sh runs
# the binary with stub qmicli/gpsd, the real nmea-broker, kill -9 and a
# respawn. nmea-broker stays in tools/ (built and packed there); the shell
# test's host copy is compiled here into build/host/ from ../nmea-broker.c,
# so this directory never builds into tools/build/ (the root `make test`
# runs both suites in parallel).

REPO_ROOT := ../..
CC ?= gcc
CFLAGS ?= -std=gnu11 -Wall -Wextra -Werror -O2 -g
LDFLAGS ?=
H := build/host
D := build/device

.PHONY: all test clean device install-device

all: $(H)/gps-manager

include ../device.mk

$(H)/gps-manager: gps-manager.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(H)/nmea-broker: ../nmea-broker.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(H)/tests/test-gps-manager: tests/test_gps-manager.c gps-manager.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(D)/gps-manager: gps-manager.c
	@mkdir -p $(@D)
	$(DEVICE_CC) $(DEVICE_CFLAGS) -o $@ gps-manager.c

device: check-device-cc $(D)/gps-manager

install-device: check-destdir device
	install -d $(DESTDIR)/usr/bin
	install -m 755 $(D)/gps-manager $(DESTDIR)/usr/bin/

test: $(H)/tests/test-gps-manager $(H)/gps-manager $(H)/nmea-broker
	$(H)/tests/test-gps-manager
	sh tests/test_gps-manager.sh

clean:
	rm -rf build
