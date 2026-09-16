/* Host regression tests for rmtfs shared-memory phys_offset resolution. */
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "rmtfs.h"

/* This device's real values (chef-cyclo GPS-FSG live test, 2026-09-16):
 * rmtfs_mem_alloc() returned base 0xfd601000; the mapped UIO region backing
 * it is 2 MiB (sdm660.dtsi's qcom,rmtfs_sharedmem@0 reg is <0x0 0x200000>;
 * the base sits 4K past that allocation's start because of
 * qcom,guard-memory). */
#define TEST_BASE ((uint64_t)0xfd601000UL)
#define TEST_SIZE ((uint64_t)0x200000UL)

static void test_relative_offset_small(void)
{
	uint64_t offset;
	int ret;

	/* Exactly the failing request from the live-test log: `iovec 0, not
	 * forced` then `read 1:1 0x200`, which the old absolute-only check
	 * rejected every cycle because 0x200 < base. */
	ret = rmtfs_mem_resolve_offset(TEST_BASE, TEST_SIZE, 0x200UL, 512, &offset);
	assert(ret == 0);
	assert(offset == 0x200);
}

static void test_absolute_address_success(void)
{
	uint64_t offset;
	int ret;

	/* Bigger than the region, so it can only be a real absolute PA. */
	ret = rmtfs_mem_resolve_offset(TEST_BASE, TEST_SIZE, TEST_BASE + 0x300, 512, &offset);
	assert(ret == 0);
	assert(offset == 0x300);
}

static void test_end_boundary(void)
{
	uint64_t offset;
	int ret;

	/* Relative: last sector of the region is valid... */
	ret = rmtfs_mem_resolve_offset(TEST_BASE, TEST_SIZE, TEST_SIZE - 512, 512, &offset);
	assert(ret == 0);
	assert(offset == TEST_SIZE - 512);

	/* ...one byte further is not. */
	ret = rmtfs_mem_resolve_offset(TEST_BASE, TEST_SIZE, TEST_SIZE - 511, 512, &offset);
	assert(ret == -EINVAL);

	/* Absolute: same two cases at the top of the mapped region. */
	ret = rmtfs_mem_resolve_offset(TEST_BASE, TEST_SIZE, TEST_BASE + TEST_SIZE - 512, 512, &offset);
	assert(ret == 0);
	assert(offset == TEST_SIZE - 512);

	ret = rmtfs_mem_resolve_offset(TEST_BASE, TEST_SIZE, TEST_BASE + TEST_SIZE - 511, 512, &offset);
	assert(ret == -EINVAL);
}

static void test_overflow_and_out_of_range_rejected(void)
{
	uint64_t offset;
	int ret;

	/* Negative length is always rejected up front. */
	ret = rmtfs_mem_resolve_offset(TEST_BASE, TEST_SIZE, 0x200UL, -1, &offset);
	assert(ret == -EINVAL);

	/* Below the mapped region and too big to be a relative offset either. */
	ret = rmtfs_mem_resolve_offset(TEST_BASE, TEST_SIZE, TEST_BASE - 1, 512, &offset);
	assert(ret == -EINVAL);

	/*
	 * The previous implementation computed `end = phys_address + len`
	 * and compared it directly; a phys_address near ULONG_MAX made that
	 * sum wrap around to a small value that could slip past the
	 * upper-bound check entirely (start >= base already held, since
	 * ULONG_MAX >= base, so only the wrapped end was checked, and a
	 * small wrapped end always looked in-range). Confirm the
	 * subtraction-based check rejects it instead of wrapping.
	 */
	ret = rmtfs_mem_resolve_offset(TEST_BASE, TEST_SIZE, ULONG_MAX, 512, &offset);
	assert(ret == -EINVAL);

	/*
	 * The exact values an independent differential harness (run against
	 * the pre-patch file) used to reproduce an actual ASan wild-write
	 * SEGV: phys_address = ULONG_MAX - 0x100 wrapped `start + len` to
	 * 0x1ff, which the old `end > base + size` check let straight
	 * through. Pin that specific regression, not just "some" ULONG_MAX
	 * value.
	 */
	ret = rmtfs_mem_resolve_offset(TEST_BASE, TEST_SIZE, ULONG_MAX - 0x100, 512, &offset);
	assert(ret == -EINVAL);

	/* Inside the absolute range, but the span runs past the region end. */
	ret = rmtfs_mem_resolve_offset(TEST_BASE, TEST_SIZE, TEST_BASE + TEST_SIZE - 100, 512, &offset);
	assert(ret == -EINVAL);

	/* A value in the dead zone between the region size and the base:
	 * too big to be relative, too small to be an absolute PA in range. */
	ret = rmtfs_mem_resolve_offset(TEST_BASE, TEST_SIZE, TEST_SIZE + 0x1000, 512, &offset);
	assert(ret == -EINVAL);

	/* Relative interpretation, but the length alone already exceeds the
	 * whole region. */
	ret = rmtfs_mem_resolve_offset(TEST_BASE, TEST_SIZE, 0, (ssize_t)(TEST_SIZE + 1), &offset);
	assert(ret == -EINVAL);

	/* SSIZE_MAX as len must not overflow the (region_size - addr)
	 * subtraction into a false accept. */
	ret = rmtfs_mem_resolve_offset(TEST_BASE, TEST_SIZE, 0x200UL, SSIZE_MAX, &offset);
	assert(ret == -EINVAL);
}

static void test_zero_length_one_past_end(void)
{
	uint64_t offset;
	int ret;

	/*
	 * addr == region_size with len == 0 resolves to offset == size: a
	 * one-past-the-end, zero-byte span, the same convention C uses for
	 * an empty range at a container's end (e.g. a valid-but-unusable
	 * `end` iterator). rmtfs_iovec() never actually issues a zero-length
	 * request (always SECTOR_SIZE), so this never fires in production;
	 * documented here so the boundary choice reads as intentional
	 * rather than untested.
	 */
	ret = rmtfs_mem_resolve_offset(TEST_BASE, TEST_SIZE, TEST_SIZE, 0, &offset);
	assert(ret == 0);
	assert(offset == TEST_SIZE);
}

#ifdef RMTFS_TEST_HOOKS
static void test_end_to_end_mmap_backend(void)
{
	struct rmtfs_mem *rmem;
	char written[512];
	char readback[512];

	memset(written, 0xAB, sizeof(written));

	rmem = rmtfs_mem_open_test(TEST_BASE, TEST_SIZE);
	assert(rmem);

	/* Modem-relative write/read round-trip. */
	assert(rmtfs_mem_write(rmem, 0x200, written, sizeof(written)) == (ssize_t)sizeof(written));
	memset(readback, 0, sizeof(readback));
	assert(rmtfs_mem_read(rmem, 0x200, readback, sizeof(readback)) == (ssize_t)sizeof(readback));
	assert(memcmp(written, readback, sizeof(written)) == 0);

	/* An absolute address for the same bytes reads back identically. */
	memset(readback, 0, sizeof(readback));
	assert(rmtfs_mem_read(rmem, TEST_BASE + 0x200, readback, sizeof(readback)) == (ssize_t)sizeof(readback));
	assert(memcmp(written, readback, sizeof(written)) == 0);

	/* Out-of-range write is rejected before touching the mapping. */
	assert(rmtfs_mem_write(rmem, TEST_SIZE, written, sizeof(written)) == -EINVAL);

	rmtfs_mem_close(rmem);
}

static void test_end_to_end_fd_backend(void)
{
	struct rmtfs_mem *rmem;
	char path[] = "/tmp/rmtfs-sharedmem-test.XXXXXX";
	char written[512];
	char readback[512];
	int fd;

	memset(written, 0xCD, sizeof(written));

	fd = mkstemp(path);
	assert(fd >= 0);
	assert(ftruncate(fd, (off_t)TEST_SIZE) == 0);
	unlink(path);

	/* The pread/pwrite backend (used for /dev/qcom_rmtfs_mem*, the rfsa
	 * path) had no bounds check at all before this fix and, for a
	 * relative phys_offset, computed a bogus offset by subtracting the
	 * base from a value that was never absolute in the first place. */
	rmem = rmtfs_mem_open_test_fd(TEST_BASE, TEST_SIZE, fd);
	assert(rmem);

	assert(rmtfs_mem_write(rmem, 0x200, written, sizeof(written)) == (ssize_t)sizeof(written));
	memset(readback, 0, sizeof(readback));
	assert(rmtfs_mem_read(rmem, TEST_BASE + 0x200, readback, sizeof(readback)) == (ssize_t)sizeof(readback));
	assert(memcmp(written, readback, sizeof(written)) == 0);

	assert(rmtfs_mem_write(rmem, TEST_SIZE, written, sizeof(written)) == -EINVAL);

	/*
	 * Before this fix, the pread/pwrite path computed
	 * `phys_address - rmem->address` with no bounds check at all: a
	 * below-base or dead-zone value produced a huge (unsigned off_t) or
	 * negative pread()/pwrite() offset instead of a clean rejection.
	 */
	assert(rmtfs_mem_read(rmem, TEST_BASE - 1, readback, sizeof(readback)) == -EINVAL);
	assert(rmtfs_mem_write(rmem, TEST_SIZE + 0x1000, written, sizeof(written)) == -EINVAL);

	rmtfs_mem_close(rmem);
}
#endif /* RMTFS_TEST_HOOKS */

int main(void)
{
	test_relative_offset_small();
	test_absolute_address_success();
	test_end_boundary();
	test_overflow_and_out_of_range_rejected();
	test_zero_length_one_past_end();
#ifdef RMTFS_TEST_HOOKS
	test_end_to_end_mmap_backend();
	test_end_to_end_fd_backend();
#endif

	printf("All sharedmem offset-resolution tests passed.\n");

	return 0;
}
