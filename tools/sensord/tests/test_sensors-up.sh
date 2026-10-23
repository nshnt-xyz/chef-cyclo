#!/bin/sh
# Host test for rootfs-overlay/usr/bin/sensors-up's persist handling and
# helpers. SENSORS_UP_SELFTEST=1 makes the script return after defining
# its functions. mount/umount/cp are replaced by shell functions (they
# take precedence over PATH here, on the host's sh), so the test sees
# exactly what would be run against persist without mounting anything.
# The two verbs run for real against stubs: `registry` up to irsc, `adsp`
# with a stub audio-up. The live ADSP part is covered by the live run only.
set -u
cd "$(dirname "$0")/../../.."
SCRIPT=rootfs-overlay/usr/bin/sensors-up
[ -r "$SCRIPT" ] || { echo "cannot find $SCRIPT" >&2; exit 1; }

T=$(mktemp -d /tmp/sensors-up-test.XXXXXX)
# the overlay's log helper, never the host's syslog (no syslogd pid file,
# no logger)
export CHEF_LOG_SOCKET="$T/no-syslog"
export CHEF_LIB_DIR="$PWD/rootfs-overlay/usr/lib/chef" CHEF_LOG_SYSLOGD_PID="$T/no-syslogd.pid" \
	CHEF_LOG_LOGGER="$T/no-logger"
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
export RUN_DIR="$T/run" SENSORD_DIR="$T/sensord"
mkdir -p "$SENSORD_DIR"
export REG_MAP="$T/sns_reg.map"
printf 'size 16\ngroup 1 0 16\n' > "$REG_MAP"
FAKE_PERSIST="$T/persist-content"
mkdir -p "$FAKE_PERSIST/sensors"
printf '0123456789abcdef' > "$FAKE_PERSIST/sensors/sns.reg"
CALLS="$T/calls"
: > "$CALLS"

SENSORS_UP_SELFTEST=1 . "./$SCRIPT"

# chef-state stub: records its calls and, for mag-restore, checks that the
# pristine copy already exists and overlays the first byte with "M".
STATE_CALLS="$T/state-calls"
: > "$STATE_CALLS"
CHEF_STATE="$T/chef-state-stub"
cat > "$CHEF_STATE" <<'STUB'
#!/bin/sh
echo "$* pristine=$CHEF_STATE_SENSORS_PRISTINE map=$CHEF_STATE_MAP" >> "$STATE_CALLS"
if [ "$1" = mag-restore ]; then
	[ -f "$CHEF_STATE_SENSORS_PRISTINE" ] || { echo "no pristine before overlay" >> "$STATE_CALLS"; exit 1; }
	printf M | dd of="$2" bs=1 conv=notrunc 2>/dev/null
fi
STUB
chmod +x "$CHEF_STATE"
export STATE_CALLS

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
# the host cannot give a file to sensord: recorded, not done
CHOWN_CALLS="$T/chown-calls"
CHOWN_FAIL=0
chown() {
	echo "chown $*" >> "$CHOWN_CALLS"
	[ "$CHOWN_FAIL" -eq 0 ]
}

eq "part persist" mmcblk0p38 "$(part persist)"
eq "map_size" 16 "$(map_size)"

# --- normal path: ro,noload mount, copy, unmount, built in root's
# directory, then given to sensord and moved into its directory ----------
copy_registry > "$T/out" 2>&1 || bad "copy_registry failed: $(cat "$T/out")"
eq "setro first" "blockdev --setro /dev/mmcblk0p38" "$(sed -n 1p "$CALLS")"
eq "getro second" "blockdev --getro /dev/mmcblk0p38" "$(sed -n 2p "$CALLS")"
eq "mount args" "mount -t ext4 -o ro,noload /dev/mmcblk0p38 $RUN_DIR/persist-ro" "$(sed -n 3p "$CALLS")"
eq "umount args" "umount $RUN_DIR/persist-ro" "$(sed -n 4p "$CALLS")"
eq "calls" 4 "$(grep -c . "$CALLS")"
grep -q "set read-only at the block layer (blockdev --getro = 1)" "$T/out" || bad "getro proof line missing"
eq "copy content with the saved group overlaid" M123456789abcdef "$(cat "$SENSORD_DIR/sns.reg")"
eq "pristine copy has no overlay" 0123456789abcdef "$(cat "$RUN_DIR/sns.reg.persist")"
eq "pristine mode (sensord's bias saves read it)" "-r--r--r--" "$(ls -l "$RUN_DIR/sns.reg.persist" | cut -c1-10)"
eq "mag-restore on the temp copy" "mag-restore $RUN_DIR/sns.reg.tmp pristine=$RUN_DIR/sns.reg.persist map=$REG_MAP" "$(cat "$STATE_CALLS")"
eq "given to sensord before the move" "chown sensord:sensord $RUN_DIR/sns.reg.tmp" "$(cat "$CHOWN_CALLS")"
eq "copy mode" "-rw-------" "$(ls -l "$SENSORD_DIR/sns.reg" | cut -c1-10)"
[ -e "$RUN_DIR/sns.reg" ] && bad "working copy in root's directory"
eq "mounted flag" 0 "$PERSIST_MOUNTED"
[ -e "$RUN_DIR/persist-ro" ] && bad "mount point left behind"
[ -e "$RUN_DIR/sns.reg.tmp" ] && bad "temp copy left behind"

# --- a link planted in sensord's directory is replaced, never written
# through --------------------------------------------------------------------
rm -f "$SENSORD_DIR/sns.reg"
ln -s "$T/planted" "$SENSORD_DIR/sns.reg"
copy_registry > "$T/out" 2>&1 || bad "copy over a planted link: $(cat "$T/out")"
[ ! -h "$SENSORD_DIR/sns.reg" ] && [ -f "$SENSORD_DIR/sns.reg" ] || bad "planted link not replaced"
[ -e "$T/planted" ] && bad "written through the planted link"
# a failed hand-over: nothing served, nothing left
: > "$CHOWN_CALLS"
rm -f "$SENSORD_DIR/sns.reg"
CHOWN_FAIL=1
if copy_registry > "$T/out" 2>&1; then bad "failed chown accepted"; fi
grep -q "cannot hand the registry copy to sensord" "$T/out" || bad "hand-over failure message: $(cat "$T/out")"
[ -e "$SENSORD_DIR/sns.reg" ] && bad "copy served after a failed chown"
[ -e "$RUN_DIR/sns.reg.tmp" ] && bad "temp copy left after a failed chown"
CHOWN_FAIL=0

# --- persist already mounted: copy from there, no mount/umount ---------
: > "$CALLS"
rm -f "$SENSORD_DIR/sns.reg"
mkdir -p "$T/already/sensors"
printf 'fedcba9876543210' > "$T/already/sensors/sns.reg"
printf '/dev/mmcblk0p38 %s ext4 ro,relatime 0 0\n' "$T/already" > "$PROC_MOUNTS"
copy_registry > "$T/out" 2>&1 || bad "copy from existing mount failed"
eq "no mount/blockdev when already mounted" 0 "$(grep -c . "$CALLS")"
eq "copied from existing mount" Medcba9876543210 "$(cat "$SENSORD_DIR/sns.reg")"
# SENSORS_FRESH=1: fresh copy, no overlay
: > "$STATE_CALLS"
rm -f "$SENSORD_DIR/sns.reg"
SENSORS_FRESH=1 copy_registry > "$T/out" 2>&1 || bad "fresh copy with SENSORS_FRESH=1"
eq "SENSORS_FRESH=1: no overlay" fedcba9876543210 "$(cat "$SENSORD_DIR/sns.reg")"
eq "SENSORS_FRESH=1: chef-state not called" 0 "$(grep -c . "$STATE_CALLS")"
grep -q "SENSORS_FRESH=1: no saved magnetometer bias applied" "$T/out" || bad "SENSORS_FRESH overlay message"
# a mag-restore that fails after writing part of its overlay: the served
# copy is persist's bytes, not the half-written one
cat > "$T/chef-state-partial" <<'STUB'
#!/bin/sh
[ "$1" = mag-restore ] && printf PARTIAL | dd of="$2" bs=1 conv=notrunc 2>/dev/null
exit 1
STUB
chmod +x "$T/chef-state-partial"
CHEF_STATE="$T/chef-state-partial"
rm -f "$SENSORD_DIR/sns.reg"
copy_registry > "$T/out" 2>&1 || bad "copy failed with a partial overlay"
eq "partial overlay: served copy is persist's" "$(cat "$RUN_DIR/sns.reg.persist")" "$(cat "$SENSORD_DIR/sns.reg")"
eq "partial overlay: plain copy" fedcba9876543210 "$(cat "$SENSORD_DIR/sns.reg")"
# a failing mag-restore costs only the overlay
CHEF_STATE=false
rm -f "$SENSORD_DIR/sns.reg"
copy_registry > "$T/out" 2>&1 || bad "copy failed with a failing chef-state"
eq "failing chef-state: plain copy" fedcba9876543210 "$(cat "$SENSORD_DIR/sns.reg")"
grep -q "saved magnetometer bias not applied" "$T/out" || bad "failed overlay message"
CHEF_STATE="$T/chef-state-stub"
: > "$PROC_MOUNTS"

# --- wrong size: refused, nothing left, persist still unmounted --------
: > "$CALLS"
rm -f "$SENSORD_DIR/sns.reg"
printf 'short' > "$FAKE_PERSIST/sensors/sns.reg"
if copy_registry > "$T/out" 2>&1; then bad "short registry accepted"; fi
grep -q "the map expects 16" "$T/out" || bad "size mismatch message: $(cat "$T/out")"
[ -e "$SENSORD_DIR/sns.reg" ] && bad "short registry copied"
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
	rm -f "$SENSORD_DIR/sns.reg"
	if copy_registry > "$T/out" 2>&1; then bad "$case: copy_registry went ahead"; fi
	[ "$(grep -c '^mount' "$CALLS")" -eq 0 ] || bad "$case: mounted anyway"
	[ -e "$SENSORD_DIR/sns.reg" ] && bad "$case: registry copied"
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

# --- reuse of this boot's /run copy (sensord wrote the DSP's writes
# back into it): no blockdev, no mount, nothing of persist touched --------
grep -q '^	reuse_registry || copy_registry || exit 1$' "$SCRIPT" || bad "reuse not tried before a fresh copy"
BOOT_ID_FILE="$T/boot_id"
echo aaa > "$BOOT_ID_FILE"
SENSORS_FRESH=0
rm -f "$RUN_DIR"/sns.reg* "$SENSORD_DIR/sns.reg"
copy_registry > "$T/out" 2>&1 || bad "fresh copy for reuse: $(cat "$T/out")"
eq "boot id recorded" aaa "$(cat "$RUN_DIR/sns.reg.boot_id")"
eq "pristine copy" 0123456789abcdef "$(cat "$RUN_DIR/sns.reg.persist")"
: > "$STATE_CALLS"
eq "pristine mode" "-r--r--r--" "$(ls -l "$RUN_DIR/sns.reg.persist" | cut -c1-10)"
# the DSP learned something (sensord's atomic write-back replaces the file)
printf 'X123456789abcdeY' > "$SENSORD_DIR/sns.reg.new" && mv -f "$SENSORD_DIR/sns.reg.new" "$SENSORD_DIR/sns.reg"
: > "$CALLS"
reuse_registry > "$T/out" 2>&1 || bad "same boot not reused: $(cat "$T/out")"
eq "reuse: no blockdev/mount/umount" 0 "$(grep -c . "$CALLS")"
grep -q "registry reused from this boot: .*DSP writes kept: 2 bytes differ from persist's; persist not touched" "$T/out" \
	|| bad "reuse message: $(cat "$T/out")"
eq "reuse keeps the DSP writes" X123456789abcdeY "$(cat "$SENSORD_DIR/sns.reg")"
eq "reuse: no overlay" 0 "$(grep -c . "$STATE_CALLS")"
# ... even with the persist partition gone: reuse never looks for it
mv "$SYSFS_BLOCK/mmcblk0p38" "$T/hidden-p38"
reuse_registry > "$T/out" 2>&1 || bad "reuse needed persist"
mv "$T/hidden-p38" "$SYSFS_BLOCK/mmcblk0p38"
# SENSORS_FRESH=1 forces a fresh copy
SENSORS_FRESH=1
if reuse_registry > "$T/out" 2>&1; then bad "SENSORS_FRESH=1 reused"; fi
grep -q "SENSORS_FRESH=1: fresh registry copy from persist" "$T/out" || bad "SENSORS_FRESH message"
[ -e "$SENSORD_DIR/sns.reg" ] && bad "SENSORS_FRESH=1 kept the working copy"
SENSORS_FRESH=0
printf 'X123456789abcdeY' > "$SENSORD_DIR/sns.reg"	# back, as sensord left it
# another boot: not reused, and the fresh copy restores persist's bytes
echo bbb > "$BOOT_ID_FILE"
if reuse_registry > "$T/out" 2>&1; then bad "copy from another boot reused"; fi
grep -q "not from this boot" "$T/out" || bad "boot mismatch message: $(cat "$T/out")"
: > "$CALLS"
copy_registry > "$T/out" 2>&1 || bad "fresh copy after boot mismatch"
eq "fresh copy goes through setro" "blockdev --setro /dev/mmcblk0p38" "$(sed -n 1p "$CALLS")"
eq "fresh content" M123456789abcdef "$(cat "$SENSORD_DIR/sns.reg")"
eq "new boot id" bbb "$(cat "$RUN_DIR/sns.reg.boot_id")"
# unreadable boot id: never reused
BOOT_ID_FILE="$T/no-such-boot-id"
if reuse_registry > "$T/out" 2>&1; then bad "reused without a boot id"; fi
BOOT_ID_FILE="$T/boot_id"
# wrong size: not reused
printf 'short' > "$SENSORD_DIR/sns.reg"
if reuse_registry > "$T/out" 2>&1; then bad "short copy reused"; fi
grep -q "registry copy is 5 bytes, the map expects 16; fresh copy from persist" "$T/out" || bad "size message: $(cat "$T/out")"
# missing copy: not reused, quietly
rm -f "$SENSORD_DIR/sns.reg"
if reuse_registry > "$T/out" 2>&1; then bad "missing copy reused"; fi
eq "missing copy: no message" 0 "$(grep -c . "$T/out")"
# a link in sensord's directory is never followed: not reused (its target
# here would pass every check)
ln -s "$RUN_DIR/sns.reg.persist" "$SENSORD_DIR/sns.reg"
if reuse_registry > "$T/out" 2>&1; then bad "linked copy reused"; fi
rm -f "$SENSORD_DIR/sns.reg"
# SENSORS_FRESH=1 removes the working copy first
printf '0123456789abcdef' > "$SENSORD_DIR/sns.reg"
SENSORS_FRESH=1 reuse_registry > /dev/null 2>&1
[ -e "$SENSORD_DIR/sns.reg" ] && bad "SENSORS_FRESH=1 kept the working copy"
: > "$CALLS"

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

# --- the lock (real script runs) -------------------------------------------
# `registry` is run for real with every path under $T; it stops at the
# missing IPC-router dump_servers right after taking the lock, so nothing
# is mounted and persist is never looked at.
RUNENV="LOCK=$T/up.lock DUMP_SERVERS=$T/no-such-dump RUN_DIR=$T/run2"
mkdir -p "$T/bin"
# a real binary for the lock holder (a multi-call sleep cannot be copied under another name)
printf '#include <unistd.h>\nint main(int c, char **v) { (void)c; (void)v; sleep(60); return 0; }\n' > "$T/nap.c"
cc -o "$T/bin/othersleep" "$T/nap.c" || { echo "no C compiler for the lock test" >&2; exit 1; }
runscript() { env $RUNENV sh "$SCRIPT" "$@" > "$T/rs.out" 2>&1; }
runscript; eq "no verb: usage" 2 "$?"
grep -q "usage: sensors-up registry|adsp" "$T/rs.out" || bad "usage: $(cat "$T/rs.out")"
# a free lock: the instance gets past it (and stops at dump_servers, exit 1)
runscript registry; eq "free lock: past the lock" 1 "$?"
grep -q "dump_servers is unavailable" "$T/rs.out" || bad "free lock run: $(cat "$T/rs.out")"
# a held lock: refused, before anything else
sh -c 'exec 9>"'"$T"'/up.lock"; flock -n 9 || exit 3; exec "'"$T"'/bin/othersleep"' &
HOLDER=$!
sleep 0.3
runscript registry; eq "held lock: refused" 1 "$?"
grep -q "already running" "$T/rs.out" || bad "held lock message: $(cat "$T/rs.out")"
grep -q "dump_servers" "$T/rs.out" && bad "held lock: ran past the lock"
# kill -9 of the holder: the kernel drops the lock, a new instance starts
kill -9 "$HOLDER" 2>/dev/null; wait "$HOLDER" 2>/dev/null
runscript registry; grep -q "dump_servers is unavailable" "$T/rs.out" || bad "no new start after kill -9 of the holder: $(cat "$T/rs.out")"
# no sensord here any more: the sensord service supervises it
grep -Eq '^[^#]*(sensord.pid|"\$SENSORD"|SMGR_WAIT)' "$SCRIPT" && bad "sensors-up still runs or waits for sensord"

# --- registry: the whole verb on a reusable copy (irsc stubbed) ---------------
H=$T/h
mkdir -p "$H/run" "$H/sensord" "$H/bin"
printf '0123456789abcdef' > "$H/sensord/sns.reg"
echo hostboot > "$H/boot_id"
cp "$H/boot_id" "$H/run/sns.reg.boot_id"
printf '0x0000010f |0x00000002 |0x00000001 |0x0000003c |\n' > "$H/dump"
printf '#!/bin/sh\necho irsc >> "%s/irsc.calls"\nexit "${IRSC_RC:-0}"\n' "$H" > "$H/bin/irsc"
chmod +x "$H/bin/irsc"
UPENV="PATH=$H/bin:$PATH LOCK=$H/up.lock RUN_DIR=$H/run SENSORD_DIR=$H/sensord REG_MAP=$T/sns_reg.map BOOT_ID_FILE=$H/boot_id DUMP_SERVERS=$H/dump"
env $UPENV sh "$SCRIPT" registry > "$H/o1" 2>&1; eq "registry: exit" 0 "$?"
grep -q "registry reused from this boot" "$H/o1" || bad "registry verb: $(cat "$H/o1")"
eq "registry: irsc once" irsc "$(cat "$H/irsc.calls")"
env $UPENV IRSC_RC=1 sh "$SCRIPT" registry > "$H/o2" 2>&1; eq "registry: irsc failure fails" 1 "$?"
grep -q "irsc failed" "$H/o2" || bad "irsc failure message: $(cat "$H/o2")"

# --- adsp: audio-up started detached unless the adsp is up or audio-up runs ---
mkdir -p "$H/subsys/subsys0"
echo adsp > "$H/subsys/subsys0/name"
cat > "$H/audio-up" <<'STUB'
#!/bin/sh
echo "audio-up sid=$(cut -d' ' -f6 /proc/$$/stat) parent=$PPID" >> "$H_AUDIO"
exec sleep 2
STUB
chmod +x "$H/audio-up"
ADSPENV="$UPENV SUBSYS_DIR=$H/subsys AUDIO_UP=$H/audio-up AUDIO_LOCK=$H/audio.lock H_AUDIO=$H/audio.calls"
echo ONLINE > "$H/subsys/subsys0/state"
env $ADSPENV sh "$SCRIPT" adsp > "$H/a1" 2>&1; eq "adsp ONLINE: exit" 0 "$?"
grep -q "adsp already ONLINE" "$H/a1" || bad "adsp ONLINE message: $(cat "$H/a1")"
echo OFFLINE > "$H/subsys/subsys0/state"
mkdir "$H/audio.lock"
env $ADSPENV sh "$SCRIPT" adsp > "$H/a2" 2>&1; eq "audio-up running: exit" 0 "$?"
grep -q "audio-up is running" "$H/a2" || bad "audio-up running message: $(cat "$H/a2")"
rmdir "$H/audio.lock"
[ -e "$H/audio.calls" ] && bad "audio-up started while the adsp was up or audio-up ran"
start=$(date +%s)
env $ADSPENV sh "$SCRIPT" adsp > "$H/a3" 2>&1; eq "adsp OFFLINE: exit" 0 "$?"
[ $(($(date +%s) - start)) -le 1 ] || bad "adsp waited for audio-up"
grep -q "started $H/audio-up (pid [0-9]*) to boot the adsp" "$H/a3" || bad "audio-up start message: $(cat "$H/a3")"
n=0; while [ ! -s "$H/audio.calls" ] && [ "$n" -lt 30 ]; do sleep 0.1; n=$((n + 1)); done
sid=$(sed -n 's/.*sid=\([0-9]*\).*/\1/p' "$H/audio.calls")
[ -n "$sid" ] && [ "$sid" != "$(cut -d' ' -f6 /proc/$$/stat)" ] || bad "audio-up not in a session of its own: $(cat "$H/audio.calls")"
# children never inherit the lock fd
grep -q 'setsid "$AUDIO_UP" </dev/null 9>&- &' "$SCRIPT" || bad "audio-up start does not close fd 9"

if [ "$FAILS" -ne 0 ]; then
	echo "test_sensors-up.sh: $FAILS failure(s)"
	exit 1
fi
echo "test_sensors-up.sh: all passed"
