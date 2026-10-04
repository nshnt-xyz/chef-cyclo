#!/bin/sh
# Temporarily boot an image on the phone (`fastboot boot`, nothing flashed)
# and wait for its USB network (172.16.42.1).
#
#   scripts/phone-boot.sh [IMAGE]          default out/boot.img
#   EXPECT=<sha256> scripts/phone-boot.sh  refuse unless IMAGE has that hash
#
# Gets the phone into fastboot from wherever it is: already in fastboot,
# stock Android over adb (`adb reboot bootloader`; Android is retired since
# 2026-10-04), or one of our images on 172.16.42.1, the installed boot_a or
# a test image (`btprobe restart bootloader` over telnet). If the first
# `fastboot boot` fails (seen as garbled getvar replies / "unknown command"
# right after adb reboot), it runs `fastboot reboot bootloader` and retries
# once. See docs/live-testing.md.
set -eu
cd "$(dirname "$0")/.."
IMAGE=${1:-out/boot.img}
[ -f "$IMAGE" ] || { echo "no such image: $IMAGE" >&2; exit 1; }
SUM=$(sha256sum "$IMAGE" | cut -d' ' -f1)
echo "$IMAGE sha256 $SUM"
if [ -n "${EXPECT:-}" ] && [ "$SUM" != "$EXPECT" ]; then
    echo "hash mismatch: expected $EXPECT" >&2
    exit 1
fi

in_fastboot() { fastboot devices 2>/dev/null | grep -q fastboot; }

if in_fastboot; then
    echo "phone already in fastboot"
elif adb devices 2>/dev/null | grep -q 'device$'; then
    echo "stock Android: adb reboot bootloader"
    adb reboot bootloader
elif ping -c1 -W1 172.16.42.1 >/dev/null 2>&1; then
    echo "our image on 172.16.42.1: btprobe restart bootloader"
    python3 scripts/phone.py -t 20 run '(sleep 2; btprobe restart bootloader) </dev/null >/dev/null 2>&1 & sleep 1'
else
    echo "phone not in fastboot, adb or on 172.16.42.1" >&2
    exit 1
fi

wait_fastboot() {
    i=0
    until in_fastboot; do
        i=$((i + 1))
        [ $i -le 90 ] || { echo "no fastboot device after 90 s" >&2; return 1; }
        sleep 1
    done
}

wait_fastboot
if ! fastboot boot "$IMAGE"; then
    echo "fastboot boot failed; fastboot reboot bootloader and retry once"
    fastboot reboot bootloader || true
    sleep 5
    wait_fastboot
    fastboot boot "$IMAGE"
fi
echo "booted at $(date -u +%FT%TZ); waiting for 172.16.42.1"

i=0
until ping -c1 -W1 172.16.42.1 >/dev/null 2>&1; do
    i=$((i + 1))
    [ $i -le 120 ] || { echo "no network from the booted image after ~2 min" >&2; exit 1; }
    sleep 1
done
echo "network up at $(date -u +%FT%TZ)"
