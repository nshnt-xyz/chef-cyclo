/* See translate.h for what replaces upstream tqftpserv's translate.c here,
 * and why.
 *
 * Allowlist (REPORT.md 8.1 point 3), exact and closed:
 *   /readonly/firmware/image/<file>
 *   /readonly/vendor/firmware/image/<file>   -- both map to the one real,
 *       already-read-only-mounted /firmware/image directory (gps-up mounts
 *       the active modem_$SLOT partition there); RRQ (O_RDONLY) only,
 *       <file> must be a single flat component (no '/' at all -- Round 1
 *       review F8) naming a regular file (fstat()-checked, same review).
 *   /readonly/fsg/...                        -- always rejected (dangling
 *       on stock too, per REPORT.md 6).
 *   /readwrite/<path>                        -- RAM shadow, seeded at
 *       startup from translate_seed()'s seed_dir, read/write, new names
 *       may be created by a WRQ.
 *   /shared/server_info.txt                  -- one fixed, pre-seeded RAM
 *       file (the constant TFTP_SERVER.1.0), exact name only -- nothing
 *       else may be created under /shared.
 *   /hlos/<path>, /ramdumps/<path>           -- empty RAM namespaces: RRQ
 *       of a name nothing has WRQ'd yet is TFTP_ERROR_ENOENT; WRQ creates
 *       it in RAM, bounded like everything else in ramfs.h.
 *   anything else                            -- TFTP_ERROR_ENOENT.
 *
 * Round 2 review F11: the four RAM namespaces (/readwrite, /hlos,
 * /ramdumps, and the single fixed /shared/server_info.txt entry) must use
 * disjoint ramfs.c keys, or a WRQ under one prefix can silently alias an
 * entry meant for another -- e.g. /readwrite/shared/server_info.txt used
 * to build the bare key "shared/server_info.txt", identical to what
 * /shared/server_info.txt itself resolves to, letting a WRQ there
 * overwrite the "read-only" server_info.txt entry; /readwrite/hlos/x
 * likewise aliased /hlos/x. Every RAM key is now namespace-prefixed
 * ("rw/", "hlos/", "ramdumps/", "shared/server_info.txt" verbatim as its
 * own fixed key) so no two prefixes can ever produce the same key.
 *
 * Two path-safety checks apply to every prefix above, not just the
 * upstream-inherited "no .." check:
 *   - no path component may be ".." (component-wise, not upstream's cruder
 *     "../ substring" scan -- rejects a bare trailing ".." with no
 *     following slash too, which upstream's scan misses since it only
 *     matches the three-character sequence "../").
 *   - the remainder after stripping a prefix may not itself start with
 *     '/'. This one has nothing to do with ".." at all: it exists because
 *     the RO path uses openat(dirfd, remainder, ...), and POSIX openat()
 *     ignores dirfd entirely when its path argument is absolute -- a
 *     request like "/readonly/firmware/image//etc/passwd" strips to
 *     "/etc/passwd", which openat() would happily open outside the
 *     firmware directory if this check weren't here first. Applied
 *     uniformly to the RAM prefixes too, even though ramfs.c has no such
 *     footgun, so one rule covers every prefix.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ramfs.h"
#include "tftp.h"
#include "translate.h"

#define READONLY_FW_PATH	"/readonly/firmware/image/"
#define READONLY_VENDOR_PATH	"/readonly/vendor/firmware/image/"
#define READONLY_FSG_PATH	"/readonly/fsg/"
#define READWRITE_PATH		"/readwrite/"
#define SHARED_SERVER_INFO	"/shared/server_info.txt"
#define HLOS_PATH		"/hlos/"
#define RAMDUMPS_PATH		"/ramdumps/"

#define SERVER_INFO_KEY		"shared/server_info.txt"
#define READWRITE_KEY_PREFIX	"rw/"	/* Round 2 review F11 */
static const char server_info_contents[] = "TFTP_SERVER.1.0";

static int firmware_dirfd = -1;

static int has_dotdot_component(const char *path)
{
	const char *p = path;
	size_t len;

	while (*p) {
		len = strcspn(p, "/");
		if (len == 2 && p[0] == '.' && p[1] == '.')
			return 1;
		p += len;
		if (*p == '/')
			p++;
	}
	return 0;
}

static int starts_with(const char *path, const char *prefix, const char **rest)
{
	size_t len = strlen(prefix);

	if (strncmp(path, prefix, len) != 0)
		return 0;
	*rest = path + len;
	return 1;
}

/* Common validity checks for the remainder of a path after a prefix has
 * been stripped, shared by every branch below (see file header). */
static int remainder_ok(const char *rest)
{
	if (*rest == '\0')
		return 0;	/* e.g. bare "/readwrite/" with nothing after it */
	if (*rest == '/')
		return 0;	/* absolute-escape guard, see file header */
	if (has_dotdot_component(rest))
		return 0;
	if (strlen(rest) > TFTP_MAX_PATH)
		return 0;
	return 1;
}

int translate_init(const char *firmware_dir)
{
	firmware_dirfd = open(firmware_dir, O_RDONLY | O_DIRECTORY);
	return firmware_dirfd < 0 ? -1 : 0;
}

int translate_seed(const char *seed_dir)
{
	static const struct {
		const char *name;
		size_t expect_size;
	} seeds[] = {
		{ "shob.bin",		37282 },
		{ "dhob.bin",		16384 },
		{ "server_check.txt",	5 },
		{ "hob_report.txt",	4757 },
		{ "dhob_report.txt",	138 },
		{ "mot_rfs/imei_sv",	1 },
		{ "datablock/id_00",	2600 },
		{ "datablock/id_01",	2600 },
	};
	char path[4096];
	char key[RAMFS_MAX_NAME];
	struct ram_file *rf;
	size_t i;
	int n;

	for (i = 0; i < sizeof(seeds) / sizeof(seeds[0]); i++) {
		n = snprintf(path, sizeof(path), "%s/%s", seed_dir, seeds[i].name);
		if (n < 0 || (size_t)n >= sizeof(path)) {
			fprintf(stderr, "[TFTP] seed path too long: %s/%s\n",
				seed_dir, seeds[i].name);
			return -1;
		}

		/* Round 2 review F11: seeded under the "rw/" prefix, same as
		 * every other /readwrite entry, so this stays the same
		 * namespace a WRQ/RRQ of /readwrite/<name> resolves to. */
		n = snprintf(key, sizeof(key), READWRITE_KEY_PREFIX "%s", seeds[i].name);
		if (n < 0 || (size_t)n >= sizeof(key)) {
			fprintf(stderr, "[TFTP] seed key too long: %s\n", seeds[i].name);
			return -1;
		}

		if (ramfs_seed_file(key, path) < 0) {
			fprintf(stderr, "[TFTP] failed to seed %s from %s: %s\n",
				key, path, strerror(errno));
			return -1;
		}

		rf = ramfs_lookup(key);
		if (!rf || rf->len != seeds[i].expect_size) {
			fprintf(stderr,
				"[TFTP] seed %s is %zu bytes, expected exactly %zu; "
				"fail-closed per REPORT.md 8.1 point 3\n",
				key, rf ? rf->len : 0, seeds[i].expect_size);
			return -1;
		}
	}

	if (ramfs_seed_bytes(SERVER_INFO_KEY, server_info_contents,
			      sizeof(server_info_contents) - 1) < 0) {
		fprintf(stderr, "[TFTP] failed to seed %s\n", SERVER_INFO_KEY);
		return -1;
	}

	return 0;
}

/* /firmware/image is a single flat directory (confirmed by every other
 * consumer of it in this tree, e.g. tools/servreg-locator/jsn.c's loader);
 * Round 1 review F8 hardens both assumptions this daemon makes about that:
 * a remainder containing '/' is rejected outright (tighter than the
 * generic prefixes' "no leading '/'" check -- this one forbids any '/' at
 * all, so a hypothetical nested symlink inside a subdirectory can never be
 * reached even without O_NOFOLLOW's final-component-only protection), and
 * the opened fd is fstat()-checked to be a regular file (closes the
 * EISDIR "read error" path for a request naming a subdirectory, and is a
 * second, independent guard against anything O_NOFOLLOW doesn't cover). */
static int open_ro_firmware(const char *rest, struct tftp_file *out,
			     int *tftp_err, const char **err_msg)
{
	struct stat st;
	int fd;

	if (strchr(rest, '/') != NULL) {
		*tftp_err = TFTP_ERROR_EACCESS;
		*err_msg = "access violation";
		return -1;
	}

	fd = openat(firmware_dirfd, rest, O_RDONLY | O_NOFOLLOW);
	if (fd < 0) {
		*tftp_err = TFTP_ERROR_ENOENT;
		*err_msg = "file not found";
		return -1;
	}

	if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
		close(fd);
		*tftp_err = TFTP_ERROR_EACCESS;
		*err_msg = "not a regular file";
		return -1;
	}

	out->kind = TFTP_FILE_RO;
	out->fd = fd;
	return 0;
}

static int open_ram(const char *key, enum tftp_open_mode mode, int append,
		     struct tftp_file *out, off_t *ram_start_offset,
		     int *tftp_err, const char **err_msg)
{
	struct ram_file *rf;

	if (mode == TFTP_OPEN_RRQ) {
		rf = ramfs_lookup(key);
		if (!rf) {
			*tftp_err = TFTP_ERROR_ENOENT;
			*err_msg = "file not found";
			return -1;
		}
	} else {
		rf = ramfs_open(key);
		if (!rf) {
			/* Round 1 review F10: ramfs_open() fails for two
			 * distinct reasons (table full vs. name too long);
			 * give each its own message instead of always
			 * blaming capacity. */
			if (errno == ENAMETOOLONG) {
				*tftp_err = TFTP_ERROR_EACCESS;
				*err_msg = "path too long for RAM shadow";
			} else {
				*tftp_err = TFTP_ERROR_UNDEF;
				*err_msg = "RAM shadow exhausted";
			}
			return -1;
		}
		if (append) {
			*ram_start_offset = (off_t)rf->len;
		} else {
			ramfs_truncate(rf);
			*ram_start_offset = 0;
		}
	}

	out->kind = TFTP_FILE_RAM;
	out->rf = rf;
	return 0;
}

/* /shared/server_info.txt is a single fixed name, not a namespace: reject
 * anything else under /shared/ instead of falling through to open_ram()
 * with an attacker/typo-controlled key. */
static int open_shared(const char *path, enum tftp_open_mode mode, int append,
			struct tftp_file *out, off_t *ram_start_offset,
			int *tftp_err, const char **err_msg)
{
	(void)append;

	if (strcmp(path, SHARED_SERVER_INFO) != 0) {
		*tftp_err = TFTP_ERROR_ENOENT;
		*err_msg = "file not found";
		return -1;
	}

	if (mode == TFTP_OPEN_WRQ) {
		*tftp_err = TFTP_ERROR_EACCESS;
		*err_msg = "server_info.txt is read-only";
		return -1;
	}

	return open_ram(SERVER_INFO_KEY, mode, 0, out, ram_start_offset, tftp_err, err_msg);
}

int translate_open(const char *path, enum tftp_open_mode mode, int append,
		    struct tftp_file *out, off_t *ram_start_offset,
		    int *tftp_err, const char **err_msg)
{
	char key[TFTP_MAX_PATH + 16];
	const char *rest;

	*ram_start_offset = 0;

	if (strlen(path) > TFTP_MAX_PATH) {
		*tftp_err = TFTP_ERROR_EACCESS;
		*err_msg = "path too long";
		return -1;
	}

	if (starts_with(path, READONLY_FSG_PATH, &rest)) {
		*tftp_err = TFTP_ERROR_ENOENT;
		*err_msg = "fsg is not served (dangling on stock too)";
		return -1;
	}

	if (starts_with(path, READONLY_FW_PATH, &rest) ||
	    starts_with(path, READONLY_VENDOR_PATH, &rest)) {
		if (!remainder_ok(rest)) {
			*tftp_err = TFTP_ERROR_EACCESS;
			*err_msg = "access violation";
			return -1;
		}
		if (mode != TFTP_OPEN_RRQ) {
			*tftp_err = TFTP_ERROR_EACCESS;
			*err_msg = "firmware image is read-only";
			return -1;
		}
		return open_ro_firmware(rest, out, tftp_err, err_msg);
	}

	if (strcmp(path, SHARED_SERVER_INFO) == 0) {
		return open_shared(path, mode, append, out, ram_start_offset,
				    tftp_err, err_msg);
	}
	if (starts_with(path, "/shared/", &rest)) {
		*tftp_err = TFTP_ERROR_ENOENT;
		*err_msg = "file not found";
		return -1;
	}

	if (starts_with(path, READWRITE_PATH, &rest)) {
		if (!remainder_ok(rest)) {
			*tftp_err = TFTP_ERROR_EACCESS;
			*err_msg = "access violation";
			return -1;
		}
		if (snprintf(key, sizeof(key), READWRITE_KEY_PREFIX "%s", rest) >= (int)sizeof(key)) {
			*tftp_err = TFTP_ERROR_EACCESS;
			*err_msg = "path too long";
			return -1;
		}
		return open_ram(key, mode, append, out, ram_start_offset, tftp_err, err_msg);
	}

	if (starts_with(path, HLOS_PATH, &rest)) {
		if (!remainder_ok(rest)) {
			*tftp_err = TFTP_ERROR_EACCESS;
			*err_msg = "access violation";
			return -1;
		}
		if (snprintf(key, sizeof(key), "hlos/%s", rest) >= (int)sizeof(key)) {
			*tftp_err = TFTP_ERROR_EACCESS;
			*err_msg = "path too long";
			return -1;
		}
		return open_ram(key, mode, append, out, ram_start_offset, tftp_err, err_msg);
	}

	if (starts_with(path, RAMDUMPS_PATH, &rest)) {
		if (!remainder_ok(rest)) {
			*tftp_err = TFTP_ERROR_EACCESS;
			*err_msg = "access violation";
			return -1;
		}
		if (snprintf(key, sizeof(key), "ramdumps/%s", rest) >= (int)sizeof(key)) {
			*tftp_err = TFTP_ERROR_EACCESS;
			*err_msg = "path too long";
			return -1;
		}
		return open_ram(key, mode, append, out, ram_start_offset, tftp_err, err_msg);
	}

	*tftp_err = TFTP_ERROR_ENOENT;
	*err_msg = "file not found";
	return -1;
}

void translate_close(struct tftp_file *f)
{
	if (f->kind == TFTP_FILE_RO && f->fd >= 0) {
		close(f->fd);
		f->fd = -1;
	}
}
