/*
 * dirwatch.c - the inotify directory watches (dirwatch.h). No LVGL.
 */
#define _GNU_SOURCE
#include <stdlib.h>
#include <unistd.h>

#include "dirwatch.h"

#define SELF_EVENTS (IN_DELETE_SELF | IN_MOVE_SELF)

void dirwatch_init(struct dirwatch *d, const struct dirwatch_dir *dir, int n)
{
	int i;

	if (n > DIRWATCH_MAX)
		abort();
	d->dir = dir;
	d->n = n;
	d->fd = -1;		/* never 0: that is somebody's descriptor */
	d->retry_at = -1;
	for (i = 0; i < DIRWATCH_MAX; i++)
		d->wd[i] = -1;
}

void dirwatch_arm(struct dirwatch *d, int64_t now)
{
	bool missing = false;
	int i;

	if (d->fd < 0)
		d->fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	for (i = 0; i < d->n; i++) {
		if (d->fd >= 0 && d->wd[i] < 0)
			d->wd[i] = inotify_add_watch(d->fd, d->dir[i].path, d->dir[i].events | SELF_EVENTS);
		if (d->wd[i] < 0)
			missing = true;
	}
	d->retry_at = d->fd < 0 || missing ? now + DIRWATCH_RETRY_MS : -1;
}

void dirwatch_drain(struct dirwatch *d)
{
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	ssize_t n;

	if (d->fd < 0)
		return;
	for (;;) {
		char *p;

		n = read(d->fd, buf, sizeof(buf));
		if (n <= 0)
			break;
		for (p = buf; p < buf + n; p += sizeof(struct inotify_event) + ((struct inotify_event *)p)->len) {
			const struct inotify_event *ev = (const struct inotify_event *)p;
			int i;

			/* a watched directory that went away: watch it again when it is back */
			if (ev->mask & (SELF_EVENTS | IN_IGNORED))
				for (i = 0; i < d->n; i++)
					if (d->wd[i] == ev->wd)
						d->wd[i] = -1;
		}
	}
}

void dirwatch_close(struct dirwatch *d)
{
	int i;

	if (d->fd >= 0)
		close(d->fd);
	d->fd = -1;
	for (i = 0; i < DIRWATCH_MAX; i++)
		d->wd[i] = -1;
	d->retry_at = -1;
}
