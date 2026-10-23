# Shell helpers for the bring-up scripts (modem-up, sensors-up, audio-up,
# speaker-test-tone, the sensord service), sourced as
# . "${CHEF_LIB_DIR:-/usr/lib/chef}/sys.sh" (the host tests point
# CHEF_LIB_DIR at rootfs-overlay/usr/lib/chef). Each function reads the
# variable named beside it, which the caller sets (from its own test
# override, defaulting to the real path) before calling it.
#
#   part NAME            prints the device basename (mmcblk0pN) of the
#                        partition whose PARTNAME is NAME, nothing when
#                        none is; reads SYSFS_BLOCK (/sys/block/mmcblk0)
#   server_present HEX   0 while the IPC router lists service 0xHEX;
#                        reads DUMP_SERVERS (msm_ipc_router/dump_servers)
#   find_subsys NAME     prints the subsysN directory whose name node is
#                        NAME, status 1 when none is; reads SUBSYS_DIR
#                        (/sys/bus/msm_subsys/devices)
#   card_ready           0 while the sdm660 sound card is listed; reads
#                        ASOUND_DIR (/proc/asound)
#   child_running PID    0 while PID exists and is not a zombie
#
# Not here: stage 1's and chef-storage's partition lookups (stricter: a
# unique match; stage 1 stays self-contained).

part() {
	for sys_u in "$SYSFS_BLOCK"/*/uevent; do
		grep -q "^PARTNAME=$1\$" "$sys_u" 2>/dev/null && { basename "$(dirname "$sys_u")"; return; }
	done
}

server_present() {
	# dump_servers in this 4.4 tree writes the service in the first column as
	# 0x%08x (0x000000NN, lower case, so "10f" matches 0x0000010f), rather
	# than the shorthand 0xNN used in some newer trees. Fails when the file
	# cannot be read.
	grep -Eq "^0x0*$1[[:space:]]*[|]" "$DUMP_SERVERS" 2>/dev/null
}

find_subsys() {
	# $1: subsystem "name" node value (e.g. "adsp"). Echoes the matching
	# /sys/bus/msm_subsys/devices/subsysN path on success.
	for sys_s in "$SUBSYS_DIR"/subsys*; do
		[ -r "$sys_s/name" ] || continue
		[ "$(cat "$sys_s/name" 2>/dev/null)" = "$1" ] || continue
		echo "$sys_s"
		return 0
	done
	return 1
}

card_ready() {
	# /proc/asound/cards containing the sdm660 machine driver's card name
	# (kernel/sound/soc/msm/sdm660-internal.c:3142, "sdm660-snd-card"; live
	# it reads ` 0 [sdm660sndcard  ]: sdm660-snd-card - sdm660-snd-card`).
	# Never test this file with [ -s ]: procfs reports st_size 0 for it even
	# when it lists the card (live 2026-09-19 run 1: the card was up 1.17 s
	# after boot_adsp and audio-up's wait still timed out at 60 s because of
	# an -s check). grep alone is the test; grep on a missing file fails too.
	grep -q 'sdm660' "$ASOUND_DIR/cards" 2>/dev/null
}

child_running() {
	# kill -0 alone considers an unreaped zombie alive. A zombie is a failed
	# daemon for our purposes, so inspect its proc state before deciding that
	# it is still up. BusyBox's wait -n availability is not assumed.
	[ -r "/proc/$1/stat" ] || return 1
	awk '{ exit ($3 == "Z") }' "/proc/$1/stat"
}
