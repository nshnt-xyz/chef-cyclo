/*
 * linesrv.h - the server side of a chef-cyclo local socket: an AF_UNIX
 * stream listener, a fixed client table, request line framing and
 * tokenizing, and a bounded output queue per client. The protocol on top
 * is the daemon's (requests as plain words, one JSON object with a "type"
 * member per daemon line); this library only frames lines and sends the
 * two envelope errors that belong to no request:
 *
 *   {"type":"error","reason":"line too long"}     a request line of more
 *       than line_max - 1 bytes before its newline; the rest of that line
 *       is discarded up to the next newline
 *   {"type":"error","reason":"too many clients"}  a connection beyond
 *       max_clients, best effort, then it is closed
 *
 * Requests: a line ends at '\n'; one '\r' before it is stripped. Words are
 * split on spaces and tabs; a line with no words is ignored. on_line gets
 * at most LINESRV_MAX_WORDS words. A line with more gets argc
 * LINESRV_MAX_WORDS + 1: argv[0..7] are the first 8 words and argv[8] is
 * the unsplit rest of the line, so no command that takes at most 8 words
 * accepts it by accident and nothing is cut silently.
 *
 * Output: linesrv_send()/linesrv_sendf() must deliver. The line is
 * appended to the client's queue (outq_size bytes) and flushed at once as
 * far as the socket takes it; the rest goes out on POLLOUT. A line that
 * does not fit in the queue drops the client (logged "client output
 * full"): a client that lets a reply overflow is not reading.
 * linesrv_send_lossy() is for lines a slow client may lose: one that does
 * not fit is counted, and {"type":"dropped","count":N} goes out ahead of
 * the next lossy line that fits.
 *
 * Lifetime: on_connect runs when a client is accepted (or adopted),
 * on_close exactly once when it goes, whatever the reason (EOF, socket
 * error, output overflow, linesrv_drop(), linesrv_close_all()). A client
 * may be dropped from inside any callback or send, including while the
 * daemon loops over the table to broadcast: a dropped client's send
 * returns -1 and does nothing, and no further line of it is handled.
 *
 * The daemon keeps its per-client state in its own array indexed by
 * linesrv_slot(c) (0 .. max_clients - 1) and resets that state in
 * on_connect or on_close; the library holds no per-client user pointer.
 *
 * Poll integration: linesrv_pollfds() fills the listen fd and every
 * client fd (POLLIN, plus POLLOUT while the client has queued output)
 * into the daemon's pollfd array; after poll(), linesrv_handle() takes
 * the same span and accepts, reads, frames, calls back, flushes and drops.
 * A span from an earlier linesrv_pollfds() is still safe to hand over
 * after clients came and went in between: entries of a slot that was
 * reused or freed since are skipped.
 */
#ifndef LINESRV_H
#define LINESRV_H

#include <poll.h>
#include <stddef.h>

#define LINESRV_MAX_WORDS 8

struct linesrv;
struct linesrv_client;

struct linesrv_config {
	int max_clients;
	size_t line_max;	/* request line buffer, its newline included */
	size_t outq_size;	/* per-client output queue */
	void (*on_connect)(struct linesrv *srv, struct linesrv_client *c);
	void (*on_line)(struct linesrv *srv, struct linesrv_client *c, int argc, char **argv);
	void (*on_close)(struct linesrv *srv, struct linesrv_client *c);
	void *ctx;		/* for the callbacks: linesrv_ctx() */
	/* NULL = chef_log; tests inject their own */
	void (*log)(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
};

/* The table and its buffers, allocated once; NULL on a bad config or no
 * memory. Nothing listens yet. */
struct linesrv *linesrv_new(const struct linesrv_config *cfg);

/* Listen on PATH: AF_UNIX stream, non-blocking, CLOEXEC, a stale socket
 * file unlinked first, backlog 8. 0 or -errno. */
int linesrv_listen(struct linesrv *srv, const char *path);

/* Put an already connected fd in the table, as if accepted (on_connect
 * runs; with the table full it gets the "too many clients" line and is
 * closed). The fd is made non-blocking. The client, or NULL when it was
 * refused or on_connect dropped it. */
struct linesrv_client *linesrv_adopt(struct linesrv *srv, int fd);

/* Close every client (on_close for each), the listener, unlink its path
 * and free SRV. */
void linesrv_close_all(struct linesrv *srv);

/* For a CLI client: a blocking, CLOEXEC connection to PATH, or -errno. */
int linesrv_connect(const char *path);

int linesrv_max_pollfds(const struct linesrv *srv);	/* 1 + max_clients */
int linesrv_pollfds(struct linesrv *srv, struct pollfd *pfd, int max);
void linesrv_handle(struct linesrv *srv, const struct pollfd *pfd, int n);

/* 0 sent or queued; -1 the client is gone (or was dropped by this send). */
int linesrv_send(struct linesrv_client *c, const char *buf, size_t len);
int linesrv_sendf(struct linesrv_client *c, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));
/* 0 queued, 1 counted as dropped, -1 the client is gone. */
int linesrv_send_lossy(struct linesrv_client *c, const char *buf, size_t len);

void linesrv_drop(struct linesrv_client *c);

void *linesrv_ctx(const struct linesrv *srv);
int linesrv_max_clients(const struct linesrv *srv);
/* The connected client in SLOT, or NULL when the slot is free. */
struct linesrv_client *linesrv_client_at(struct linesrv *srv, int slot);
int linesrv_slot(const struct linesrv_client *c);
int linesrv_fd(const struct linesrv_client *c);	/* -1 once dropped */
struct linesrv *linesrv_of(const struct linesrv_client *c);

#endif /* LINESRV_H */
