/*
 * chef_log.h - one log call for every chef-cyclo daemon.
 *
 * chef_log() sends "<30>TAG[PID]: MESSAGE" (facility daemon, level info,
 * no timestamp, so syslogd stamps it with its own clock) as one datagram
 * to the syslog socket, /dev/log, and, when echo is on, writes
 * "TAG: MESSAGE\n" to stderr. A message is cut at 320 bytes. The lines
 * end up in busybox syslogd's RAM ring (`logread`, docs/features/logging.md).
 *
 * Not syslog(3), for two reasons. syslog(3) sends blocking: a wedged or
 * full syslogd would stall the caller, and buttond is the power-off path,
 * so this sends with MSG_DONTWAIT | MSG_NOSIGNAL. And syslog(3) drops a
 * line it cannot deliver; this one must not vanish. The socket is opened
 * lazily and kept; on a lost connection (syslogd restarted or not up yet)
 * it reconnects once and resends, and a line that still cannot be sent
 * (no syslogd, a full queue, any other error) goes to /dev/kmsg as
 * "TAG: MESSAGE\n" instead (opened, written and closed per line, so it
 * works before and after /dev is remounted). Boot lines written before
 * syslogd binds, and lines during a syslogd respawn, are in `dmesg`.
 *
 * Thread safe (one mutex around the socket); never call it from a signal
 * handler.
 */
#ifndef CHEF_LOG_H
#define CHEF_LOG_H

#include <stdbool.h>

/* TAG is kept by pointer. Read here from the environment:
 * CHEF_LOG_SOCKET replaces /dev/log (unless chef_log_set_socket() chose a
 * path); CHEF_LOG_KMSG_ONLY=1 sends every line to kmsg, as the shell
 * helper rootfs-overlay/usr/lib/chef/log.sh does: the shutdown work sets
 * it so its lines (and its children's) reach pstore. */
void chef_log_init(const char *tag, bool echo_stderr);
void chef_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Tests only: send to PATH instead of /dev/log or CHEF_LOG_SOCKET, write
 * the fallback to PATH instead of /dev/kmsg (NULL restores either). */
void chef_log_set_socket(const char *path);
void chef_log_set_kmsg(const char *path);

#endif /* CHEF_LOG_H */
