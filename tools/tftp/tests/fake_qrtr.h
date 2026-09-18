/* fake_qrtr: an in-memory stand-in for tools/msmipc.c's qrtr_* transport,
 * used only by tests/test_e2e.c (Round 1 review F9). Same idea as
 * tools/tests/test_msmipc.c's `-Wl,--wrap=recvfrom`, but a full fake
 * rather than a wrap: tftpserv.c is compiled with -DTFTP_TEST_HOOKS and
 * linked against this file's qrtr_open()/qrtr_close()/qrtr_sendto()/
 * qrtr_recvfrom()/qrtr_publish() instead of msmipc.c's, so the daemon's
 * real RRQ/WRQ/OACK/DATA/ACK logic runs against a controllable, in-memory
 * "network" instead of a real (and, on this host, nonexistent) AF_MSM_IPC
 * socket. Every fake socket is non-blocking by construction (an empty
 * queue returns -EAGAIN immediately, never blocks), which is what makes
 * it safe for tftp_service_clients_once() to poll every client
 * unconditionally in a test -- see tftpserv.c's file header for why that
 * would be unsafe against a real, blocking transport.
 */
#ifndef CHEF_CYCLO_TFTP_FAKE_QRTR_H
#define CHEF_CYCLO_TFTP_FAKE_QRTR_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Clears every fake socket, its queues, and the publish/open records.
 * Call between test cases. */
void fake_qrtr_reset(void);

/* Makes `sock`'s next qrtr_recvfrom() return this packet as if it arrived
 * from (node, port). Queued FIFO per socket; a socket that was never
 * qrtr_open()'d may still be injected into (the server is expected to
 * open its own ephemeral sockets, but a test may want to pre-seed the
 * control socket's queue before the daemon has even published it). */
void fake_qrtr_inject(int sock, uint32_t node, uint32_t port,
		       const void *data, size_t len);

/* How many packets qrtr_sendto() has sent on `sock` so far. */
int fake_qrtr_sent_count(int sock);

/* Fetches the i'th (0-based) packet sent on `sock`. Returns its length, or
 * -1 if `index` is out of range. `buf`/`bufsz` may be NULL/0 to just query
 * node/port without copying the payload. */
ssize_t fake_qrtr_sent(int sock, int index, uint32_t *node, uint32_t *port,
			void *buf, size_t bufsz);

/* The socket id most recently returned by qrtr_open() -- lets a test learn
 * which ephemeral socket the server just created for a transfer it can't
 * otherwise name in advance. */
int fake_qrtr_last_opened(void);

/* Records of the single most recent qrtr_publish() call, for asserting the
 * exact service/version/instance a server registered (Round 1 review F1). */
int fake_qrtr_last_publish(uint32_t *service, uint16_t *version, uint16_t *instance);

#ifdef __cplusplus
}
#endif

#endif
