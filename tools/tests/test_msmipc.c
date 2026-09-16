/* Host unit tests for tools/msmipc.c.
 *
 * Most of msmipc.c is a thin wrapper around AF_MSM_IPC socket calls that
 * only exist on the target kernel (this host will get EAFNOSUPPORT from
 * socket(AF_MSM_IPC, ...) -- there is no net/ipc_router here). What *is*
 * verifiable on any Linux host:
 *  - the pure address-builder functions (no I/O at all);
 *  - qrtr_decode(), which is also pure;
 *  - the immediate RESUME_TX return behavior of qrtr_recv()/qrtr_recvfrom(), which
 *    only calls recv()/recvfrom() on whatever fd it's given -- a
 *    AF_UNIX SOCK_DGRAM socketpair exercises the exact same code path
 *    (the do-while-0 retry loop) without needing AF_MSM_IPC. Address
 *    translation itself (reading the kernel's sockaddr_msm_ipc back into
 *    node/port) is NOT exercised by that test, since a unix socket's
 *    peer address has a different shape -- see the comment at
 *    test_qrtr_recvfrom_returns_resume_then_payload().
 *  - that qrtr_open()/msmipc_irsc() fail cleanly (not a crash) when
 *    AF_MSM_IPC isn't available, so the error path itself is covered even
 *    though the success path can only be verified on the phone (owned by
 *    the integration session's live tests).
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>

#include "../msmipc.h"

static int g_failures;
static int g_tests;
static int g_recvfrom_errno;

/* The test binary is linked with --wrap=recvfrom so it can reproduce the
 * libc contract observed on the target: recvfrom() returns -1 and records
 * ENETRESET in errno. All ordinary test receives pass through unchanged. */
ssize_t __real_recvfrom(int sock, void *buf, size_t len, int flags,
			struct sockaddr *addr, socklen_t *addrlen);

ssize_t __wrap_recvfrom(int sock, void *buf, size_t len, int flags,
			struct sockaddr *addr, socklen_t *addrlen)
{
	if (g_recvfrom_errno) {
		errno = g_recvfrom_errno;
		return -1;
	}

	return __real_recvfrom(sock, buf, len, flags, addr, addrlen);
}

#define CHECK(cond) do { \
	g_tests++; \
	if (!(cond)) { \
		g_failures++; \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	} \
} while (0)

static void test_build_addr_id(void)
{
	struct sockaddr_msm_ipc a;

	msmipc_build_addr_id(&a, 0x11, 0x22);
	CHECK(a.family == AF_MSM_IPC);
	CHECK(a.address.addrtype == MSM_IPC_ADDR_ID);
	CHECK(a.address.addr.port_addr.node_id == 0x11);
	CHECK(a.address.addr.port_addr.port_id == 0x22);
}

static void test_build_addr_name(void)
{
	struct sockaddr_msm_ipc a;

	msmipc_build_addr_name(&a, 16, 0xff00);
	CHECK(a.family == AF_MSM_IPC);
	CHECK(a.address.addrtype == MSM_IPC_ADDR_NAME);
	CHECK(a.address.addr.port_name.service == 16);
	CHECK(a.address.addr.port_name.instance == 0xff00);
}

static void test_qrtr_decode(void)
{
	struct sockaddr_qrtr sq = { .sq_family = AF_QIPCRTR, .sq_node = 3, .sq_port = 4 };
	uint8_t buf[4] = { 1, 2, 3, 4 };
	struct qrtr_packet pkt;

	CHECK(qrtr_decode(&pkt, buf, sizeof(buf), &sq) == 0);
	CHECK(pkt.type == QRTR_TYPE_DATA);
	CHECK(pkt.node == 3);
	CHECK(pkt.port == 4);
	CHECK(pkt.data == buf);
	CHECK(pkt.data_len == sizeof(buf));

	/* Defensive: a 0-length packet must decode, not crash (qrtr_recvfrom
	 * never hands this a len of 0 in practice -- see file header -- but
	 * qrtr_decode() itself must not assume that). */
	CHECK(qrtr_decode(&pkt, buf, 0, &sq) == 0);
	CHECK(pkt.data_len == 0);
}

/* recv()/recvfrom() on the receiving end of a SOCK_DGRAM socketpair behave
 * identically to the same calls on an AF_MSM_IPC socket as far as the
 * "does a 0-byte read get treated as a skippable flow-control datagram, not
 * EOF or an error" question goes -- that decision in qrtr_recv()/
 * qrtr_recvfrom() is entirely address-family-agnostic. */
static void test_qrtr_recv_skips_resume_tx(void)
{
	int sv[2];
	char payload[3] = { 'a', 'b', 'c' };
	char buf[16];
	int rc;

	CHECK(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0);

	CHECK(send(sv[0], "", 0, 0) == 0);           /* the RESUME_TX stand-in */
	CHECK(send(sv[0], payload, sizeof(payload), 0) == (ssize_t)sizeof(payload));

	rc = qrtr_recv(sv[1], buf, sizeof(buf));
	CHECK(rc == QRTR_RECV_RESUME_TX);
	rc = qrtr_recv(sv[1], buf, sizeof(buf));
	CHECK(rc == (int)sizeof(payload));
	CHECK(memcmp(buf, payload, sizeof(payload)) == 0);

	close(sv[0]);
	close(sv[1]);
}

static void test_qrtr_recvfrom_returns_resume_then_payload(void)
{
	int sv[2];
	char payload[2] = { 'x', 'y' };
	char buf[16];
	uint32_t node = 0xdeadbeef, port = 0xdeadbeef;
	int rc;

	CHECK(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0);

	CHECK(send(sv[0], "", 0, 0) == 0);
	CHECK(send(sv[0], payload, sizeof(payload), 0) == (ssize_t)sizeof(payload));

	/* Only the length/content and "did it skip the 0-byte datagram"
	 * behavior is meaningful here -- see file header. node/port are
	 * whatever qrtr_recvfrom() read out of a struct sockaddr_un
	 * misinterpreted as struct sockaddr_msm_ipc, whose real value is
	 * asserted separately by test_build_addr_id() against the real
	 * type. We only check the call didn't crash and still returns the
	 * skip-then-real-payload behavior. */
	rc = qrtr_recvfrom(sv[1], buf, sizeof(buf), &node, &port);
	CHECK(rc == QRTR_RECV_RESUME_TX);
	rc = qrtr_recvfrom(sv[1], buf, sizeof(buf), &node, &port);
	CHECK(rc == (int)sizeof(payload));
	CHECK(memcmp(buf, payload, sizeof(payload)) == 0);

	close(sv[0]);
	close(sv[1]);
}

static void test_qrtr_recvfrom_normalizes_enetreset(void)
{
	char buf[16];
	uint32_t node = 0, port = 0;
	int rc;

	g_recvfrom_errno = ENETRESET;
	rc = qrtr_recvfrom(-1, buf, sizeof(buf), &node, &port);
	g_recvfrom_errno = 0;

	CHECK(rc == -ENETRESET);
	CHECK(errno == ENETRESET);
}

/* A SOCK_DGRAM socket has no EOF/peer-closed signal -- recv() with nothing
 * pending blocks forever regardless of the peer, datagram sockets simply
 * don't have the concept qrtr_recv()'s do-while-0 loop could mistake for
 * one. The property actually worth guarding is that the loop only retries
 * on a genuine 0-byte read (RESUME_TX), never spins on a real error: every
 * a non-blocking caller surfaces a stalled peer as -1/EAGAIN, not a hang --
 * reproduce exactly that here. */
static void test_qrtr_recv_propagates_real_error(void)
{
	int sv[2];
	char buf[16];
	int rc;

	CHECK(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0);
	CHECK(fcntl(sv[1], F_SETFL, fcntl(sv[1], F_GETFL) | O_NONBLOCK) == 0);

	rc = qrtr_recv(sv[1], buf, sizeof(buf)); /* nothing ever sent: must time out, not hang */
	CHECK(rc == -EAGAIN || rc == -EWOULDBLOCK);
	CHECK(errno == EAGAIN || errno == EWOULDBLOCK);

	close(sv[0]);
	close(sv[1]);
}

static void test_qrtr_recv_normalizes_eagain(void)
{
	int sv[2];
	char buf[8];
	int rc;

	CHECK(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0);
	CHECK(fcntl(sv[1], F_SETFL, fcntl(sv[1], F_GETFL) | O_NONBLOCK) == 0);
	rc = qrtr_recvfrom(sv[1], buf, sizeof(buf), NULL, NULL);
	CHECK(rc == -EAGAIN || rc == -EWOULDBLOCK);
	close(sv[0]);
	close(sv[1]);
}

static void test_qrtr_poll(void)
{
	int sv[2];
	char byte = 'z';

	CHECK(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0);

	CHECK(qrtr_poll(sv[1], 20) == 0); /* nothing pending: times out, doesn't block forever */

	CHECK(send(sv[0], &byte, 1, 0) == 1);
	CHECK(qrtr_poll(sv[1], 1000) == 1); /* readable immediately */

	close(sv[0]);
	close(sv[1]);
}

/* This host is not the target kernel: AF_MSM_IPC doesn't exist here. The
 * only thing to verify without the phone is that failure is reported
 * cleanly (errno set, no crash) rather than, say, blocking forever or
 * dereferencing something invalid. */
static void test_qrtr_open_fails_cleanly_off_target(void)
{
	int fd = qrtr_open(0);

	CHECK(fd < 0);
	if (fd >= 0)
		close(fd);
}

static void test_msmipc_irsc_fails_cleanly_off_target(void)
{
	CHECK(msmipc_irsc() < 0);
}

int main(void)
{
	test_build_addr_id();
	test_build_addr_name();
	test_qrtr_decode();
	test_qrtr_recv_skips_resume_tx();
	test_qrtr_recvfrom_returns_resume_then_payload();
	test_qrtr_recvfrom_normalizes_enetreset();
	test_qrtr_recv_propagates_real_error();
	test_qrtr_recv_normalizes_eagain();
	test_qrtr_poll();
	test_qrtr_open_fails_cleanly_off_target();
	test_msmipc_irsc_fails_cleanly_off_target();

	fprintf(stderr, "%s: %d/%d checks passed\n", g_failures ? "FAIL" : "PASS", g_tests - g_failures, g_tests);
	return g_failures ? 1 : 0;
}
