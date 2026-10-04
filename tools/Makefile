# Builds this directory's new GPS transport/protocol code -- msmipc
# (libqrtr-compatible layer on AF_MSM_IPC), irsc, qmuxd-lite, the vendored
# qrtr/ QMI codec -- plus the display/touch probe fbtouch, the on-device
# log screen fblog (fblog/, binary fblog/fblog), the gpsd feed nmea-broker,
# the GNSS demand manager gps-manager, the side-button gesture daemon
# buttond, the battery daemon powerd, the WAV tone generator wavtone
# and atomic TAS2560 calibration-control writer tas2560-send-cal
# (for the speaker test, initramfs/usr/bin/speaker-test-tone), the two
# resident speaker-protection experiment helpers afe-topology-cal and
# tert-tx-hold (for initramfs/usr/bin/spk-protect-probe), and their
# host tests. Does not touch btprobe.c
# or build anything under scripts/, initramfs/ -- those are built/wired into
# the boot image by scripts/mkinitramfs.sh. `make test` does read (but not
# build) initramfs/usr/bin/gps-up, to run its host-testable selection logic
# alongside the rest of the GPS test suite (see gps-userspace-handoff.md).
# tools/rmtfs/ and tools/servreg-locator/ each have their own Makefile and
# `test` target (`make -C tools/rmtfs test`, `make -C tools/servreg-locator
# test`) and are deliberately not folded into this one's -- run all three to
# get full GPS test coverage. The same goes for the sensors daemon,
# tools/sensord/ (`make -C tools/sensord test`).
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

BIN := irsc qmuxd-lite fbtouch fblog/fblog nmea-broker gps-manager buttond powerd wavtone tas2560-send-cal \
       afe-topology-cal tert-tx-hold wifi-psk
VENDOR_OBJ := qmi.o logging.o
TEST_BIN := tests/test-qmux tests/test-msmipc tests/test-evdev tests/test-fbtouch tests/test-fblog \
            tests/test-nmea-broker tests/test-gps-manager tests/test-buttond tests/test-powerd tests/test-wavtone \
            tests/test-tas2560-send-cal tests/test-afe-topology-cal tests/test-tert-tx-hold tests/test-wlan-fw

.PHONY: all test test-shell test-wifi clean

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

# Shared evdev discovery and MT-B slot decoding (evdev.h, header-only;
# used by fbtouch, buttond and chefui's touch.c). open/close/ioctl are
# wrapped so the scan runs over a temporary directory of fake devices.
tests/test-evdev: tests/test_evdev.c evdev.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) -Wl,--wrap=open -Wl,--wrap=close -Wl,--wrap=ioctl

# Display + touch probe (see fbtouch.c's header). It only needs the libc's
# own <linux/fb.h>/<linux/input.h>, so it is built without the vendor
# kernel include paths the QMI code needs (those would shadow the host's
# headers). The test includes the .c with FBTOUCH_NO_MAIN to reach the
# pure functions.
fbtouch: fbtouch.c fbdev.h evdev.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

tests/test-fbtouch: tests/test_fbtouch.c fbtouch.c fbdev.h evdev.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

.PHONY: test-display-inventory
test-display-inventory:
	@set -eu; test_bin=$$(mktemp /tmp/chef-display-test.XXXXXX); \
	trap 'rm -f "$$test_bin"' EXIT; \
	$(CC) $(CFLAGS) -o "$$test_bin" tests/test_display_input_info.c $(LDFLAGS) -Wl,--wrap=open -Wl,--wrap=close -Wl,--wrap=ioctl; \
	"$$test_bin"
	python3 tests/test_display_touch_inventory.py

# Read-only boot/probe output screen (fblog/fblog.c): tails /dev/kmsg onto
# the panel through the same fbdev.h contract, never opens an input
# device. Font table is generated (fblog/mkfont.py) and committed.
fblog/fblog: fblog/fblog.c fblog/font9x15.h fbdev.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# ioctl() is wrapped so the daemon's borrow/resume/idle path runs against a
# temporary file standing in for fb0.
tests/test-fblog: tests/test_fblog.c buttond.c fblog/fblog.c fblog/font9x15.h fbdev.h evdev.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) -Wl,--wrap=ioctl

# gpsd feed (nmea-broker.c): qmicli follower stdout -> one UDP datagram per
# valid NMEA sentence to gpsd on loopback, optional tee/log, set-once clock
# step from the first valid RMC. Plain libc, no kernel headers. The test
# includes the .c with NMEA_BROKER_NO_MAIN (injected clock, never
# settimeofday) and drives the loop against a real loopback UDP socket.
nmea-broker: nmea-broker.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

tests/test-nmea-broker: tests/test_nmea-broker.c nmea-broker.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# GNSS demand manager (gps-manager.c): leases on /run/gps-manager.sock drive
# the LOC CID/session, gpsd and the follower | nmea-broker pipeline on top of
# gps-up's shared modem. Plain libc. The test includes the .c with
# GPSMGR_NO_MAIN and drives the core through fake operations;
# tests/test_gps-manager.sh runs the binary with stub qmicli/gpsd, the real
# nmea-broker, kill -9 and a respawn.
gps-manager: gps-manager.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

tests/test-gps-manager: tests/test_gps-manager.c gps-manager.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# Side-button gesture daemon (buttond.c): grabs the qpnp_pon/gpio-keys evdev
# nodes, turns presses into short/double/long/chord gestures, publishes them
# on /run/buttond.sock to claimants, and toggles the panel (fblog's
# /run/fblog.off protocol) on an unclaimed power.short. Only libc's
# <linux/input.h>. The test includes the .c with BUTTOND_NO_MAIN and drives
# the server over socketpairs with an injected log/kill and a temp flag file.
buttond: buttond.c evdev.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

tests/test-buttond: tests/test_buttond.c buttond.c evdev.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# Battery daemon (powerd.c): BatteryService low-battery/over-temperature
# shutdown policy, batt_health-style power_supply CSV log under /run/power
# and the thermal-engine SS-BATT-BATT charge throttle on
# battery/system_temp_level. Plain libc + <linux/netlink.h>. The test
# includes the .c with POWERD_NO_MAIN against a fake sysfs tree with
# injected hooks; tests/test_powerd.sh runs the host binary end to end with
# -x so nothing powers off.
powerd: powerd.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

tests/test-powerd: tests/test_powerd.c powerd.c
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

# AFE topology installer for the speaker-protection experiment
# (afe-topology-cal.c): one AUDIO_SET_CALIBRATION of AFE_TOPOLOGY_CAL_TYPE
# on /dev/msm_audio_cal, then resident until SIGTERM (the kernel frees the
# block when the last fd closes). The message layout is spelled out in the
# source (the vendor UAPI header is not userspace-clean), so the tool
# itself builds with plain libc; the test includes that header through the
# vendor include paths, with the two shims it needs, to check the layout,
# cal-type index and ioctl number against it byte for byte.
afe-topology-cal: afe-topology-cal.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

tests/test-afe-topology-cal: tests/test_afe_topology_cal.c afe-topology-cal.c
	$(CC) $(ALL_CFLAGS) -Wno-cpp -o $@ $< $(LDFLAGS)

# TERT_MI2S_TX holder (tert-tx-hold.c): opens/configures/starts the
# kernel's "TERT MI2S_TX Hostless" capture PCM so AFE port 0x1005 comes
# up, resident until SIGTERM, then DROP + close. Only libc's
# <sound/asound.h>; the test injects open/ioctl/wait/close.
tert-tx-hold: tert-tx-hold.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

tests/test-tert-tx-hold: tests/test_tert_tx_hold.c tert-tx-hold.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

tests/test-wlan-fw: tests/test_wlan_fw.c wlan-fw.c qmux.c qmux.h msmipc.h
	$(CC) $(ALL_CFLAGS) -Wno-cpp -o $@ tests/test_wlan_fw.c qmux.c $(LDFLAGS)

wifi-psk: wifi-psk.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

test-wifi: tests/test-wlan-fw wifi-psk
	./tests/test-wlan-fw
	python3 tests/test_wifi.py
	python3 tests/test_wifi_psk.py
	python3 tests/test_wifi_build.py
	python3 tests/test_wifi_nm.py

test: $(TEST_BIN) test-shell wifi-psk test-display-inventory
	./tests/test-qmux
	./tests/test-msmipc
	./tests/test-evdev
	./tests/test-fbtouch
	./tests/test-fblog
	./tests/test-nmea-broker
	./tests/test-gps-manager
	./tests/test-buttond
	./tests/test-powerd
	./tests/test-wavtone
	./tests/test-tas2560-send-cal
	./tests/test-afe-topology-cal
	./tests/test-tert-tx-hold
	./tests/test-wlan-fw
	python3 tests/test_wifi.py
	python3 tests/test_wifi_psk.py
	python3 tests/test_wifi_build.py
	python3 tests/test_wifi_nm.py

# gps-up (initramfs/) selection-logic regression; not a build product of
# this directory, but this is the natural place to run it alongside the
# rest of the GPS test suite. See tests/test_gps-up.sh.
test-shell: powerd gps-manager nmea-broker
	sh tests/test_powerd.sh
	sh tests/test_gps-up.sh
	sh tests/test_gps-manager.sh
	sh tests/test_audio-up.sh
	sh tests/test_speaker-test-tone.sh
	sh tests/test_afe-debug.sh
	sh tests/test_spk-protect-probe.sh
	sh tests/test_acdb-afe-topology.sh

clean:
	rm -f *.o irsc qmuxd-lite fbtouch fblog/fblog nmea-broker gps-manager buttond powerd wavtone \
		tas2560-send-cal afe-topology-cal tert-tx-hold wifi-psk $(TEST_BIN)
