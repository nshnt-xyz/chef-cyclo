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
TEST_OUT := tests/test-sns-msgs tests/test-sns-reg tests/test-sensord \
	    tests/sensord-fake tests/fake_smgr

COMMON := sns_msgs.c sns_reg.c $(QRTR_DIR)/qmi.c $(QRTR_DIR)/logging.c
HDRS := sns_msgs.h sns_reg.h ../msmipc.h $(QRTR_DIR)/libqrtr.h

.PHONY: all test clean

all: $(OUT)

$(OUT): sensord.c $(COMMON) ../msmipc.c $(HDRS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INC) -o $@ sensord.c $(COMMON) ../msmipc.c $(LDFLAGS)

tests/test-sns-msgs: tests/test_sns_msgs.c $(COMMON) $(HDRS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INC) -o $@ tests/test_sns_msgs.c $(COMMON) $(LDFLAGS)

tests/test-sns-reg: tests/test_sns_reg.c sns_reg.c sns_reg.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ tests/test_sns_reg.c sns_reg.c $(LDFLAGS)

# Includes sensord.c with SENSORD_NO_MAIN, so the I/O-only statics are
# unused there.
tests/test-sensord: tests/test_sensord.c sensord.c tests/fake_ipc.c $(COMMON) $(HDRS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wno-unused-function $(INC) -o $@ tests/test_sensord.c tests/fake_ipc.c $(COMMON) $(LDFLAGS)

# The real daemon, on the fake transport, for the end-to-end test.
tests/sensord-fake: sensord.c tests/fake_ipc.c $(COMMON) $(HDRS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INC) -o $@ sensord.c tests/fake_ipc.c $(COMMON) $(LDFLAGS)

tests/fake_smgr: tests/fake_smgr.c tests/fake_ipc.c $(COMMON) $(HDRS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INC) -o $@ tests/fake_smgr.c tests/fake_ipc.c $(COMMON) $(LDFLAGS)

# The same unit tests under ASan/UBSan: the truncated-message cases in
# test_sns_msgs.c and test_sensord.c are only meaningful with it.
SAN := -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all

tests/test-sns-msgs-san: tests/test_sns_msgs.c $(COMMON) $(HDRS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SAN) $(INC) -o $@ tests/test_sns_msgs.c $(COMMON) $(LDFLAGS)

tests/test-sensord-san: tests/test_sensord.c sensord.c tests/fake_ipc.c $(COMMON) $(HDRS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SAN) -Wno-unused-function $(INC) -o $@ tests/test_sensord.c tests/fake_ipc.c $(COMMON) $(LDFLAGS)

test: $(TEST_OUT) tests/test-sns-msgs-san tests/test-sensord-san
	./tests/test-sns-msgs
	./tests/test-sns-msgs-san
	./tests/test-sns-reg
	./tests/test-sensord
	./tests/test-sensord-san
	sh tests/test_e2e.sh
	sh tests/test_sensors-up.sh
	sh tests/test_magcal_run.sh
	python3 tests/test_sns_tools.py
	python3 tests/test_mag_cal_check.py

clean:
	rm -f $(OUT) $(TEST_OUT) tests/test-sns-msgs-san tests/test-sensord-san
