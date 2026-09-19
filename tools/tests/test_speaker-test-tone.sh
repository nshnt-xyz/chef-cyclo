#!/bin/sh
# Host regression test for initramfs/usr/bin/speaker-test-tone: argument
# validation/clamping (via SPEAKER_TEST_TONE_SELFTEST=1, same sourcing
# hook pattern as tools/tests/test_audio-up.sh), and the full mixer/
# playback flow run as a real subprocess against a fake /proc/asound tree
# with stub tinymix/tinyplay/wavtone on PATH that record their argv --
# asserting the exact tinymix argv sequence (incl. tinyplay -M), the -p
# best-effort protection attempt, the -s status mode, refusal without a card, and
# the mixer route being reset on exit even when tinyplay fails.
set -eu
cd "$(dirname "$0")/../.."
SCRIPT=initramfs/usr/bin/speaker-test-tone
[ -r "$SCRIPT" ] || { echo "cannot find $SCRIPT" >&2; exit 1; }

TMPROOT=$(mktemp -d /tmp/speaker-test-tone-test.XXXXXX)
trap 'rm -rf "$TMPROOT"' EXIT

pass=0
fail=0

ok() { pass=$((pass + 1)); }
bad() { fail=$((fail + 1)); echo "FAIL: $*" >&2; }
eq() {
	if [ "$2" = "$3" ]; then ok; else bad "$1: expected '$2', got '$3'"; fi
}

# ---- pure functions, via the sourcing hook ----
SPEAKER_TEST_TONE_SELFTEST=1 . "./$SCRIPT"

eq "clamp_duration under the cap" 3 "$(clamp_duration 3)"
eq "clamp_duration at the cap" 5 "$(clamp_duration 5)"
eq "clamp_duration over the cap" 5 "$(clamp_duration 10)"
eq "clamp_amplitude under the ceiling (quieter)" -30 "$(clamp_amplitude -30)"
eq "clamp_amplitude at the ceiling" -6 "$(clamp_amplitude -6)"
eq "clamp_amplitude over the ceiling (louder, clamped)" -6 "$(clamp_amplitude -3)"
eq "clamp_amplitude well over the ceiling" -6 "$(clamp_amplitude 0)"

if is_number 1000; then ok; else bad "is_number 1000 must accept an integer"; fi
if is_number -20.5; then ok; else bad "is_number -20.5 must accept a signed decimal"; fi
if is_number 1000abc; then bad "is_number 1000abc must reject trailing garbage"; else ok; fi
if is_number ''; then bad "is_number '' must reject an empty string"; else ok; fi

ASOUND_DIR="$TMPROOT/asound-pcm"
mkdir -p "$ASOUND_DIR"
cat > "$ASOUND_DIR/pcm" <<'EOF'
00-00: MultiMedia1 (*) :  : playback 1 : capture 1
00-01: MultiMedia2 (*) :  : playback 1 : capture 1
EOF
eq "pcm_mm1_device strips the leading zero (live: '00-00: MultiMedia1' -> device 0)" "0" "$(ASOUND_DIR=$ASOUND_DIR pcm_mm1_device)"

# card_ready() against a size-0 "file": /proc/asound/cards has st_size 0 on
# the device even when it lists the card (live 2026-09-19 run 1 -- an [ -s ]
# test made the first version refuse forever). A FIFO fed by a background
# printf has st_size 0 too and still delivers the line to one grep.
ASOUND_DIR="$TMPROOT/asound-ready"
mkdir -p "$ASOUND_DIR"
mkfifo "$ASOUND_DIR/cards"
printf ' 0 [sdm660sndcard  ]: sdm660-snd-card - sdm660-snd-card\n' > "$ASOUND_DIR/cards" &
FEEDER=$!
if ASOUND_DIR="$ASOUND_DIR" card_ready; then ok; else bad "card_ready must succeed on a size-0 cards file (procfs) that lists sdm660"; fi
wait "$FEEDER" 2>/dev/null || true
rm -f "$ASOUND_DIR/cards"
: > "$ASOUND_DIR/cards"
if ASOUND_DIR="$ASOUND_DIR" card_ready; then bad "card_ready must fail on an empty cards file"; else ok; fi
if ASOUND_DIR="$TMPROOT/asound-does-not-exist" card_ready 2>/dev/null; then bad "card_ready must fail on a missing cards file"; else ok; fi
# Belt and braces: the function body itself must never grow an -s test back.
body=$(sed -n '/^card_ready() {/,/^}/p' "$SCRIPT" | grep -v '^[[:space:]]*#')
case "$body" in
*'[ -s'*|*'test -s'*) bad "card_ready() in $SCRIPT must not use -s (procfs size is 0)" ;;
*) ok ;;
esac

# ---- full-flow integration: real subprocess, fake /proc/asound, stub
# tinymix/tinyplay/wavtone on PATH recording their argv to $LOGFILE ----
STUBDIR="$TMPROOT/stubbin"
mkdir -p "$STUBDIR"

cat > "$STUBDIR/tinymix" <<'STUB'
#!/bin/sh
echo "tinymix $*" >> "$LOGFILE"
# STUB_TINYMIX_FAIL_ON=<control>: reject a set of that control the way the
# live kernel rejects TAS2560_ALGO_FF_MODULE with no active AFE port.
if [ "$3" = set ] && [ -n "${STUB_TINYMIX_FAIL_ON:-}" ] && [ "$4" = "$STUB_TINYMIX_FAIL_ON" ]; then
	echo "Error: invalid enum value" >&2
	exit 1
fi
if [ "$3" = get ]; then
	echo "stub-value-for: $4"
fi
exit "${STUB_TINYMIX_EXIT:-0}"
STUB
cat > "$STUBDIR/tinyplay" <<'STUB'
#!/bin/sh
echo "tinyplay $*" >> "$LOGFILE"
[ -n "${STUB_TINYPLAY_SLEEP:-}" ] && sleep "$STUB_TINYPLAY_SLEEP"
exit "${STUB_TINYPLAY_EXIT:-0}"
STUB
cat > "$STUBDIR/wavtone" <<'STUB'
#!/bin/sh
echo "wavtone $*" >> "$LOGFILE"
# Find the -o argument and create that file, like the real wavtone would.
prev=
for a in "$@"; do
	[ "$prev" = -o ] && : > "$a"
	prev=$a
done
exit "${STUB_WAVTONE_EXIT:-0}"
STUB
chmod +x "$STUBDIR"/tinymix "$STUBDIR"/tinyplay "$STUBDIR"/wavtone

setup_fixture() {
	FIXROOT=$(mktemp -d "$TMPROOT/fixture.XXXXXX")
	mkdir -p "$FIXROOT/asound" "$FIXROOT/run"
	printf ' 0 [sdm660snd      ]: sdm660-asoc-s - sdm660-snd-card\n' > "$FIXROOT/asound/cards"
	printf '00-00: MultiMedia1 (*) :  : playback 1 : capture 1\n' > "$FIXROOT/asound/pcm"
	echo "$FIXROOT"
}

# Success path: exact tinymix argv sequence, wavtone/tinyplay called with
# the requested options, route reset to 0 at the end, wav file cleaned up.
FIXROOT=$(setup_fixture)
LOGFILE="$FIXROOT/log"
: > "$LOGFILE"
if ASOUND_DIR="$FIXROOT/asound" RUN_DIR="$FIXROOT/run" LOGFILE="$LOGFILE" \
   PATH="$STUBDIR:$PATH" \
   sh "$SCRIPT" -f 500 -d 2 -a -10 -v 8 >/dev/null 2>&1; then
	ok
else
	bad "speaker-test-tone should exit 0 on the success path"
fi
want_log="tinymix -D 0 set TERT_MI2S_RX Audio Mixer MultiMedia1 1
tinymix -D 0 set DAC Playback Volume 8"
got_log=$(head -n 2 "$LOGFILE")
eq "tinymix argv sequence (route, volume, in order; protection module untouched by default)" "$want_log" "$got_log"
if grep -q 'TAS2560_ALGO_FF_MODULE' "$LOGFILE"; then
	bad "the default run must not touch TAS2560_ALGO_FF_MODULE (live: unsettable before the stream, reads back 0 during it)"
else
	ok
fi
if grep -q '^wavtone -f 500 -d 2 -a -10 -o ' "$LOGFILE"; then ok; else bad "wavtone must be called with the requested -f/-d/-a"; fi
# -M (mmap) is mandatory on this card: without it every play XRUN-loops
# (live 2026-09-19 run 1). Device is the leading-zero-stripped MultiMedia1 device.
if grep -q '^tinyplay .*-D 0 -d 0 -M$' "$LOGFILE"; then ok; else bad "tinyplay must be called with -D 0 -d 0 -M (mmap is mandatory), got: $(grep '^tinyplay' "$LOGFILE")"; fi
last_log=$(tail -n 1 "$LOGFILE")
eq "route is reset to 0 as the last mixer action" "tinymix -D 0 set TERT_MI2S_RX Audio Mixer MultiMedia1 0" "$last_log"
remaining=$(find "$FIXROOT/run" -type f | wc -l)
eq "the generated wav is removed after playback" 0 "$remaining"

# -p: best-effort protection attempt, in the background after tinyplay has
# started (the stub tinyplay sleeps so the attempt lands mid-"stream"),
# never fatal even when the set fails, and the read-back is logged.
FIXROOT=$(setup_fixture)
LOGFILE="$FIXROOT/log"
: > "$LOGFILE"
ERRLOG="$FIXROOT/stderr"
if ASOUND_DIR="$FIXROOT/asound" RUN_DIR="$FIXROOT/run" LOGFILE="$LOGFILE" PROTECT_DELAY=0.2 \
   STUB_TINYPLAY_SLEEP=0.6 PATH="$STUBDIR:$PATH" sh "$SCRIPT" -p >/dev/null 2>"$ERRLOG"; then
	ok
else
	bad "-p run should exit 0 (attempt is best-effort)"
fi
if grep -q '^tinymix -D 0 set TAS2560_ALGO_FF_MODULE ENABLE$' "$LOGFILE"; then ok; else bad "-p must attempt to set TAS2560_ALGO_FF_MODULE ENABLE"; fi
if grep -q '^tinymix -D 0 get TAS2560_ALGO_FF_MODULE$' "$LOGFILE"; then ok; else bad "-p must read TAS2560_ALGO_FF_MODULE back after the set"; fi
# Ordering: the attempt must come after tinyplay started, and tinyplay must
# still have been started with -M.
tp_line=$(grep -n '^tinyplay' "$LOGFILE" | head -n 1 | cut -d: -f1)
ff_line=$(grep -n 'FF_MODULE ENABLE' "$LOGFILE" | head -n 1 | cut -d: -f1)
if [ -n "$tp_line" ] && [ -n "$ff_line" ] && [ "$ff_line" -gt "$tp_line" ]; then ok; else bad "-p attempt must happen after tinyplay starts (tinyplay line $tp_line, attempt line $ff_line)"; fi
if grep -q 'protection: set rc=0, read back:' "$ERRLOG"; then ok; else bad "-p must log the set rc and the read-back value"; fi

# -p with the set rejected (live behaviour before the port is active): the
# failure carries tinymix's stderr and the run is still a success.
FIXROOT=$(setup_fixture)
LOGFILE="$FIXROOT/log"
: > "$LOGFILE"
ERRLOG="$FIXROOT/stderr"
if ASOUND_DIR="$FIXROOT/asound" RUN_DIR="$FIXROOT/run" LOGFILE="$LOGFILE" PROTECT_DELAY=0.2 \
   STUB_TINYPLAY_SLEEP=0.6 STUB_TINYMIX_FAIL_ON=TAS2560_ALGO_FF_MODULE \
   PATH="$STUBDIR:$PATH" sh "$SCRIPT" -p >/dev/null 2>"$ERRLOG"; then
	ok
else
	bad "-p run must still exit 0 when the protection set is rejected"
fi
if grep -q "failed: Error: invalid enum value" "$ERRLOG"; then ok; else bad "a failed tinymix set must carry tinymix's own stderr in the log line"; fi
if grep -q 'protection: set failed (best-effort' "$ERRLOG"; then ok; else bad "-p must log that the attempt failed and playback continued"; fi
last_log=$(tail -n 1 "$LOGFILE")
eq "route is still reset to 0 after a -p run" "tinymix -D 0 set TERT_MI2S_RX Audio Mixer MultiMedia1 0" "$last_log"

# -P (the old flag) is gone: rejected as an unknown option, nothing touched.
FIXROOT=$(setup_fixture)
LOGFILE="$FIXROOT/log"
: > "$LOGFILE"
if ASOUND_DIR="$FIXROOT/asound" RUN_DIR="$FIXROOT/run" LOGFILE="$LOGFILE" \
   PATH="$STUBDIR:$PATH" sh "$SCRIPT" -P >/dev/null 2>&1; then
	bad "-P must be rejected (removed; protection is off by default now)"
else
	ok
fi
eq "nothing was touched on a rejected -P" "" "$(cat "$LOGFILE")"

# -s: status only -- cards line, MultiMedia1 device, the three control
# values via tinymix get; no set, no wavtone, no tinyplay.
FIXROOT=$(setup_fixture)
LOGFILE="$FIXROOT/log"
: > "$LOGFILE"
out=$(ASOUND_DIR="$FIXROOT/asound" RUN_DIR="$FIXROOT/run" LOGFILE="$LOGFILE" \
      PATH="$STUBDIR:$PATH" sh "$SCRIPT" -s 2>/dev/null) || bad "-s must exit 0 when the card is present"
case "$out" in
*"card:  0 [sdm660snd"*) ok ;;
*) bad "-s must print the cards line, got: $out" ;;
esac
case "$out" in
*"MultiMedia1 playback: card 0 device 0"*) ok ;;
*) bad "-s must print the MultiMedia1 device, got: $out" ;;
esac
for ctl in "TERT_MI2S_RX Audio Mixer MultiMedia1" "DAC Playback Volume" "TAS2560_ALGO_FF_MODULE"; do
	case "$out" in
	*"$ctl: "*) ok ;;
	*) bad "-s must print '$ctl', got: $out" ;;
	esac
done
want_log="tinymix -D 0 get TERT_MI2S_RX Audio Mixer MultiMedia1
tinymix -D 0 get DAC Playback Volume
tinymix -D 0 get TAS2560_ALGO_FF_MODULE"
eq "-s only runs tinymix get for the three controls (no set/wavtone/tinyplay)" "$want_log" "$(cat "$LOGFILE")"
# -s without a card still exits 0 and says so (it is for inspection).
FIXROOT=$(setup_fixture)
: > "$FIXROOT/asound/cards"
LOGFILE="$FIXROOT/log"
: > "$LOGFILE"
out=$(ASOUND_DIR="$FIXROOT/asound" LOGFILE="$LOGFILE" PATH="$STUBDIR:$PATH" sh "$SCRIPT" -s 2>/dev/null) || bad "-s must exit 0 even without a card"
case "$out" in
*"card: none"*) ok ;;
*) bad "-s without a card must say 'card: none', got: $out" ;;
esac

# Route reset must still happen when tinyplay fails.
FIXROOT=$(setup_fixture)
LOGFILE="$FIXROOT/log"
: > "$LOGFILE"
if ASOUND_DIR="$FIXROOT/asound" RUN_DIR="$FIXROOT/run" LOGFILE="$LOGFILE" \
   PATH="$STUBDIR:$PATH" STUB_TINYPLAY_EXIT=1 \
   sh "$SCRIPT" >/dev/null 2>&1; then
	bad "speaker-test-tone must exit non-zero when tinyplay fails"
else
	ok
fi
last_log=$(tail -n 1 "$LOGFILE")
eq "route is still reset to 0 when tinyplay fails" "tinymix -D 0 set TERT_MI2S_RX Audio Mixer MultiMedia1 0" "$last_log"

# Refusal without a card: no tinymix/wavtone/tinyplay calls at all.
FIXROOT=$(setup_fixture)
: > "$FIXROOT/asound/cards"
LOGFILE="$FIXROOT/log"
: > "$LOGFILE"
if ASOUND_DIR="$FIXROOT/asound" RUN_DIR="$FIXROOT/run" LOGFILE="$LOGFILE" \
   PATH="$STUBDIR:$PATH" sh "$SCRIPT" >/dev/null 2>&1; then
	bad "speaker-test-tone must refuse to run without the sdm660 card"
else
	ok
fi
eq "nothing was touched when the card is missing" "" "$(cat "$LOGFILE")"

# Argument validation: garbage -f is rejected before anything runs.
FIXROOT=$(setup_fixture)
LOGFILE="$FIXROOT/log"
: > "$LOGFILE"
if ASOUND_DIR="$FIXROOT/asound" RUN_DIR="$FIXROOT/run" LOGFILE="$LOGFILE" \
   PATH="$STUBDIR:$PATH" sh "$SCRIPT" -f notanumber >/dev/null 2>&1; then
	bad "speaker-test-tone must reject a non-numeric -f"
else
	ok
fi
eq "nothing was touched when -f is invalid" "" "$(cat "$LOGFILE")"

# Amplitude clamp end to end: -a 0 (full scale) must be clamped to -6 before
# it ever reaches wavtone.
FIXROOT=$(setup_fixture)
LOGFILE="$FIXROOT/log"
: > "$LOGFILE"
ASOUND_DIR="$FIXROOT/asound" RUN_DIR="$FIXROOT/run" LOGFILE="$LOGFILE" \
	PATH="$STUBDIR:$PATH" sh "$SCRIPT" -a 0 >/dev/null 2>&1 || true
if grep -q -- '-a -6 ' "$LOGFILE"; then ok; else bad "an -a louder than -6 dBFS must be clamped to -6 before reaching wavtone"; fi

echo "PASS: $pass/$((pass + fail)) checks passed"
[ "$fail" -eq 0 ]
