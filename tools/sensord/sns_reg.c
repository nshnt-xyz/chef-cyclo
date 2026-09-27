#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

#include "sns_reg.h"

#define SNS_REG_MAX_SIZE 0x10000
/* statfs f_type of the RAM filesystems (<linux/magic.h>). */
#define SNS_REG_TMPFS_MAGIC 0x01021994
#define SNS_REG_RAMFS_MAGIC 0x858458f6

static int span_cmp(const void *a, const void *b)
{
	const struct sns_reg_span *x = a, *y = b;

	return (int)x->id - (int)y->id;
}

static int push(struct sns_reg_span **v, size_t *n, size_t *cap, struct sns_reg_span s)
{
	if (*n == *cap) {
		size_t nc = *cap ? *cap * 2 : 128;
		struct sns_reg_span *nv = realloc(*v, nc * sizeof(**v));

		if (!nv)
			return -ENOMEM;
		*v = nv;
		*cap = nc;
	}
	(*v)[(*n)++] = s;
	return 0;
}

void sns_reg_free(struct sns_reg *r)
{
	free(r->data);
	free(r->groups);
	free(r->items);
	memset(r, 0, sizeof(*r));
}

int sns_reg_load_map(struct sns_reg *r, const char *path, int *line)
{
	size_t gcap = 0, icap = 0, i;
	char buf[256];
	FILE *f;
	int ln = 0, rc = 0;

	memset(r, 0, sizeof(*r));
	f = fopen(path, "re");
	if (!f)
		return -errno;
	while (fgets(buf, sizeof(buf), f)) {
		char kind[8];
		unsigned long a, b, c;
		int k;

		ln++;
		if (buf[0] == '#' || buf[0] == '\n')
			continue;
		k = sscanf(buf, "%7s %lu %lu %lu", kind, &a, &b, &c);
		if (k == 2 && strcmp(kind, "size") == 0 && a > 0 && a <= SNS_REG_MAX_SIZE) {
			r->size = a;
		} else if (k == 4 && (strcmp(kind, "group") == 0 || strcmp(kind, "item") == 0) &&
			   a <= 0xffff && c > 0 && c <= (kind[0] == 'g' ? 256u : 8u) &&
			   b + c <= SNS_REG_MAX_SIZE) {
			struct sns_reg_span s = { (uint16_t)a, (uint16_t)c, (uint32_t)b };

			rc = kind[0] == 'g' ? push(&r->groups, &r->ngroups, &gcap, s)
					    : push(&r->items, &r->nitems, &icap, s);
			if (rc)
				break;
		} else {
			rc = -EINVAL;
			break;
		}
	}
	fclose(f);
	if (!rc && (!r->size || !r->ngroups))
		rc = -EINVAL;
	if (!rc) {
		qsort(r->groups, r->ngroups, sizeof(*r->groups), span_cmp);
		qsort(r->items, r->nitems, sizeof(*r->items), span_cmp);
		for (i = 0; !rc && i < r->ngroups; i++)
			if (r->groups[i].offset + r->groups[i].size > r->size ||
			    (i && r->groups[i].id == r->groups[i - 1].id))
				rc = -EINVAL;
		for (i = 0; !rc && i < r->nitems; i++)
			if (r->items[i].offset + r->items[i].size > r->size ||
			    (i && r->items[i].id == r->items[i - 1].id))
				rc = -EINVAL;
	}
	if (rc) {
		if (line)
			*line = ln;
		sns_reg_free(r);
	}
	return rc;
}

int sns_reg_load_data(struct sns_reg *r, const char *path, int allow_short, size_t *got)
{
	size_t n = 0;
	int fd;

	if (!r->size)
		return -EINVAL;
	/* O_RDONLY, always: the source is a copy of device calibration. */
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	free(r->data);
	r->data = calloc(1, r->size + 1);
	if (!r->data) {
		close(fd);
		return -ENOMEM;
	}
	/* Read one byte past the expected size to notice a longer file. */
	while (n < r->size + 1) {
		ssize_t k = read(fd, r->data + n, r->size + 1 - n);

		if (k < 0 && errno == EINTR)
			continue;
		if (k < 0) {
			int err = errno;

			close(fd);
			return -err;
		}
		if (k == 0)
			break;
		n += (size_t)k;
	}
	close(fd);
	if (got)
		*got = n;
	if (n > r->size || (n < r->size && !allow_short))
		return -EMSGSIZE;
	r->data[r->size] = 0;
	return 0;
}

const struct sns_reg_span *sns_reg_find(const struct sns_reg_span *v, size_t n, uint16_t id)
{
	struct sns_reg_span key = { .id = id };

	return bsearch(&key, v, n, sizeof(*v), span_cmp);
}

static const struct sns_reg_span *lookup(struct sns_reg *r, int group, uint16_t id)
{
	return group ? sns_reg_find(r->groups, r->ngroups, id)
		     : sns_reg_find(r->items, r->nitems, id);
}

int sns_reg_read(struct sns_reg *r, int group, uint16_t id, uint8_t *out, size_t outsz)
{
	const struct sns_reg_span *s = lookup(r, group, id);

	if (!s || s->size > outsz || !r->data) {
		r->misses++;
		return -1;
	}
	memcpy(out, r->data + s->offset, s->size);
	r->reads++;
	return s->size;
}

int sns_reg_write(struct sns_reg *r, int group, uint16_t id, const uint8_t *in, size_t len)
{
	const struct sns_reg_span *s = lookup(r, group, id);

	if (!s || !r->data) {
		r->misses++;
		return -ENOENT;
	}
	if (!len || len > s->size)
		return -EINVAL;
	memcpy(r->data + s->offset, in, len);
	r->writes++;
	return 0;
}

static int ram_fs(const struct statfs *sf)
{
	return (unsigned long)sf->f_type == SNS_REG_TMPFS_MAGIC ||
	       (unsigned long)sf->f_type == SNS_REG_RAMFS_MAGIC;
}

int sns_reg_save(const struct sns_reg *r, const char *path)
{
	char tmp[4096];
	struct statfs sf;
	struct stat st;
	size_t n = 0;
	int fd, err = 0;

	if (!r->data || !r->size)
		return -EINVAL;
	if ((size_t)snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= sizeof(tmp))
		return -ENAMETOOLONG;
	if (lstat(path, &st) < 0)
		return -errno;
	if (!S_ISREG(st.st_mode))
		return -EINVAL;
	if (statfs(path, &sf) < 0)
		return -errno;
	/* The whole point of the copy is that persist is never written: only
	 * a RAM filesystem may receive the write-back. */
	if (!ram_fs(&sf))
		return -EXDEV;
	unlink(tmp);
	fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, st.st_mode & 07777);
	if (fd < 0)
		return -errno;
	/* The temp lives in path's directory, which could in theory be on
	 * another filesystem than path (a bind-mounted file): check where it
	 * really is before writing a byte. */
	if (fstatfs(fd, &sf) < 0 || !ram_fs(&sf)) {
		close(fd);
		unlink(tmp);
		return -EXDEV;
	}
	while (n < r->size) {
		ssize_t k = write(fd, r->data + n, r->size - n);

		if (k < 0 && errno == EINTR)
			continue;
		if (k <= 0) {
			err = k < 0 ? errno : EIO;
			break;
		}
		n += (size_t)k;
	}
	if (!err && fsync(fd) < 0)
		err = errno;
	if (close(fd) < 0 && !err)
		err = errno;
	if (!err && rename(tmp, path) < 0)
		err = errno;
	if (err) {
		unlink(tmp);
		return -err;
	}
	return 0;
}
