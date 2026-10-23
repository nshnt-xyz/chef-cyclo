# sensord: SSC sensors daemon (REG2 server + SMGR client + socket API),
# see main.c's file header and docs/features/sensors.md. On the phone it
# is the REG2 registry server + SMGR client behind
# /run/chef/sensord/sensord.sock, the supervised OpenRC service sensord
# (registry copy made by rootfs-overlay/usr/bin/sensors-up registry); same
# transport/codec as servreg-locator.
#
#   make                 host build: build/host/sensord and the host tool
#                        build/host/compass-replay
#   make test            host tests (build/host/tests/)
#   make device          the phone binary: build/device/sensord
#   make install-device DESTDIR=DIR   it into DIR/usr/bin
#
# The tests never touch AF_MSM_IPC: tests/fake_ipc.c implements the same
# qrtr_*/msmipc_lookup API on AF_UNIX datagram sockets, and
# tests/fake_smgr.c plays the ADSP (REG2 client at "boot", then SMGR
# server). The registry fixture is synthetic (tests/fixtures/sns_reg.map
# plus a generated data file); no device data is used.

REPO_ROOT := ../..
QRTR_DIR := ../qrtr
CC ?= cc
CFLAGS ?= -std=gnu11 -Wall -Wextra -Werror -g -O2
H := build/host
# No test reaches the host's syslog: chef_log() (../common/) finds no
# socket at this path, and the scripts' log helper
# (rootfs-overlay/usr/lib/chef/log.sh) no syslogd and no logger; the lines
# go to the kmsg path each test sets.
export CHEF_LOG_SOCKET := /nonexistent/chef-log-socket
export CHEF_LOG_SYSLOGD_PID := /nonexistent/syslogd.pid
export CHEF_LOG_LOGGER := /nonexistent/logger
D := build/device
# msmipc.h needs <linux/msm_ipc.h>, which ../device.mk copies into the
# build tree.
INC := -I. -I.. -I$(QRTR_DIR) -I$(H)/include
T := $(H)/tests

COMMON := sns_msgs.c sns_reg.c $(QRTR_DIR)/qmi.c $(QRTR_DIR)/logging.c
HDRS := sns_msgs.h sns_reg.h compass.h ../msmipc.h $(QRTR_DIR)/libqrtr.h $(H)/include/linux/msm_ipc.h
# The socket, its JSON lines and the log come from ../common/, compiled
# in from source.
SHARED := ../common/chef_log.c ../common/linesrv.c ../common/json.c
SHARED_H := ../common/chef_log.h ../common/linesrv.h ../common/json.h ../common/chef_paths.h
# The daemon's own sources (main.c's header); the unit test links all
# but main.c and cli.c.
LIB_SRC := channel.c server.c heading.c smgr.c sam.c regserv.c compass.c $(SHARED)
SRC := $(LIB_SRC) main.c cli.c

TEST_OUT := $(T)/test-sns-msgs $(T)/test-sns-reg $(T)/test-sensord \
	    $(T)/sensord-fake $(T)/fake_smgr $(T)/test-compass

.PHONY: all test clean device install-device

all: $(H)/sensord $(H)/compass-replay

include ../device.mk

# The compass filter alone, on recorded captures (host tool, not shipped).
$(H)/compass-replay: compass-replay.c compass.c compass.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ compass-replay.c compass.c $(LDFLAGS) -lm

$(H)/sensord: $(SRC) $(COMMON) ../msmipc.c sensord.h $(SHARED_H) $(HDRS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INC) -o $@ $(SRC) $(COMMON) ../msmipc.c $(LDFLAGS) -lm

$(D)/sensord: $(SRC) $(COMMON) ../msmipc.c sensord.h $(SHARED_H) sns_msgs.h sns_reg.h compass.h ../msmipc.h \
		$(QRTR_DIR)/libqrtr.h $(D)/include/linux/msm_ipc.h
	@mkdir -p $(@D)
	$(DEVICE_CC) $(DEVICE_CFLAGS) -I$(D)/include -I. -I$(QRTR_DIR) -I.. -o $@ \
		$(SRC) sns_msgs.c sns_reg.c $(QRTR_DIR)/qmi.c $(QRTR_DIR)/logging.c ../msmipc.c -lm

device: check-device-cc $(D)/sensord

install-device: check-destdir device
	install -d $(DESTDIR)/usr/bin
	install -m 755 $(D)/sensord $(DESTDIR)/usr/bin/

$(T)/test-sns-msgs: tests/test_sns_msgs.c $(COMMON) $(HDRS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INC) -o $@ tests/test_sns_msgs.c $(COMMON) $(LDFLAGS)

$(T)/test-sns-reg: tests/test_sns_reg.c sns_reg.c sns_reg.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ tests/test_sns_reg.c sns_reg.c $(LDFLAGS)

# Links the daemon's sources but main.c and cli.c.
$(T)/test-sensord: tests/test_sensord.c $(LIB_SRC) tests/fake_ipc.c $(COMMON) sensord.h $(SHARED_H) $(HDRS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INC) -o $@ tests/test_sensord.c $(LIB_SRC) tests/fake_ipc.c $(COMMON) $(LDFLAGS) -lm

# The real daemon, on the fake transport, for the end-to-end test.
$(T)/sensord-fake: $(SRC) tests/fake_ipc.c $(COMMON) sensord.h $(SHARED_H) $(HDRS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INC) -o $@ $(SRC) tests/fake_ipc.c $(COMMON) $(LDFLAGS) -lm

# The compass filter on synthetic motion (includes compass.c).
$(T)/test-compass: tests/test_compass.c compass.c compass.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ tests/test_compass.c $(LDFLAGS) -lm

$(T)/fake_smgr: tests/fake_smgr.c tests/fake_ipc.c $(COMMON) $(HDRS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INC) -o $@ tests/fake_smgr.c tests/fake_ipc.c $(COMMON) $(LDFLAGS)

# The same unit tests under ASan/UBSan: the truncated-message cases in
# test_sns_msgs.c and test_sensord.c are only meaningful with it.
SAN := -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all

$(T)/test-sns-msgs-san: tests/test_sns_msgs.c $(COMMON) $(HDRS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SAN) $(INC) -o $@ tests/test_sns_msgs.c $(COMMON) $(LDFLAGS)

$(T)/test-sensord-san: tests/test_sensord.c $(LIB_SRC) tests/fake_ipc.c $(COMMON) sensord.h $(SHARED_H) $(HDRS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SAN) $(INC) -o $@ tests/test_sensord.c $(LIB_SRC) tests/fake_ipc.c $(COMMON) $(LDFLAGS) -lm

$(T)/test-compass-san: tests/test_compass.c compass.c compass.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SAN) -I. -o $@ tests/test_compass.c $(LDFLAGS) -lm

# The daemon under ASan/UBSan against the fake, for the compass/rotvec
# part of the end-to-end test.
$(T)/sensord-fake-san: $(SRC) tests/fake_ipc.c $(COMMON) sensord.h $(SHARED_H) $(HDRS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SAN) $(INC) -o $@ $(SRC) tests/fake_ipc.c $(COMMON) $(LDFLAGS) -lm

SAN_OUT := $(T)/test-sns-msgs-san $(T)/test-sensord-san $(T)/test-compass-san \
	   $(T)/sensord-fake-san

test: $(H)/sensord $(H)/compass-replay $(TEST_OUT) $(SAN_OUT)
	$(T)/test-sns-msgs
	$(T)/test-sns-msgs-san
	$(T)/test-sns-reg
	$(T)/test-sensord
	$(T)/test-sensord-san
	$(T)/test-compass
	$(T)/test-compass-san
	sh tests/test_e2e.sh
	sh tests/test_compass_e2e.sh
	sh tests/test_sensors-up.sh
	sh tests/test_magcal_run.sh
	sh tests/test_compass_run.sh
	python3 tests/test_sns_tools.py
	python3 tests/test_mag_cal_check.py
	python3 tests/test_compass_check.py

clean:
	rm -rf build
