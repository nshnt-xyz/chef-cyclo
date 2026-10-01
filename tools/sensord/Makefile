# sensord: SSC sensors daemon (REG2 server + SMGR client + socket API),
# see sensord.c's file header. CC/CFLAGS/LDFLAGS come from the caller for
# a cross build; scripts/mkinitramfs.sh builds the shipped binary itself,
# the same way it builds tools/servreg-locator.
#
# Native (host) build + tests:
#   make && make test
#
# The tests never touch AF_MSM_IPC: tests/fake_ipc.c implements the same
# qrtr_*/msmipc_lookup API on AF_UNIX datagram sockets, and
# tests/fake_smgr.c plays the ADSP (REG2 client at "boot", then SMGR
# server). The registry fixture is synthetic (tests/fixtures/sns_reg.map
# plus a generated data file); no device data is used.

REPO_ROOT := ../..
QRTR_DIR := ../qrtr
# msmipc.h pulls in the vendor kernel's verbatim <linux/msm_ipc.h>.
KHDR := -I$(REPO_ROOT)/kernel/include/uapi -I$(REPO_ROOT)/kernel/include

# -Wno-cpp: <linux/msm_ipc.h> is the vendor kernel's own header (see
# ../msmipc.h), which warns about being used from user space.
CFLAGS += -std=gnu11 -Wall -Wextra -Wno-cpp -g -O2
INC := -I. -I.. -I$(QRTR_DIR) $(KHDR)

OUT := sensord
TOOLS := compass-replay
TEST_OUT := tests/test-sns-msgs tests/test-sns-reg tests/test-sensord \
	    tests/sensord-fake tests/fake_smgr tests/test-compass

COMMON := sns_msgs.c sns_reg.c $(QRTR_DIR)/qmi.c $(QRTR_DIR)/logging.c
HDRS := sns_msgs.h sns_reg.h compass.h ../msmipc.h $(QRTR_DIR)/libqrtr.h

.PHONY: all test clean

all: $(OUT) $(TOOLS)

# The compass filter alone, on recorded captures (host tool, not shipped).
compass-replay: compass-replay.c compass.c compass.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ compass-replay.c compass.c $(LDFLAGS) -lm

$(OUT): sensord.c compass.c $(COMMON) ../msmipc.c $(HDRS) compass.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INC) -o $@ sensord.c compass.c $(COMMON) ../msmipc.c $(LDFLAGS) -lm

tests/test-sns-msgs: tests/test_sns_msgs.c $(COMMON) $(HDRS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INC) -o $@ tests/test_sns_msgs.c $(COMMON) $(LDFLAGS)

tests/test-sns-reg: tests/test_sns_reg.c sns_reg.c sns_reg.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ tests/test_sns_reg.c sns_reg.c $(LDFLAGS)

# Includes sensord.c with SENSORD_NO_MAIN, so the I/O-only statics are
# unused there.
tests/test-sensord: tests/test_sensord.c sensord.c compass.c tests/fake_ipc.c $(COMMON) $(HDRS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wno-unused-function $(INC) -o $@ tests/test_sensord.c compass.c tests/fake_ipc.c $(COMMON) $(LDFLAGS) -lm

# The real daemon, on the fake transport, for the end-to-end test.
tests/sensord-fake: sensord.c compass.c tests/fake_ipc.c $(COMMON) $(HDRS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INC) -o $@ sensord.c compass.c tests/fake_ipc.c $(COMMON) $(LDFLAGS) -lm

# The compass filter on synthetic motion (includes compass.c).
tests/test-compass: tests/test_compass.c compass.c compass.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ tests/test_compass.c $(LDFLAGS) -lm

tests/fake_smgr: tests/fake_smgr.c tests/fake_ipc.c $(COMMON) $(HDRS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INC) -o $@ tests/fake_smgr.c tests/fake_ipc.c $(COMMON) $(LDFLAGS)

# The same unit tests under ASan/UBSan: the truncated-message cases in
# test_sns_msgs.c and test_sensord.c are only meaningful with it.
SAN := -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all

tests/test-sns-msgs-san: tests/test_sns_msgs.c $(COMMON) $(HDRS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SAN) $(INC) -o $@ tests/test_sns_msgs.c $(COMMON) $(LDFLAGS)

tests/test-sensord-san: tests/test_sensord.c sensord.c compass.c tests/fake_ipc.c $(COMMON) $(HDRS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SAN) -Wno-unused-function $(INC) -o $@ tests/test_sensord.c compass.c tests/fake_ipc.c $(COMMON) $(LDFLAGS) -lm

tests/test-compass-san: tests/test_compass.c compass.c compass.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SAN) -I. -o $@ tests/test_compass.c $(LDFLAGS) -lm

# The daemon under ASan/UBSan against the fake, for the compass/rotvec
# part of the end-to-end test.
tests/sensord-fake-san: sensord.c compass.c tests/fake_ipc.c $(COMMON) $(HDRS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SAN) $(INC) -o $@ sensord.c compass.c tests/fake_ipc.c $(COMMON) $(LDFLAGS) -lm

SAN_OUT := tests/test-sns-msgs-san tests/test-sensord-san tests/test-compass-san \
	   tests/sensord-fake-san

test: $(OUT) $(TOOLS) $(TEST_OUT) $(SAN_OUT)
	./tests/test-sns-msgs
	./tests/test-sns-msgs-san
	./tests/test-sns-reg
	./tests/test-sensord
	./tests/test-sensord-san
	./tests/test-compass
	./tests/test-compass-san
	sh tests/test_e2e.sh
	sh tests/test_compass_e2e.sh
	sh tests/test_sensors-up.sh
	sh tests/test_magcal_run.sh
	sh tests/test_compass_run.sh
	python3 tests/test_sns_tools.py
	python3 tests/test_mag_cal_check.py
	python3 tests/test_compass_check.py

clean:
	rm -f $(OUT) $(TOOLS) $(TEST_OUT) $(SAN_OUT)
