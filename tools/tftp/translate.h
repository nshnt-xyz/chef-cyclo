/* translate: maps an RFS wire path (as sent by a TFTP RRQ/WRQ) to a handle
 * this daemon can actually read or write, per the exact allowlist in
 * REPORT.md 8.1 point 3. This replaces upstream tqftpserv's translate.c
 * (linux-msm/tqftpserv, BSD-3-Clause) entirely rather than porting it:
 * upstream's translate_readonly() scans /sys/class/remoteproc for whichever
 * firmware happens to be loaded and its translate_readwrite() opens a real
 * persistent directory on disk; neither matches this daemon's requirements
 * (a fixed, already-known firmware directory; RAM-only writes; a closed set
 * of allowed prefixes). See tftpserv.c's file header for the full list of
 * deviations from upstream.
 */
#ifndef CHEF_CYCLO_TFTP_TRANSLATE_H
#define CHEF_CYCLO_TFTP_TRANSLATE_H

#include <stddef.h>
#include <sys/types.h>

#include "ramfs.h"

#ifdef __cplusplus
extern "C" {
#endif

enum tftp_open_mode {
	TFTP_OPEN_RRQ,
	TFTP_OPEN_WRQ,
};

enum tftp_file_kind {
	TFTP_FILE_RO,	/* real, read-only fd under the firmware image dir */
	TFTP_FILE_RAM,	/* ramfs.h handle */
};

struct tftp_file {
	enum tftp_file_kind kind;
	int fd;			/* TFTP_FILE_RO */
	struct ram_file *rf;	/* TFTP_FILE_RAM */
};

/* translate_init() must be called once before any translate_open() call.
 * `firmware_dir` is opened once (O_DIRECTORY) and kept for the process
 * lifetime; every RO lookup is an openat() against that fixed dirfd, so a
 * later change to what /firmware/image points at can never redirect an
 * already-running daemon. Returns 0, or -1/errno if firmware_dir can't be
 * opened (fatal at startup -- see tftpserv.c's main()). */
int translate_init(const char *firmware_dir);

/* Seeds the /readwrite RAM namespace from `seed_dir` (built at image-build
 * time by scripts/mkinitramfs.sh from the persist partition -- see
 * ramfs.h). Fails closed: returns -1 if `seed_dir` or any of the eight
 * required files is missing or the wrong size, matching REPORT.md 8.1
 * point 3's exact seed-set/size requirement (mkinitramfs.sh independently
 * enforces the same sizes at build time; this is the runtime half of the
 * same fail-closed contract). Also seeds the single fixed
 * /shared/server_info.txt entry (no file involved, see tftpserv.c). */
int translate_seed(const char *seed_dir);

/* Translates `path` (as received on the wire, e.g.
 * "/readwrite/mot_rfs/imei_sv") into *out. Returns 0 on success. On
 * rejection returns -1 and sets *tftp_err to the TFTP_ERROR_* code the
 * caller should send back (never silence, per REPORT.md 8.1 point 2) and
 * *err_msg to a static, human-readable reason string.
 *
 * `mode` selects whether a RAM path may be created if it doesn't exist yet
 * (WRQ) or must already exist (RRQ -> ENOENT if not). `append` is only
 * meaningful for TFTP_OPEN_WRQ: a RAM file opened with append=0 is
 * truncated to empty first (see ramfs_truncate()'s comment for why); with
 * append=1 the existing content is kept and the caller's initial write
 * offset (returned in *ram_start_offset) is the file's current length. RO
 * firmware paths are always O_RDONLY and never honor append.
 */
int translate_open(const char *path, enum tftp_open_mode mode, int append,
		    struct tftp_file *out, off_t *ram_start_offset,
		    int *tftp_err, const char **err_msg);

/* Releases whatever translate_open() acquired (closes TFTP_FILE_RO's fd;
 * a no-op for TFTP_FILE_RAM, whose storage outlives the transfer). */
void translate_close(struct tftp_file *f);

#ifdef __cplusplus
}
#endif

#endif
