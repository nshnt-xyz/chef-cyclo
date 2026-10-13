# blesensd: the BLE sensor daemon (heart rate, cycling power, speed and
# cadence), see blesensd.c's file header and docs/features/bluetooth.md
# ("BLE sensors"). blesens.c is its pure measurement code. The host build
# uses libsystemd's sd-bus; scripts/mkinitramfs.sh builds the shipped
# binary itself against static basu (-DCHEF_BASU).
#
# Native (host) build + tests:
#   make && make test
#
# test-blesens: the parsers and rates. tests/test_blesensd.py: the daemon
# binary against tests/fake_bluez.py (a fake bluetoothd on a private
# dbus-daemon, never the system bus) and a stub chef-state; needs
# libsystemd, dbus-daemon and python3 dbus + gi.

CC ?= cc
CFLAGS ?= -std=gnu11 -Wall -Wextra -O2 -g
SDBUS_LIBS ?= $(shell pkg-config --libs libsystemd 2>/dev/null)

.PHONY: all test clean

all: blesensd

blesensd: blesensd.c blesens.c blesens.h
	$(CC) $(CFLAGS) -o $@ blesensd.c blesens.c $(LDFLAGS) $(SDBUS_LIBS) -lm

tests/test-blesens: tests/test_blesens.c blesens.c blesens.h
	$(CC) $(CFLAGS) -o $@ tests/test_blesens.c blesens.c $(LDFLAGS) -lm

test: tests/test-blesens blesensd
	./tests/test-blesens
	python3 tests/test_blesensd.py

clean:
	rm -f blesensd tests/test-blesens
