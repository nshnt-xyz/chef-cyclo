# servreg-locator: userspace SERVREG_LOC (pd-mapper) server for this vendor
# kernel's AF_MSM_IPC transport (see servreg-locator.c's file header).
#
#   make                 host build: build/host/servreg-locator
#   make test            host tests (build/host/tests/)
#   make device          the phone binary: build/device/servreg-locator
#   make install-device DESTDIR=DIR   it into DIR/usr/bin
#
# On the phone it answers the modem's boot-time domain-list lookup; see
# servreg-locator.c's file header and docs/build-log.md's 2026-09-17 (dog
# timeout) entry for why it exists.

REPO_ROOT := ../..
QRTR_DIR := ../qrtr
CC ?= cc
CFLAGS ?= -Wall -Wextra -Werror -g -O2
H := build/host
D := build/device

SRCS := jsn.c servreg_loc.c servreg-locator.c
HDRS := jsn.h servreg_loc.h ../msmipc.h $(QRTR_DIR)/libqrtr.h $(QRTR_DIR)/logging.h

.PHONY: all test clean device install-device

all: $(H)/servreg-locator

include ../device.mk

# Borrowed sources are built into local object files (qrtr-qmi.o etc, not
# ../qrtr/qmi.o -- this Makefile doesn't write into shared directories).
# Only msmipc.c/msmipc.h need <linux/msm_ipc.h> (see ../device.mk).
$(H)/servreg-locator: $(H)/jsn.o $(H)/servreg_loc.o $(H)/servreg-locator.o $(H)/qrtr-qmi.o \
		$(H)/qrtr-logging.o $(H)/msmipc.o
	$(CC) -o $@ $^ $(LDFLAGS) -lpthread

$(H)/jsn.o: jsn.c jsn.h servreg_loc.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -I$(QRTR_DIR) -c -o $@ jsn.c

$(H)/servreg_loc.o: servreg_loc.c servreg_loc.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -I$(QRTR_DIR) -c -o $@ servreg_loc.c

$(H)/servreg-locator.o: servreg-locator.c jsn.h servreg_loc.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -I$(QRTR_DIR) -c -o $@ servreg-locator.c

$(H)/qrtr-qmi.o: $(QRTR_DIR)/qmi.c $(QRTR_DIR)/libqrtr.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I$(QRTR_DIR) -c -o $@ $(QRTR_DIR)/qmi.c

$(H)/qrtr-logging.o: $(QRTR_DIR)/logging.c $(QRTR_DIR)/logging.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I$(QRTR_DIR) -c -o $@ $(QRTR_DIR)/logging.c

$(H)/msmipc.o: ../msmipc.c ../msmipc.h $(H)/include/linux/msm_ipc.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I$(H)/include -I$(QRTR_DIR) -I.. -c -o $@ ../msmipc.c

# The phone binary is built from the sources in one step, as it always was.
$(D)/servreg-locator: $(SRCS) $(QRTR_DIR)/qmi.c $(QRTR_DIR)/logging.c ../msmipc.c $(HDRS) \
		$(D)/include/linux/msm_ipc.h
	@mkdir -p $(@D)
	$(DEVICE_CC) $(DEVICE_CFLAGS) -I$(D)/include -I. -I$(QRTR_DIR) -I.. -o $@ $(SRCS) \
		$(QRTR_DIR)/qmi.c $(QRTR_DIR)/logging.c ../msmipc.c

device: check-device-cc $(D)/servreg-locator

install-device: check-destdir device
	install -d $(DESTDIR)/usr/bin
	install -m 755 $(D)/servreg-locator $(DESTDIR)/usr/bin/

clean:
	rm -rf build

# Pure-protocol/parser tests: no sockets, no AF_MSM_IPC, so no msmipc.c and
# no vendor kernel headers needed -- see the two test files' headers.
$(H)/tests/test-jsn: tests/test_jsn.c jsn.c jsn.h servreg_loc.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -I$(QRTR_DIR) -o $@ tests/test_jsn.c jsn.c

$(H)/tests/test-servreg-loc: tests/test_servreg_loc.c servreg_loc.c servreg_loc.h $(QRTR_DIR)/qmi.c $(QRTR_DIR)/logging.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -I$(QRTR_DIR) -o $@ tests/test_servreg_loc.c servreg_loc.c $(QRTR_DIR)/qmi.c $(QRTR_DIR)/logging.c

# test-jsn reads its fixtures relative to tests/
test: $(H)/tests/test-jsn $(H)/tests/test-servreg-loc
	cd tests && ../$(H)/tests/test-jsn
	$(H)/tests/test-servreg-loc
