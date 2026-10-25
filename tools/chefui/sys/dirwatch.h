/*
 * dirwatch.h - an inotify fd on a short list of directories, for the
 * caller's loop, without LVGL: the one place that makes the fd, adds the
 * watches, drains the events and watches a directory again when it is
 * back. steptrack.h (the step lists) and the shutdown screen's marker
 * watch are built on it.
 *
 * A directory that is missing or went away is tried again by
 * dirwatch_arm(); retry_at says when (DIRWATCH_RETRY_MS after the arm that
 * found it missing, -1 when nothing is missing). No inotify (out of
 * descriptors) is a missing watch too: the fd is made again by the arm.
 */
#ifndef CHEF_DIRWATCH_H
#define CHEF_DIRWATCH_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/inotify.h>

#define DIRWATCH_MAX 8
#define DIRWATCH_RETRY_MS 1000
/* The event bits a watch usually wants; the directory going away is added. */
#define DIRWATCH_DIR_EVENTS (IN_MOVED_FROM | IN_MOVED_TO | IN_CREATE | IN_DELETE)

struct dirwatch_dir {
	const char *path;		/* must outlive the watch */
	uint32_t events;		/* inotify bits */
};

struct dirwatch {
	const struct dirwatch_dir *dir;	/* the caller's list, at most DIRWATCH_MAX */
	int n;
	int fd;				/* read: inotify, -1 none */
	int wd[DIRWATCH_MAX];		/* read */
	int64_t retry_at;		/* read: -1 none */
};

/* Nothing watched yet (fd -1): call dirwatch_arm(). A list over
 * DIRWATCH_MAX aborts (a programming error). */
void dirwatch_init(struct dirwatch *d, const struct dirwatch_dir *dir, int n);
/* Makes the fd when there is none and watches every directory not watched
 * now; sets retry_at from NOW (ms) if one could not be. 0, or the first
 * failure's -errno (callers that retry later ignore it). */
int dirwatch_arm(struct dirwatch *d, int64_t now);
/* The fd readable: drained, and a watch whose directory went away
 * forgotten. True if an event for the entry NAME (any event when NAME is
 * NULL) was read, or the queue overflowed. */
bool dirwatch_drain(struct dirwatch *d, const char *name);
/* The fd closed, nothing watched, no retry. */
void dirwatch_close(struct dirwatch *d);

#endif /* CHEF_DIRWATCH_H */
