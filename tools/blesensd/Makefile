# blesensd: the BLE sensor daemon (heart rate, cycling power, speed and
# cadence), see main.c's file header and docs/features/bluetooth.md
# ("BLE sensors"). Sources: state.c, server.c, bus.c, link.c, pair.c,
# records.c, main.c and cli.c with the internal header blesensd.h;
# blesens.c is its pure measurement code. The host build
# uses libsystemd's sd-bus; the phone build uses static basu
# (-DCHEF_BASU; scripts/setup-toolchain.sh fetches it to toolchain/basu).
# On the phone it is the one owner of the BLE sensors (pairing,
# reconnection, notifications) behind /run/chef/blesensd.sock, records through
# chef-state blesens-load/-save, started from inittab (respawn).
#
#   make                 host build: build/host/blesensd
#   make test            host tests (build/host/tests/)
#   make device          the phone binary: build/device/blesensd
#   make install-device DESTDIR=DIR   it into DIR/usr/bin
#
# test-blesens: the parsers and rates. tests/test_blesensd.py: the daemon
# binary against tests/fake_bluez.py (a fake bluetoothd on a private
# dbus-daemon, never the system bus) and a stub chef-state; needs
# libsystemd, dbus-daemon and python3 dbus + gi.

REPO_ROOT := ../..
CC ?= cc
CFLAGS ?= -std=gnu11 -Wall -Wextra -Werror -O2 -g
SDBUS_LIBS ?= $(shell pkg-config --libs libsystemd 2>/dev/null)
BASU ?= $(REPO_ROOT)/toolchain/basu
H := build/host
# No test reaches the host's syslog: chef_log() (../common/) finds no
# socket at this path, and the scripts' log helper
# (rootfs-overlay/usr/lib/chef/log.sh) no syslogd and no logger; the lines
# go to the kmsg path each test sets.
export CHEF_LOG_SOCKET := /nonexistent/chef-log-socket
export CHEF_LOG_SYSLOGD_PID := /nonexistent/syslogd.pid
export CHEF_LOG_LOGGER := /nonexistent/logger
D := build/device
# The socket, its JSON lines and the log come from ../common/, compiled
# in from source.
SHARED := ../common/chef_log.c ../common/linesrv.c ../common/json.c
SHARED_H := ../common/chef_log.h ../common/linesrv.h ../common/json.h ../common/chef_paths.h
SRC := state.c server.c bus.c link.c pair.c records.c main.c cli.c blesens.c $(SHARED)
HDRS := blesensd.h blesens.h $(SHARED_H)
INC := -I..

.PHONY: all test clean device install-device check-basu

all: $(H)/blesensd

include ../device.mk

$(H)/blesensd: $(SRC) $(HDRS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(INC) -o $@ $(SRC) $(LDFLAGS) $(SDBUS_LIBS) -lm

check-basu:
	@test -f $(BASU)/lib/libbasu.a || { echo "basu not found at $(BASU): run scripts/setup-toolchain.sh" >&2; exit 1; }

$(D)/blesensd: $(SRC) $(HDRS)
	@mkdir -p $(@D)
	$(DEVICE_CC) $(DEVICE_CFLAGS) -DCHEF_BASU $(INC) -I$(BASU)/include -o $@ \
		$(SRC) $(BASU)/lib/libbasu.a -lm

device: check-device-cc check-basu $(D)/blesensd

install-device: check-destdir device
	install -d $(DESTDIR)/usr/bin
	install -m 755 $(D)/blesensd $(DESTDIR)/usr/bin/

$(H)/tests/test-blesens: tests/test_blesens.c blesens.c blesens.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ tests/test_blesens.c blesens.c $(LDFLAGS) -lm

test: $(H)/tests/test-blesens $(H)/blesensd
	$(H)/tests/test-blesens
	python3 tests/test_blesensd.py

clean:
	rm -rf build
