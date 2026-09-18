# tftp-server: bounded TFTP/RFS server for this vendor kernel's AF_MSM_IPC
# transport (see tftpserv.c's file header). CC/CFLAGS/LDFLAGS/CPPFLAGS come
# from the caller (scripts/mkinitramfs.sh sets them for the musl aarch64
# cross toolchain and points -I at ../msmipc.h); this Makefile only adds to
# them, the same shape as tools/servreg-locator/Makefile.
#
# Native (host) build + tests:
#   make && make test
#
# Cross build for the phone: see scripts/mkinitramfs.sh, which builds this
# the same way it builds tools/rmtfs and tools/servreg-locator.
#
# Unlike rmtfs/servreg-locator, this daemon speaks raw TFTP (RFC 1350), not
# QMI, so it needs only msmipc.c's qrtr_* transport calls -- no
# qrtr/qmi.c/logging.c codec, and no vendor kernel headers outside
# msmipc.c/.h's own requirement (see msmipc.h's file header).

REPO_ROOT := ../..
QRTR_DIR := ../qrtr
KHDR := -I$(REPO_ROOT)/kernel/include/uapi -I$(REPO_ROOT)/kernel/include

OUT := tftp-server
TEST_OUT := tests/test-translate tests/test-protocol tests/test-e2e

CFLAGS += -std=gnu11 -Wall -Wextra -g -O2 -static
LDFLAGS += -static

SRCS := ramfs.c translate.c protocol.c tftpserv.c
OBJS := $(SRCS:.c=.o)

.PHONY: all test clean

all: $(OUT)

$(OUT): ramfs.o translate.o protocol.o tftpserv.o msmipc.o
	$(CC) -o $@ $^ $(LDFLAGS)

ramfs.o: ramfs.c ramfs.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ -c ramfs.c

translate.o: translate.c translate.h ramfs.h tftp.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ -c translate.c

protocol.o: protocol.c protocol.h tftp.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ -c protocol.c

tftpserv.o: tftpserv.c tftp.h translate.h ramfs.h protocol.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -I$(QRTR_DIR) -I.. -o $@ -c tftpserv.c

# Borrowed into a local object purely so `make all` is a standalone host
# sanity build, same rationale as tools/servreg-locator/Makefile's
# msmipc.o rule -- the real cross build that ships is
# scripts/mkinitramfs.sh.
msmipc.o: ../msmipc.c ../msmipc.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(KHDR) -I$(QRTR_DIR) -I.. -o $@ -c ../msmipc.c

clean:
	rm -f $(OUT) *.o $(TEST_OUT)

# Pure-protocol/allowlist/RAM-shadow tests: no sockets, no AF_MSM_IPC, so no
# msmipc.c and no vendor kernel headers needed (same rationale as
# tools/servreg-locator/tests/test_jsn.c and test_servreg_loc.c).
tests/test-translate: tests/test_translate.c translate.c translate.h ramfs.c ramfs.h tftp.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ tests/test_translate.c translate.c ramfs.c

tests/test-protocol: tests/test_protocol.c protocol.c protocol.h tftp.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ tests/test_protocol.c protocol.c

# End-to-end loopback harness (Round 1 review F9): tftpserv.c compiled with
# -DTFTP_TEST_HOOKS and linked against tests/fake_qrtr.c's in-memory
# qrtr_open/close/sendto/recvfrom/publish instead of msmipc.c's real
# AF_MSM_IPC ones -- no vendor kernel headers needed here either, since
# tftpserv.c itself only needs the qrtr_* prototypes (qrtr/libqrtr.h), not
# msmipc.h (see tftpserv.c's includes).
tests/test-e2e: tests/test_e2e.c tests/fake_qrtr.c tests/fake_qrtr.h tests/tftpserv_test.h \
		tftpserv.c translate.c ramfs.c protocol.c tftp.h translate.h ramfs.h protocol.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -DTFTP_TEST_HOOKS -I. -I$(QRTR_DIR) -I.. -o $@ \
		tests/test_e2e.c tests/fake_qrtr.c tftpserv.c translate.c ramfs.c protocol.c

test: $(TEST_OUT)
	./tests/test-translate
	./tests/test-protocol
	./tests/test-e2e

# Same test list, built and run under ASan/UBSan (REPORT.md 8.1 point 7:
# "Run them plain and under -fsanitize=address,undefined").
SANOUT := tests/test-translate-san tests/test-protocol-san tests/test-e2e-san
SANFLAGS := -std=gnu11 -Wall -Wextra -g -O1 -fsanitize=address,undefined

tests/test-translate-san: tests/test_translate.c translate.c translate.h ramfs.c ramfs.h tftp.h
	$(CC) $(CPPFLAGS) $(SANFLAGS) -I. -o $@ tests/test_translate.c translate.c ramfs.c

tests/test-protocol-san: tests/test_protocol.c protocol.c protocol.h tftp.h
	$(CC) $(CPPFLAGS) $(SANFLAGS) -I. -o $@ tests/test_protocol.c protocol.c

tests/test-e2e-san: tests/test_e2e.c tests/fake_qrtr.c tests/fake_qrtr.h tests/tftpserv_test.h \
		tftpserv.c translate.c ramfs.c protocol.c tftp.h translate.h ramfs.h protocol.h
	$(CC) $(CPPFLAGS) $(SANFLAGS) -DTFTP_TEST_HOOKS -I. -I$(QRTR_DIR) -I.. -o $@ \
		tests/test_e2e.c tests/fake_qrtr.c tftpserv.c translate.c ramfs.c protocol.c

test-san: $(SANOUT)
	./tests/test-translate-san
	./tests/test-protocol-san
	./tests/test-e2e-san
