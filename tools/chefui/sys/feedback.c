/*
 * feedback.c - UI haptics: a short pulse on the vibrator for taps, Back and
 * wake. The sysfs write can wait on a kernel mutex, so a dedicated thread,
 * "ui-feedback", does it; the UI thread only hands it one fixed record over
 * a pipe and never waits.
 *
 * Ownership: the UI thread calls everything here except the worker. The
 * worker reads records, checks the gates and writes the vibrator node; it
 * touches no LVGL or app state and keeps the app signals blocked. Requests
 * while the worker is busy, within 100 ms of the last accepted one, or with
 * haptics or the screen off are dropped, never queued or replayed.
 *
 * Each start gets its own worker state and generations never repeat, so a
 * worker from an earlier start cannot clear the new one's busy flag or write
 * a stale pulse.
 *
 * Safety buzzes of buttond and powerd are separate and untouched. Production
 * has no teardown path (the process exits), so sys_feedback_stop() is for
 * tests.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "chefui.h"
#include "feedback.h"

#define PULSE_MS 10
#define PULSE_EXPIRY_MS 100
#define PULSE_MIN_GAP_MS 100

struct pulse {
	uint64_t generation, expires_ms;
	uint16_t duration_ms;
	uint16_t reserved;
};
_Static_assert(sizeof(struct pulse) <= 4096, "feedback pipe record must fit PIPE_BUF");

/* One per started worker: what the worker and the UI share. Freed by
 * whichever of the two drops the last reference. */
struct fb_worker {
	int rfd;
	char path[256];
	_Atomic bool busy, shutdown;
	_Atomic int last_error;
	_Atomic int refs;	/* 2: the UI and the worker */
};

static struct {
	int write_fd;
	struct fb_worker *cur;		/* UI thread only */
	_Atomic bool started, enabled, screen_allowed;
	_Atomic uint64_t generation;
	_Atomic bool test_signals_blocked;
	int64_t last_request_ms;
	int logged_error;		/* UI thread only */
} fb = { .write_fd = -1 };

#ifdef CHEFUI_TEST
static unsigned test_calls;
static _Atomic unsigned test_live_workers, test_opens;
#endif

static void worker_put(struct fb_worker *w)
{
	if (atomic_fetch_sub(&w->refs, 1) == 1)
		free(w);
}

static int64_t mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static bool write_all(int fd, const char *buf, size_t size)
{
	while (size) {
		ssize_t n = write(fd, buf, size);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return false;
		buf += n;
		size -= (size_t)n;
	}
	return true;
}

/* The gates the worker re-checks right before and after opening the node. */
static bool pulse_allowed(const struct fb_worker *w, const struct pulse *p)
{
	return !atomic_load(&w->shutdown) && atomic_load(&fb.enabled) &&
	       atomic_load(&fb.screen_allowed) && p->generation == atomic_load(&fb.generation) &&
	       (uint64_t)mono_ms() <= p->expires_ms;
}

static void write_pulse(struct fb_worker *w, const struct pulse *p)
{
	char ms[8];
	int n;
	int fd;

#ifdef CHEFUI_TEST
	atomic_fetch_add(&test_opens, 1);
#endif
	fd = open(w->path, O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		atomic_store(&w->last_error, errno);
		return;
	}
	if (pulse_allowed(w, p)) {
		n = snprintf(ms, sizeof(ms), "%u\n", p->duration_ms);
		if (n < 0 || !write_all(fd, ms, (size_t)n))
			atomic_store(&w->last_error, errno ? errno : EIO);
	}
	close(fd);
}

/* arg is this worker's own state, including its read end (a restart in a
 * test must not touch the new worker's) */
static void *feedback_worker(void *arg)
{
	struct fb_worker *w = arg;
	sigset_t current;

	pthread_setname_np(pthread_self(), "ui-feedback");
	pthread_sigmask(SIG_SETMASK, NULL, &current);
	atomic_store(&fb.test_signals_blocked, sigismember(&current, SIGUSR1) == 1);
	for (;;) {
		struct pulse p;
		char *dst = (char *)&p;
		size_t left = sizeof(p);

		while (left) {
			ssize_t n = read(w->rfd, dst, left);

			if (n < 0 && errno == EINTR)
				continue;
			if (n < 0)
				break;
			if (!n)
				goto done;
			dst += n;
			left -= (size_t)n;
		}
		if (left)
			break;
		if (pulse_allowed(w, &p))
			write_pulse(w, &p);
		atomic_store(&w->busy, false);
	}
done:
	close(w->rfd);
#ifdef CHEFUI_TEST
	atomic_fetch_sub(&test_live_workers, 1);
#endif
	worker_put(w);
	return NULL;
}

int sys_feedback_start(const char *sysfs_path, bool enabled)
{
	pthread_t thread;
	struct fb_worker *w;
	int fds[2], e, flags;
	sigset_t all, old;

	if (atomic_load(&fb.started))
		return 0;
	w = calloc(1, sizeof(*w));
	if (!w)
		return -ENOMEM;
	if (pipe2(fds, O_CLOEXEC)) {
		e = errno;
		free(w);
		return -e;
	}
	flags = fcntl(fds[1], F_GETFL);
	if (flags < 0 || fcntl(fds[1], F_SETFL, flags | O_NONBLOCK)) {
		e = errno;
		close(fds[0]);
		close(fds[1]);
		free(w);
		return -e;
	}
	w->rfd = fds[0];
	atomic_store(&w->refs, 2);
	snprintf(w->path, sizeof(w->path), "%s", sysfs_path && *sysfs_path ? sysfs_path :
		 "/sys/class/timed_output/vibrator/enable");
	fb.write_fd = fds[1];
	fb.last_request_ms = 0;
	fb.logged_error = 0;
	atomic_store(&fb.enabled, enabled);
	atomic_store(&fb.screen_allowed, true);
	atomic_fetch_add(&fb.generation, 1);
	/* the worker is created with every signal blocked and keeps them so */
	sigfillset(&all);
	pthread_sigmask(SIG_SETMASK, &all, &old);
	e = pthread_create(&thread, NULL, feedback_worker, w);
	pthread_sigmask(SIG_SETMASK, &old, NULL);
	if (e) {
		close(fds[0]);
		close(fds[1]);
		free(w);
		fb.write_fd = -1;
		return -e;
	}
#ifdef CHEFUI_TEST
	atomic_fetch_add(&test_live_workers, 1);
#endif
	pthread_detach(thread);
	fb.cur = w;
	atomic_store(&fb.started, true);
	return 0;
}

void sys_feedback_enabled(bool enabled)
{
	if (atomic_load(&fb.enabled) == enabled)
		return;
	atomic_store(&fb.enabled, enabled);
	if (!enabled)
		atomic_fetch_add(&fb.generation, 1);
}

void sys_feedback_screen_allowed(bool allowed)
{
	if (atomic_load(&fb.screen_allowed) == allowed)
		return;
	atomic_store(&fb.screen_allowed, allowed);
	if (!allowed)
		atomic_fetch_add(&fb.generation, 1);
}

/* A failure the worker recorded is logged once per distinct errno, from the
 * UI thread at its next interaction. */
static void report_error(void)
{
	int e;

	if (!fb.cur)
		return;
	e = atomic_load(&fb.cur->last_error);
	if (e && e != fb.logged_error) {
		fb.logged_error = e;
		chefui_log("haptics: %s: %s", fb.cur->path, strerror(e));
	}
}

bool sys_feedback_request(void)
{
	struct pulse p;
	ssize_t n;
	int64_t now = mono_ms();
	bool expected = false;

#ifdef CHEFUI_TEST
	test_calls++;
#endif
	report_error();
	if (!fb.cur || !atomic_load(&fb.started) || atomic_load(&fb.cur->shutdown) ||
	    !atomic_load(&fb.enabled) || !atomic_load(&fb.screen_allowed))
		return false;
	if (fb.last_request_ms && now - fb.last_request_ms < PULSE_MIN_GAP_MS)
		return false;
	if (!atomic_compare_exchange_strong(&fb.cur->busy, &expected, true))
		return false;
	p = (struct pulse){ .generation = atomic_load(&fb.generation),
		.expires_ms = (uint64_t)now + PULSE_EXPIRY_MS, .duration_ms = PULSE_MS };
	n = write(fb.write_fd, &p, sizeof(p));
	if (n != (ssize_t)sizeof(p)) {
		atomic_store(&fb.cur->busy, false);
		if (n < 0 && errno == EPIPE)
			atomic_store(&fb.cur->last_error, EPIPE);
		return false;
	}
	fb.last_request_ms = now;
	return true;
}

void sys_feedback_stop(void)
{
	if (!atomic_load(&fb.started))
		return;
	atomic_store(&fb.cur->shutdown, true);
	atomic_store(&fb.enabled, false);
	atomic_store(&fb.screen_allowed, false);
	atomic_fetch_add(&fb.generation, 1);
	close(fb.write_fd);
	fb.write_fd = -1;
	atomic_store(&fb.started, false);
	worker_put(fb.cur);
	fb.cur = NULL;
}

#ifdef CHEFUI_TEST
bool sys_feedback_test_signals_blocked(void)
{
	return atomic_load(&fb.test_signals_blocked);
}

unsigned sys_feedback_test_calls(void)
{
	return test_calls;
}

uint64_t sys_feedback_test_generation(void)
{
	return atomic_load(&fb.generation);
}

bool sys_feedback_test_busy(void)
{
	return fb.cur && atomic_load(&fb.cur->busy);
}

unsigned sys_feedback_test_live_workers(void)
{
	return atomic_load(&test_live_workers);
}

unsigned sys_feedback_test_opens(void)
{
	return atomic_load(&test_opens);
}

void sys_feedback_test_reset_rate(void)
{
	fb.last_request_ms = 0;
}
#endif
