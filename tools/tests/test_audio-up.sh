#!/bin/sh
# Host regression test for initramfs/usr/bin/audio-up's selection/wait logic:
# part(), active_slot(), set_firmware_path() (copied from gps-up, same
# behavior), server_present()/wait_for_server() (the 0x40 symmetry check),
# find_subsys(), boot_adsp(), card_ready()/wait_for_card(), pcm_mm1_device().
#
# AUDIO_UP_SELFTEST=1 makes audio-up return after defining its functions
# instead of running the ADSP bring-up. Real-filesystem behavior this test
# cannot exercise without a device (mountpoint -q /firmware, the actual
# servreg-locator/irsc subprocesses, the supervision loop) is covered by
# manual review and live verification only -- see docs/features/audio.md.
set -eu
cd "$(dirname "$0")/../.."
AUDIO_UP=initramfs/usr/bin/audio-up
[ -r "$AUDIO_UP" ] || { echo "cannot find $AUDIO_UP" >&2; exit 1; }

TMPROOT=$(mktemp -d /tmp/audio-up-test.XXXXXX)
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
	# mk_part <mmcblk0pN> <PARTNAME>
	d="$SYSFS_BLOCK/$1"
	mkdir -p "$d"
	printf 'PARTNAME=%s\n' "$2" > "$d/uevent"
}

# --- fixture: same slot-cmdline shape as test_gps-up.sh ---
export SYSFS_BLOCK="$TMPROOT/sysfs-good"
export CMDLINE="$TMPROOT/cmdline-a"
mk_part mmcblk0p6 modem_a
mk_part mmcblk0p7 modem_b
printf 'console=ttyMSM0 androidboot.slot_suffix=_a rootwait ro\n' > "$CMDLINE"
printf 'console=ttyMSM0 androidboot.slot_suffix=_b rootwait ro\n' > "$TMPROOT/cmdline-b"

AUDIO_UP_SELFTEST=1 . "./$AUDIO_UP"

# --- active_slot() / part(): copied from gps-up, same behavior ---
eq "active_slot _a" a "$(CMDLINE=$TMPROOT/cmdline-a active_slot)"
eq "active_slot _b" b "$(CMDLINE=$TMPROOT/cmdline-b active_slot)"
eq "part modem_a" mmcblk0p6 "$(part modem_a)"
eq "part modem_b" mmcblk0p7 "$(part modem_b)"

# --- set_firmware_path(): identical contract to gps-up's (printf, no
# trailing newline -- see that test's comment for why). ---
FW_PATH_NODE="$TMPROOT/fw-class-path"
: > "$FW_PATH_NODE"
set_firmware_path
got=$(od -An -tx1 < "$FW_PATH_NODE" | tr -d ' \n')
want=$(printf '%s' /firmware/image | od -An -tx1 | tr -d ' \n')
eq "set_firmware_path writes exactly /firmware/image, byte for byte" "$want" "$got"

# --- server_present() / wait_for_server(): the 0x40 symmetry check ---
DUMP_SERVERS="$TMPROOT/dump_servers-present"
printf '0x00000040 |0x00000000|...\n' > "$DUMP_SERVERS"
if DUMP_SERVERS="$DUMP_SERVERS" server_present 40; then ok; else bad "server_present 40 must succeed when present"; fi

DUMP_SERVERS="$TMPROOT/dump_servers-absent"
: > "$DUMP_SERVERS"
if DUMP_SERVERS="$DUMP_SERVERS" server_present 40; then bad "server_present 40 must fail when absent"; else ok; fi

# wait_for_server must return immediately (limit 0, no sleep) when the
# service is absent and nothing is running to wait on.
DUMP_SERVERS="$TMPROOT/dump_servers-absent"
if DUMP_SERVERS="$DUMP_SERVERS" wait_for_server 40 TEST 0; then
	bad "wait_for_server must time out when the service never appears"
else
	ok
fi
# ...and return immediately (no sleep) when the service is already present.
DUMP_SERVERS="$TMPROOT/dump_servers-present"
if DUMP_SERVERS="$DUMP_SERVERS" wait_for_server 40 TEST 0; then
	ok
else
	bad "wait_for_server must succeed immediately when the service is already present"
fi
# A dead SERVREG_PID aborts the wait even with attempts left.
DUMP_SERVERS="$TMPROOT/dump_servers-absent"
sh -c 'exit 0' &
DEAD_PID=$!
wait "$DEAD_PID" 2>/dev/null
if DUMP_SERVERS="$DUMP_SERVERS" SERVREG_PID="$DEAD_PID" wait_for_server 40 TEST 5; then
	bad "wait_for_server must abort once its own servreg-locator has exited"
else
	ok
fi

# --- find_subsys(): locate the "adsp" msm_subsys device among others ---
SUBSYS_DIR="$TMPROOT/subsys-good"
mkdir -p "$SUBSYS_DIR/subsys0" "$SUBSYS_DIR/subsys1" "$SUBSYS_DIR/subsys2"
printf 'modem\n' > "$SUBSYS_DIR/subsys0/name"
printf 'adsp\n' > "$SUBSYS_DIR/subsys1/name"
printf 'wcnss\n' > "$SUBSYS_DIR/subsys2/name"
eq "find_subsys adsp" "$SUBSYS_DIR/subsys1" "$(SUBSYS_DIR=$SUBSYS_DIR find_subsys adsp)"

SUBSYS_DIR="$TMPROOT/subsys-missing"
mkdir -p "$SUBSYS_DIR/subsys0"
printf 'modem\n' > "$SUBSYS_DIR/subsys0/name"
if out=$(SUBSYS_DIR="$SUBSYS_DIR" find_subsys adsp 2>/dev/null); then
	bad "find_subsys adsp must fail when no subsys device is named adsp (got '$out')"
else
	ok
fi

# --- boot_adsp(): writes exactly "1" to BOOT_ADSP_NODE ---
BOOT_ADSP_NODE="$TMPROOT/boot_adsp-node"
: > "$BOOT_ADSP_NODE"
BOOT_ADSP_NODE="$BOOT_ADSP_NODE" boot_adsp
eq "boot_adsp writes 1" "1" "$(cat "$BOOT_ADSP_NODE")"

BOOT_ADSP_NODE="$TMPROOT/boot_adsp-missing/boot"
if BOOT_ADSP_NODE="$BOOT_ADSP_NODE" boot_adsp 2>/dev/null; then
	bad "boot_adsp must fail when the node's directory doesn't exist"
else
	ok
fi

# --- card_ready() / wait_for_card(): success is immediate (no sleep needed
# when the card is already there), and a limit of 0 times out instantly.
# The "card present" fixture is a FIFO fed by a background printf: like
# /proc/asound/cards on the device (live 2026-09-19 run 1) it has st_size 0
# while still listing the card, which is exactly what an [ -s ] test got
# wrong the first time (audio-up waited its full 60 s with the card up). ---
ASOUND_DIR="$TMPROOT/asound-ready"
mkdir -p "$ASOUND_DIR"
mkfifo "$ASOUND_DIR/cards"
printf ' 0 [sdm660sndcard  ]: sdm660-snd-card - sdm660-snd-card\n' > "$ASOUND_DIR/cards" &
FEEDER=$!
if ASOUND_DIR="$ASOUND_DIR" card_ready; then ok; else bad "card_ready must succeed on a size-0 (procfs-like) cards file that lists sdm660"; fi
wait "$FEEDER" 2>/dev/null || true
rm -f "$ASOUND_DIR/cards"
cat > "$ASOUND_DIR/cards" <<'EOF'
 0 [sdm660sndcard  ]: sdm660-snd-card - sdm660-snd-card
                      sdm660-snd-card
EOF
if ASOUND_DIR="$ASOUND_DIR" wait_for_card 0; then ok; else bad "wait_for_card must succeed immediately when the card is already up"; fi
body=$(sed -n '/^card_ready() {/,/^}/p' "$AUDIO_UP" | grep -v '^[[:space:]]*#')
case "$body" in
*'[ -s'*|*'test -s'*) bad "card_ready() in $AUDIO_UP must not use -s (procfs size is 0)" ;;
*) ok ;;
esac
if ASOUND_DIR="$TMPROOT/asound-does-not-exist" card_ready 2>/dev/null; then bad "card_ready must fail on a missing cards file"; else ok; fi

ASOUND_DIR="$TMPROOT/asound-empty"
mkdir -p "$ASOUND_DIR"
: > "$ASOUND_DIR/cards"
if ASOUND_DIR="$ASOUND_DIR" card_ready; then bad "card_ready must fail on an empty cards file"; else ok; fi
if ASOUND_DIR="$ASOUND_DIR" wait_for_card 0; then
	bad "wait_for_card must time out (limit 0) when the card never appears"
else
	ok
fi

ASOUND_DIR="$TMPROOT/asound-other-card"
mkdir -p "$ASOUND_DIR"
cat > "$ASOUND_DIR/cards" <<'EOF'
 0 [SomeOtherCard  ]: some-other - Some Other Card
                      Some Other Card at 0x0
EOF
if ASOUND_DIR="$ASOUND_DIR" card_ready; then bad "card_ready must not match an unrelated card"; else ok; fi

# --- pcm_mm1_device(): parsed from a realistic /proc/asound/pcm, front-end
# ordering taken from kernel/sound/soc/msm/sdm660-internal.c's msm_int_dai[]
# (MultiMedia1 first, hw:x,0; a handful of the other MultiMediaN entries
# included so the parser must actually match on name, not position). ---
ASOUND_DIR="$TMPROOT/asound-pcm"
mkdir -p "$ASOUND_DIR"
cat > "$ASOUND_DIR/pcm" <<'EOF'
00-00: MultiMedia1 (*) :  : playback 1 : capture 1
00-01: MultiMedia2 (*) :  : playback 1 : capture 1
00-02: MultiMedia3 (*) :  : playback 1 : capture 1
00-09: MultiMedia10 (*) :  : capture 1
00-16: Tertiary MI2S Playback : Tertiary MI2S Playback : playback 1
EOF
eq "pcm_mm1_device finds MultiMedia1's playback device, leading zero stripped" "0" "$(ASOUND_DIR=$ASOUND_DIR pcm_mm1_device)"
cat > "$ASOUND_DIR/pcm" <<'EOF'
00-00: MultiMedia2 (*) :  : playback 1 : capture 1
00-08: MultiMedia1 (*) :  : playback 1 : capture 1
EOF
eq "pcm_mm1_device handles a device number with a leading zero that is not valid octal (08)" "8" "$(ASOUND_DIR=$ASOUND_DIR pcm_mm1_device)"

ASOUND_DIR="$TMPROOT/asound-pcm-missing"
mkdir -p "$ASOUND_DIR"
cat > "$ASOUND_DIR/pcm" <<'EOF'
00-09: MultiMedia10 (*) :  : capture 1
EOF
if out=$(ASOUND_DIR="$ASOUND_DIR" pcm_mm1_device 2>/dev/null); then
	bad "pcm_mm1_device must fail when MultiMedia1 is not in the pcm list (got '$out')"
else
	ok
fi

# --- uptime_s() / elapsed_s(): the boot_adsp -> card timing in the "adsp
# up" log line (live: 1.17 s). ---
t0=$(uptime_s)
case "$t0" in
[0-9]*.[0-9]*) ok ;;
*) bad "uptime_s must echo a decimal seconds value, got '$t0'" ;;
esac
el=$(elapsed_s "$t0")
if [ "$(awk -v e="$el" 'BEGIN { print (e >= 0 && e < 5) ? 1 : 0 }')" = 1 ]; then ok; else bad "elapsed_s must be a small non-negative number, got '$el'"; fi
# A t0 1.5 s in the past must read back as ~1.50 (two decimals, small
# tolerance for the time the two uptime reads take).
t_past=$(awk -v t="$(uptime_s)" 'BEGIN { printf "%.2f", t - 1.5 }')
el=$(elapsed_s "$t_past")
case "$el" in
[0-9]*.[0-9][0-9]) ok ;;
*) bad "elapsed_s must format to two decimals, got '$el'" ;;
esac
if [ "$(awk -v e="$el" 'BEGIN { print (e >= 1.5 && e < 1.7) ? 1 : 0 }')" = 1 ]; then ok; else bad "elapsed_s for a t0 1.5 s ago must be ~1.50, got '$el'"; fi

# A failing run must SAY so rather than print "PASS:" regardless and leave
# the exit status as the only signal -- the last line of a suite's output is
# what a reader (and `make test`'s log) actually sees. Exit behaviour is
# unchanged: 0 only when nothing failed.
if [ "$fail" -eq 0 ]; then
	echo "PASS: $pass/$((pass + fail)) checks passed"
else
	echo "FAILED: $fail of $((pass + fail)) checks failed ($pass passed)" >&2
	exit 1
fi
