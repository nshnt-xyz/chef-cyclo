/*
 * storage_usage.c - bounded, read-only scan of /data/v1/{maps,rides,routes}
 * for the Storage page. Runs only on the settings worker thread (a musl
 * thread with a 128 KB stack, so the inode table lives on the heap).
 *
 * Allocated blocks (st_blocks * 512) are counted, never file contents.
 * Traversal is descriptor-relative with O_NOFOLLOW, stays on the owned
 * mount (fdinfo mnt_id), counts a hard link once (the first category wins)
 * and stops at the bounds below. A category is UNSCANNED, PARTIAL or EXACT;
 * nothing truncated is shown as an exact zero.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include "settings.h"
#include "storage_usage.h"

#define WALK_MAX_DEPTH 32
#define WALK_MAX_ENTRIES 10000
#define WALK_MAX_KEYS 10000
#define WALK_HASH_SIZE 16384
#define WALK_BUDGET_NS 250000000LL

struct inode_key {
	dev_t dev;
	ino_t ino;
	bool used;
};

struct walk {
	struct sys_usage_result *out;
	struct inode_key keys[WALK_HASH_SIZE];
	unsigned entries, keys_used;
	bool stopped;		/* entries, inode keys or time ended the whole scan */
	int64_t deadline_ns;
	unsigned max_depth, max_entries, max_keys;
#ifdef CHEFUI_TEST
	uint64_t mismatch_ino;
	void (*before_open)(const char *name, void *user);
	void *user;
#endif
	int category;
	uint64_t *sums[3];
};

static int64_t mono_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* An entry that could not be read, bounded or followed: the category and the
 * result become estimates, the scan goes on with the siblings. */
static void walk_error(struct walk *w)
{
	w->out->complete = false;
	w->out->cat_state[w->category] = USAGE_CAT_PARTIAL;
}

static bool budget_ok(struct walk *w)
{
	if (mono_ns() >= w->deadline_ns || w->entries >= w->max_entries ||
	    w->keys_used >= w->max_keys) {
		walk_error(w);
		w->stopped = true;
		return false;
	}
	return true;
}

static bool fd_mount_id(int fd, uint64_t *id)
{
	char path[64], buf[257], *line, *end;
	int n = snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", fd);
	int f, used = 0;
	unsigned long long v;

	if (n < 0 || (size_t)n >= sizeof(path))
		return false;
	f = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	if (f < 0)
		return false;
	while (used < (int)sizeof(buf) - 1) {
		ssize_t got = read(f, buf + used, sizeof(buf) - 1 - (size_t)used);

		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			break;
		used += (int)got;
	}
	close(f);
	buf[used] = 0;
	line = strstr(buf, "mnt_id:");
	if (!line)
		return false;
	line += 7;
	while (*line == ' ' || *line == '\t')
		line++;
	if (*line < '0' || *line > '9')
		return false;
	errno = 0;
	v = strtoull(line, &end, 10);
	if (errno || end == line || (*end != '\n' && *end != 0))
		return false;
	*id = v;
	return true;
}

static bool walk_mount_id(struct walk *w, int fd, uint64_t *id)
{
#ifdef CHEFUI_TEST
	struct stat st;
#endif

	if (!fd_mount_id(fd, id))
		return false;
#ifdef CHEFUI_TEST
	if (w->mismatch_ino && !fstat(fd, &st) && (uint64_t)st.st_ino == w->mismatch_ino &&
	    *id < UINT64_MAX)
		(*id)++;
#else
	(void)w;
#endif
	return true;
}

/* false: already counted (or the table is full) */
static bool dedup_add(struct walk *w, dev_t dev, ino_t ino)
{
	uint64_t x = (uint64_t)dev * 11400714819323198485ull ^ (uint64_t)ino;
	unsigned slot = (unsigned)(x & (WALK_HASH_SIZE - 1));

	for (unsigned n = 0; n < WALK_HASH_SIZE; n++) {
		struct inode_key *k = &w->keys[(slot + n) & (WALK_HASH_SIZE - 1)];

		if (!k->used) {
			if (w->keys_used >= w->max_keys)
				return false;
			*k = (struct inode_key){ .dev = dev, .ino = ino, .used = true };
			w->keys_used++;
			return true;
		}
		if (k->dev == dev && k->ino == ino)
			return false;
	}
	return false;
}

static bool add_allocated(struct walk *w, const struct stat *st)
{
	uint64_t blocks, *sum = w->sums[w->category];

	if (st->st_blocks < 0 || (uint64_t)st->st_blocks > UINT64_MAX / 512) {
		walk_error(w);
		return false;
	}
	blocks = (uint64_t)st->st_blocks * 512;
	if (*sum > UINT64_MAX - blocks) {
		*sum = UINT64_MAX;
		walk_error(w);
		return false;
	}
	*sum += blocks;
	return true;
}

/* false only when a bound stopped the whole scan */
static bool scan_dir(struct walk *w, int dirfd, uint64_t expected_mount, unsigned depth)
{
	DIR *dir;
	struct dirent *de;
	int iterfd = dup(dirfd);

	if (iterfd < 0) {
		walk_error(w);
		return true;
	}
	dir = fdopendir(iterfd);
	if (!dir) {
		close(iterfd);
		walk_error(w);
		return true;
	}
	for (;;) {
		int pathfd = -1, childfd = -1;
		struct stat st, verify;
		uint64_t mnt;

		errno = 0;
		de = readdir(dir);
		if (!de) {
			if (errno)
				walk_error(w);
			break;
		}
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		if (!budget_ok(w)) {
			closedir(dir);
			return false;
		}
		w->entries++;
#ifdef CHEFUI_TEST
		if (w->before_open)
			w->before_open(de->d_name, w->user);
#endif
		pathfd = openat(dirfd, de->d_name, O_PATH | O_NOFOLLOW | O_CLOEXEC);
		if (pathfd < 0 || fstat(pathfd, &st)) {
			/* removed between readdir and open: gone, not unreadable */
			bool gone = pathfd < 0 && errno == ENOENT;

			if (pathfd >= 0)
				close(pathfd);
			if (!gone)
				walk_error(w);
			continue;
		}
		if (!walk_mount_id(w, pathfd, &mnt)) {
			close(pathfd);
			walk_error(w);
			continue;
		}
		if (mnt != expected_mount) {
			close(pathfd);
			walk_error(w);
			continue;
		}
		if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
			close(pathfd);
			continue;
		}
		if (dedup_add(w, st.st_dev, st.st_ino))
			(void)add_allocated(w, &st);
		else if (w->keys_used >= w->max_keys)
			walk_error(w);
		if (!S_ISDIR(st.st_mode)) {
			close(pathfd);
			continue;
		}
		if (depth >= w->max_depth) {
			/* too deep: this directory's contents are not counted */
			close(pathfd);
			walk_error(w);
			continue;
		}
		childfd = openat(dirfd, de->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if (childfd < 0 || fstat(childfd, &verify) || verify.st_dev != st.st_dev ||
		    verify.st_ino != st.st_ino || !walk_mount_id(w, childfd, &mnt) ||
		    mnt != expected_mount) {
			if (childfd >= 0)
				close(childfd);
			close(pathfd);
			walk_error(w);
			continue;
		}
		close(pathfd);
		if (!scan_dir(w, childfd, expected_mount, depth + 1)) {
			close(childfd);
			closedir(dir);
			return false;
		}
		close(childfd);
	}
	closedir(dir);
	return true;
}

static bool read_marker(int rootfd)
{
	char b[64];
	size_t used = 0;
	struct stat st;
	int fd = openat(rootfd, "chef-layout", O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);

	if (fd < 0)
		return false;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size >= (off_t)sizeof(b)) {
		close(fd);
		return false;
	}
	while (used < sizeof(b)) {
		ssize_t n = read(fd, b + used, sizeof(b) - used);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		used += (size_t)n;
	}
	close(fd);
	while (used && b[used - 1] == '\n')
		used--;
	return used == 18 && !memcmp(b, "chef-cyclo-data-v1", 18);
}

/* total and free bytes of a statvfs, checked; false on overflow */
static bool capacity(const struct statvfs *v, uint64_t *total, uint64_t *avail)
{
	if (!v->f_frsize || (uint64_t)v->f_blocks > UINT64_MAX / v->f_frsize ||
	    (uint64_t)v->f_bavail > UINT64_MAX / v->f_frsize)
		return false;
	*total = (uint64_t)v->f_blocks * v->f_frsize;
	*avail = (uint64_t)v->f_bavail * v->f_frsize;
	return true;
}

static void usage_scan(const char *data, bool (*mountpoint)(const char *),
		       const struct sys_usage_limits *lim, struct sys_usage_result *out)
{
	static const char *const names[] = { "maps", "rides", "routes" };
	struct sys_usage_result r = { .complete = true };
	/* on the heap: the table is ~400 KB and the worker's stack 128 KB */
	struct walk *wp;
	struct stat root_st, end_st, path_st;
	struct statvfs v, endv;
	uint64_t root_mount, v1_mount, end_mount, tot, avail;
	uint64_t *sums[3] = { &r.maps_bytes, &r.rides_bytes, &r.routes_bytes };
	int rootfd = -1, v1fd = -1;
	int64_t start = mono_ns(), end;
	bool (*is_mount)(const char *) = mountpoint ? mountpoint : ui_mountpoint;

	if (!data || !out)
		return;
	wp = calloc(1, sizeof(*wp));
	if (!wp) {
		memset(out, 0, sizeof(*out));	/* unknown, not complete */
		out->sampled_mono_ms = mono_ns() / 1000000;
		return;
	}
	wp->out = &r;
	for (int i = 0; i < 3; i++)
		wp->sums[i] = sums[i];
	wp->max_depth = lim->max_depth;
	wp->max_entries = lim->max_entries;
	wp->max_keys = lim->max_keys;
#ifdef CHEFUI_TEST
	wp->mismatch_ino = lim->mismatch_ino;
	wp->before_open = lim->before_open;
	wp->user = lim->user;
#endif
	wp->deadline_ns = start + lim->budget_ns;
	rootfd = open(data, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (rootfd < 0 || fstat(rootfd, &root_st) || !S_ISDIR(root_st.st_mode) ||
	    !is_mount(data) || !read_marker(rootfd) || !fd_mount_id(rootfd, &root_mount) ||
	    fstatvfs(rootfd, &v))
		goto done;
	r.ours = r.known = true;
	r.root_dev = (uint64_t)root_st.st_dev;
	r.root_ino = (uint64_t)root_st.st_ino;
	r.mount_id = root_mount;
	if (!capacity(&v, &r.total_bytes, &r.free_bytes)) {
		r.complete = false;
		goto done;
	}
	/* O_PATH: v1 is root 0711, so chefui may search it but not read it. The
	 * scan never lists v1, it only openat()s the category names under it,
	 * and fd_mount_id's fdinfo has mnt_id for an O_PATH descriptor too. */
	v1fd = openat(rootfd, "v1", O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (v1fd < 0) {
		if (errno != ENOENT)
			r.complete = false;
		else
			for (int c = 0; c < 3; c++)
				r.cat_state[c] = USAGE_CAT_EXACT;	/* no v1: all zero */
		goto finish_scan;
	}
	if (!fd_mount_id(v1fd, &v1_mount) || v1_mount != root_mount) {
		r.complete = false;
		goto finish_scan;
	}
	for (int c = 0; c < 3; c++) {
		struct stat st;
		uint64_t mnt;
		int catfd;

		wp->category = c;
		if (!budget_ok(wp)) {
			r.cat_state[c] = USAGE_CAT_UNSCANNED;	/* never started */
			break;
		}
		catfd = openat(v1fd, names[c], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if (catfd < 0) {
			if (errno == ENOENT)
				r.cat_state[c] = USAGE_CAT_EXACT;
			else
				r.complete = false;
			continue;
		}
		if (fstat(catfd, &st) || !fd_mount_id(catfd, &mnt) || mnt != root_mount ||
		    !dedup_add(wp, st.st_dev, st.st_ino)) {
			close(catfd);
			r.complete = false;
			continue;
		}
		r.cat_state[c] = USAGE_CAT_EXACT;	/* until an entry says otherwise */
		(void)add_allocated(wp, &st);
		(void)scan_dir(wp, catfd, root_mount, 1);
		close(catfd);
		if (wp->stopped)
			break;
	}
finish_scan:
	if (fstat(rootfd, &end_st) || end_st.st_dev != root_st.st_dev ||
	    end_st.st_ino != root_st.st_ino || lstat(data, &path_st) || !S_ISDIR(path_st.st_mode) ||
	    path_st.st_dev != root_st.st_dev || path_st.st_ino != root_st.st_ino ||
	    !fd_mount_id(rootfd, &end_mount) || end_mount != root_mount || !read_marker(rootfd) ||
	    !is_mount(data)) {
		r.known = r.ours = false;
		r.complete = false;
	} else if (fstatvfs(rootfd, &endv) || !capacity(&endv, &tot, &avail)) {
		r.complete = false;	/* keep the start values */
	} else {
		/* the newest sample: the page shows these, and a change during the
		 * walk makes the categories an estimate */
		if (endv.f_blocks != v.f_blocks || endv.f_bavail != v.f_bavail ||
		    endv.f_frsize != v.f_frsize)
			r.complete = false;
		r.total_bytes = tot;
		r.free_bytes = avail;
	}
	if (r.free_bytes > r.total_bytes) {
		r.free_bytes = r.total_bytes;
		r.complete = false;
	}
	if (r.known) {
		uint64_t used = r.total_bytes - r.free_bytes;
		uint64_t categories = r.maps_bytes;

		if (categories <= UINT64_MAX - r.rides_bytes)
			categories += r.rides_bytes;
		else
			categories = UINT64_MAX;
		if (categories <= UINT64_MAX - r.routes_bytes)
			categories += r.routes_bytes;
		else
			categories = UINT64_MAX;
		if (categories > used) {
			r.other_bytes = 0;
			r.complete = false;
		} else {
			r.other_bytes = used - categories;
		}
	}
done:
	if (rootfd >= 0)
		close(rootfd);
	if (v1fd >= 0)
		close(v1fd);
	end = mono_ns();
	r.sampled_mono_ms = end / 1000000;
	if (end >= wp->deadline_ns)
		r.complete = false;
	free(wp);
	*out = r;
}

void sys_storage_usage_scan(const char *data, bool (*mountpoint)(const char *),
			    struct sys_usage_result *out)
{
	static const struct sys_usage_limits limits = {
		.max_depth = WALK_MAX_DEPTH, .max_entries = WALK_MAX_ENTRIES,
		.max_keys = WALK_MAX_KEYS, .budget_ns = WALK_BUDGET_NS,
	};

	usage_scan(data, mountpoint, &limits, out);
}

#ifdef CHEFUI_TEST
void sys_storage_usage_scan_limited(const char *data, bool (*mountpoint)(const char *),
				    const struct sys_usage_limits *limits,
				    struct sys_usage_result *out)
{
	if (limits)
		usage_scan(data, mountpoint, limits, out);
}
#endif
