/*
 * linebuf.h - newline framing for the line protocols the state sources
 * read (gps-manager, gpsd). Bytes go in as they arrive; each complete
 * line comes out without its newline. A line longer than the buffer is
 * dropped whole (up to its newline), never cut into pieces.
 */
#ifndef CHEF_LINEBUF_H
#define CHEF_LINEBUF_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#define LINEBUF_SIZE 8192

struct linebuf {
	char buf[LINEBUF_SIZE];
	size_t len;
	bool skipping;		/* inside an over-long line */
	unsigned long dropped;	/* over-long lines dropped */
};

typedef void (*linebuf_cb)(char *line, void *user);

static inline void linebuf_reset(struct linebuf *b)
{
	b->len = 0;
	b->skipping = false;
}

static inline void linebuf_feed(struct linebuf *b, const char *p, size_t n, linebuf_cb cb, void *user)
{
	while (n) {
		const char *nl = memchr(p, '\n', n);
		size_t k = nl ? (size_t)(nl - p) : n;

		if (b->skipping) {
			/* nothing kept until the newline */
		} else if (b->len + k >= sizeof(b->buf)) {
			b->skipping = true;
			b->dropped++;
			b->len = 0;
		} else {
			memcpy(b->buf + b->len, p, k);
			b->len += k;
		}
		if (!nl)
			return;
		if (!b->skipping) {
			if (b->len && b->buf[b->len - 1] == '\r')
				b->len--;
			b->buf[b->len] = 0;
			cb(b->buf, user);
		}
		b->len = 0;
		b->skipping = false;
		p = nl + 1;
		n -= k + 1;
	}
}

#endif /* CHEF_LINEBUF_H */
