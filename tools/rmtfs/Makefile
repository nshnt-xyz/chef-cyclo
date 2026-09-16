# Port of linux-msm/rmtfs (BSD) for chef-cyclo: no rproc (-s dropped, no
# /sys/class/remoteproc on this 4.4 kernel) and no libudev (musl static
# build; sharedmem.c reads /sys/dev/char/<major>:<minor>/... directly).
# CC/CFLAGS/LDFLAGS/CPPFLAGS come from the caller (scripts/mkinitramfs.sh
# sets them for the musl aarch64 cross toolchain and points -I at
# tools/msmipc.h + tools/qrtr/libqrtr.h); this Makefile only adds to them.
OUT := rmtfs
TEST_OUT := tests/test-storage
TEST_SHAREDMEM_OUT := tests/test-sharedmem

CFLAGS += -Wall -g -O2 -static
LDFLAGS += -lqrtr -lpthread -static

SRCS := qmi_rmtfs.c rmtfs.c sharedmem.c storage.c util.c
OBJS := $(SRCS:.c=.o)

$(OUT): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(OUT) $(OBJS) $(TEST_OUT) $(TEST_SHAREDMEM_OUT)

$(TEST_OUT): tests/test_storage.c storage.c util.c rmtfs.h util.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -I../qrtr -o $@ tests/test_storage.c storage.c util.c

# -DRMTFS_TEST_HOOKS pulls in sharedmem.c's test-only rmtfs_mem
# constructors (never compiled into $(OUT)) so this can exercise
# rmtfs_mem_read()/rmtfs_mem_write() end-to-end, not just the pure
# offset-resolution math.
$(TEST_SHAREDMEM_OUT): tests/test_sharedmem.c sharedmem.c rmtfs.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -DRMTFS_TEST_HOOKS -I. -I../qrtr -o $@ tests/test_sharedmem.c sharedmem.c

test: $(TEST_OUT) $(TEST_SHAREDMEM_OUT)
	./$(TEST_OUT)
	./$(TEST_SHAREDMEM_OUT)
