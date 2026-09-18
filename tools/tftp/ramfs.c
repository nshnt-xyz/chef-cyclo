/* See ramfs.h for the design note. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ramfs.h"

static struct ram_file g_files[RAMFS_MAX_FILES];
static size_t g_total_size;

void ramfs_init(void)
{
	int i;

	for (i = 0; i < RAMFS_MAX_FILES; i++) {
		free(g_files[i].buf);
		g_files[i].buf = NULL;
		g_files[i].len = 0;
		g_files[i].in_use = 0;
		g_files[i].name[0] = '\0';
	}
	g_total_size = 0;
}

struct ram_file *ramfs_lookup(const char *name)
{
	int i;

	for (i = 0; i < RAMFS_MAX_FILES; i++) {
		if (g_files[i].in_use && strcmp(g_files[i].name, name) == 0)
			return &g_files[i];
	}
	return NULL;
}

struct ram_file *ramfs_open(const char *name)
{
	struct ram_file *rf;
	int i;
	int free_slot = -1;

	rf = ramfs_lookup(name);
	if (rf)
		return rf;

	if (strlen(name) >= RAMFS_MAX_NAME) {
		errno = ENAMETOOLONG;
		return NULL;
	}

	for (i = 0; i < RAMFS_MAX_FILES; i++) {
		if (!g_files[i].in_use) {
			free_slot = i;
			break;
		}
	}
	if (free_slot < 0) {
		errno = ENOSPC;
		return NULL;
	}

	rf = &g_files[free_slot];
	strcpy(rf->name, name);
	rf->buf = NULL;
	rf->len = 0;
	rf->in_use = 1;
	return rf;
}

void ramfs_truncate(struct ram_file *rf)
{
	g_total_size -= rf->len;
	free(rf->buf);
	rf->buf = NULL;
	rf->len = 0;
}

ssize_t ramfs_pread(const struct ram_file *rf, void *buf, size_t nbyte, off_t offset)
{
	ssize_t n;

	if (offset < 0 || (size_t)offset >= rf->len)
		return 0;

	n = (ssize_t)(rf->len - (size_t)offset);
	if ((size_t)n > nbyte)
		n = (ssize_t)nbyte;

	memcpy(buf, rf->buf + offset, (size_t)n);
	return n;
}

ssize_t ramfs_pwrite(struct ram_file *rf, const void *buf, size_t nbyte, off_t offset)
{
	size_t new_len;
	size_t grow;
	unsigned char *new_buf;

	if (offset < 0) {
		errno = EINVAL;
		return -1;
	}
	if ((uintmax_t)offset > RAMFS_MAX_FILE_SIZE ||
	    nbyte > RAMFS_MAX_FILE_SIZE - (size_t)offset) {
		errno = ENOSPC;
		return -1;
	}
	new_len = (size_t)offset + nbyte;

	if (new_len > rf->len) {
		grow = new_len - rf->len;
		if (grow > RAMFS_MAX_TOTAL_SIZE - g_total_size) {
			errno = ENOSPC;
			return -1;
		}

		new_buf = realloc(rf->buf, new_len);
		if (!new_buf) {
			errno = ENOMEM;
			return -1;
		}
		/* Zero the gap between the old end and `offset` (a write
		 * starting past the current EOF, e.g. a "seek" option past
		 * end-of-file) so a later read never returns uninitialized
		 * heap bytes. */
		if ((size_t)offset > rf->len)
			memset(new_buf + rf->len, 0, (size_t)offset - rf->len);

		rf->buf = new_buf;
		g_total_size += grow;
		rf->len = new_len;
	}

	memcpy(rf->buf + offset, buf, nbyte);
	return (ssize_t)nbyte;
}

int ramfs_seed_bytes(const char *name, const void *data, size_t len)
{
	struct ram_file *rf;

	rf = ramfs_open(name);
	if (!rf)
		return -1;

	if (ramfs_pwrite(rf, data, len, 0) != (ssize_t)len)
		return -1;

	return 0;
}

int ramfs_seed_file(const char *name, const char *path)
{
	unsigned char buf[65536];
	struct ram_file *rf;
	off_t offset = 0;
	ssize_t n;
	int fd;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;

	rf = ramfs_open(name);
	if (!rf) {
		close(fd);
		return -1;
	}

	for (;;) {
		n = read(fd, buf, sizeof(buf));
		if (n < 0) {
			if (errno == EINTR)
				continue;
			close(fd);
			return -1;
		}
		if (n == 0)
			break;
		if (ramfs_pwrite(rf, buf, (size_t)n, offset) != n) {
			close(fd);
			return -1;
		}
		offset += n;
	}

	close(fd);
	return 0;
}

size_t ramfs_total_size(void)
{
	return g_total_size;
}
