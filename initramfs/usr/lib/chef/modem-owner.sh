# Read-only shared modem ownership; never signals/reaps/restarts its owner.
modem_owner_pid() {
    local_lock=${MODEM_OWNER_LOCK:-/run/gps-up.lock}
    [ -e "$local_lock/ready" ] && [ -S "${MODEM_QMUX_SOCKET:-/run/qmux_socket}" ] || return 1
    read -r owner_pid owner_start < "$local_lock/owner" || return 1
    case "$owner_pid:$owner_start" in *[!0-9:]*|:*|*:) return 1 ;; esac
    [ "$owner_pid" -gt 1 ] || return 1
    owner_current=$(awk 'sub(/^.*\) /, "") { if($1 != "Z") print $20 }' "/proc/$owner_pid/stat" 2>/dev/null) || return 1
    [ -n "$owner_current" ] && [ "$owner_current" = "$owner_start" ] || return 1
    kill -0 "$owner_pid" 2>/dev/null || return 1
    printf '%s\n' "$owner_pid"
}
