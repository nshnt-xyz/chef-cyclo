/* msmipc: libqrtr-compatible transport on the vendor kernel's AF_MSM_IPC
 * (net/ipc_router) socket family, in place of net/qrtr (absent on this
 * kernel/4.4 tree -- see gps-userspace-handoff.md sections 1-2).
 *
 * qrtr/libqrtr.h declares the qrtr_open/sendto/recvfrom/... API that
 * msmipc.c implements on top of AF_MSM_IPC. This header adds the pieces
 * that have no upstream-qrtr equivalent: building a real
 * struct sockaddr_msm_ipc, and a synchronous service lookup (AF_MSM_IPC
 * resolves services by an immediate ioctl, not by an async NEW_SERVER
 * packet the way QRTR does -- see the note on qrtr_new_lookup() below).
 *
 * Needs -I<repo>/kernel/include/uapi so <linux/msm_ipc.h> resolves to the
 * vendor kernel's verbatim header (do not hand-copy it: sendmsg() rejects
 * any sockaddr_msm_ipc whose layout doesn't match what the kernel expects).
 */
#ifndef CHEF_CYCLO_MSMIPC_H
#define CHEF_CYCLO_MSMIPC_H

#include <stdint.h>
#include <sys/types.h>
#include <linux/msm_ipc.h>

#include "qrtr/libqrtr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fills *out as an MSM_IPC_ADDR_ID address (a specific node:port, used to
 * sendto() a peer discovered via msmipc_lookup() or msmipc_recvfrom()'s
 * sender). Pure/no I/O. */
void msmipc_build_addr_id(struct sockaddr_msm_ipc *out, uint32_t node, uint32_t port);

/* Fills *out as an MSM_IPC_ADDR_NAME address (service:instance, used to
 * bind() a server or as the target of IPC_ROUTER_IOCTL_LOOKUP_SERVER).
 * Pure/no I/O. */
void msmipc_build_addr_name(struct sockaddr_msm_ipc *out, uint32_t service, uint32_t instance);

/* Synchronous service discovery: ioctl(IPC_ROUTER_IOCTL_LOOKUP_SERVER) with
 * lookup_mask=0 (matches every instance). Returns the number of servers
 * found (0..max, written into out[]), or -1/errno (ENODEV if the service
 * isn't registered yet). This is the primitive qmuxd-lite uses to resolve
 * (service) -> (node,port) when a client allocates a CID; unlike upstream
 * QRTR's qrtr_new_lookup(), which just fires off an async NEW_LOOKUP packet
 * and expects the caller to later qrtr_recvfrom() synthesized NEW_SERVER
 * packets, AF_MSM_IPC's lookup ioctl is synchronous and returns the answer
 * immediately -- so msmipc_lookup() is a distinct, non-qrtr-shaped call.
 * qrtr_new_lookup() (see libqrtr.h) is still provided for API parity with
 * anything written against real libqrtr, but only reports success/failure;
 * it does not queue synthesized packets for qrtr_recvfrom() to return (no
 * current caller in this tree needs that -- see msmipc.c). */
int msmipc_lookup(int sock, uint32_t service, uint32_t instance,
		   struct msm_ipc_server_info *out, int max);

#ifdef __cplusplus
}
#endif

#endif
