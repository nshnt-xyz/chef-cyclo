# Builds this directory's new GPS transport/protocol code -- msmipc
# (libqrtr-compatible layer on AF_MSM_IPC), irsc, qmuxd-lite, the vendored
# qrtr/ QMI codec -- plus the display/touch probe fbtouch, the on-device
# log screen fblog (fblog/, binary fblog/fblog), the gpsd feed nmea-broker,
# the side-button gesture daemon buttond, the WAV tone generator wavtone
# and atomic TAS2560 calibration-control writer tas2560-send-cal
# (for the speaker test, initramfs/usr/bin/speaker-test-tone), and their
# host tests. Does not touch btprobe.c
# or build anything under scripts/, initramfs/ -- those are built/wired into
# the boot image by scripts/mkinitramfs.sh. `make test` does read (but not
# build) initramfs/usr/bin/gps-up, to run its host-testable selection logic
# alongside the rest of the GPS test suite (see gps-userspace-handoff.md).
# tools/rmtfs/ and tools/servreg-locator/ each have their own Makefile and
# `test` target (`make -C tools/rmtfs test`, `make -C tools/servreg-locator
# test`) and are deliberately not folded into this one's -- run all three to
# get full GPS test coverage.
#
# Native (host) build + tests:
#   make && make test
#
# Cross build for the phone (aarch64, musl): once a musl aarch64 toolchain
# exists under toolchain/ (see gps-userspace-handoff.md 3.1 -- provisioning
# it is scripts/setup-toolchain.sh's job, not this Makefile's), do:
#   make clean && make CC=<toolchain>/bin/aarch64-linux-musl-gcc LDFLAGS=-static
# CFLAGS/LDFLAGS are plain make variables, not "+="-only, so they can be
# overridden wholesale on the command line same as above.

REPO_ROOT := ..
CC ?= gcc
CFLAGS ?= -std=gnu11 -Wall -Wextra -O2 -g
LDFLAGS ?=
INCLUDES := -I. -Iqrtr -I$(REPO_ROOT)/kernel/include/uapi -I$(REPO_ROOT)/kernel/include
ALL_CFLAGS = $(CFLAGS) $(INCLUDES)

BIN := irsc qmuxd-lite fbtouch fblog/fblog nmea-broker buttond wavtone tas2560-send-cal
VENDOR_OBJ := qmi.o logging.o
TEST_BIN := tests/test-qmux tests/test-msmipc tests/test-fbtouch tests/test-fblog \
            tests/test-nmea-broker tests/test-buttond tests/test-wavtone \
            tests/test-tas2560-send-cal

.PHONY: all test test-shell clean

all: $(BIN) $(VENDOR_OBJ)

qmi.o: qrtr/qmi.c qrtr/libqrtr.h
	$(CC) $(ALL_CFLAGS) -c -o $@ $<

logging.o: qrtr/logging.c qrtr/logging.h
	$(CC) $(ALL_CFLAGS) -c -o $@ $<

msmipc.o: msmipc.c msmipc.h qrtr/libqrtr.h
	$(CC) $(ALL_CFLAGS) -c -o $@ $<

qmux.o: qmux.c qmux.h
	$(CC) $(ALL_CFLAGS) -c -o $@ $<

irsc: irsc.c msmipc.o
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

qmuxd-lite: qmuxd-lite.c msmipc.o qmux.o
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

tests/test-qmux: tests/test_qmux.c qmux.o
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

tests/test-msmipc: tests/test_msmipc.c msmipc.o
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS) -Wl,--wrap=recvfrom

# Display + touch probe (see fbtouch.c's header). It only needs the libc's
# own <linux/fb.h>/<linux/input.h>, so it is built without the vendor
# kernel include paths the QMI code needs (those would shadow the host's
# headers). The test includes the .c with FBTOUCH_NO_MAIN to reach the
# pure functions.
fbtouch: fbtouch.c fbdev.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

tests/test-fbtouch: tests/test_fbtouch.c fbtouch.c fbdev.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# Read-only boot/probe output screen (fblog/fblog.c): tails /dev/kmsg onto
# the panel through the same fbdev.h contract, never opens an input
# device. Font table is generated (fblog/mkfont.py) and committed.
fblog/fblog: fblog/fblog.c fblog/font9x15.h fbdev.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

tests/test-fblog: tests/test_fblog.c fblog/fblog.c fblog/font9x15.h fbdev.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# gpsd feed (nmea-broker.c): qmicli follower stdout -> one UDP datagram per
# valid NMEA sentence to gpsd on loopback, optional tee/log, set-once clock
# step from the first valid RMC. Plain libc, no kernel headers. The test
# includes the .c with NMEA_BROKER_NO_MAIN (injected clock, never
# settimeofday) and drives the loop against a real loopback UDP socket.
nmea-broker: nmea-broker.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

tests/test-nmea-broker: tests/test_nmea-broker.c nmea-broker.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# Side-button gesture daemon (buttond.c): grabs the qpnp_pon/gpio-keys evdev
# nodes, turns presses into short/double/long/chord gestures, publishes them
# on /run/buttond.sock to claimants, and toggles the panel (fblog's
# /run/fblog.off protocol) on an unclaimed power.short. Only libc's
# <linux/input.h>. The test includes the .c with BUTTOND_NO_MAIN and drives
# the server over socketpairs with an injected log/kill and a temp flag file.
buttond: buttond.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

tests/test-buttond: tests/test_buttond.c buttond.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# WAV sine-tone generator for the speaker test (tools/wavtone.c): tinyalsa's
# tinyplay only plays WAV files, so this writes one at the requested
# frequency/duration/dBFS. Only libc's <math.h>, so no kernel include paths;
# link -lm for the host build. The test includes the .c with WAVTONE_NO_MAIN.
wavtone: wavtone.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) -lm

tests/test-wavtone: tests/test_wavtone.c wavtone.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) -lm

# Atomic writer for the TAS2560's write-only five-integer calibration
# control. Uses only libc and the kernel ALSA UAPI; no alsa-lib/tinyalsa.
# The test includes the source with TAS2560_SEND_CAL_NO_MAIN and injects
# open/ioctl/close boundaries, so it needs no ALSA device on the host.
tas2560-send-cal: tas2560-send-cal.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

tests/test-tas2560-send-cal: tests/test_tas2560_send_cal.c tas2560-send-cal.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

test: $(TEST_BIN) test-shell
	./tests/test-qmux
	./tests/test-msmipc
	./tests/test-fbtouch
	./tests/test-fblog
	./tests/test-nmea-broker
	./tests/test-buttond
	./tests/test-wavtone
	./tests/test-tas2560-send-cal

# gps-up (initramfs/) selection-logic regression and the ride image's
# ride-logger (initramfs-ride/) supervision/failure/HTTP-extraction
# regression (stub gps-up/qmicli, real busybox httpd); neither is a build
# product of this directory, but this is the natural place to run them
# alongside the rest of the GPS test suite. See tests/test_gps-up.sh and
# tests/test_ride-logger.sh.
test-shell:
	sh tests/test_gps-up.sh
	sh tests/test_ride-logger.sh
	sh tests/test_audio-up.sh
	sh tests/test_speaker-test-tone.sh

clean:
	rm -f *.o irsc qmuxd-lite fbtouch fblog/fblog nmea-broker buttond wavtone \
		tas2560-send-cal $(TEST_BIN)
