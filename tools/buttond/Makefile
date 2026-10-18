# buttond: the side-button gesture daemon (buttond.c), see buttond.c's file
# header and docs/features/buttons-and-power-off.md. It grabs the
# qpnp_pon/gpio-keys evdev nodes, turns presses into
# short/double/long/chord gestures, publishes them on /run/buttond.sock to
# claimants, by default toggles the panel (fblog's /run/fblog.off protocol)
# on an unclaimed power.short and powers off cleanly (sync + busybox
# `poweroff`) when power is held 3 s and released. Started from inittab
# (respawn). Only libc's <linux/input.h> (device discovery through the
# header-only ../evdev.h). tools/tests/test_fblog.c (tools/Makefile) also
# includes buttond.c, for the screen-off flag protocol.
#
#   make                 host build: build/host/buttond
#   make test            host tests (build/host/tests/)
#   make device          the phone binary: build/device/buttond
#   make install-device DESTDIR=DIR   it into DIR/usr/bin
#
# The test includes buttond.c with BUTTOND_NO_MAIN and drives the server
# over socketpairs with an injected log/kill and a temp flag file.

REPO_ROOT := ../..
CC ?= cc
CFLAGS ?= -std=gnu11 -Wall -Wextra -Werror -O2 -g
LDFLAGS ?=
H := build/host
D := build/device

.PHONY: all test clean device install-device

all: $(H)/buttond

include ../device.mk

$(H)/buttond: buttond.c ../evdev.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -I.. -o $@ buttond.c $(LDFLAGS)

$(H)/tests/test-buttond: tests/test_buttond.c buttond.c ../evdev.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -I.. -o $@ tests/test_buttond.c $(LDFLAGS)

$(D)/buttond: buttond.c ../evdev.h
	@mkdir -p $(@D)
	$(DEVICE_CC) $(DEVICE_CFLAGS) -I.. -o $@ buttond.c

device: check-device-cc $(D)/buttond

install-device: check-destdir device
	install -d $(DESTDIR)/usr/bin
	install -m 755 $(D)/buttond $(DESTDIR)/usr/bin/

test: $(H)/buttond $(H)/tests/test-buttond
	$(H)/tests/test-buttond

clean:
	rm -rf build
