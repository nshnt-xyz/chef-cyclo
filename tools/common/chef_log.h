/*
 * chef_log.h - one log call for every chef-cyclo daemon.
 *
 * chef_log() writes "<tag>: <message>\n" to /dev/kmsg (opened, written and
 * closed per message, so it works before and after /dev is remounted) and,
 * when echo is on, the same line to stderr. A message is cut at 320 bytes.
 * Step 6 of docs/next-steps/standard-interfaces.md swaps the body for
 * syslog(3), so a daemon on this library never writes /dev/kmsg itself.
 */
#ifndef CHEF_LOG_H
#define CHEF_LOG_H

#include <stdbool.h>

void chef_log_init(const char *tag, bool echo_stderr);
void chef_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Tests only: write to PATH instead of /dev/kmsg (NULL restores it). */
void chef_log_set_kmsg(const char *path);

#endif /* CHEF_LOG_H */
