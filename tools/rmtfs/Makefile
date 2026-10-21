# Port of linux-msm/rmtfs (BSD) for chef-cyclo: no rproc (-s dropped, no
# /sys/class/remoteproc on this 4.4 kernel) and no libudev (musl static
# build; sharedmem.c reads /sys/dev/char/<major>:<minor>/... directly).
# Linked with the vendored ../qrtr QMI codec and ../msmipc.c (the
# libqrtr API on AF_MSM_IPC) instead of libqrtr.
#
#   make                 host build: build/host/rmtfs
#   make test            host tests (build/host/tests/)
#   make device          the phone binary: build/device/rmtfs
#   make install-device DESTDIR=DIR   it into DIR/usr/bin

REPO_ROOT := ../..
CC ?= cc
CFLAGS ?= -Wall -Werror -g -O2
H := build/host
D := build/device

SRCS := qmi_rmtfs.c rmtfs.c sharedmem.c storage.c util.c
HDRS := qmi_rmtfs.h rmtfs.h util.h ../msmipc.h ../qrtr/libqrtr.h ../qrtr/logging.h
SHARED := ../qrtr/qmi.c ../qrtr/logging.c ../msmipc.c

.PHONY: all test clean device install-device

all: $(H)/rmtfs

include ../device.mk

$(H)/rmtfs: $(SRCS) $(SHARED) $(HDRS) $(H)/include/linux/msm_ipc.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I$(H)/include -I. -I../qrtr -I.. -o $@ $(SRCS) $(SHARED) -lpthread

# The phone binary: RAM-shadow writes only (modem-up runs it with -r), see
# docs/features/gps.md.
$(D)/rmtfs: $(SRCS) $(SHARED) $(HDRS) $(D)/include/linux/msm_ipc.h
	@mkdir -p $(@D)
	$(DEVICE_CC) $(DEVICE_CFLAGS) -I$(D)/include -I. -I../qrtr -I.. -o $@ $(SRCS) $(SHARED) -lpthread

device: check-device-cc $(D)/rmtfs

install-device: check-destdir device
	install -d $(DESTDIR)/usr/bin
	install -m 755 $(D)/rmtfs $(DESTDIR)/usr/bin/

clean:
	rm -rf build

$(H)/tests/test-storage: tests/test_storage.c storage.c util.c rmtfs.h util.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -I../qrtr -o $@ tests/test_storage.c storage.c util.c

# -DRMTFS_TEST_HOOKS pulls in sharedmem.c's test-only rmtfs_mem
# constructors (never compiled into rmtfs) so this can exercise
# rmtfs_mem_read()/rmtfs_mem_write() end-to-end, not just the pure
# offset-resolution math.
$(H)/tests/test-sharedmem: tests/test_sharedmem.c sharedmem.c rmtfs.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DRMTFS_TEST_HOOKS -I. -I../qrtr -o $@ tests/test_sharedmem.c sharedmem.c

# The open-request decode under ASan/UBSan: the 256-byte path case only
# fails loudly with it (a one-byte NUL overrun past req.path).
$(H)/tests/test-qmi-rmtfs: tests/test_qmi_rmtfs.c qmi_rmtfs.c qmi_rmtfs.h ../qrtr/qmi.c ../qrtr/logging.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Wall -Werror -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
		-fno-sanitize-recover=all -I. -I../qrtr -o $@ tests/test_qmi_rmtfs.c qmi_rmtfs.c \
		../qrtr/qmi.c ../qrtr/logging.c

test: $(H)/tests/test-storage $(H)/tests/test-sharedmem $(H)/tests/test-qmi-rmtfs
	$(H)/tests/test-storage
	$(H)/tests/test-sharedmem
	$(H)/tests/test-qmi-rmtfs
