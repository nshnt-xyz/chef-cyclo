# Builds this directory's new GPS transport/protocol code -- msmipc
# (libqrtr-compatible layer on AF_MSM_IPC), irsc, qmuxd-lite, the vendored
# qrtr/ QMI codec -- plus the display/touch probe fbtouch, the on-device
# log screen fblog (fblog/), the gpsd feed nmea-broker, the GNSS demand
# manager gps-manager, the battery daemon powerd, the WAV tone generator
# wavtone and atomic TAS2560 calibration-control writer tas2560-send-cal
# (for the speaker test, rootfs-overlay/usr/bin/speaker-test-tone), the
# Wi-Fi helpers wlan-fw and wifi-psk, the A/B slot marker abslot, rtc-edge,
# the freestanding Bluetooth helper btprobe, and their host tests. `make
# test` also runs the host tests of the scripts under rootfs-overlay/
# (gps-up's selection logic, audio-up, chef-state, chef-reboot, SSH,
# storage, stage 1). The modem services in tools/rmtfs/,
# tools/servreg-locator/, tools/tftp/, the sensors daemon tools/sensord/
# and the button daemon tools/buttond/ have their own Makefiles; the
# top-level Makefile runs them all.
#
#   make                 host build: build/host/ (fblog is host-only, a
#                        debug tool that is not packed)
#   make test            host tests (binaries in build/host/tests/)
#   make device          static aarch64 musl build for the phone:
#                        build/device/ (tools/device.mk has the flags)
#   make install-device DESTDIR=DIR   the device binaries into DIR/usr/bin
#
# CFLAGS/LDFLAGS are plain make variables, so they can be overridden
# wholesale on the command line.

REPO_ROOT := ..

CC ?= gcc
CFLAGS ?= -std=gnu11 -Wall -Wextra -Werror -O2 -g
LDFLAGS ?=
H := build/host
D := build/device
INCLUDES := -I. -Iqrtr -I$(H)/include
ALL_CFLAGS = $(CFLAGS) $(INCLUDES)

BIN := $(addprefix $(H)/,irsc qmuxd-lite fbtouch fblog nmea-broker gps-manager powerd wavtone \
       tas2560-send-cal wifi-psk abslot rtc-edge)
VENDOR_OBJ := $(H)/qmi.o $(H)/logging.o
TEST_BIN := $(addprefix $(H)/tests/,test-qmux test-msmipc test-evdev test-fbtouch test-fblog \
            test-nmea-broker test-gps-manager test-powerd test-wavtone \
            test-tas2560-send-cal test-wlan-fw)
DEVICE_BIN := btprobe wifi-psk wlan-fw irsc qmuxd-lite fbtouch nmea-broker gps-manager \
              abslot rtc-edge powerd wavtone tas2560-send-cal

.PHONY: all test test-shell test-wifi clean device install-device

all: $(BIN) $(VENDOR_OBJ)

# after the first rule, so `all` stays the default goal
include device.mk

$(H)/qmi.o: qrtr/qmi.c qrtr/libqrtr.h
	@mkdir -p $(@D)
	$(CC) $(ALL_CFLAGS) -c -o $@ $<

$(H)/logging.o: qrtr/logging.c qrtr/logging.h
	@mkdir -p $(@D)
	$(CC) $(ALL_CFLAGS) -c -o $@ $<

$(H)/msmipc.o: msmipc.c msmipc.h qrtr/libqrtr.h $(H)/include/linux/msm_ipc.h
	@mkdir -p $(@D)
	$(CC) $(ALL_CFLAGS) -c -o $@ $<

$(H)/qmux.o: qmux.c qmux.h
	@mkdir -p $(@D)
	$(CC) $(ALL_CFLAGS) -c -o $@ $<

$(H)/irsc: irsc.c $(H)/msmipc.o
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

$(H)/qmuxd-lite: qmuxd-lite.c $(H)/msmipc.o $(H)/qmux.o
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

$(H)/tests/test-qmux: tests/test_qmux.c $(H)/qmux.o
	@mkdir -p $(@D)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

$(H)/tests/test-msmipc: tests/test_msmipc.c $(H)/msmipc.o
	@mkdir -p $(@D)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS) -Wl,--wrap=recvfrom

# GPS/QMI userspace helpers for the phone (musl aarch64, real libc unlike
# btprobe's freestanding build): msmipc.c/irsc.c/qmuxd-lite.c/qmux.c and
# the vendored qrtr/ are the protocol transport (see docs/features/gps.md
# for the interface); rmtfs, servreg-locator and tftp-server are built by
# their own Makefiles. gps-up is in every image, so all five helpers are
# mandatory: no bootable GPS script without its RMTFS, IRSC,
# SERVREG-LOCATOR, TFTP-SERVER and QMUX counterparts. msmipc.h pulls in the
# vendor kernel's <linux/msm_ipc.h>, which device.mk copies into
# build/device/include.
$(D)/irsc: irsc.c msmipc.c msmipc.h qrtr/libqrtr.h $(D)/include/linux/msm_ipc.h
	@mkdir -p $(@D)
	$(DEVICE_CC) $(DEVICE_CFLAGS) -I$(D)/include -Iqrtr -I. -o $@ irsc.c msmipc.c

$(D)/qmuxd-lite: qmuxd-lite.c qmux.c qmux.h msmipc.c msmipc.h qrtr/qmi.c qrtr/logging.c qrtr/libqrtr.h \
		$(D)/include/linux/msm_ipc.h
	@mkdir -p $(@D)
	$(DEVICE_CC) $(DEVICE_CFLAGS) -I$(D)/include -Iqrtr -I. -o $@ qmuxd-lite.c qmux.c msmipc.c \
		qrtr/qmi.c qrtr/logging.c -lpthread

# Shared evdev discovery and MT-B slot decoding (evdev.h, header-only;
# used by fbtouch, buttond and chefui's touch.c). open/close/ioctl are
# wrapped so the scan runs over a temporary directory of fake devices.
$(H)/tests/test-evdev: tests/test_evdev.c evdev.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) -Wl,--wrap=open -Wl,--wrap=close -Wl,--wrap=ioctl

# Display + touch probe (see fbtouch.c's header). It only needs the libc's
# own <linux/fb.h>/<linux/input.h>, so it is built without the include
# path the QMI code needs. The test includes the .c with FBTOUCH_NO_MAIN to
# reach the pure functions.
$(H)/fbtouch: fbtouch.c fbdev.h evdev.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(H)/tests/test-fbtouch: tests/test_fbtouch.c fbtouch.c fbdev.h evdev.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# On the phone: holds /dev/fb0 open, draws a test pattern through mmap +
# FBIOPAN_DISPLAY, drives lcd-backlight and decodes the NT36xxx evdev
# stream. Manual opt-in from the shell, not in inittab (same policy as
# gps-up).
$(D)/fbtouch: fbtouch.c fbdev.h evdev.h
	@mkdir -p $(@D)
	$(DEVICE_CC) $(DEVICE_CFLAGS) -o $@ fbtouch.c

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
$(H)/fblog: fblog/fblog.c fblog/font9x15.h fbdev.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# ioctl() is wrapped so the daemon's borrow/resume/idle path runs against a
# temporary file standing in for fb0. The test also includes
# buttond/buttond.c, which finds evdev.h through -I.
$(H)/tests/test-fblog: tests/test_fblog.c buttond/buttond.c fblog/fblog.c fblog/font9x15.h fbdev.h evdev.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -I. -o $@ $< $(LDFLAGS) -Wl,--wrap=ioctl

# gpsd feed (nmea-broker.c): qmicli follower stdout -> one UDP datagram per
# valid NMEA sentence to gpsd on loopback, optional tee/log, set-once clock
# step from the first valid RMC. Plain libc, no kernel headers. The test
# includes the .c with NMEA_BROKER_NO_MAIN (injected clock, never
# settimeofday) and drives the loop against a real loopback UDP socket.
$(H)/nmea-broker: nmea-broker.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(H)/tests/test-nmea-broker: tests/test_nmea-broker.c nmea-broker.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# On the phone gpsd (127.0.0.1:20175) cannot read a FIFO, hence the UDP
# feed. The rootfs must carry gpsd itself (scripts/mkrootfs.sh's package
# list, checked by mkinitramfs.sh) or the broker has nothing to feed.
$(D)/nmea-broker: nmea-broker.c
	@mkdir -p $(@D)
	$(DEVICE_CC) $(DEVICE_CFLAGS) -o $@ nmea-broker.c

# GNSS demand manager (gps-manager.c, docs/features/gps.md): leases on
# /run/gps-manager.sock drive the LOC CID/session, gpsd and the follower |
# nmea-broker pipeline on top of gps-up's shared modem, and stop them after
# the last release. Never touches gps-up. Started from inittab (respawn).
# Plain libc. The test includes the .c with GPSMGR_NO_MAIN and drives the
# core through fake operations; tests/test_gps-manager.sh runs the binary
# with stub qmicli/gpsd, the real nmea-broker, kill -9 and a respawn.
$(H)/gps-manager: gps-manager.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(H)/tests/test-gps-manager: tests/test_gps-manager.c gps-manager.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(D)/gps-manager: gps-manager.c
	@mkdir -p $(@D)
	$(DEVICE_CC) $(DEVICE_CFLAGS) -o $@ gps-manager.c

# Battery daemon (powerd.c, docs/features/battery-and-charging.md):
# BatteryService low-battery warn/critical and over-temperature clean
# shutdown, batt_health-style power_supply CSV log (/data/v1/power or
# /run/power) and state file under /run/power, and the thermal-engine
# SS-BATT-BATT 44/42 C charge throttle on battery/system_temp_level.
# Started from both inittabs (respawn). Plain libc + <linux/netlink.h>.
# The test includes the .c with POWERD_NO_MAIN against a fake sysfs tree
# with injected hooks; tests/test_powerd.sh runs the host binary end to end
# with -x so nothing powers off.
$(H)/powerd: powerd.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(H)/tests/test-powerd: tests/test_powerd.c powerd.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(D)/powerd: powerd.c
	@mkdir -p $(@D)
	$(DEVICE_CC) $(DEVICE_CFLAGS) -o $@ powerd.c

# WAV sine-tone generator for the speaker test (wavtone.c,
# docs/features/audio.md): tinyalsa's tinyplay only plays WAV files, so
# this writes the one speaker-test-tone hands to it at the requested
# frequency/duration/dBFS. Only libc's <math.h>, so no kernel include
# paths, but -lm explicitly (musl's static libm is a separate archive). The
# test includes the .c with WAVTONE_NO_MAIN.
$(H)/wavtone: wavtone.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) -lm

$(H)/tests/test-wavtone: tests/test_wavtone.c wavtone.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) -lm

$(D)/wavtone: wavtone.c
	@mkdir -p $(@D)
	$(DEVICE_CC) $(DEVICE_CFLAGS) -o $@ wavtone.c -lm

# Atomic writer for the TAS2560's write-only five-integer calibration
# control. Uses only libc and the kernel ALSA UAPI; no alsa-lib/tinyalsa,
# and no runtime library dependency beyond the statically linked musl libc
# on the phone. The test includes the source with TAS2560_SEND_CAL_NO_MAIN
# and injects open/ioctl/close boundaries, so it needs no ALSA device on
# the host.
$(H)/tas2560-send-cal: tas2560-send-cal.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(H)/tests/test-tas2560-send-cal: tests/test_tas2560_send_cal.c tas2560-send-cal.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(D)/tas2560-send-cal: tas2560-send-cal.c
	@mkdir -p $(@D)
	$(DEVICE_CC) $(DEVICE_CFLAGS) -o $@ tas2560-send-cal.c

# WLFW board-data client (wlan-fw.c) and the WPA2 PSK derivation
# (wifi-psk.c), docs/features/wifi.md.
$(H)/tests/test-wlan-fw: tests/test_wlan_fw.c wlan-fw.c qmux.c qmux.h msmipc.h $(H)/include/linux/msm_ipc.h
	@mkdir -p $(@D)
	$(CC) $(ALL_CFLAGS) -o $@ tests/test_wlan_fw.c qmux.c $(LDFLAGS)

$(H)/wifi-psk: wifi-psk.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(D)/wifi-psk: wifi-psk.c
	@mkdir -p $(@D)
	$(DEVICE_CC) $(DEVICE_CFLAGS) -o $@ wifi-psk.c

$(D)/wlan-fw: wlan-fw.c qmux.c qmux.h msmipc.c msmipc.h qrtr/libqrtr.h $(D)/include/linux/msm_ipc.h
	@mkdir -p $(@D)
	$(DEVICE_CC) $(DEVICE_CFLAGS) -I$(D)/include -I. -Iqrtr -o $@ wlan-fw.c qmux.c msmipc.c

# A/B slot marker (abslot.c, docs/features/boot.md): `fastboot flash
# boot_a` leaves boot_a unsuccessful with 7 retries and abl spends one per
# boot; inittab runs `abslot mark-successful` 30 s into each boot, the job
# Android's boot_control HAL did. Plain libc. tests/test_abslot.py runs the
# host binary against synthetic GPT disk images and a fake sysfs.
$(H)/abslot: abslot.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(D)/abslot: abslot.c
	@mkdir -p $(@D)
	$(DEVICE_CC) $(DEVICE_CFLAGS) -o $@ abslot.c

# Sub-second wall clock against the PMIC RTC (rtc-edge.c,
# docs/features/storage.md): chef-state saves and restores the
# wall-minus-RTC offset with it. Plain libc. tests/test_rtc_edge.py runs
# the host binary against a fake since_epoch file (dry run, nothing sets
# the host clock); tests/test_chef_state.py covers
# rootfs-overlay/usr/bin/chef-state with stubs, and tests/test_chef_reboot.py
# rootfs-overlay/usr/bin/chef-reboot. tests/test_ssh.py covers SSH:
# rootfs-overlay/usr/bin/chef-sshd, scripts/ssh-keys.py, the Wi-Fi address
# dispatcher, the build wiring and scripts/phone.py --ssh.
$(H)/rtc-edge: rtc-edge.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(D)/rtc-edge: rtc-edge.c
	@mkdir -p $(@D)
	$(DEVICE_CC) $(DEVICE_CFLAGS) -o $@ rtc-edge.c

# Small libc-free helper used by bt-up to exercise /dev/btpower and the
# WCN3990 UART; also a raw H4/QCA attach and LE scan for debugging without
# BlueZ. Built freestanding with the Android GCC that builds the kernel
# (scripts/setup-toolchain.sh), not with the musl flags.
BTCC ?= $(REPO_ROOT)/toolchain/aarch64-linux-android-4.9/bin/aarch64-linux-android-gcc

$(D)/btprobe: btprobe.c
	@mkdir -p $(@D)
	$(BTCC) -Os -static -nostdlib -fno-stack-protector -o $@ btprobe.c

device: check-device-cc $(addprefix $(D)/,$(DEVICE_BIN))

install-device: check-destdir device
	install -d $(DESTDIR)/usr/bin
	install -m 755 $(addprefix $(D)/,$(DEVICE_BIN)) $(DESTDIR)/usr/bin/

test-wifi: $(H)/tests/test-wlan-fw $(H)/wifi-psk
	$(H)/tests/test-wlan-fw
	python3 tests/test_wifi.py
	python3 tests/test_wifi_psk.py
	python3 tests/test_wifi_build.py
	python3 tests/test_wifi_nm.py

test: $(TEST_BIN) test-shell $(H)/wifi-psk $(H)/abslot $(H)/rtc-edge test-display-inventory
	$(H)/tests/test-qmux
	$(H)/tests/test-msmipc
	$(H)/tests/test-evdev
	$(H)/tests/test-fbtouch
	$(H)/tests/test-fblog
	$(H)/tests/test-nmea-broker
	$(H)/tests/test-gps-manager
	$(H)/tests/test-powerd
	$(H)/tests/test-wavtone
	$(H)/tests/test-tas2560-send-cal
	$(H)/tests/test-wlan-fw
	python3 tests/test_wifi.py
	python3 tests/test_wifi_psk.py
	python3 tests/test_wifi_build.py
	python3 tests/test_wifi_nm.py
	python3 tests/test_abslot.py
	python3 tests/test_storage.py
	python3 tests/test_rtc_edge.py
	python3 tests/test_chef_state.py
	python3 tests/test_chef_reboot.py
	python3 tests/test_stage1.py
	python3 tests/test_ssh.py

# gps-up (rootfs-overlay/) selection-logic regression; not a build product of
# this directory, but this is the natural place to run it alongside the
# rest of the GPS test suite. See tests/test_gps-up.sh.
test-shell: $(H)/powerd $(H)/gps-manager $(H)/nmea-broker
	sh tests/test_powerd.sh
	sh tests/test_gps-up.sh
	sh tests/test_gps-manager.sh
	sh tests/test_audio-up.sh
	sh tests/test_speaker-test-tone.sh

clean:
	rm -rf build
