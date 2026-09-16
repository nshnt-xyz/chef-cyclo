#ifndef __UTIL_H__
#define __UTIL_H__

#define MIN(x, y) ((x) < (y) ? (x) : (y))
#define MAX(x, y) ((x) > (y) ? (x) : (y))

void print_hex_dump(const char *prefix, const void *buf, size_t len);

/* Negative errno values that are not fatal to a server's event loop. */
int rmtfs_recv_is_retryable_error(int ret);
/* A normalized IPC-router subsystem reset requires closing and rebinding the
 * RMTFS service before the modem can issue requests again. */
int rmtfs_recv_requires_rebind(int ret);

#endif
