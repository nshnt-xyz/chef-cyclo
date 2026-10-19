/*
 * lineclient.h - the client side of a chef-cyclo local socket (or of
 * gpsd's JSON socket): one connection to an AF_UNIX path or a TCP
 * loopback port, newline framing, and reconnection with backoff. No
 * threads, no timers and no poll loop of its own, no allocation: the
 * caller embeds a struct lineclient, puts lineclient_pollfd() into its
 * own poll() (or chefui's fd watch), hands the result to
 * lineclient_handle() and calls lineclient_tick() at lineclient_deadline().
 * Every call that can act takes the caller's monotonic clock (ms).
 *
 * States: idle (not wanted: lineclient_stop() or never started), waiting
 * (wanted, disconnected, a retry at lineclient_deadline()), connecting
 * (a non-blocking connect, finished on POLLOUT through SO_ERROR) and up
 * (POLLIN).
 *
 * Callbacks, all with the clock of the call that runs them:
 *   on_connected  the connection is up (send the first request here)
 *   on_line       one complete line, its newline gone, one '\r' before it
 *                 stripped, NUL-terminated (LINE[LEN] == 0); writable
 *   on_lost       exactly once for every connection on_connected ran for,
 *                 whatever ended it: EOF, a read error, a failed send,
 *                 lineclient_close() or lineclient_stop()
 * A connect that fails (refused, no such socket, SO_ERROR) calls none of
 * them; it schedules the next attempt. Any callback may close, stop,
 * start or send on its own client: no further line of a closed
 * connection is delivered.
 *
 * Framing: lines up to LINECLIENT_LINE_MAX - 1 bytes; a longer one is
 * dropped whole up to its newline and counted in `dropped`, never cut.
 * At most LINECLIENT_READS reads of LINECLIENT_READ_SIZE bytes per
 * lineclient_handle(), so one busy peer cannot starve the caller's loop.
 *
 * Backoff: a failed connect or a lost connection retries after the
 * current backoff, which then doubles up to backoff_max_ms. The caller
 * calls lineclient_healthy() when it sees a reply that proves the peer
 * works (each client has its own rule), which puts it back to
 * backoff_min_ms; so does lineclient_stop().
 */
#ifndef CHEF_LINECLIENT_H
#define CHEF_LINECLIENT_H

#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LINECLIENT_LINE_MAX 8192
#define LINECLIENT_READS 4
#define LINECLIENT_READ_SIZE 4096
#define LINECLIENT_PATH_MAX 108

struct lineclient;

struct lineclient_config {
	const char *path;	/* AF_UNIX stream socket, or NULL for TCP */
	int port;		/* 127.0.0.1:port when path is NULL */
	int backoff_min_ms, backoff_max_ms;
	void (*on_connected)(struct lineclient *lc, int64_t now);
	void (*on_line)(struct lineclient *lc, char *line, size_t len, int64_t now);
	void (*on_lost)(struct lineclient *lc, int64_t now);
	void *ctx;		/* for the callbacks: lineclient_ctx() */
};

/* Private to lineclient.c; embedded so the caller allocates nothing. */
struct lineclient {
	struct lineclient_config cfg;
	char path[LINECLIENT_PATH_MAX];
	int fd;			/* -1 while disconnected */
	bool wanted, connecting, up;
	int backoff_ms;
	int64_t retry_at;	/* -1: none */
	unsigned long gen;	/* bumped at every close */
	bool skipping;		/* inside an over-long line */
	size_t len;
	unsigned long dropped;	/* over-long lines dropped */
	char buf[LINECLIENT_LINE_MAX];
};

/* Idle until lineclient_start(). CFG->path is copied (at most
 * LINECLIENT_PATH_MAX - 1 bytes; a longer one never connects). */
void lineclient_init(struct lineclient *lc, const struct lineclient_config *cfg);

/* Wanted: connect now when idle; when already waiting for a retry, the
 * retry time stands (the backoff is not skipped); connecting or up:
 * nothing. */
void lineclient_start(struct lineclient *lc, int64_t now);
/* Not wanted: closed (on_lost when it was up), no retry, backoff reset. */
void lineclient_stop(struct lineclient *lc, int64_t now);
/* Drop this connection (the peer said something unusable): closed,
 * on_lost when it was up, and a retry after the backoff when wanted. */
void lineclient_close(struct lineclient *lc, int64_t now);
/* The peer works: the next retry waits backoff_min_ms again. */
void lineclient_healthy(struct lineclient *lc);

/* A short request, sent whole or not at all: 0, or -1 when not up or the
 * send failed (a partial send counts as a broken connection, which is
 * then closed as by lineclient_close()). */
int lineclient_send(struct lineclient *lc, const char *line, int64_t now);

/* The fd to poll and its events (fd -1, events 0 while disconnected). */
struct pollfd lineclient_pollfd(const struct lineclient *lc);
/* After poll(): REVENTS of that fd. */
void lineclient_handle(struct lineclient *lc, short revents, int64_t now);
/* When lineclient_tick() has to run next (monotonic ms), -1 none. */
int64_t lineclient_deadline(const struct lineclient *lc);
/* Runs a retry that is due. */
void lineclient_tick(struct lineclient *lc, int64_t now);

bool lineclient_up(const struct lineclient *lc);
void *lineclient_ctx(const struct lineclient *lc);

#endif /* CHEF_LINECLIENT_H */
