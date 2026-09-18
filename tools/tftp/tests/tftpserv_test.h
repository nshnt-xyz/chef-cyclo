/* Declarations for tftpserv.c's TFTP_TEST_HOOKS-only entry points (Round 1
 * review F9). tftpserv.c itself is compiled with -DTFTP_TEST_HOOKS and
 * linked into tests/test-e2e alongside tests/fake_qrtr.c instead of
 * msmipc.c -- see tftpserv.c's file header and tests/fake_qrtr.h.
 */
#ifndef CHEF_CYCLO_TFTP_TFTPSERV_TEST_H
#define CHEF_CYCLO_TFTP_TFTPSERV_TEST_H

#ifdef __cplusplus
extern "C" {
#endif

/* Set directly by a test to simulate time passing (idle-timeout sweeps)
 * without a real clock. */
extern long long tftp_test_clock_ms;

/* Opens the (fake) control socket and publishes service 0x1000 the same
 * way production's run_server() does -- a test asserts the exact
 * qrtr_publish() arguments via fake_qrtr_last_publish() after calling
 * this. Returns the control socket, or -1 on failure. */
int tftp_open_and_publish(void);

/* Reads and dispatches exactly one pending packet on the control socket
 * (RRQ/WRQ), or does nothing if none is pending. */
void service_control(int ctrl);

/* Services every currently-active client socket once, then sweeps idle
 * timeouts. Drives ACK/DATA exchanges and WRQ writes to completion when
 * called repeatedly. */
void tftp_service_clients_once(void);

/* Resets the client table between test cases. */
void tftp_test_reset(void);

/* Number of client slots currently in use. */
int tftp_test_active_clients(void);

#ifdef __cplusplus
}
#endif

#endif
