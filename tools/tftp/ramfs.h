/* ramfs: the bounded in-RAM filesystem tftpserv.c's translate.c hands out
 * handles into. Every writable RFS namespace this daemon serves
 * (/readwrite, /shared, /hlos, /ramdumps -- see translate.h) is backed by
 * this table, never by a real file: REPORT.md 8.1 point 4 requires that
 * this daemon "never opens the persist partition, any block device, or
 * anything writable outside RAM" at runtime. The seed content for
 * /readwrite comes from scripts/mkinitramfs.sh, which extracts it from the
 * persist partition image at *build* time with a read-only ext4 reader
 * (debugfs) and ships it as plain files under /usr/share/rfs/msm/mpss in
 * the initramfs -- see ramfs_seed_dir() below and gps-up's -s flag.
 *
 * Growable-buffer design lifted directly from tools/rmtfs/storage.c's
 * storage_pread()/storage_pwrite() (STORAGE_MAX_SIZE, realloc-on-grow,
 * memcpy at an explicit offset) -- same shape, applied per named file
 * instead of per partition, with an added whole-table size cap (§8.1 point
 * 5's "bounded RAM shadow size").
 */
#ifndef CHEF_CYCLO_TFTP_RAMFS_H
#define CHEF_CYCLO_TFTP_RAMFS_H

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Per-file and whole-table caps, per REPORT.md 8.1 point 5 ("RAM shadow cap
 * (e.g. 4 MiB total, 1 MiB per file)"). */
#define RAMFS_MAX_FILE_SIZE	(1u * 1024 * 1024)
#define RAMFS_MAX_TOTAL_SIZE	(4u * 1024 * 1024)

/* Generous headroom above the 8 seeded files + server_info.txt: bounds the
 * table itself against a client WRQing many distinct new names under
 * /hlos or /ramdumps. */
#define RAMFS_MAX_FILES		64
#define RAMFS_MAX_NAME		192

struct ram_file {
	char name[RAMFS_MAX_NAME];
	unsigned char *buf;
	size_t len;
	int in_use;
};

/* Resets the table to empty. Frees nothing on first use (in_use starts
 * clear); safe to call again in a test to reset state (frees any buffers
 * first). */
void ramfs_init(void);

/* Exact-name lookup. Returns NULL if no such file has been seeded or
 * created yet -- the RRQ caller turns that into TFTP_ERROR_ENOENT, matching
 * "/hlos/..., /ramdumps/... -> empty RAM directories (RRQ -> ERROR not
 * found)" (REPORT.md 8.1 point 3). */
struct ram_file *ramfs_lookup(const char *name);

/* Creates a new zero-length entry, or returns the existing one if `name`
 * is already present (idempotent, matching a WRQ that reopens a file this
 * daemon already holds). Returns NULL if the table is full. */
struct ram_file *ramfs_open(const char *name);

/* Discards a file's content in place (len -> 0) without removing its table
 * slot. Used for a plain (non-append) WRQ: unlike upstream tqftpserv,
 * which only ever grows-or-overwrites-in-place via a real fd's monotonic
 * write() position (tools/tftp/tftpserv.c's file header explains why that
 * doesn't fully cover "overwrite" for a RAM file), a fresh WRQ here starts
 * from a clean, empty file so old trailing bytes never survive a shorter
 * overwrite. */
void ramfs_truncate(struct ram_file *rf);

/* Bounds-checked read: short reads past EOF return the number of bytes
 * actually available (0 at or past EOF), like storage_pread()'s RAM-shadow
 * branch -- never short-pads with zeroes (unlike storage_pread(); TFTP RRQ
 * callers use the true byte count to size the final DATA packet, so a
 * TFTP-level "less than blksize" end-of-file signal is length-accurate). */
ssize_t ramfs_pread(const struct ram_file *rf, void *buf, size_t nbyte, off_t offset);

/* Bounds-checked, cap-enforced write. Fails (-1/ENOSPC) if the write would
 * exceed RAMFS_MAX_FILE_SIZE for this file or RAMFS_MAX_TOTAL_SIZE across
 * the whole table; never partially applies a rejected write. */
ssize_t ramfs_pwrite(struct ram_file *rf, const void *buf, size_t nbyte, off_t offset);

/* Seeds `name` with exactly `len` bytes read from `path` (used only at
 * daemon startup, from the build-time-extracted seed directory -- see
 * tftpserv.c's main()). Fails closed (returns -1) if `path` can't be
 * opened, read in full, or would exceed the per-file/total caps -- callers
 * treat any failure here as fatal, matching REPORT.md 8.1 point 3's
 * "Missing required seed files -> fail closed." */
int ramfs_seed_file(const char *name, const char *path);

/* Seeds a fixed in-memory buffer directly (no file involved) -- used for
 * /shared/server_info.txt's constant 15 bytes. */
int ramfs_seed_bytes(const char *name, const void *data, size_t len);

/* Current sum of every file's len, exposed only for tests. */
size_t ramfs_total_size(void);

#ifdef __cplusplus
}
#endif

#endif
