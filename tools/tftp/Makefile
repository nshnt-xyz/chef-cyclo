# tftp-server: bounded TFTP/RFS server for this vendor kernel's AF_MSM_IPC
# transport (see tftpserv.c's file header), the server (service 0x1000)
# the 2026-09-17 stock-read probe found the locator alone doesn't replace.
# Its /readwrite RAM shadow is seeded at build time by
# scripts/mkinitramfs.sh from the persist partition image.
# REPORT.md below is the report in the local 2026-09-17 stock-read evidence
# directory (stock-root-read-fable-<timestamp>); it is not in the repository.
#
#   make                 host build: build/host/tftp-server
#   make test            host tests (build/host/tests/); make test-san
#                        runs them under ASan/UBSan
#   make device          the phone binary: build/device/tftp-server
#   make install-device DESTDIR=DIR   it into DIR/usr/bin
#
# Unlike rmtfs/servreg-locator, this daemon speaks raw TFTP (RFC 1350), not
# QMI, so it needs only msmipc.c's qrtr_* transport calls -- no
# qrtr/qmi.c/logging.c codec, and no vendor kernel headers outside
# msmipc.c/.h's own requirement (see msmipc.h's file header).

REPO_ROOT := ../..
QRTR_DIR := ../qrtr
CC ?= cc
CFLAGS ?= -std=gnu11 -Wall -Wextra -Werror -g -O2
H := build/host
D := build/device

SRCS := ramfs.c translate.c protocol.c tftpserv.c
HDRS := tftp.h translate.h ramfs.h protocol.h ../msmipc.h $(QRTR_DIR)/libqrtr.h

.PHONY: all test test-san clean device install-device

all: $(H)/tftp-server

include ../device.mk

$(H)/tftp-server: $(H)/ramfs.o $(H)/translate.o $(H)/protocol.o $(H)/tftpserv.o $(H)/msmipc.o
	$(CC) -o $@ $^ $(LDFLAGS)

$(H)/ramfs.o: ramfs.c ramfs.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ -c ramfs.c

$(H)/translate.o: translate.c translate.h ramfs.h tftp.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ -c translate.c

$(H)/protocol.o: protocol.c protocol.h tftp.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ -c protocol.c

$(H)/tftpserv.o: tftpserv.c tftp.h translate.h ramfs.h protocol.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -I$(QRTR_DIR) -I.. -o $@ -c tftpserv.c

# Borrowed into a local object, same rationale as
# tools/servreg-locator/Makefile's msmipc.o rule.
$(H)/msmipc.o: ../msmipc.c ../msmipc.h $(H)/include/linux/msm_ipc.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I$(H)/include -I$(QRTR_DIR) -I.. -o $@ -c ../msmipc.c

$(D)/tftp-server: $(SRCS) ../msmipc.c $(HDRS) $(D)/include/linux/msm_ipc.h
	@mkdir -p $(@D)
	$(DEVICE_CC) $(DEVICE_CFLAGS) -I$(D)/include -I. -I$(QRTR_DIR) -I.. -o $@ $(SRCS) ../msmipc.c

device: check-device-cc $(D)/tftp-server

install-device: check-destdir device
	install -d $(DESTDIR)/usr/bin
	install -m 755 $(D)/tftp-server $(DESTDIR)/usr/bin/

clean:
	rm -rf build

# Pure-protocol/allowlist/RAM-shadow tests: no sockets, no AF_MSM_IPC, so no
# msmipc.c and no vendor kernel headers needed (same rationale as
# tools/servreg-locator/tests/test_jsn.c and test_servreg_loc.c).
$(H)/tests/test-translate: tests/test_translate.c translate.c translate.h ramfs.c ramfs.h tftp.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ tests/test_translate.c translate.c ramfs.c

$(H)/tests/test-protocol: tests/test_protocol.c protocol.c protocol.h tftp.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ tests/test_protocol.c protocol.c

# End-to-end loopback harness (Round 1 review F9): tftpserv.c compiled with
# -DTFTP_TEST_HOOKS and linked against tests/fake_qrtr.c's in-memory
# qrtr_open/close/sendto/recvfrom/publish instead of msmipc.c's real
# AF_MSM_IPC ones -- no vendor kernel headers needed here either, since
# tftpserv.c itself only needs the qrtr_* prototypes (qrtr/libqrtr.h), not
# msmipc.h (see tftpserv.c's includes).
$(H)/tests/test-e2e: tests/test_e2e.c tests/fake_qrtr.c tests/fake_qrtr.h tests/tftpserv_test.h \
		tftpserv.c translate.c ramfs.c protocol.c tftp.h translate.h ramfs.h protocol.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DTFTP_TEST_HOOKS -I. -I$(QRTR_DIR) -I.. -o $@ \
		tests/test_e2e.c tests/fake_qrtr.c tftpserv.c translate.c ramfs.c protocol.c

test: $(H)/tests/test-translate $(H)/tests/test-protocol $(H)/tests/test-e2e
	$(H)/tests/test-translate
	$(H)/tests/test-protocol
	$(H)/tests/test-e2e

# Same test list, built and run under ASan/UBSan (REPORT.md 8.1 point 7:
# "Run them plain and under -fsanitize=address,undefined").
SANFLAGS := -std=gnu11 -Wall -Wextra -Werror -g -O1 -fsanitize=address,undefined

$(H)/tests/test-translate-san: tests/test_translate.c translate.c translate.h ramfs.c ramfs.h tftp.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(SANFLAGS) -I. -o $@ tests/test_translate.c translate.c ramfs.c

$(H)/tests/test-protocol-san: tests/test_protocol.c protocol.c protocol.h tftp.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(SANFLAGS) -I. -o $@ tests/test_protocol.c protocol.c

$(H)/tests/test-e2e-san: tests/test_e2e.c tests/fake_qrtr.c tests/fake_qrtr.h tests/tftpserv_test.h \
		tftpserv.c translate.c ramfs.c protocol.c tftp.h translate.h ramfs.h protocol.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(SANFLAGS) -DTFTP_TEST_HOOKS -I. -I$(QRTR_DIR) -I.. -o $@ \
		tests/test_e2e.c tests/fake_qrtr.c tftpserv.c translate.c ramfs.c protocol.c

test-san: $(H)/tests/test-translate-san $(H)/tests/test-protocol-san $(H)/tests/test-e2e-san
	$(H)/tests/test-translate-san
	$(H)/tests/test-protocol-san
	$(H)/tests/test-e2e-san
