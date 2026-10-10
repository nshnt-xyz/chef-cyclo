# Login shells (SSH, docs/features/ssh.md) get root's session bus like the
# telnet shell, whose environment the inittab telnetd line sets.
: "${XDG_RUNTIME_DIR:=/run/user/0}" "${DBUS_SESSION_BUS_ADDRESS:=unix:path=/run/user/0/bus}"
export XDG_RUNTIME_DIR DBUS_SESSION_BUS_ADDRESS
