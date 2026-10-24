# sensor-proxy: net.hadess.SensorProxy on the system bus, a bridge to
# sensord (main.c's file header, docs/features/sensors.md). Sources:
# orient.c (orientation and tilt), proxy.c (claims and sensord's lines),
# bus.c (the bus side) and main.c with the internal header sensor-proxy.h.
# The host build uses libsystemd's sd-bus; the phone build uses static
# basu (-DCHEF_BASU; scripts/setup-toolchain.sh fetches it to
# toolchain/basu). The supervised OpenRC service sensor-proxy.
#
#   make                 host build: build/host/sensor-proxy
#   make test            host tests (build/host/tests/)
#   make device          the phone binary: build/device/sensor-proxy
#   make install-device DESTDIR=DIR   it into DIR/usr/bin
#
# test-orient: orient.c against iio-sensor-proxy's test vectors in
# sensord's frame. tests/test_sensor_proxy.py: the daemon binary on a
# private dbus-daemon (never the system bus) against a fake sensord, with
# dbus-python clients; needs libsystemd, dbus-daemon and python3 dbus + gi.

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
# The sensord connection, its JSON lines and the log come from ../common/,
# compiled in from source.
SHARED := ../common/chef_log.c ../common/lineclient.c ../common/json.c
SHARED_H := ../common/chef_log.h ../common/lineclient.h ../common/json.h ../common/chef_paths.h
SRC := orient.c proxy.c bus.c main.c $(SHARED)
HDRS := sensor-proxy.h $(SHARED_H)
INC := -I..

.PHONY: all test clean device install-device check-basu

all: $(H)/sensor-proxy

include ../device.mk

$(H)/sensor-proxy: $(SRC) $(HDRS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(INC) -o $@ $(SRC) $(LDFLAGS) $(SDBUS_LIBS) -lm

check-basu:
	@test -f $(BASU)/lib/libbasu.a || { echo "basu not found at $(BASU): run scripts/setup-toolchain.sh" >&2; exit 1; }

$(D)/sensor-proxy: $(SRC) $(HDRS)
	@mkdir -p $(@D)
	$(DEVICE_CC) $(DEVICE_CFLAGS) -DCHEF_BASU $(INC) -I$(BASU)/include -o $@ \
		$(SRC) $(BASU)/lib/libbasu.a -lm

device: check-device-cc check-basu $(D)/sensor-proxy

install-device: check-destdir device
	install -d $(DESTDIR)/usr/bin
	install -m 755 $(D)/sensor-proxy $(DESTDIR)/usr/bin/

$(H)/tests/test-orient: tests/test_orient.c orient.c sensor-proxy.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -I. -o $@ tests/test_orient.c orient.c $(LDFLAGS) -lm

test: $(H)/tests/test-orient $(H)/sensor-proxy
	$(H)/tests/test-orient
	python3 tests/test_sensor_proxy.py

clean:
	rm -rf build
