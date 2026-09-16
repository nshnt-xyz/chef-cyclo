#!/bin/sh
# Host regression test for initramfs/usr/bin/gps-up's active-slot EFS
# selection logic: part(), part_size_bytes(), active_slot(), resolve_fsg().
#
# gps-up sources these against real /sys/block/mmcblk0 and /proc/cmdline;
# SYSFS_BLOCK/CMDLINE let this test point it at a throwaway fake tree
# instead, so the exact-slot / exact-size / no-cross-slot-fallback rules can
# be exercised without a device. GPS_UP_SELFTEST=1 makes gps-up return after
# defining its functions instead of running the modem bring-up.
set -eu
cd "$(dirname "$0")/../.."
GPS_UP=initramfs/usr/bin/gps-up
[ -r "$GPS_UP" ] || { echo "cannot find $GPS_UP" >&2; exit 1; }

TMPROOT=$(mktemp -d /tmp/gps-up-test.XXXXXX)
trap 'rm -rf "$TMPROOT"' EXIT

pass=0
fail=0

ok() {
	pass=$((pass + 1))
}

bad() {
	fail=$((fail + 1))
	echo "FAIL: $*" >&2
}

eq() {
	# eq <description> <expected> <actual>
	if [ "$2" = "$3" ]; then
		ok
	else
		bad "$1: expected '$2', got '$3'"
	fi
}

mk_part() {
	# mk_part <mmcblk0pN> <PARTNAME> <size_bytes>
	d="$SYSFS_BLOCK/$1"
	mkdir -p "$d"
	printf 'PARTNAME=%s\n' "$2" > "$d/uevent"
	echo $(($3 / 512)) > "$d/size"
}

# --- fixture: a device with modemst1/modemst2/fsc unsuffixed, fsg_a/fsg_b
# both present and exactly 10485760 bytes -- the confirmed live layout. ---
export SYSFS_BLOCK="$TMPROOT/sysfs-good"
export CMDLINE="$TMPROOT/cmdline-a"
mk_part mmcblk0p1 modemst1 4194304
mk_part mmcblk0p2 modemst2 4194304
mk_part mmcblk0p3 fsc 1048576
mk_part mmcblk0p4 fsg_a 10485760
mk_part mmcblk0p5 fsg_b 10485760
printf 'console=ttyMSM0 androidboot.slot_suffix=_a rootwait ro\n' > "$CMDLINE"
printf 'console=ttyMSM0 androidboot.slot_suffix=_b rootwait ro\n' > "$TMPROOT/cmdline-b"
printf 'console=ttyMSM0 rootwait ro\n' > "$TMPROOT/cmdline-none"

GPS_UP_SELFTEST=1 . "./$GPS_UP"

# --- active_slot() ---
eq "active_slot _a" a "$(CMDLINE=$TMPROOT/cmdline-a active_slot)"
eq "active_slot _b" b "$(CMDLINE=$TMPROOT/cmdline-b active_slot)"
if CMDLINE="$TMPROOT/cmdline-none" active_slot >/dev/null 2>&1; then
	bad "active_slot must fail with no androidboot.slot_suffix on cmdline"
else
	ok
fi

# --- part() ---
eq "part modemst1" mmcblk0p1 "$(part modemst1)"
eq "part fsg_a" mmcblk0p4 "$(part fsg_a)"
eq "part fsg_b" mmcblk0p5 "$(part fsg_b)"
eq "part unknown label" "" "$(part does-not-exist)"
# Live evidence: there is no unsuffixed fsg partition on this device.
eq "part fsg (unsuffixed) must not exist" "" "$(part fsg)"

# --- part_size_bytes() ---
eq "part_size_bytes fsg_a" 10485760 "$(part_size_bytes mmcblk0p4)"
if part_size_bytes mmcblk0p999 >/dev/null 2>&1; then
	bad "part_size_bytes must fail for a missing partition"
else
	ok
fi

# --- resolve_fsg(): the active slot's exact partition and exact size ---
eq "resolve_fsg a" "fsg_a mmcblk0p4" "$(resolve_fsg a)"
eq "resolve_fsg b" "fsg_b mmcblk0p5" "$(resolve_fsg b)"

# --- resolve_fsg() must never fall back to the other slot ---
export SYSFS_BLOCK="$TMPROOT/sysfs-missing-a"
mk_part mmcblk0p5 fsg_b 10485760
# fsg_a is deliberately absent; only the (correctly sized) other slot exists.
if out=$(resolve_fsg a 2>/dev/null); then
	bad "resolve_fsg a must fail when fsg_a is missing, even though fsg_b exists (got '$out')"
else
	ok
fi

export SYSFS_BLOCK="$TMPROOT/sysfs-wrong-size"
mk_part mmcblk0p4 fsg_a 5242880
mk_part mmcblk0p5 fsg_b 10485760
if out=$(resolve_fsg a 2>/dev/null); then
	bad "resolve_fsg a must reject a wrong-sized fsg_a rather than fall back to fsg_b (got '$out')"
else
	ok
fi
# The correctly-sized *other* slot resolving fine on its own confirms the
# rejection above was about fsg_a's size, not a fixture mistake.
eq "resolve_fsg b (control: still resolves on its own slot)" "fsg_b mmcblk0p5" "$(resolve_fsg b)"

# --- set_firmware_path(): must not embed a trailing newline (or any other
# extra byte) into the firmware_class "path" module param. echo's trailing
# \n survives kernel/params.c's param_set_copystring() (a bare strcpy(), no
# trimming) and lands in the middle of "%s/%s" with the firmware name in
# fw_get_filesystem_firmware(), so a real firmware file the kernel could
# otherwise find directly is never located -- see the 2026-09-16 (live,
# fix) README entry for the on-device failure this caused. ---
FW_PATH_NODE="$TMPROOT/fw-class-path"
: > "$FW_PATH_NODE"
set_firmware_path
got=$(od -An -tx1 < "$FW_PATH_NODE" | tr -d ' \n')
want=$(printf '%s' /firmware/image | od -An -tx1 | tr -d ' \n')
eq "set_firmware_path writes exactly /firmware/image, byte for byte (no trailing newline)" "$want" "$got"

echo "PASS: $pass/$((pass + fail)) checks passed"
[ "$fail" -eq 0 ]
