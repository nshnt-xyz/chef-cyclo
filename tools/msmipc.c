/* msmipc: libqrtr-compatible transport on AF_MSM_IPC. See msmipc.h and
 * gps-userspace-handoff.md sections 1-2 for the protocol background.
 *
 * Deliberate departures from a literal port of linux-msm's lib/qrtr.c,
 * each noted at its call site below:
 *  - qrtr_recvfrom()/qrtr_recv() return QRTR_RECV_RESUME_TX immediately for
 *    a 0-byte RESUME_TX flow-control datagram, and normalize libc receive
 *    failures from -1/errno to the negative-errno convention used by libqrtr
 *    callers. A 0-length "packet" fed to
 *    qmi_decode_header() underflows
 *    (pkt->data_len - sizeof(qmi_header), both unsigned) and misreports a
 *    malformed message; it is simpler and strictly more correct for every
 *    caller if this transport never surfaces it at all.
 *  - qrtr_bye() is a no-op (AF_MSM_IPC has no separate "unbind"; the NAME
 *    binding goes away when the socket is closed by qrtr_close()). It does
 *    NOT close the socket -- do not assume the fd is invalid after calling.
 *  - qrtr_new_lookup() only reports success/failure of the synchronous
 *    IPC_ROUTER_IOCTL_LOOKUP_SERVER; unlike real QRTR it cannot queue a
 *    synthesized NEW_SERVER packet for a later qrtr_recvfrom() to return,
 *    because AF_MSM_IPC's lookup is synchronous and out-of-band (an ioctl,
 *    not a socket message). Use msmipc_lookup() directly when the actual
 *    (node,port) results are needed (that's what qmuxd-lite does).
 */
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include "msmipc.h"

void msmipc_build_addr_id(struct sockaddr_msm_ipc *out, uint32_t node, uint32_t port)
{
	memset(out, 0, sizeof(*out));
	out->family = AF_MSM_IPC;
	out->address.addrtype = MSM_IPC_ADDR_ID;
	out->address.addr.port_addr.node_id = node;
	out->address.addr.port_addr.port_id = port;
}

void msmipc_build_addr_name(struct sockaddr_msm_ipc *out, uint32_t service, uint32_t instance)
{
	memset(out, 0, sizeof(*out));
	out->family = AF_MSM_IPC;
	out->address.addrtype = MSM_IPC_ADDR_NAME;
	out->address.addr.port_name.service = service;
	out->address.addr.port_name.instance = instance;
}

int msmipc_lookup(int sock, uint32_t service, uint32_t instance,
		   struct msm_ipc_server_info *out, int max)
{
	struct server_lookup_args *args;
	size_t sz;
	int rc, n;

	if (max < 0) {
		errno = EINVAL;
		return -1;
	}

	sz = sizeof(*args) + (size_t)max * sizeof(struct msm_ipc_server_info);
	args = calloc(1, sz);
	if (!args)
		return -1;

	args->port_name.service = service;
	args->port_name.instance = instance;
	args->num_entries_in_array = max;
	args->num_entries_found = 0;
	args->lookup_mask = 0; /* matches every instance, handoff 2.1 */

	rc = ioctl(sock, IPC_ROUTER_IOCTL_LOOKUP_SERVER, args);
	if (rc < 0) {
		free(args);
		return -1;
	}

	n = args->num_entries_found;
	if (n > max)
		n = max;
	if (out && n > 0)
		memcpy(out, args->srv_info, (size_t)n * sizeof(*out));

	free(args);
	return n;
}

int msmipc_irsc(void)
{
	struct {
		struct config_sec_rules_args a;
		gid_t g[1];
	} r;
	int fd, rc;

	fd = socket(AF_MSM_IPC, SOCK_DGRAM, 0);
	if (fd < 0)
		return -1;

	memset(&r, 0, sizeof(r));
	r.a.num_group_info = 1;
	r.a.service_id = 16;          /* QMI_LOC_SERVICE, handoff 2.7 */
	r.a.instance_id = 0xffffffff; /* match every instance */
	r.g[0] = 0;

	rc = ioctl(fd, IPC_ROUTER_IOCTL_CONFIG_SEC_RULES, &r);
	close(fd); /* close of the IRSC port is what signals completion, 2.2 */
	return rc;
}

int qrtr_open(int rport)
{
	int sock;

	(void)rport; /* no per-socket local port bind on AF_MSM_IPC */

	sock = socket(AF_MSM_IPC, SOCK_DGRAM, 0);
	if (sock < 0)
		return -1;

	return sock;
}

void qrtr_close(int sock)
{
	close(sock);
}

int qrtr_sendto(int sock, uint32_t node, uint32_t port, const void *data, unsigned int sz)
{
	struct sockaddr_msm_ipc addr;
	int rc;

	msmipc_build_addr_id(&addr, node, port);

	rc = sendto(sock, data, sz, 0, (struct sockaddr *)&addr, sizeof(addr));
	if (rc < 0)
		return -errno;

	return 0;
}

int qrtr_recvfrom(int sock, void *buf, unsigned int bsz, uint32_t *node, uint32_t *port)
{
	struct sockaddr_msm_ipc addr;
	socklen_t sl;
	int rc;

	sl = sizeof(addr);
	rc = recvfrom(sock, buf, bsz, 0, (struct sockaddr *)&addr, &sl);
	if (rc < 0)
		return -errno;
	if (rc == 0)
		return QRTR_RECV_RESUME_TX; /* never block for a follow-up packet */

	if (node)
		*node = addr.address.addr.port_addr.node_id;
	if (port)
		*port = addr.address.addr.port_addr.port_id;

	return rc;
}

int qrtr_recv(int sock, void *buf, unsigned int bsz)
{
	int rc = recv(sock, buf, bsz, 0);

	if (rc < 0)
		return -errno;

	return rc; /* 0 is QRTR_RECV_RESUME_TX */
}

int qrtr_publish(int sock, uint32_t service, uint16_t version, uint16_t instance)
{
	struct sockaddr_msm_ipc addr;
	uint32_t inst = ((uint32_t)instance << 8) | version;

	msmipc_build_addr_name(&addr, service, inst);

	return bind(sock, (struct sockaddr *)&addr, sizeof(addr));
}

int qrtr_bye(int sock, uint32_t service, uint16_t version, uint16_t instance)
{
	(void)sock;
	(void)service;
	(void)version;
	(void)instance;
	return 0; /* see file header: does not close the socket */
}

int qrtr_new_lookup(int sock, uint32_t service, uint16_t version, uint16_t instance)
{
	struct msm_ipc_server_info info[1];
	uint32_t inst = ((uint32_t)instance << 8) | version;
	int n;

	n = msmipc_lookup(sock, service, inst, info, 1);
	if (n < 0)
		return -1;

	return n > 0 ? 0 : -1;
}

int qrtr_poll(int sock, unsigned int ms)
{
	struct pollfd fds;

	fds.fd = sock;
	fds.events = POLLIN;
	fds.revents = 0;

	return poll(&fds, 1, ms);
}

int qrtr_decode(struct qrtr_packet *dest, void *buf, size_t len,
		const struct sockaddr_qrtr *sq)
{
	/* AF_MSM_IPC has no synthetic control port distinct from data
	 * traffic (no BYE/NEW_SERVER/DEL_CLIENT packets arrive this way --
	 * see gps-userspace-handoff.md 2.1); everything a client or server
	 * receives is application data, even a 0-length one (defensive only:
	 * qrtr_recvfrom()/qrtr_recv() never hand this function a len of 0). */
	dest->type = QRTR_TYPE_DATA;
	dest->node = sq->sq_node;
	dest->port = sq->sq_port;
	dest->service = 0;
	dest->instance = 0;
	dest->version = 0;
	dest->data = buf;
	dest->data_len = len;

	return 0;
}
