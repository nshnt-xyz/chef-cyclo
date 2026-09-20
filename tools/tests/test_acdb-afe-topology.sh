#!/bin/sh
# Host regression test for tools/acdb-afe-topology.py: builds a synthetic
# QCMSNDDB file with the same chunk shape as the stock SDM660 .acdb files
# (AVDB container whose size field undercounts its own 4-byte length
# prefix, DPROPLUT entries pointing into DATAPOOL, UTF-16LE names, a device
# without the topology property) and checks the decoder's output; then, if
# the stock vendor image and debugfs are at hand, decodes the real
# Speaker_cal.acdb and checks the three speaker devices read 0x000112FC
# (the value tools/afe-topology-cal.c defaults to) -- skipped otherwise.
set -eu
cd "$(dirname "$0")/../.."
TOOL=tools/acdb-afe-topology.py
[ -r "$TOOL" ] || { echo "cannot find $TOOL" >&2; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "python3 is required for this test" >&2; exit 1; }

TMPROOT=$(mktemp -d /tmp/acdb-afe-topology-test.XXXXXX)
trap 'rm -rf "$TMPROOT"' EXIT

pass=0
fail=0
ok() { pass=$((pass + 1)); }
bad() { fail=$((fail + 1)); echo "FAIL: $*" >&2; }
eq() { if [ "$2" = "$3" ]; then ok; else bad "$1: expected '$2', got '$3'"; fi; }

python3 - "$TMPROOT" <<'EOF'
import struct, sys
out = sys.argv[1]

def chunk(tag, payload):
    assert len(tag) == 8
    return tag + struct.pack("<I", len(payload)) + payload

pool = b""
lut = []
def add(dev, prop, value):
    global pool
    lut.append((dev, prop, len(pool)))
    pool += struct.pack("<I", len(value)) + value

add(14, 0x113B8, "SPKR_PHONE_SPKR_MONO".encode("utf-16le"))
add(14, 0x13150, struct.pack("<I", 0x000112FC))
add(11, 0x113B8, "SPKR_PHONE_MIC".encode("utf-16le"))
add(11, 0x13150, struct.pack("<I", 0x000112FB))
add(11, 0x12EED, struct.pack("<III", 1, 48000, 0x242))
add(99, 0x113B8, "NO_TOPOLOGY_DEVICE".encode("utf-16le"))
add(7, 0x13150, struct.pack("<I", 0x000112FC))   # topology but no name
dproplut = struct.pack("<I", len(lut)) + b"".join(struct.pack("<III", *e) for e in lut)

chain = (chunk(b"MODIFIED", b"2019-11-25 15:9:20")
         + chunk(b"SWPNAME ", "SDM660.LA.1.0".encode("utf-16le"))
         + chunk(b"DPROPLUT", dproplut)
         + chunk(b"DATAPOOL", pool)
         + chunk(b"ACSWVER2", b"\0" * 16))
# Outer size field == chain length (undercounts the 4-byte prefix), as in
# the stock files.
avdb = b"AVDB\0\0\0\0" + struct.pack("<I", len(chain)) + struct.pack("<I", len(chain)) + chain
open(out + "/good.acdb", "wb").write(b"QCMSNDDB" + b"\0" * 8 + avdb)

# Same file with a DPROPLUT offset pointing past DATAPOOL.
bad_lut = struct.pack("<I", 1) + struct.pack("<III", 14, 0x13150, len(pool) + 100)
chain2 = chunk(b"DPROPLUT", bad_lut) + chunk(b"DATAPOOL", pool)
avdb2 = b"AVDB\0\0\0\0" + struct.pack("<I", len(chain2)) + struct.pack("<I", len(chain2)) + chain2
open(out + "/bad-offset.acdb", "wb").write(b"QCMSNDDB" + b"\0" * 8 + avdb2)

# No device table at all (like Global_cal.acdb).
chain3 = chunk(b"MODIFIED", b"x")
avdb3 = b"AVDB\0\0\0\0" + struct.pack("<I", len(chain3)) + struct.pack("<I", len(chain3)) + chain3
open(out + "/no-devices.acdb", "wb").write(b"QCMSNDDB" + b"\0" * 8 + avdb3)

open(out + "/not.acdb", "wb").write(b"definitely not an acdb file")
EOF

if python3 "$TOOL" "$TMPROOT/good.acdb" > "$TMPROOT/out" 2> "$TMPROOT/err"; then ok; else bad "decoding the synthetic file must succeed: $(cat "$TMPROOT/err")"; fi
eq "header line: software package, modified date, device count" \
	"$TMPROOT/good.acdb: SDM660.LA.1.0, modified 2019-11-25 15:9:20, 4 device(s)" "$(head -n 1 "$TMPROOT/out")"
eq "devices sorted by id with name and topology; missing name/topology flagged" \
	"  device   7 (0x07) ?                                    AFE topology 0x000112FC
  device  11 (0x0b) SPKR_PHONE_MIC                       AFE topology 0x000112FB
  device  14 (0x0e) SPKR_PHONE_SPKR_MONO                 AFE topology 0x000112FC
  device  99 (0x63) NO_TOPOLOGY_DEVICE                   AFE topology (none)" "$(tail -n +2 "$TMPROOT/out")"

python3 "$TOOL" -v "$TMPROOT/good.acdb" > "$TMPROOT/out" 2>&1 || bad "verbose decode must succeed"
if grep -q '^      property 0x12eed len  12 0100000080bb000042020000$' "$TMPROOT/out"; then ok; else bad "verbose mode must hex-dump every property"; fi
if grep -q '^      property 0x13150 len   4 fc120100$' "$TMPROOT/out"; then ok; else bad "verbose mode must show the topology property bytes little-endian"; fi

if python3 "$TOOL" "$TMPROOT/bad-offset.acdb" > "$TMPROOT/out" 2> "$TMPROOT/err"; then bad "an offset outside DATAPOOL must fail"; else ok; fi
case "$(cat "$TMPROOT/err")" in
*"outside DATAPOOL"*) ok ;;
*) bad "bad offset must be reported, got: $(cat "$TMPROOT/err")" ;;
esac
if python3 "$TOOL" "$TMPROOT/no-devices.acdb" > /dev/null 2> "$TMPROOT/err"; then bad "a file without DPROPLUT must fail"; else ok; fi
case "$(cat "$TMPROOT/err")" in
*"no DPROPLUT/DATAPOOL chunk"*) ok ;;
*) bad "missing device table must be reported, got: $(cat "$TMPROOT/err")" ;;
esac
if python3 "$TOOL" "$TMPROOT/not.acdb" > /dev/null 2> "$TMPROOT/err"; then bad "a non-ACDB file must fail"; else ok; fi
case "$(cat "$TMPROOT/err")" in
*"not an ACDB file"*) ok ;;
*) bad "bad magic must be reported, got: $(cat "$TMPROOT/err")" ;;
esac
if python3 "$TOOL" "$TMPROOT/missing.acdb" > /dev/null 2>&1; then bad "a missing file must fail"; else ok; fi
# One bad file among good ones: still decodes the good ones, exits 1.
if python3 "$TOOL" "$TMPROOT/good.acdb" "$TMPROOT/not.acdb" > "$TMPROOT/out" 2>/dev/null; then bad "exit must be 1 with a bad file in the list"; else ok; fi
if grep -q 'SPKR_PHONE_SPKR_MONO' "$TMPROOT/out"; then ok; else bad "good files must still be decoded alongside a bad one"; fi

# The decoder must never open anything for writing.
if grep -Eq 'open\([^)]*"[wa]' "$TOOL"; then bad "decoder must not open files for writing"; else ok; fi

# ---- the real thing, when available (stock image + debugfs) ----
VENDOR_IMG=stock/partitions/vendor_a.img
if [ -r "$VENDOR_IMG" ] && command -v debugfs >/dev/null 2>&1; then
	debugfs -R "dump /etc/acdbdata/Speaker_cal.acdb $TMPROOT/Speaker_cal.acdb" "$VENDOR_IMG" >/dev/null 2>&1 || true
	if [ -s "$TMPROOT/Speaker_cal.acdb" ]; then
		if python3 "$TOOL" "$TMPROOT/Speaker_cal.acdb" > "$TMPROOT/out" 2> "$TMPROOT/err"; then ok; else bad "stock Speaker_cal.acdb must decode: $(cat "$TMPROOT/err")"; fi
		eq "stock speaker devices 14, 15 and 202 all carry AFE topology 0x000112FC" \
			"  device  14 (0x0e) SPKR_PHONE_SPKR_MONO                 AFE topology 0x000112FC
  device  15 (0x0f) SPKR_PHONE_SPKR_VOICE                AFE topology 0x000112FC
  device 202 (0xca) SPKR_PHONE_SPKR_VOLTE_NB             AFE topology 0x000112FC" \
			"$(grep -E 'device +(14|15|202) ' "$TMPROOT/out")"
		eq "stock speaker-file mic devices all carry 0x000112FB" "7" "$(grep -c 'AFE topology 0x000112FB' "$TMPROOT/out")"
		eq "afe-topology-cal's default RX topology matches the stock value" "0x000112FCU" \
			"$(sed -n 's/^#define AFE_TOP_DEFAULT_RX_TOPOLOGY \([0-9A-Fx]*U\).*/\1/p' tools/afe-topology-cal.c)"
	else
		echo "note: could not dump Speaker_cal.acdb from $VENDOR_IMG; skipping the stock check" >&2
	fi
else
	echo "note: $VENDOR_IMG or debugfs unavailable; skipping the stock check" >&2
fi

echo "PASS: $pass/$((pass + fail)) checks passed"
[ "$fail" -eq 0 ]
