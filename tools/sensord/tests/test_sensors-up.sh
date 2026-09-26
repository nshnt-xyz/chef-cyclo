#!/bin/sh
# Host test for initramfs/usr/bin/sensors-up's persist handling and
# helpers. SENSORS_UP_SELFTEST=1 makes the script return after defining
# its functions. mount/umount/cp are replaced by shell functions (they
# take precedence over PATH here, on the host's sh), so the test sees
# exactly what would be run against persist without mounting anything.
# The live ADSP/sensord part is covered by the live run only.
set -u
cd "$(dirname "$0")/../../.."
SCRIPT=initramfs/usr/bin/sensors-up
[ -r "$SCRIPT" ] || { echo "cannot find $SCRIPT" >&2; exit 1; }

T=$(mktemp -d /tmp/sensors-up-test.XXXXXX)
trap 'rm -rf "$T"' EXIT
FAILS=0

bad() {
	FAILS=$((FAILS + 1))
	echo "FAIL: $*" >&2
}

eq() {
	[ "$2" = "$3" ] || bad "$1: expected '$2', got '$3'"
}

# --- static checks on the script text -----------------------------------
# Every mount in the script is read-only + noload, nothing remounts or
# writes the persist device, and none of the forbidden nodes appear.
mounts=$(grep -E '^[[:space:]]*mount |[;&|][[:space:]]*mount ' "$SCRIPT" | grep -v 'debugfs')
[ "$(echo "$mounts" | grep -c .)" -eq 1 ] || bad "expected exactly one non-debugfs mount: $mounts"
echo "$mounts" | grep -q -- '-o ro,noload "/dev/$persist"' || bad "persist mount is not ro,noload: $mounts"
grep -Eq '^[^#]*(remount|[ ,]rw[ ,]|-o rw|--setrw)' "$SCRIPT" && bad "script mentions a read-write mount or setrw"
# setro comes before the (only) persist mount in the text too
setro_line=$(grep -n -- 'blockdev --setro "/dev/$persist"' "$SCRIPT" | cut -d: -f1)
mount_line=$(grep -n -- '-o ro,noload "/dev/$persist"' "$SCRIPT" | cut -d: -f1)
[ -n "$setro_line" ] && [ -n "$mount_line" ] && [ "$setro_line" -lt "$mount_line" ] \
	|| bad "blockdev --setro does not precede the persist mount"
grep -Eq '^[^#]*(boot_slpi|restart_level|adsprpc|msm_subsys/.*/restart|> */dev/mmcblk)' "$SCRIPT" \
	&& bad "script touches a forbidden node"

# --- fixture ---------------------------------------------------------------
export SYSFS_BLOCK="$T/sys"
mkdir -p "$SYSFS_BLOCK/mmcblk0p38" "$SYSFS_BLOCK/mmcblk0p6"
printf 'PARTNAME=persist\n' > "$SYSFS_BLOCK/mmcblk0p38/uevent"
printf 'PARTNAME=modem_a\n' > "$SYSFS_BLOCK/mmcblk0p6/uevent"
export PROC_MOUNTS="$T/mounts"
: > "$PROC_MOUNTS"
export RUN_DIR="$T/run"
export REG_MAP="$T/sns_reg.map"
printf 'size 16\ngroup 1 0 16\n' > "$REG_MAP"
FAKE_PERSIST="$T/persist-content"
mkdir -p "$FAKE_PERSIST/sensors"
printf '0123456789abcdef' > "$FAKE_PERSIST/sensors/sns.reg"
CALLS="$T/calls"
: > "$CALLS"

SENSORS_UP_SELFTEST=1 . "./$SCRIPT"

MOUNT_FAIL=0
UMOUNT_FAIL=0
SETRO_FAIL=0
GETRO=1
blockdev() {
	echo "blockdev $*" >> "$CALLS"
	case "$1" in
	--setro) [ "$SETRO_FAIL" -eq 0 ] ;;
	--getro) echo "$GETRO" ;;
	*) return 1 ;;
	esac
}
mount() {
	echo "mount $*" >> "$CALLS"
	[ "$MOUNT_FAIL" -eq 1 ] && return 1
	eval "mnt=\${$#}"
	mkdir -p "$mnt"
	cp -r "$FAKE_PERSIST/." "$mnt/"
}
umount() {
	echo "umount $*" >> "$CALLS"
	[ "$UMOUNT_FAIL" -eq 1 ] && return 1
	rm -rf "${1:?}"/*
}

eq "part persist" mmcblk0p38 "$(part persist)"
eq "map_size" 16 "$(map_size)"

# --- normal path: ro,noload mount, copy, unmount, 0400 copy -------------
copy_registry > "$T/out" 2>&1 || bad "copy_registry failed: $(cat "$T/out")"
eq "setro first" "blockdev --setro /dev/mmcblk0p38" "$(sed -n 1p "$CALLS")"
eq "getro second" "blockdev --getro /dev/mmcblk0p38" "$(sed -n 2p "$CALLS")"
eq "mount args" "mount -t ext4 -o ro,noload /dev/mmcblk0p38 $RUN_DIR/persist-ro" "$(sed -n 3p "$CALLS")"
eq "umount args" "umount $RUN_DIR/persist-ro" "$(sed -n 4p "$CALLS")"
eq "calls" 4 "$(grep -c . "$CALLS")"
grep -q "set read-only at the block layer (blockdev --getro = 1)" "$T/out" || bad "getro proof line missing"
eq "copy content" 0123456789abcdef "$(cat "$RUN_DIR/sns.reg")"
eq "copy mode" "-r--------" "$(ls -l "$RUN_DIR/sns.reg" | cut -c1-10)"
eq "mounted flag" 0 "$PERSIST_MOUNTED"
[ -e "$RUN_DIR/persist-ro" ] && bad "mount point left behind"
[ -e "$RUN_DIR/sns.reg.tmp" ] && bad "temp copy left behind"

# --- persist already mounted: copy from there, no mount/umount ---------
: > "$CALLS"
rm -f "$RUN_DIR/sns.reg"
mkdir -p "$T/already/sensors"
printf 'fedcba9876543210' > "$T/already/sensors/sns.reg"
printf '/dev/mmcblk0p38 %s ext4 ro,relatime 0 0\n' "$T/already" > "$PROC_MOUNTS"
copy_registry > "$T/out" 2>&1 || bad "copy from existing mount failed"
eq "no mount/blockdev when already mounted" 0 "$(grep -c . "$CALLS")"
eq "copied from existing mount" fedcba9876543210 "$(cat "$RUN_DIR/sns.reg")"
: > "$PROC_MOUNTS"

# --- wrong size: refused, nothing left, persist still unmounted --------
: > "$CALLS"
rm -f "$RUN_DIR/sns.reg"
printf 'short' > "$FAKE_PERSIST/sensors/sns.reg"
if copy_registry > "$T/out" 2>&1; then bad "short registry accepted"; fi
grep -q "the map expects 16" "$T/out" || bad "size mismatch message: $(cat "$T/out")"
[ -e "$RUN_DIR/sns.reg" ] && bad "short registry copied"
eq "unmounted after size failure" "umount $RUN_DIR/persist-ro" "$(sed -n 4p "$CALLS")"
printf '0123456789abcdef' > "$FAKE_PERSIST/sensors/sns.reg"

# --- mount failure --------------------------------------------------------
: > "$CALLS"
MOUNT_FAIL=1
if copy_registry > "$T/out" 2>&1; then bad "mount failure not reported"; fi
eq "no umount after failed mount" 0 "$(grep -c '^umount' "$CALLS")"
MOUNT_FAIL=0

# --- getro != 1 / setro failing: refuse before any mount ----------------
for case in getro0 getroempty setrofail; do
	: > "$CALLS"
	GETRO=1
	SETRO_FAIL=0
	case $case in
	getro0) GETRO=0 ;;
	getroempty) GETRO= ;;
	setrofail) SETRO_FAIL=1 ;;
	esac
	rm -f "$RUN_DIR/sns.reg"
	if copy_registry > "$T/out" 2>&1; then bad "$case: copy_registry went ahead"; fi
	[ "$(grep -c '^mount' "$CALLS")" -eq 0 ] || bad "$case: mounted anyway"
	[ -e "$RUN_DIR/sns.reg" ] && bad "$case: registry copied"
	grep -q "not mounting" "$T/out" || bad "$case: refusal not logged"
done
GETRO=1
SETRO_FAIL=0

# --- umount failure: refuse to go on, flag stays set for cleanup -------
: > "$CALLS"
UMOUNT_FAIL=1
if copy_registry > "$T/out" 2>&1; then bad "umount failure not fatal"; fi
grep -q "could not unmount persist" "$T/out" || bad "umount failure message"
eq "mounted flag kept for cleanup" 1 "$PERSIST_MOUNTED"
UMOUNT_FAIL=0
PERSIST_MOUNTED=0

# --- no persist partition ----------------------------------------------
rm -rf "$SYSFS_BLOCK/mmcblk0p38"
if copy_registry > "$T/out" 2>&1; then bad "missing persist not reported"; fi

# --- helpers --------------------------------------------------------------
DUMP_SERVERS="$T/dump"
printf '0x0000010f |0x00000002 |0x00000001 |0x0000003c |\n0x00000100 |0x00003201 |0x00000005 |0x00000059 |\n' > "$DUMP_SERVERS"
server_present 10f || bad "server_present 10f"
server_present 100 || bad "server_present 100"
server_present 118 && bad "server_present 118 must fail"
SUBSYS_DIR="$T/subsys"
mkdir -p "$SUBSYS_DIR/subsys0" "$SUBSYS_DIR/subsys1"
echo modem > "$SUBSYS_DIR/subsys0/name"
echo adsp > "$SUBSYS_DIR/subsys1/name"
echo OFFLINE > "$SUBSYS_DIR/subsys1/state"
adsp_online && bad "adsp_online with OFFLINE"
echo ONLINE > "$SUBSYS_DIR/subsys1/state"
adsp_online || bad "adsp_online with ONLINE"

if [ "$FAILS" -ne 0 ]; then
	echo "test_sensors-up.sh: $FAILS failure(s)"
	exit 1
fi
echo "test_sensors-up.sh: all passed"
