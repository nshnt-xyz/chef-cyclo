#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <endian.h>	/* be32toh/be64toh: both glibc and musl have this, not <sys/endian.h> (BSD) */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>
#include "rmtfs.h"

static int rmtfs_mem_enumerate(struct rmtfs_mem *rmem);

struct rmtfs_mem {
	uint64_t address;
	uint64_t size;
	void *base;
	int fd;
};

/*
 * No libudev on this musl/busybox target (and none of the helpers here link
 * it, to keep static -Os builds simple). sysfs gives the same two attributes
 * udev_device_get_sysattr_value() would have: resolve the open fd's
 * major:minor and read the attribute file directly under
 * /sys/dev/char/<major>:<minor>/.
 */
static int read_hex_sysattr(const char *path, uint64_t *value)
{
	char buf[64];
	char *endptr;
	ssize_t n;
	int fd;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -errno;

	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return -EIO;
	buf[n] = '\0';

	errno = 0;
	*value = strtoull(buf, &endptr, 16);
	if ((*value == ULLONG_MAX && errno == ERANGE) || endptr == buf)
		return errno ? -errno : -EINVAL;

	return 0;
}

static int sysattr_path(int fd, char *out, size_t outlen, const char *attr)
{
	struct stat sb;

	if (fstat(fd, &sb) < 0)
		return -errno;

	snprintf(out, outlen, "/sys/dev/char/%u:%u/%s",
		 major(sb.st_rdev), minor(sb.st_rdev), attr);
	return 0;
}

static int rmtfs_mem_open_rfsa(struct rmtfs_mem *rmem, int client_id)
{
	char path[PATH_MAX];
	int saved_errno;
	int ret;
	int fd;

	snprintf(path, sizeof(path), "/dev/qcom_rmtfs_mem%d", client_id);

	fd = open(path, O_RDWR);
	if (fd < 0) {
		saved_errno = errno;
		fprintf(stderr, "failed to open %s: %s\n", path, strerror(errno));
		return -saved_errno;
	}
	rmem->fd = fd;

	ret = sysattr_path(fd, path, sizeof(path), "phys_addr");
	if (ret < 0) {
		saved_errno = -ret;
		goto err_close_fd;
	}

	ret = read_hex_sysattr(path, &rmem->address);
	if (ret < 0) {
		fprintf(stderr, "failed to parse phys_addr of qcom_rmtfs_mem%d\n", client_id);
		saved_errno = -ret;
		goto err_close_fd;
	}

	ret = sysattr_path(fd, path, sizeof(path), "size");
	if (ret < 0) {
		saved_errno = -ret;
		goto err_close_fd;
	}

	ret = read_hex_sysattr(path, &rmem->size);
	if (ret < 0) {
		fprintf(stderr, "failed to parse size of qcom_rmtfs_mem%d\n", client_id);
		saved_errno = -ret;
		goto err_close_fd;
	}

	return 0;

err_close_fd:
	close(fd);
	return -saved_errno;
}

static int rmtfs_mem_open_uio(struct rmtfs_mem *rmem, int client_id)
{
	char path[PATH_MAX];
	int saved_errno;
	int ret;
	int fd;

	/*
	 * gps-up symlinks the uio device whose /sys/class/uio/uioN/name is
	 * "rmtfs" (msm_sharedmem, sdm660.dtsi qcom,sharedmem-uio client-id 1)
	 * to this fixed name so we don't have to scan /sys/class/uio here.
	 */
	snprintf(path, sizeof(path), "/dev/qcom_rmtfs_uio%d", client_id);

	fd = open(path, O_RDWR);
	if (fd < 0) {
		saved_errno = errno;
		fprintf(stderr, "failed to open %s: %s\n", path, strerror(errno));
		return -saved_errno;
	}
	rmem->fd = fd;

	ret = sysattr_path(fd, path, sizeof(path), "maps/map0/addr");
	if (ret < 0) {
		saved_errno = -ret;
		goto err_close_fd;
	}

	ret = read_hex_sysattr(path, &rmem->address);
	if (ret < 0) {
		fprintf(stderr, "failed to parse phys_addr of qcom_rmtfs_uio%d\n", client_id);
		saved_errno = -ret;
		goto err_close_fd;
	}

	ret = sysattr_path(fd, path, sizeof(path), "maps/map0/size");
	if (ret < 0) {
		saved_errno = -ret;
		goto err_close_fd;
	}

	ret = read_hex_sysattr(path, &rmem->size);
	if (ret < 0) {
		fprintf(stderr, "failed to parse size of qcom_rmtfs_uio%d\n", client_id);
		saved_errno = -ret;
		goto err_close_fd;
	}

	rmem->base = mmap(0, rmem->size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
	if (rmem->base == MAP_FAILED) {
		saved_errno = errno;
		fprintf(stderr, "failed to mmap: %s\n", strerror(errno));
		goto err_close_fd;
	}

	return 0;

err_close_fd:
	close(fd);
	return -saved_errno;
}

struct rmtfs_mem *rmtfs_mem_open(void)
{
	struct rmtfs_mem *rmem;
	void *base;
	int ret;
	int fd;

	rmem = malloc(sizeof(*rmem));
	if (!rmem)
		return NULL;

	memset(rmem, 0, sizeof(*rmem));

	ret = rmtfs_mem_open_rfsa(rmem, 1);
	if (ret < 0 && ret != -ENOENT) {
		goto err;
	} else if (ret < 0) {
		fprintf(stderr, "falling back to uio access\n");
		ret = rmtfs_mem_open_uio(rmem, 1);
		if (ret < 0 && ret != -ENOENT) {
			goto err;
		} else if (ret < 0) {
			fprintf(stderr, "falling back to /dev/mem access\n");

			ret = rmtfs_mem_enumerate(rmem);
			if (ret < 0)
				goto err;

			fd = open("/dev/mem", O_RDWR|O_SYNC);
			if (fd < 0) {
				fprintf(stderr, "failed to open /dev/mem\n");
				goto err;
			}

			base = mmap(0, rmem->size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, rmem->address);
			if (base == MAP_FAILED) {
				fprintf(stderr, "failed to mmap: %s\n", strerror(errno));
				goto err_close_fd;
			}

			rmem->base = base;
			rmem->fd = fd;
		}
	}

	return rmem;

err_close_fd:
	close(fd);
err:
	free(rmem);
	return NULL;
}

#ifdef RMTFS_TEST_HOOKS
/*
 * Test-only constructors: `struct rmtfs_mem` is opaque outside this file, so
 * host unit tests need a way to build one without the real /dev nodes
 * rmtfs_mem_open() requires. Compiled in only when the test binary passes
 * -DRMTFS_TEST_HOOKS (see tests/test_sharedmem.c and the Makefile); never
 * part of the production rmtfs binary.
 */
struct rmtfs_mem *rmtfs_mem_open_test(uint64_t address, uint64_t size)
{
	struct rmtfs_mem *rmem;

	rmem = malloc(sizeof(*rmem));
	if (!rmem)
		return NULL;

	memset(rmem, 0, sizeof(*rmem));
	rmem->address = address;
	rmem->size = size;
	rmem->fd = -1;

	rmem->base = mmap(NULL, size, PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (rmem->base == MAP_FAILED) {
		free(rmem);
		return NULL;
	}

	return rmem;
}

struct rmtfs_mem *rmtfs_mem_open_test_fd(uint64_t address, uint64_t size, int fd)
{
	struct rmtfs_mem *rmem;

	rmem = malloc(sizeof(*rmem));
	if (!rmem)
		return NULL;

	memset(rmem, 0, sizeof(*rmem));
	rmem->address = address;
	rmem->size = size;
	rmem->fd = fd;
	rmem->base = NULL;

	return rmem;
}
#endif /* RMTFS_TEST_HOOKS */

int64_t rmtfs_mem_alloc(struct rmtfs_mem *rmem, size_t alloc_size)
{
	if (alloc_size > rmem->size) {
		fprintf(stderr,
			"[RMTFS] rmtfs shared memory not large enough for allocation request 0x%zx vs %" PRIu64 "\n",
			alloc_size, rmem->size);
		return -EINVAL;
	}

	return rmem->address;
}

void rmtfs_mem_free(struct rmtfs_mem *rmem)
{
	(void)rmem;
}

/*
 * Resolve a QMI_RMTFS_RW_IOVEC phys_offset value into a byte offset within
 * the mapped RMTFS region, validating that the whole [offset, offset+len)
 * span lies inside it.
 *
 * Most modem firmware reports phys_offset as an absolute physical address
 * (the historical assumption here). Some downstream Qualcomm modem stacks
 * instead report it as an offset relative to the base address
 * rmtfs_mem_alloc() handed back -- observed on this device (chef-cyclo GPS
 * bring-up, 2026-09-16 live test: `alloc 0,0 => 0xfd601000`, then every
 * `iovec` read arrived as `phys_offset 0x200`, far below the mapped base,
 * and was rejected every cycle) and matches the identical fix in upstream
 * linux-msm/rmtfs PR #41 (github.com/linux-msm/rmtfs/pull/41, a different
 * device: MSM8909W downstream, same symptom).
 *
 * There's no in-band signal for which encoding a given request uses, so
 * this auto-detects from range rather than tracking an alloc-time mode:
 * this platform's RMTFS region sits at a multi-GB physical base
 * (0xfd601000) while the region itself is 2 MiB (sdm660.dtsi's
 * `qcom,rmtfs_sharedmem@0` reg is `<0x0 0x200000>`; the base observed here
 * sits 4K past that allocation's start because of `qcom,guard-memory`), so
 * any request whose whole span fits below region_size cannot be a real
 * absolute PA here and is treated as relative; anything else falls back to
 * the original absolute-address interpretation. All bounds checks are
 * subtraction-based
 * (never phys_address + len) so an oversized or near-ULONG_MAX phys_offset
 * can't wrap into a false accept, which the previous start+len form could.
 */
int rmtfs_mem_resolve_offset(uint64_t base_address, uint64_t region_size,
			      unsigned long phys_address, ssize_t len,
			      uint64_t *offset_out)
{
	uint64_t addr = phys_address;
	uint64_t relative;

	if (len < 0)
		return -EINVAL;

	if (addr <= region_size && (uint64_t)len <= region_size - addr) {
		*offset_out = addr;
		return 0;
	}

	if (addr < base_address)
		return -EINVAL;

	relative = addr - base_address;
	if (relative > region_size || (uint64_t)len > region_size - relative)
		return -EINVAL;

	*offset_out = relative;
	return 0;
}

ssize_t rmtfs_mem_read(struct rmtfs_mem *rmem, unsigned long phys_address, void *buf, ssize_t len)
{
	uint64_t offset;
	int ret;

	ret = rmtfs_mem_resolve_offset(rmem->address, rmem->size, phys_address, len, &offset);
	if (ret < 0)
		return ret;

	if (rmem->base) {
		memcpy(buf, (char *)rmem->base + offset, len);
	} else {
		len = pread(rmem->fd, buf, len, (off_t)offset);
	}

	return len;
}

ssize_t rmtfs_mem_write(struct rmtfs_mem *rmem, unsigned long phys_address, const void *buf, ssize_t len)
{
	uint64_t offset;
	int ret;

	ret = rmtfs_mem_resolve_offset(rmem->address, rmem->size, phys_address, len, &offset);
	if (ret < 0)
		return ret;

	if (rmem->base) {
		memcpy((char *)rmem->base + offset, buf, len);
	} else {
		len = pwrite(rmem->fd, buf, len, (off_t)offset);
	}

	return len;
}

void rmtfs_mem_close(struct rmtfs_mem *rmem)
{
	if (rmem->base)
		munmap(rmem->base, rmem->size);

	close(rmem->fd);

	free(rmem);
}

static int rmtfs_mem_enumerate(struct rmtfs_mem *rmem)
{
	union {
		uint32_t dw[2];
		uint64_t qw[2];
	} reg;
	struct dirent *de;
	int basefd;
	int dirfd;
	int regfd;
	DIR *dir;
	int ret = 0;
	int n;

	basefd = open("/proc/device-tree/reserved-memory/", O_DIRECTORY);
	if (basefd < 0) {
		fprintf(stderr,
			"Unable to open reserved-memory device tree node: %s\n",
			strerror(errno));
		return -1;
	}
	dir = fdopendir(basefd);
	if (!dir) {
		fprintf(stderr,
			"Unable to open reserved-memory device tree node: %s\n",
			strerror(errno));
		close(basefd);
		return -1;
	}

	while ((de = readdir(dir)) != NULL) {
		if (strncmp(de->d_name, "rmtfs", 5) != 0)
			continue;

		dirfd = openat(basefd, de->d_name, O_DIRECTORY);
		if (dirfd < 0) {
			fprintf(stderr, "failed to open %s: %s\n",
				de->d_name, strerror(errno));
			ret = -1;
			goto out;
		}

		regfd = openat(dirfd, "reg", O_RDONLY);
		if (regfd < 0) {
			fprintf(stderr, "failed to open reg of %s: %s\n",
				de->d_name, strerror(errno));
			close(dirfd);
			ret = -1;
			goto out;
		}

		n = read(regfd, &reg, sizeof(reg));
		if (n == 2 * sizeof(uint32_t)) {
			rmem->address = be32toh(reg.dw[0]);
			rmem->size = be32toh(reg.dw[1]);
		} else if (n == 2 * sizeof(uint64_t)) {
			rmem->address = be64toh(reg.qw[0]);
			rmem->size = be64toh(reg.qw[1]);
		} else {
			fprintf(stderr, "failed to read reg of %s: %s\n",
				de->d_name, strerror(errno));
			ret = -1;
		}

		close(regfd);
		close(dirfd);
		break;
	}

out:
	closedir(dir);
	return ret;
}
