/* qmuxd-lite: bridges stock Alpine qmi-utils (libqmi, unmodified) to the
 * vendor kernel's AF_MSM_IPC QMI transport. Listens on a unix stream socket
 * whose path must contain "qmux_socket" (libqmi's own requirement for
 * picking the QMUX transport, see gps-userspace-handoff.md 2.4) and speaks
 * raw QMUX to any number of concurrent qmicli-style clients; service 0
 * (CTL) is answered locally (2.5), every other service is a thin relay to
 * one AF_MSM_IPC socket per allocated CID (3.4).
 *
 * All protocol logic (framing, TLVs, CTL responses, CID bookkeeping) lives
 * in qmux.c/qmux.h and is unit-tested there without any of the I/O in this
 * file. This file is deliberately just the poll() loop wiring that logic to
 * real sockets.
 *
 * Usage: qmuxd-lite [-v] [-s /run/qmux_socket]
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>

#include "msmipc.h"
#include "qmux.h"

#define QMUXD_DEFAULT_SOCKET "/run/qmux_socket"
#define QMUXD_MAX_CLIENTS    32
#define QMUXD_MAX_CIDS       256

struct client {
	int fd;
	int in_use;
	uint8_t buf[QMUX_MAX_FRAME];
	size_t used;
};

static void close_client(struct client *c);

static struct qmuxd_cid_table g_table;
static struct client g_clients[QMUXD_MAX_CLIENTS];
static int g_verbose;
static volatile sig_atomic_t g_stop;

static void on_sigterm(int sig)
{
	(void)sig;
	g_stop = 1;
}

static int write_all(int fd, const uint8_t *buf, size_t len)
{
	size_t off = 0;

	while (off < len) {
		ssize_t n = send(fd, buf + off, len - off, MSG_NOSIGNAL);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		off += (size_t)n;
	}

	return 0;
}

static void send_qmux(struct client *c, uint8_t flags, uint8_t service,
		       uint8_t client_id, const uint8_t *sdu, size_t sdu_len)
{
	uint8_t frame[QMUX_MAX_FRAME];
	ssize_t n = qmux_frame_build(frame, sizeof(frame), flags, service, client_id, sdu, sdu_len);

	if (n < 0) {
		fprintf(stderr, "qmuxd-lite: dropping oversized response (service %u, %zu bytes)\n",
			service, sdu_len);
		return;
	}

	if (write_all(c->fd, frame, (size_t)n) < 0) {
		fprintf(stderr, "qmuxd-lite: write to client fd %d failed: %s\n", c->fd, strerror(errno));
		close_client(c);
	}
}

static void do_get_version_info(uint16_t txn, uint8_t *out, size_t cap, ssize_t *out_len)
{
	struct qmi_svc_version list[256];
	size_t nsvc = 0;
	int probe;

	probe = socket(AF_MSM_IPC, SOCK_DGRAM, 0);
	if (probe >= 0) {
		int svc;

		for (svc = 1; svc <= 255 && nsvc < 256; svc++) {
			struct msm_ipc_server_info info[1];
			int found = msmipc_lookup(probe, (uint32_t)svc, 0, info, 1);

			if (found > 0) {
				list[nsvc].service = (uint8_t)svc;
				list[nsvc].major = (uint16_t)(info[0].instance & 0xff);
				list[nsvc].minor = 0;
				nsvc++;
			}
		}
		close(probe);
	}

	*out_len = ctl_build_get_version_info_resp(list, nsvc, txn, out, cap);
}

/* Allocate CID (handoff 3.4): look the service up on the modem, open one
 * fresh AF_MSM_IPC socket for this session, and remember its (node,port)
 * as the sendto() target for every future request on this cid. */
static void do_allocate_cid(struct client *c, uint8_t service, uint16_t txn, uint8_t *out, size_t cap, ssize_t *out_len)
{
	struct msm_ipc_server_info info[1];
	int lookup_sock, sock, n;
	uint8_t cid;

	lookup_sock = socket(AF_MSM_IPC, SOCK_DGRAM, 0);
	if (lookup_sock < 0) {
		*out_len = ctl_build_allocate_cid_fail(QMI_CTL_ERR_NOT_SUPPORTED, txn, out, cap);
		return;
	}

	n = msmipc_lookup(lookup_sock, service, 0, info, 1);
	close(lookup_sock);
	if (n <= 0) {
		*out_len = ctl_build_allocate_cid_fail(QMI_CTL_ERR_NOT_SUPPORTED, txn, out, cap);
		return;
	}

	sock = qrtr_open(0);
	if (sock < 0) {
		*out_len = ctl_build_allocate_cid_fail(QMI_CTL_ERR_NOT_SUPPORTED, txn, out, cap);
		return;
	}

	if (cid_alloc(&g_table, service, sock, info[0].node_id, info[0].port_id, c, &cid) != 0) {
		qrtr_close(sock);
		*out_len = ctl_build_allocate_cid_fail(QMI_CTL_ERR_NOT_SUPPORTED, txn, out, cap);
		return;
	}

	if (g_verbose)
		fprintf(stderr, "qmuxd-lite: allocated cid %u for service %u -> node %u port %u\n",
			cid, service, info[0].node_id, info[0].port_id);

	*out_len = ctl_build_allocate_cid_resp(service, cid, txn, out, cap);
}

static void do_release_cid(uint8_t service, uint8_t cid, uint16_t txn, uint8_t *out, size_t cap, ssize_t *out_len)
{
	int sock = cid_get_sock(&g_table, service, cid);

	if (sock < 0 || cid_release(&g_table, service, cid) != 0) {
		*out_len = ctl_build_release_cid_fail(QMI_CTL_ERR_NOT_SUPPORTED, txn, out, cap);
		return;
	}

	qrtr_close(sock);
	*out_len = ctl_build_release_cid_resp(service, cid, txn, out, cap);
}

static void handle_ctl_frame(struct client *c, const uint8_t *sdu, size_t sdu_len)
{
	struct qmi_sdu_header hdr;
	uint8_t resp[1400];
	ssize_t n = -1;

	if (qmi_sdu_parse_header(sdu, sdu_len, QMI_SERVICE_CTL, &hdr) < 0) {
		fprintf(stderr, "qmuxd-lite: malformed CTL request from fd %d, ignoring\n", c->fd);
		return;
	}

	const uint8_t *tlvs = sdu + qmi_sdu_hdr_len(QMI_SERVICE_CTL);
	size_t tlv_len = hdr.tlv_len;

	switch (hdr.msg_id) {
	case QMI_CTL_MSG_SET_INSTANCE_ID:
		n = ctl_build_set_instance_id_resp(hdr.txn, resp, sizeof(resp));
		break;
	case QMI_CTL_MSG_GET_VERSION_INFO:
		do_get_version_info(hdr.txn, resp, sizeof(resp), &n);
		break;
	case QMI_CTL_MSG_ALLOCATE_CID: {
		uint8_t service;

		if (ctl_parse_allocate_cid_req(tlvs, tlv_len, &service) != 0)
			n = ctl_build_allocate_cid_fail(QMI_CTL_ERR_NOT_SUPPORTED, hdr.txn, resp, sizeof(resp));
		else
			do_allocate_cid(c, service, hdr.txn, resp, sizeof(resp), &n);
		break;
	}
	case QMI_CTL_MSG_RELEASE_CID: {
		uint8_t service, cid;

		if (ctl_parse_release_cid_req(tlvs, tlv_len, &service, &cid) != 0)
			n = ctl_build_release_cid_fail(QMI_CTL_ERR_NOT_SUPPORTED, hdr.txn, resp, sizeof(resp));
		else
			do_release_cid(service, cid, hdr.txn, resp, sizeof(resp), &n);
		break;
	}
	case QMI_CTL_MSG_SET_DATA_FORMAT: {
		uint16_t protocol = 0;

		ctl_parse_set_data_format_req(tlvs, tlv_len, &protocol); /* optional TLV */
		n = ctl_build_set_data_format_resp(protocol, hdr.txn, resp, sizeof(resp));
		break;
	}
	case QMI_CTL_MSG_SYNC:
		n = ctl_build_sync_resp(hdr.txn, resp, sizeof(resp));
		break;
	default:
		n = ctl_build_unsupported_resp(hdr.msg_id, hdr.txn, resp, sizeof(resp));
		break;
	}

	if (n > 0)
		send_qmux(c, QMUX_FLAG_S2C, QMI_SERVICE_CTL, 0, resp, (size_t)n);
}

static void handle_service_frame(struct client *c, uint8_t service, uint8_t cid,
				  const uint8_t *sdu, size_t sdu_len)
{
	uint32_t node, port;
	int sock = cid_get_sock(&g_table, service, cid);

	if (sock < 0 || cid_get_target(&g_table, service, cid, &node, &port) != 0) {
		fprintf(stderr, "qmuxd-lite: request for unknown cid %u on service %u, dropping\n", cid, service);
		return;
	}

	/* Whoever last spoke for this cid gets its indications -- lets a
	 * fresh qmicli --client-cid=N attach to a session another process
	 * allocated (handoff 3.4). */
	cid_set_owner(&g_table, service, cid, c);

	if (qrtr_sendto(sock, node, port, sdu, (unsigned int)sdu_len) < 0)
		fprintf(stderr, "qmuxd-lite: sendto cid %u service %u failed: %s\n",
			cid, service, strerror(errno));
}

static void handle_client_frame(struct client *c, const struct qmux_frame *f)
{
	if (g_verbose)
		fprintf(stderr, "qmuxd-lite: fd %d -> service %u cid %u %zu bytes\n",
			c->fd, f->service, f->client, f->sdu_len);

	if (f->flags != QMUX_FLAG_C2S || f->sdu_len == 0 || f->sdu[0] != QMI_REQUEST) {
		fprintf(stderr, "qmuxd-lite: invalid client frame on fd %d\n", c->fd);
		return;
	}

	if (f->service == QMI_SERVICE_CTL)
		handle_ctl_frame(c, f->sdu, f->sdu_len);
	else
		handle_service_frame(c, f->service, f->client, f->sdu, f->sdu_len);
}

static void close_client(struct client *c)
{
	cid_clear_owner_all(&g_table, c);
	close(c->fd);
	c->fd = -1;
	c->in_use = 0;
	c->used = 0;
}

static void on_client_readable(struct client *c)
{
	ssize_t n;

	if (c->used >= sizeof(c->buf)) {
		fprintf(stderr, "qmuxd-lite: fd %d buffer full with no valid frame, dropping\n", c->fd);
		close_client(c);
		return;
	}

	n = read(c->fd, c->buf + c->used, sizeof(c->buf) - c->used);
	if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
		close_client(c);
		return;
	}
	if (n < 0)
		return;

	c->used += (size_t)n;

	for (;;) {
		ssize_t frame_size = qmux_frame_scan(c->buf, c->used);
		struct qmux_frame f;

		if (frame_size == 0)
			break;
		if (frame_size < 0) {
			fprintf(stderr, "qmuxd-lite: malformed QMUX frame on fd %d, disconnecting\n", c->fd);
			close_client(c);
			return;
		}

		if (qmux_frame_parse(c->buf, (size_t)frame_size, &f) == 0)
			handle_client_frame(c, &f);
		if (!c->in_use)
			return;

		c->used -= (size_t)frame_size;
		memmove(c->buf, c->buf + frame_size, c->used);
	}
}

static void on_cid_socket_readable(int sock, uint8_t expected_service, uint8_t expected_cid)
{
	uint8_t buf[QMUX_MAX_FRAME];
	uint8_t service, cid;
	uint32_t node, port;
	int rc;

	if (cid_find_by_sock(&g_table, sock, &service, &cid) != 0 ||
	    service != expected_service || cid != expected_cid ||
	    cid_get_sock(&g_table, service, cid) != sock)
		return; /* raced with a Release CID this same iteration */

	rc = qrtr_recvfrom(sock, buf, sizeof(buf), &node, &port);
	if (rc == QRTR_RECV_RESUME_TX)
		return;
	if (rc < 0) {
		/* Includes -ENETRESET (modem/subsystem restart, handoff 2.1
		 * and 5.6): this session cannot continue, so tear it down --
		 * a client that wants GPS back has to Allocate CID again,
		 * which will re-resolve the (possibly restarted) service. */
		fprintf(stderr, "qmuxd-lite: recvfrom cid %u service %u failed (%s), releasing session\n",
			cid, service, strerror(-rc));
		cid_release(&g_table, service, cid);
		qrtr_close(sock);
		return;
	}

	void *owner = cid_get_owner(&g_table, service, cid);
	if (!owner)
		return; /* no connection currently attached to this cid; drop */

	{
		struct client *c = owner;

		if (g_verbose)
			fprintf(stderr, "qmuxd-lite: service %u cid %u <- %zd bytes, routing to fd %d\n",
				service, cid, (ssize_t)rc, c->fd);
		send_qmux(c, QMUX_FLAG_S2C, service, cid, buf, (size_t)rc);
	}
}

static int make_listen_socket(const char *path)
{
	struct sockaddr_un addr;
	struct stat st;
	int fd;

	if (strlen(path) >= sizeof(addr.sun_path)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	if (!strstr(path, "qmux_socket"))
		fprintf(stderr, "qmuxd-lite: warning: socket path %s does not contain "
				"\"qmux_socket\" -- libqmi will not pick QMUX for it\n", path);

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
	if (fd < 0)
		return -1;

	if (lstat(path, &st) == 0) {
		int probe;
		if (!S_ISSOCK(st.st_mode)) {
			errno = EEXIST;
			return -1;
		}
		probe = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
		if (probe >= 0) {
			int rc;
			memset(&addr, 0, sizeof(addr));
			addr.sun_family = AF_UNIX;
			strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
			rc = connect(probe, (struct sockaddr *)&addr, sizeof(addr));
			close(probe);
			if (rc == 0 || (errno != ECONNREFUSED && errno != ENOENT)) {
				errno = EADDRINUSE;
				return -1;
			}
		}
		if (unlink(path) < 0 && errno != ENOENT)
			return -1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}
	if (chmod(path, 0600) < 0) {
		int e = errno;
		close(fd);
		unlink(path);
		errno = e;
		return -1;
	}
	if (listen(fd, 16) < 0) {
		close(fd);
		return -1;
	}

	return fd;
}

static void shutdown_all(void)
{
	int i;

	for (i = 0; i < g_table.capacity; i++) {
		if (g_table.slots[i].allocated)
			qrtr_close(g_table.slots[i].sock);
	}
	for (i = 0; i < QMUXD_MAX_CLIENTS; i++) {
		if (g_clients[i].in_use)
			close(g_clients[i].fd);
	}
}

int main(int argc, char **argv)
{
	const char *path = QMUXD_DEFAULT_SOCKET;
	int listen_fd;
	int i, opt;

	while ((opt = getopt(argc, argv, "vs:")) != -1) {
		switch (opt) {
		case 'v':
			g_verbose = 1;
			break;
		case 's':
			path = optarg;
			break;
		default:
			fprintf(stderr, "usage: %s [-v] [-s /run/qmux_socket]\n", argv[0]);
			return 2;
		}
	}

	if (cid_table_init(&g_table, QMUXD_MAX_CIDS) != 0) {
		fprintf(stderr, "qmuxd-lite: cid_table_init failed\n");
		return 1;
	}
	for (i = 0; i < QMUXD_MAX_CLIENTS; i++) {
		g_clients[i].fd = -1;
		g_clients[i].in_use = 0;
	}

	if (msmipc_irsc() < 0)
		fprintf(stderr, "qmuxd-lite: warning: msmipc_irsc() failed: %s "
				"(harmless if the separate irsc step already ran)\n", strerror(errno));

	listen_fd = make_listen_socket(path);
	if (listen_fd < 0) {
		fprintf(stderr, "qmuxd-lite: could not create %s: %s\n", path, strerror(errno));
		return 1;
	}

	signal(SIGTERM, on_sigterm);
	signal(SIGINT, on_sigterm);

	fprintf(stderr, "qmuxd-lite: listening on %s\n", path);

	while (!g_stop) {
		struct pollfd fds[1 + QMUXD_MAX_CLIENTS + QMUXD_MAX_CIDS];
		struct client *client_snap[QMUXD_MAX_CLIENTS];
		int cid_fd_snap[QMUXD_MAX_CIDS];
		uint8_t cid_service_snap[QMUXD_MAX_CIDS], cid_id_snap[QMUXD_MAX_CIDS];
		int nfds = 0;
		int listen_idx, client_idx_start, cid_idx_start;
		int rc;

		listen_idx = nfds;
		fds[nfds].fd = listen_fd;
		fds[nfds].events = POLLIN;
		nfds++;

		client_idx_start = nfds;
		for (i = 0; i < QMUXD_MAX_CLIENTS; i++) {
			if (g_clients[i].in_use) {
				client_snap[nfds - client_idx_start] = &g_clients[i];
				fds[nfds].fd = g_clients[i].fd;
				fds[nfds].events = POLLIN;
				nfds++;
			}
		}

		cid_idx_start = nfds;
		for (i = 0; i < g_table.capacity; i++) {
			if (g_table.slots[i].allocated) {
				int ci = nfds - cid_idx_start;
				cid_fd_snap[ci] = g_table.slots[i].sock;
				cid_service_snap[ci] = g_table.slots[i].service;
				cid_id_snap[ci] = g_table.slots[i].cid;
				fds[nfds].fd = g_table.slots[i].sock;
				fds[nfds].events = POLLIN;
				nfds++;
			}
		}

		rc = poll(fds, (nfds_t)nfds, -1);
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "qmuxd-lite: poll failed: %s\n", strerror(errno));
			break;
		}

		if (fds[listen_idx].revents & POLLIN) {
				int cfd = accept4(listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);

			if (cfd >= 0) {
				int slot = -1;

				for (i = 0; i < QMUXD_MAX_CLIENTS; i++) {
					if (!g_clients[i].in_use) {
						slot = i;
						break;
					}
				}
				if (slot < 0) {
					fprintf(stderr, "qmuxd-lite: too many clients, rejecting fd %d\n", cfd);
					close(cfd);
				} else {
						g_clients[slot].fd = cfd;
					g_clients[slot].in_use = 1;
					g_clients[slot].used = 0;
				}
			}
		}

		{
			int count = cid_idx_start - client_idx_start;
			for (i = 0; i < count; i++)
				if (fds[client_idx_start + i].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))
					on_client_readable(client_snap[i]);
		}

		{
			int count = nfds - cid_idx_start;
			for (i = 0; i < count; i++)
				if (fds[cid_idx_start + i].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))
					on_cid_socket_readable(cid_fd_snap[i], cid_service_snap[i], cid_id_snap[i]);
			}
	}

	fprintf(stderr, "qmuxd-lite: exiting, releasing all cids\n");
	shutdown_all();
	close(listen_fd);
	unlink(path);
	cid_table_free(&g_table);

	return 0;
}
