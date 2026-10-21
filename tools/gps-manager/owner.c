/*
 * gps-manager's checks on other processes and its own record: the modem
 * owner (modem-up's lock directory, the QMUX socket, the owner's /proc start
 * time), /proc/PID/stat start times, and DIR/state, the record a respawned
 * instance uses to stop the children and release the CID of the one that
 * died. See main.c's header.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "gps-manager.h"

int gm_proc_start(const char *proc, int pid, unsigned long long *start, bool *zombie,
			 char *comm, size_t commlen)
{
	char path[128], buf[512], *p, *q;
	ssize_t n;
	int fd, field;
	char st;

	snprintf(path, sizeof(path), "%s/%d/stat", proc, pid);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return -EIO;
	buf[n] = '\0';
	p = strchr(buf, '(');
	q = strrchr(buf, ')');
	if (!p || !q || q < p || q[1] != ' ')
		return -EINVAL;
	if (comm)
		snprintf(comm, commlen, "%.*s", (int)(q - p - 1), p + 1);
	p = q + 2;	/* field 3: state */
	st = *p;
	for (field = 3; field < 22 && p; field++) {
		p = strchr(p, ' ');
		if (p)
			p++;
	}
	if (!p)
		return -EINVAL;
	*start = strtoull(p, NULL, 10);
	if (zombie)
		*zombie = st == 'Z';
	return 0;
}

bool gm_bridge_present(const struct gm_paths *p)
{
	struct stat sb;

	return stat(p->qmux_sock, &sb) == 0 && S_ISSOCK(sb.st_mode);
}

bool gm_owner_check(const struct gm_paths *p, struct gm_owner *o)
{
	char path[256], buf[64];
	unsigned long long want, have;
	bool zombie;
	int fd, pid;
	ssize_t n;

	memset(o, 0, sizeof(*o));
	snprintf(path, sizeof(path), "%s/ready", p->owner_lock);
	if (access(path, F_OK) != 0 || !gm_bridge_present(p))
		return false;
	snprintf(path, sizeof(path), "%s/owner", p->owner_lock);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return false;
	buf[n] = '\0';
	if (sscanf(buf, "%d %llu", &pid, &want) != 2 || pid <= 1)
		return false;
	if (gm_proc_start(p->proc, pid, &have, &zombie, NULL, 0) || zombie || have != want)
		return false;
	o->pid = pid;
	o->start = have;
	return true;
}

static int gm_child_from_name(const char *s)
{
	int i;

	for (i = 0; i < CH_STEP; i++)
		if (strcmp(s, gm_child_names[i]) == 0)
			return i;
	return -1;
}

void gm_record_parse(struct gm_record *r, const char *text)
{
	const char *p = text;

	memset(r, 0, sizeof(*r));
	r->cid = -1;
	while (p && *p) {
		char name[16];
		int a;
		unsigned long long b;
		int k = sscanf(p, "%15s %d %llu", name, &a, &b);

		if (k >= 2 && strcmp(name, "cid") == 0) {
			if (a >= 1 && a <= 255)
				r->cid = a;
		} else if (k == 3 && a > 1 && strcmp(name, "owner") == 0) {
			r->owner.pid = a;
			r->owner.start = b;
		} else if (k == 3 && a > 1) {
			int c = gm_child_from_name(name);

			if (c >= 0) {
				r->pid[c] = a;
				r->start[c] = b;
			}
		}
		p = strchr(p, '\n');
		if (p)
			p++;
	}
}

int gm_record_format(const struct gm_record *r, char *buf, size_t len)
{
	size_t off = 0;
	int i;

	off += (size_t)snprintf(buf + off, len - off, "cid %d\n", r->cid);
	if (r->owner.pid > 0 && off < len)
		off += (size_t)snprintf(buf + off, len - off, "owner %d %llu\n", r->owner.pid, r->owner.start);
	for (i = 0; i < CH_STEP && off < len; i++)
		if (r->pid[i] > 0)
			off += (size_t)snprintf(buf + off, len - off, "%s %d %llu\n",
						gm_child_names[i], r->pid[i], r->start[i]);
	return off < len ? 0 : -ENOSPC;
}

/* Name each recorded child must still have to be signalled (comm, 15 chars). */
static const char *const gm_child_comm[] = { "qmicli", "nmea-broker", "gpsd" };

bool gm_record_match(const char *proc, const struct gm_record *r, int c)
{
	unsigned long long start;
	bool zombie;
	char comm[32];

	if (r->pid[c] <= 1)
		return false;
	if (gm_proc_start(proc, r->pid[c], &start, &zombie, comm, sizeof(comm)))
		return false;
	return !zombie && start == r->start[c] && strcmp(comm, gm_child_comm[c]) == 0;
}
