/* Host regression tests for rmtfs storage safety properties. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "rmtfs.h"
#include "util.h"
#include <libqrtr.h>

static void write_all(int fd, const void *buf, size_t len)
{
	const char *p = buf;
	while (len) {
		ssize_t n = write(fd, p, len);
		assert(n > 0);
		p += n;
		len -= (size_t)n;
	}
}

static int main_ram_shadow_test(void)
{
	char root[] = "/tmp/rmtfs-storage-test.XXXXXX";
	char path[PATH_MAX];
	const char initial[] = "immutable EFS image";
	const char changed[] = "RAM shadow only";
	char buf[sizeof(initial)] = {};
	struct rmtfd *fd;
	int file;

	assert(mkdtemp(root));
	/* Regression for the AF_MSM_IPC receive contract: a standalone
	 * RESUME_TX (0) must return control to select(), not trigger a second
	 * blocking receive. Transient nonblocking/interrupted reads do likewise;
	 * real data and a modem reset are not conflated with them. */
	assert(QRTR_RECV_RESUME_TX == 0);
	assert(rmtfs_recv_is_retryable_error(-EAGAIN));
	assert(rmtfs_recv_is_retryable_error(-EWOULDBLOCK));
	assert(rmtfs_recv_is_retryable_error(-EINTR));
	assert(!rmtfs_recv_is_retryable_error(1));
	assert(!rmtfs_recv_is_retryable_error(-ENETRESET));
	assert(rmtfs_recv_requires_rebind(-ENETRESET));
	assert(!rmtfs_recv_requires_rebind(QRTR_RECV_RESUME_TX));
	assert(!rmtfs_recv_requires_rebind(-EAGAIN));
	assert(snprintf(path, sizeof(path), "%s/modem_fs1", root) > 0);
	file = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
	assert(file >= 0);
	write_all(file, initial, sizeof(initial));
	assert(close(file) == 0);

	/* A RAM-shadow session never modifies its backing image. Unknown callers
	 * are rejected before GET_DEV_ERROR; its defensive NULL result must never
	 * dereference a bogus handle. */
	assert(storage_init(root, true, false) == 0);
	assert(storage_get(0x1234, -1) == NULL);
	assert(storage_get(0x1234, 0) == NULL);
	assert(storage_get_error(NULL) == EINVAL);

	fd = storage_open(0x1234, "/boot/modem_fs1", "");
	assert(fd);
	assert(storage_pread(fd, buf, sizeof(buf), 0) == (ssize_t)sizeof(buf));
	assert(memcmp(buf, initial, sizeof(initial)) == 0);
	assert(storage_pwrite(fd, changed, sizeof(changed), 0) == (ssize_t)sizeof(changed));
	errno = 0;
	assert(storage_pwrite(fd, changed, 1, 16 * 1024 * 1024) == -1);
	assert(errno == EFBIG);
	memset(buf, 0, sizeof(buf));
	assert(storage_pread(fd, buf, sizeof(buf), 0) == (ssize_t)sizeof(buf));
	assert(memcmp(buf, changed, sizeof(changed)) == 0);
	storage_close(fd);
	storage_exit();

	file = open(path, O_RDONLY);
	assert(file >= 0);
	memset(buf, 0, sizeof(buf));
	assert(read(file, buf, sizeof(buf)) == (ssize_t)sizeof(buf));
	assert(close(file) == 0);
	assert(memcmp(buf, initial, sizeof(initial)) == 0);
	assert(unlink(path) == 0);
	assert(rmdir(root) == 0);

	return 0;
}

/*
 * Regression for the active-slot fsg fix: this device has no unsuffixed fsg
 * partition, only fsg_a/fsg_b, so gps-up passes rmtfs -S "$SLOT" and never
 * creates an unsuffixed "fsg" by-partlabel link itself. storage_open()'s
 * unsuffixed-then-suffixed fallback is the only thing standing in for that
 * link; this test proves it lands on exactly the one slot it was given and
 * never reaches for the other slot's fsg.
 */
static void test_fsg_slot_suffix_only(void)
{
	char root[] = "/tmp/rmtfs-storage-fsg-test.XXXXXX";
	char path_a[PATH_MAX], path_b[PATH_MAX];
	const char slot_a_data[] = "fsg_a contents";
	const char slot_b_data[] = "fsg_b contents";
	char buf[sizeof(slot_a_data)] = {};
	struct rmtfd *fd;
	int file;

	assert(mkdtemp(root));

	/* Both fsg_a and fsg_b exist, with distinct content, and neither an
	 * unsuffixed "fsg" -- the real live layout, so a request for one
	 * slot succeeding is never merely "the other slot was absent". */
	assert(snprintf(path_a, sizeof(path_a), "%s/fsg_a", root) > 0);
	file = open(path_a, O_CREAT | O_EXCL | O_WRONLY, 0600);
	assert(file >= 0);
	write_all(file, slot_a_data, sizeof(slot_a_data));
	assert(close(file) == 0);

	assert(snprintf(path_b, sizeof(path_b), "%s/fsg_b", root) > 0);
	file = open(path_b, O_CREAT | O_EXCL | O_WRONLY, 0600);
	assert(file >= 0);
	write_all(file, slot_b_data, sizeof(slot_b_data));
	assert(close(file) == 0);

	/* use_partitions=true, storage_root=root: mirrors gps-up handing rmtfs
	 * a by-partlabel-style directory via -P, with our temp dir standing in
	 * for /dev/disk/by-partlabel. */
	assert(storage_init(root, true, true) == 0);

	/* An unsuffixed request (as if gps-up wrongly created an unsuffixed
	 * "fsg" alias, or rmtfs were run without -S) must fail: there is
	 * genuinely no unsuffixed fsg on this device. */
	assert(storage_open(0x1, "/boot/modem_fsg", "") == NULL);

	/* Each slot must resolve to *its own* file's content, not merely
	 * succeed because the other slot happens to be missing -- both are
	 * present here, so this proves exact selection, not absence. */
	fd = storage_open(0x1, "/boot/modem_fsg", "_a");
	assert(fd);
	assert(storage_pread(fd, buf, sizeof(buf), 0) == (ssize_t)sizeof(buf));
	assert(memcmp(buf, slot_a_data, sizeof(slot_a_data)) == 0);
	storage_close(fd);

	memset(buf, 0, sizeof(buf));
	fd = storage_open(0x2, "/boot/modem_fsg", "_b");
	assert(fd);
	assert(storage_pread(fd, buf, sizeof(buf), 0) == (ssize_t)sizeof(buf));
	assert(memcmp(buf, slot_b_data, sizeof(slot_b_data)) == 0);
	storage_close(fd);

	storage_exit();
	assert(unlink(path_a) == 0);
	assert(unlink(path_b) == 0);
	assert(rmdir(root) == 0);
}

int main(void)
{
	test_fsg_slot_suffix_only();
	return main_ram_shadow_test();
}
