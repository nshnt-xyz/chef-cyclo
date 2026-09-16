#ifndef __RMTFS_H__
#define __RMTFS_H__

#include <stdint.h>
#include "qmi_rmtfs.h"

#define SECTOR_SIZE		512

struct qmi_packet {
	uint8_t flags;
	uint16_t txn_id;
	uint16_t msg_id;
	uint16_t msg_len;
	uint8_t data[];
} __attribute__((__packed__));

struct rmtfs_mem;

struct rmtfs_mem *rmtfs_mem_open(void);
void rmtfs_mem_close(struct rmtfs_mem *rmem);
int64_t rmtfs_mem_alloc(struct rmtfs_mem *rmem, size_t size);
void rmtfs_mem_free(struct rmtfs_mem *rmem);
ssize_t rmtfs_mem_read(struct rmtfs_mem *rmem, unsigned long phys_address, void *buf, ssize_t len);
ssize_t rmtfs_mem_write(struct rmtfs_mem *rmem, unsigned long phys_address, const void *buf, ssize_t len);
/* Exposed so host tests can exercise the absolute-vs-relative phys_offset
 * resolution directly; see the comment on its definition in sharedmem.c. */
int rmtfs_mem_resolve_offset(uint64_t base_address, uint64_t region_size,
			      unsigned long phys_address, ssize_t len,
			      uint64_t *offset_out);

#ifdef RMTFS_TEST_HOOKS
/* Test-only rmtfs_mem constructors; never linked into the production
 * binary. See the comment on their definitions in sharedmem.c. */
struct rmtfs_mem *rmtfs_mem_open_test(uint64_t address, uint64_t size);
struct rmtfs_mem *rmtfs_mem_open_test_fd(uint64_t address, uint64_t size, int fd);
#endif

struct rmtfd;

int storage_init(const char *storage_root, bool read_only, bool use_partitions);
#define SLOT_SUFFIX_LEN	(2 + 1) /* "_a" or "_b", null-terminated */
struct rmtfd *storage_open(unsigned node, const char *path, const char *slot_suffix);
struct rmtfd *storage_get(unsigned node, int caller_id);
void storage_close(struct rmtfd *rmtfd);
int storage_get_caller_id(const struct rmtfd *rmtfd);
int storage_get_error(const struct rmtfd *rmtfd);
void storage_exit(void);
ssize_t storage_pread(const struct rmtfd *rmtfd, void *buf, size_t nbyte, off_t offset);
ssize_t storage_pwrite(struct rmtfd *rmtfd, const void *buf, size_t nbyte, off_t offset);
int storage_sync(struct rmtfd *rmtfd);

#endif
