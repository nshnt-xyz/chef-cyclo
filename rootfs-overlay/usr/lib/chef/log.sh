# Shell log helper (docs/features/logging.md), sourced by our scripts as
# . "${CHEF_LIB_DIR:-/usr/lib/chef}/log.sh" (the host tests point
# CHEF_LIB_DIR at rootfs-overlay/usr/lib/chef).
#
#   chef_log TAG MESSAGE...
#   chef_log_syslog TAG MESSAGE...   syslogd only: returns 1, writing
#                                    nothing, while it is not up
#
# While syslogd is up: `logger -t TAG -p daemon.info -- MESSAGE`, into its
# RAM ring (logread). Otherwise "TAG: MESSAGE" to kmsg: before init starts
# syslogd (/init, chef-state boot, chef-storage boot), during a respawn,
# and whenever CHEF_LOG_KMSG_ONLY=1, which the shutdown work sets so its
# lines reach pstore. Up means CHEF_LOG_SYSLOGD_PID (default
# /run/syslogd.pid) names a live process whose comm is syslogd: a stale
# pid file after a crash must not swallow lines.
#
# CHEF_LOG_KMSG (default /dev/kmsg) is where the kmsg lines go; a script
# sets it from its own test override (CHEF_STATE_KMSG and the like). A
# kmsg that is neither a character device nor a regular file is skipped.
# CHEF_LOG_LOGGER replaces logger (tests). Never fails.

chef_log_syslogd_up() {
	[ "${CHEF_LOG_KMSG_ONLY:-}" != 1 ] || return 1
	read -r chef_log_pid 2>/dev/null < "${CHEF_LOG_SYSLOGD_PID:-/run/syslogd.pid}" || return 1
	case $chef_log_pid in ''|*[!0-9]*) return 1 ;; esac
	read -r chef_log_comm 2>/dev/null < "/proc/$chef_log_pid/comm" || return 1
	[ "$chef_log_comm" = syslogd ]
}

chef_log_syslog() {
	chef_log_tag=$1
	shift
	chef_log_syslogd_up && "${CHEF_LOG_LOGGER:-logger}" -t "$chef_log_tag" -p daemon.info -- "$*" 2>/dev/null
}

chef_log() {
	chef_log_syslog "$@" && return 0
	shift
	chef_log_k=${CHEF_LOG_KMSG:-/dev/kmsg}
	[ ! -c "$chef_log_k" ] && [ ! -f "$chef_log_k" ] ||
		printf '%s: %s\n' "$chef_log_tag" "$*" 2>/dev/null >> "$chef_log_k" || true
}
