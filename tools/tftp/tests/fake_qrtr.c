/* See fake_qrtr.h for what this replaces and why. */
#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "fake_qrtr.h"

#define MAX_SOCKS	64
#define MAX_QUEUE	32
#define MAX_PKT		4096

struct pkt {
	uint32_t node;
	uint32_t port;
	size_t len;
	uint8_t data[MAX_PKT];
};

struct fake_sock {
	int used;
	struct pkt inq[MAX_QUEUE];
	int inq_head;
	int inq_count;
	struct pkt outq[MAX_QUEUE];
	int outq_count;
};

static struct fake_sock g_socks[MAX_SOCKS];
static int g_next_fd = 1;
static int g_last_opened = -1;

static int g_have_publish;
static uint32_t g_pub_service;
static uint16_t g_pub_version;
static uint16_t g_pub_instance;

void fake_qrtr_reset(void)
{
	memset(g_socks, 0, sizeof(g_socks));
	g_next_fd = 1;
	g_last_opened = -1;
	g_have_publish = 0;
	g_pub_service = 0;
	g_pub_version = 0;
	g_pub_instance = 0;
}

static struct fake_sock *lookup(int sock)
{
	if (sock < 1 || sock > MAX_SOCKS)
		return NULL;
	return &g_socks[sock - 1];
}

void fake_qrtr_inject(int sock, uint32_t node, uint32_t port, const void *data, size_t len)
{
	struct fake_sock *s = lookup(sock);
	struct pkt *p;
	int tail;

	if (!s || s->inq_count >= MAX_QUEUE)
		return;

	/* Allow injecting into a socket a test hasn't formally qrtr_open()'d
	 * yet (e.g. pre-seeding the control socket's queue right after the
	 * daemon publishes it, without a separate "mark it open" step). */
	s->used = 1;

	tail = (s->inq_head + s->inq_count) % MAX_QUEUE;
	p = &s->inq[tail];
	p->node = node;
	p->port = port;
	p->len = len > MAX_PKT ? MAX_PKT : len;
	memcpy(p->data, data, p->len);
	s->inq_count++;
}

int fake_qrtr_sent_count(int sock)
{
	struct fake_sock *s = lookup(sock);

	return s ? s->outq_count : 0;
}

ssize_t fake_qrtr_sent(int sock, int index, uint32_t *node, uint32_t *port,
			void *buf, size_t bufsz)
{
	struct fake_sock *s = lookup(sock);
	struct pkt *p;
	size_t n;

	if (!s || index < 0 || index >= s->outq_count)
		return -1;

	p = &s->outq[index];
	if (node)
		*node = p->node;
	if (port)
		*port = p->port;
	if (buf && bufsz) {
		n = p->len < bufsz ? p->len : bufsz;
		memcpy(buf, p->data, n);
	}
	return (ssize_t)p->len;
}

int fake_qrtr_last_opened(void)
{
	return g_last_opened;
}

int fake_qrtr_last_publish(uint32_t *service, uint16_t *version, uint16_t *instance)
{
	if (!g_have_publish)
		return -1;
	if (service)
		*service = g_pub_service;
	if (version)
		*version = g_pub_version;
	if (instance)
		*instance = g_pub_instance;
	return 0;
}

/* ---- qrtr_* transport API tftpserv.c actually calls ---- */

int qrtr_open(int rport)
{
	int fd;

	(void)rport;

	if (g_next_fd > MAX_SOCKS)
		return -1;

	fd = g_next_fd++;
	g_socks[fd - 1].used = 1;
	g_socks[fd - 1].inq_head = 0;
	g_socks[fd - 1].inq_count = 0;
	g_socks[fd - 1].outq_count = 0;
	g_last_opened = fd;
	return fd;
}

void qrtr_close(int sock)
{
	struct fake_sock *s = lookup(sock);

	if (s)
		s->used = 0;
}

int qrtr_sendto(int sock, uint32_t node, uint32_t port, const void *data, unsigned int sz)
{
	struct fake_sock *s = lookup(sock);
	struct pkt *p;

	if (!s || !s->used)
		return -EBADF;
	if (sz > MAX_PKT)
		return -EMSGSIZE;
	if (s->outq_count >= MAX_QUEUE)
		return -ENOBUFS;

	p = &s->outq[s->outq_count++];
	p->node = node;
	p->port = port;
	p->len = sz;
	memcpy(p->data, data, sz);
	return 0;
}

int qrtr_recvfrom(int sock, void *buf, unsigned int bsz, uint32_t *node, uint32_t *port)
{
	struct fake_sock *s = lookup(sock);
	struct pkt *p;
	size_t n;

	if (!s || !s->used)
		return -EBADF;
	if (s->inq_count == 0)
		return -EAGAIN;

	p = &s->inq[s->inq_head];
	n = p->len < bsz ? p->len : bsz;
	memcpy(buf, p->data, n);
	if (node)
		*node = p->node;
	if (port)
		*port = p->port;

	s->inq_head = (s->inq_head + 1) % MAX_QUEUE;
	s->inq_count--;
	return (int)n;
}

int qrtr_publish(int sock, uint32_t service, uint16_t version, uint16_t instance)
{
	(void)sock;

	g_have_publish = 1;
	g_pub_service = service;
	g_pub_version = version;
	g_pub_instance = instance;
	return 0;
}
