#!/usr/bin/env python3
"""Extract the sensors registry layout (REG2 item/group ID -> byte range in
sns.reg) from a stock sensors.qti and write it as the text map sensord
loads (-m). Read-only, standard library only.

Why: the ADSP's sensor manager reads its configuration (driver UUIDs, I2C
bus/address, axis orientation, calibration) from the apps-side REG2 QMI
service (0x10f). Stock serves it from sensors.qti, backed by
/persist/sensors/sns.reg; tools/sensord serves it instead, from a RAM copy
of the same file. The file has no self-describing structure: which bytes
belong to which item or group is compiled into sensors.qti. That table is
vendor data and is not committed; scripts/mkinitramfs.sh runs this script
at image build time against stock/partitions/vendor_a.img, the same way it
pulls the Bluetooth firmware. Run by hand:

    debugfs -R "dump /bin/sensors.qti /tmp/sensors.qti" \\
        stock/partitions/vendor_a.img
    python3 tools/sns-reg-map.py /tmp/sensors.qti > /tmp/sns_reg.map
    python3 tools/sns-reg-map.py /tmp/sensors.qti --show /path/to/sns.reg 2309 2310

Layout recovered from the stock aarch64 build (sha256 3acd39d7...; the
lookup functions at 0x2cc10/0x2cc5c walk these tables, and two leaf
functions return their counts, 2561 and 108):

  item table:  20-byte records {u32 type, u32 file_offset, u32 ?, u32 ?,
               u32 item_id}; the two middle words are not needed to serve
               the registry (always 2, and a small per-item number).
  group table: 6-byte records {u16 size, u16 file_offset, u16 group_id},
               immediately after the item table. Groups sit in 256-byte
               slots; every item lies inside exactly one group.
  type sizes:  a byte table 01 02 04 08 01 02 04 08 04 04 in .rodata
               (u8 u16 u32 u64 s8 s16 s32 s64 q16 and one more 4-byte type).

The tables are located by content, not by fixed address: the longest run
of plausible item records, followed by plausible group records, then
cross-checked (unique IDs, items inside groups, ranges inside the file
size the groups imply). --items OFF:COUNT / --groups OFF:COUNT override
the search for a different build.

Map format (one record per line, decimal, '#' comments):
    size <total sns.reg bytes>
    group <id> <offset> <size>
    item <id> <offset> <size>
"""
import argparse
import hashlib
import struct
import sys

TYPE_SIZES = bytes([1, 2, 4, 8, 1, 2, 4, 8, 4, 4])
ITEM_REC = 20
GROUP_REC = 6
MAX_FILE = 0x10000


class MapError(Exception):
    pass


def item_ok(rec):
    t, off, _a, _b, iid = rec
    return t < len(TYPE_SIZES) and off < MAX_FILE and iid < 0x10000


def find_items(d):
    """Longest run of 20-byte records that look like item entries, at any
    4-byte alignment. Returns (offset, count)."""
    best = (0, 0)
    for align in range(0, ITEM_REC, 4):
        run_start, run = None, 0
        for off in range(align, len(d) - ITEM_REC, ITEM_REC):
            if item_ok(struct.unpack_from("<5I", d, off)):
                if run_start is None:
                    run_start, run = off, 0
                run += 1
                if run > best[1]:
                    best = (run_start, run)
            else:
                run_start = None
    return best


def read_groups(d, off, count=None):
    groups = []
    while count is None or len(groups) < count:
        if off + GROUP_REC * (len(groups) + 1) > len(d):
            break
        size, goff, gid = struct.unpack_from("<3H", d, off + GROUP_REC * len(groups))
        if count is None and (size == 0 or size > 256 or goff % 256):
            break
        groups.append((gid, goff, size))
    return groups


def parse_span(s):
    off, _, cnt = s.partition(":")
    return int(off, 0), int(cnt, 0)


def extract(d, items_span=None, groups_span=None):
    if items_span:
        ioff, icount = items_span
    else:
        ioff, icount = find_items(d)
    if icount < 100:
        raise MapError("no item table found (best run %d records)" % icount)
    recs = [struct.unpack_from("<5I", d, ioff + ITEM_REC * i) for i in range(icount)]
    items = [(r[4], r[1], TYPE_SIZES[r[0]]) for r in recs]
    if groups_span:
        groups = read_groups(d, groups_span[0], groups_span[1])
    else:
        groups = read_groups(d, ioff + ITEM_REC * icount)
    if not groups:
        raise MapError("no group table after the item table at 0x%x" % ioff)

    if len({i[0] for i in items}) != len(items):
        raise MapError("duplicate item IDs")
    if len({g[0] for g in groups}) != len(groups):
        raise MapError("duplicate group IDs")
    total = max(off + size for _gid, off, size in groups)
    spans = sorted((off, off + size) for _gid, off, size in groups)
    for (a0, a1), (b0, _b1) in zip(spans, spans[1:]):
        if a1 > b0:
            raise MapError("groups overlap at 0x%x" % b0)
    for iid, off, size in items:
        if not any(g0 <= off and off + size <= g1 for g0, g1 in spans):
            raise MapError("item %d (0x%x+%d) is outside every group" % (iid, off, size))
    return ioff, groups, items, total


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("sensors_qti")
    ap.add_argument("--items", type=parse_span, help="item table OFF:COUNT (file offset)")
    ap.add_argument("--groups", type=parse_span, help="group table OFF:COUNT")
    ap.add_argument("--show", metavar="SNS_REG",
                    help="print the listed item IDs' values from this sns.reg instead")
    ap.add_argument("ids", nargs="*", type=int)
    args = ap.parse_args()
    try:
        d = open(args.sensors_qti, "rb").read()
        ioff, groups, items, total = extract(d, args.items, args.groups)
    except (MapError, OSError, struct.error) as e:
        print("sns-reg-map: %s" % e, file=sys.stderr)
        return 1
    if args.show:
        reg = open(args.show, "rb").read()
        if len(reg) != total:
            print("sns-reg-map: warning: %s is %d bytes, map expects %d"
                  % (args.show, len(reg), total), file=sys.stderr)
        byid = {i[0]: i for i in items}
        for iid in args.ids:
            if iid not in byid:
                print("item %d: not in map" % iid)
                continue
            _, off, size = byid[iid]
            raw = reg[off:off + size]
            print("item %d @0x%x size %d: %s (u%d LE %d)" % (
                iid, off, size, raw.hex(" "), size * 8, int.from_bytes(raw, "little")))
        return 0
    print("# sns_reg.map: REG2 registry layout for sensord, generated by")
    print("# tools/sns-reg-map.py from sensors.qti sha256 %s" % hashlib.sha256(d).hexdigest())
    print("# item table at file offset 0x%x (%d items), %d groups" % (ioff, len(items), len(groups)))
    print("size %d" % total)
    for gid, off, size in groups:
        print("group %d %d %d" % (gid, off, size))
    for iid, off, size in sorted(items):
        print("item %d %d %d" % (iid, off, size))
    return 0


if __name__ == "__main__":
    sys.exit(main())
