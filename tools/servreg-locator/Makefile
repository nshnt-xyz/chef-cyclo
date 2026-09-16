# servreg-locator: userspace SERVREG_LOC (pd-mapper) server for this vendor
# kernel's AF_MSM_IPC transport (see servreg-locator.c's file header).
# CC/CFLAGS/LDFLAGS/CPPFLAGS come from the caller (scripts/mkinitramfs.sh
# sets them for the musl aarch64 cross toolchain and points -I at
# ../msmipc.h + ../qrtr/libqrtr.h); this Makefile only adds to them.
#
# Native (host) build + tests:
#   make && make test
#
# Cross build for the phone: see scripts/mkinitramfs.sh, which builds this
# the same way it builds tools/rmtfs.

REPO_ROOT := ../..
QRTR_DIR := ../qrtr
# Only msmipc.c/msmipc.h need the vendor kernel's verbatim <linux/msm_ipc.h>
# (see tools/msmipc.h) -- kept off jsn.o/servreg_loc.o/servreg-locator.o's
# own flags below so they don't pick up the unrelated "kernel headers from
# user space" build warning.
KHDR := -I$(REPO_ROOT)/kernel/include/uapi -I$(REPO_ROOT)/kernel/include

OUT := servreg-locator
TEST_OUT := tests/test-jsn tests/test-servreg-loc

CFLAGS += -Wall -Wextra -g -O2 -static
LDFLAGS += -static

SRCS := jsn.c servreg_loc.c servreg-locator.c
OBJS := $(SRCS:.c=.o)

.PHONY: all test clean

all: $(OUT)

# Borrowed sources are built into local object files (qrtr-qmi.o etc, not
# ../qrtr/qmi.o -- this Makefile doesn't write into shared directories)
# purely so `make all` is a standalone host sanity build; the real cross
# build that ships is scripts/mkinitramfs.sh, which compiles all of these
# together directly, the same way it builds tools/rmtfs.
$(OUT): jsn.o servreg_loc.o servreg-locator.o qrtr-qmi.o qrtr-logging.o msmipc.o
	$(CC) -o $@ $^ $(LDFLAGS) -lpthread

jsn.o: jsn.c jsn.h servreg_loc.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -I$(QRTR_DIR) -c -o $@ jsn.c

servreg_loc.o: servreg_loc.c servreg_loc.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -I$(QRTR_DIR) -c -o $@ servreg_loc.c

servreg-locator.o: servreg-locator.c jsn.h servreg_loc.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -I$(QRTR_DIR) -c -o $@ servreg-locator.c

qrtr-qmi.o: $(QRTR_DIR)/qmi.c $(QRTR_DIR)/libqrtr.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -I$(QRTR_DIR) -c -o $@ $(QRTR_DIR)/qmi.c

qrtr-logging.o: $(QRTR_DIR)/logging.c $(QRTR_DIR)/logging.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -I$(QRTR_DIR) -c -o $@ $(QRTR_DIR)/logging.c

msmipc.o: ../msmipc.c ../msmipc.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(KHDR) -I$(QRTR_DIR) -I.. -c -o $@ ../msmipc.c

clean:
	rm -f $(OUT) *.o $(TEST_OUT)

# Pure-protocol/parser tests: no sockets, no AF_MSM_IPC, so no msmipc.c and
# no vendor kernel headers needed -- see the two test files' headers.
tests/test-jsn: tests/test_jsn.c jsn.c jsn.h servreg_loc.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -I$(QRTR_DIR) -o $@ tests/test_jsn.c jsn.c

tests/test-servreg-loc: tests/test_servreg_loc.c servreg_loc.c servreg_loc.h $(QRTR_DIR)/qmi.c $(QRTR_DIR)/logging.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -I$(QRTR_DIR) -o $@ tests/test_servreg_loc.c servreg_loc.c $(QRTR_DIR)/qmi.c $(QRTR_DIR)/logging.c

test: tests/test-jsn tests/test-servreg-loc
	cd tests && ./test-jsn
	./tests/test-servreg-loc
