/* fake_ipc: the msmipc.c/libqrtr API sensord uses (qrtr_open, _close,
 * _sendto, _recvfrom, _publish, msmipc_lookup), on AF_UNIX datagram
 * sockets in a directory, so sensord and tests/fake_smgr.c can talk QMI
 * on a host with no IPC router. Linked in place of ../msmipc.c.
 *
 * $SENSORD_FAKE_IPC names the directory. Every socket binds
 * <dir>/p<port> (node is always 1); qrtr_publish() writes
 * <dir>/s<service>.<instance> containing the port, which
 * msmipc_lookup() reads back with the kernel's matching rule for the
 * lookup_mask 0 that ../msmipc.c always sends: a server matches only if
 * (instance & 0) == requested instance, i.e. only a request for
 * instance 0 finds anything (and then finds every instance).
 * A missing peer socket reports -ENETRESET, the way the router reports
 * a port on a restarted DSP. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "msmipc.h"

#define FAKE_MAX_SOCKS 16

static struct {
	int fd;
	uint32_t port;
	char svc[64];
} socks[FAKE_MAX_SOCKS];
static uint32_t next_port;

static const char *dir(void)
{
	const char *d = getenv("SENSORD_FAKE_IPC");

	return d ? d : "/tmp/sensord-fake-ipc";
}

static int slot_of(int fd)
{
	int i;

	for (i = 0; i < FAKE_MAX_SOCKS; i++)
		if (socks[i].fd == fd && socks[i].port)
			return i;
	return -1;
}

static void port_path(struct sockaddr_un *sa, uint32_t port)
{
	memset(sa, 0, sizeof(*sa));
	sa->sun_family = AF_UNIX;
	snprintf(sa->sun_path, sizeof(sa->sun_path), "%s/p%u", dir(), port);
}

int qrtr_open(int rport)
{
	struct sockaddr_un sa;
	int fd, i;

	(void)rport;
	if (!next_port)
		next_port = ((uint32_t)getpid() & 0xffff) << 8;
	for (i = 0; i < FAKE_MAX_SOCKS; i++)
		if (!socks[i].port)
			break;
	if (i == FAKE_MAX_SOCKS) {
		errno = EMFILE;
		return -1;
	}
	fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	socks[i].fd = fd;
	socks[i].port = ++next_port;
	socks[i].svc[0] = '\0';
	port_path(&sa, socks[i].port);
	unlink(sa.sun_path);
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		int err = errno;

		close(fd);
		socks[i].port = 0;
		errno = err;
		return -1;
	}
	return fd;
}

void qrtr_close(int sock)
{
	struct sockaddr_un sa;
	int i = slot_of(sock);

	if (i >= 0) {
		port_path(&sa, socks[i].port);
		unlink(sa.sun_path);
		if (socks[i].svc[0])
			unlink(socks[i].svc);
		socks[i].port = 0;
	}
	close(sock);
}

int qrtr_publish(int sock, uint32_t service, uint16_t version, uint16_t instance)
{
	char tmp[80];
	FILE *f;
	int i = slot_of(sock);

	if (i < 0) {
		errno = EBADF;
		return -1;
	}
	snprintf(socks[i].svc, sizeof(socks[i].svc), "%s/s%u.%u", dir(), service,
		 ((uint32_t)instance << 8) | version);
	snprintf(tmp, sizeof(tmp), "%s.tmp", socks[i].svc);
	f = fopen(tmp, "w");
	if (!f)
		return -1;
	fprintf(f, "%u\n", socks[i].port);
	fclose(f);
	return rename(tmp, socks[i].svc);
}

int msmipc_lookup(int sock, uint32_t service, uint32_t instance,
		  struct msm_ipc_server_info *out, int max)
{
	char prefix[32];
	struct dirent *e;
	DIR *d;
	int n = 0;

	(void)sock;
	/* kernel: (server instance & lookup_mask) != requested -> skip; the
	 * mask is always 0 here */
	if (instance != 0)
		return 0;
	d = opendir(dir());
	if (!d)
		return -1;
	snprintf(prefix, sizeof(prefix), "s%u.", service);
	while ((e = readdir(d)) && n < max) {
		char path[512];
		unsigned port;
		FILE *f;

		if (strncmp(e->d_name, prefix, strlen(prefix)) || strstr(e->d_name, ".tmp"))
			continue;
		snprintf(path, sizeof(path), "%s/%s", dir(), e->d_name);
		f = fopen(path, "r");
		if (!f)
			continue;
		if (fscanf(f, "%u", &port) == 1) {
			out[n].node_id = 1;
			out[n].port_id = port;
			out[n].service = service;
			out[n].instance = (uint32_t)strtoul(e->d_name + strlen(prefix), NULL, 10);
			n++;
		}
		fclose(f);
	}
	closedir(d);
	return n;
}

int qrtr_sendto(int sock, uint32_t node, uint32_t port, const void *data, unsigned int sz)
{
	struct sockaddr_un sa;

	(void)node;
	port_path(&sa, port);
	if (sendto(sock, data, sz, 0, (struct sockaddr *)&sa, sizeof(sa)) < 0)
		return errno == ENOENT || errno == ECONNREFUSED ? -ENETRESET : -errno;
	return 0;
}

int qrtr_recvfrom(int sock, void *buf, unsigned int bsz, uint32_t *node, uint32_t *port)
{
	struct sockaddr_un sa;
	socklen_t sl = sizeof(sa);
	const char *p;
	ssize_t rc;

	rc = recvfrom(sock, buf, bsz, MSG_DONTWAIT, (struct sockaddr *)&sa, &sl);
	if (rc < 0)
		return -errno;
	if (rc == 0)
		return QRTR_RECV_RESUME_TX;
	p = strrchr(sa.sun_path, '/');
	if (node)
		*node = 1;
	if (port)
		*port = p && p[1] == 'p' ? (uint32_t)strtoul(p + 2, NULL, 10) : 0;
	return (int)rc;
}

int qrtr_decode(struct qrtr_packet *dest, void *buf, size_t len, const struct sockaddr_qrtr *sq)
{
	dest->type = QRTR_TYPE_DATA;
	dest->node = sq->sq_node;
	dest->port = sq->sq_port;
	dest->service = dest->instance = dest->version = 0;
	dest->data = buf;
	dest->data_len = len;
	return 0;
}
