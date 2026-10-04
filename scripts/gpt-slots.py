#!/usr/bin/env python3
"""Print the A/B slot attributes of GPT entries from raw dumps of the disk.

Read-only: it parses files, for example the first 34 sectors of the eMMC
pulled from the phone (`dd if=/dev/mmcblk0 of=/run/gpt-primary.bin bs=512
count=34`) and, with --backup, the last 33 (`skip=` disk size in sectors
minus 33), which end with the backup header. Both copies must have valid
signatures and CRCs and byte-identical entry arrays. Bit layout as
Qualcomm's abl uses it in the entry attributes: 48-49 priority, 50 active,
51-53 retry count, 54 successful, 55 unbootable.
"""

import argparse
import struct
import sys
import zlib

SECTOR = 512
SLOTTED = ("boot", "system", "vendor")


def slot_flags(attr):
    a = attr >> 48
    return {
        "priority": a & 3,
        "active": (a >> 2) & 1,
        "retry": (a >> 3) & 7,
        "successful": (a >> 6) & 1,
        "unbootable": (a >> 7) & 1,
    }


def parse_header(header, my_lba):
    if header[:8] != b"EFI PART":
        raise ValueError(f"no GPT header at LBA {my_lba}")
    hsize = struct.unpack_from("<I", header, 12)[0]
    if not 92 <= hsize <= SECTOR:
        raise ValueError(f"GPT header at LBA {my_lba} has size {hsize}")
    hcrc = struct.unpack_from("<I", header, 16)[0]
    check = bytearray(header[:hsize])
    check[16:20] = b"\0\0\0\0"
    if zlib.crc32(check) != hcrc:
        raise ValueError(f"GPT header CRC mismatch at LBA {my_lba}")
    mine, alt = struct.unpack_from("<QQ", header, 24)
    if mine != my_lba:
        raise ValueError(f"GPT header at LBA {my_lba} says it is at LBA {mine}")
    lba, count, size, acrc = struct.unpack_from("<QIII", header, 72)
    return alt, lba, count, size, acrc


def entry_array(data, base_lba, lba, count, size, acrc):
    off = (lba - base_lba) * SECTOR
    array = data[off:off + count * size] if off >= 0 else b""
    if len(array) != count * size:
        raise ValueError(f"dump too short for {count} entries of {size} bytes at LBA {lba}")
    if zlib.crc32(array) != acrc:
        raise ValueError(f"GPT entry array CRC mismatch at LBA {lba}")
    return array


def primary(data):
    """(backup header LBA, entry array, entry size) of the primary GPT in a disk-head dump."""
    alt, lba, count, size, acrc = parse_header(data[SECTOR:2 * SECTOR], 1)
    return alt, entry_array(data, 0, lba, count, size, acrc), size


def backup(tail, alt_lba):
    """Entry array of the backup GPT; tail is a dump whose last sector is LBA alt_lba."""
    if not tail or len(tail) % SECTOR:
        raise ValueError("backup dump is not a whole number of sectors")
    base = alt_lba - len(tail) // SECTOR + 1
    alt, lba, count, size, acrc = parse_header(tail[-SECTOR:], alt_lba)
    if alt != 1:
        raise ValueError(f"backup GPT header points to LBA {alt}, not 1")
    return entry_array(tail, base, lba, count, size, acrc)


def entries(array, size):
    """Yield (partition number, name, attributes) of the used entries.

    The number is the entry index plus one, as the kernel numbers mmcblk0pN."""
    for i in range(len(array) // size):
        e = array[i * size:(i + 1) * size]
        if e[:16] == b"\0" * 16:
            continue
        attr = struct.unpack_from("<Q", e, 48)[0]
        name = e[56:128].decode("utf-16le").split("\0", 1)[0]
        yield i + 1, name, attr


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("dump", help="raw dump of the disk head (LBA 0 onwards)")
    ap.add_argument("--backup", metavar="TAIL", help="raw dump of the disk tail (ends at the last LBA)")
    ap.add_argument("--all", action="store_true", help="every entry, not only boot/system/vendor")
    args = ap.parse_args()
    with open(args.dump, "rb") as f:
        data = f.read()
    try:
        alt, array, size = primary(data)
        if args.backup:
            with open(args.backup, "rb") as f:
                if backup(f.read(), alt) != array:
                    raise ValueError("primary and backup entry arrays differ")
            print(f"primary and backup GPT valid and identical (backup header LBA {alt})")
    except ValueError as err:
        print(f"gpt-slots: {err}", file=sys.stderr)
        return 1
    for i, name, attr in entries(array, size):
        if not args.all and not name.startswith(SLOTTED):
            continue
        f = slot_flags(attr)
        print(f"p{i:<3d} {name:16s} attr=0x{attr:016x} " +
              " ".join(f"{k}={v}" for k, v in f.items()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
