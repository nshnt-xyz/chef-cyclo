#!/usr/bin/env python3
"""Print the per-device AFE topology (ACDB device property 0x13150) of a
Qualcomm .acdb file, read-only, standard library only.

Why: stock Android installs an AFE topology for every audio device before
the first port start (libacdbloader -> AUDIO_SET_CALIBRATION of
AFE_TOPOLOGY_CAL_TYPE). This build has no ACDB loader, so
tools/afe-topology-cal.c installs one block by hand; the value it defaults
to (0x000112FC for the speaker devices) must stay reproducible from the
stock partition rather than be a number someone once typed. Run:

    debugfs -R "dump /etc/acdbdata/Speaker_cal.acdb /tmp/Speaker_cal.acdb" \\
        stock/partitions/vendor_a.img
    python3 tools/acdb-afe-topology.py /tmp/Speaker_cal.acdb

File format (QCMSNDDB, as found in the stock SDM660 vendor image and
cross-checked against libaudcal.so's AcdbCmdGetAfeTopId): an 8-byte magic
"QCMSNDDB" + 8 bytes, then a chain of chunks, each an 8-byte ASCII tag, a
little-endian u32 payload size and the payload. "AVDB" is a container:
its payload is a u32 chain length followed by the nested chunks. Of those,
"DPROPLUT" (device property lookup table) is a u32 entry count followed by
12-byte entries (device id, property id, offset), each offset pointing into
the "DATAPOOL" chunk's payload at a u32 length + value. Property 0x113B8
is the device's UTF-16LE name, 0x13150 its AFE topology id (u32). Every
other property is printed as hex with -v; nothing is interpreted beyond
those two, and nothing is written.
"""
import argparse
import struct
import sys

MAGIC = b"QCMSNDDB"
PROP_DEVICE_NAME = 0x113B8
PROP_AFE_TOPOLOGY = 0x13150


class AcdbError(Exception):
    pass


def iter_chunks(buf, start, end, lenient=False):
    """Yield (tag, payload_offset, payload_size) for the chunk chain in
    buf[start:end]. A chunk that does not fit is an error, except at the
    top level (lenient), where the AVDB container's size field is 4 bytes
    short and its real extent comes from its own length prefix."""
    off = start
    while off + 12 <= end:
        tag = buf[off:off + 8]
        size = struct.unpack_from("<I", buf, off + 8)[0]
        if off + 12 + size > end and not lenient:
            raise AcdbError("chunk %r at 0x%x claims %d bytes past the end"
                            % (tag, off, size))
        yield tag, off + 12, size
        off += 12 + size


def parse(buf):
    """Return {'header': {tag: bytes}, 'chunks': {tag: (offset, size)}}
    for the AVDB container's nested chunks."""
    if buf[:8] != MAGIC:
        raise AcdbError("not an ACDB file (magic %r)" % buf[:8])
    chunks = {}
    for tag, off, size in iter_chunks(buf, 16, len(buf), lenient=True):
        if tag.rstrip(b"\0") == b"AVDB":
            # The container's payload is a u32 chain length followed by
            # the nested chain (the outer size field repeats that length,
            # so it undercounts its own 4-byte prefix: Speaker_cal.acdb
            # has 0x2EF0C twice, and the chain does run 0x2EF0C bytes
            # from offset 0x20 to the end of the file).
            inner = struct.unpack_from("<I", buf, off)[0]
            if off + 4 + inner > len(buf):
                raise AcdbError("AVDB chain length %d runs past the end of the file" % inner)
            for tag2, off2, size2 in iter_chunks(buf, off + 4, off + 4 + inner):
                chunks[tag2.rstrip(b" \0").decode("ascii", "replace")] = (off2, size2)
        else:
            chunks[tag.rstrip(b" \0").decode("ascii", "replace")] = (off, size)
    return chunks


def text_chunk(buf, chunks, name):
    if name not in chunks:
        return None
    off, size = chunks[name]
    raw = buf[off:off + size]
    if size % 2 == 0 and raw[1::2] == b"\0" * (size // 2):
        return raw.decode("utf-16le", "replace")
    return raw.decode("ascii", "replace")


def device_properties(buf, chunks):
    """Return an ordered {device_id: [(property_id, value_bytes), ...]}."""
    if "DPROPLUT" not in chunks or "DATAPOOL" not in chunks:
        raise AcdbError("no DPROPLUT/DATAPOOL chunk (not a device calibration file?)")
    lut_off, lut_size = chunks["DPROPLUT"]
    pool_off, pool_size = chunks["DATAPOOL"]
    count = struct.unpack_from("<I", buf, lut_off)[0]
    if 4 + count * 12 > lut_size:
        raise AcdbError("DPROPLUT count %d does not fit its %d-byte payload" % (count, lut_size))
    devices = {}
    for i in range(count):
        dev, prop, off = struct.unpack_from("<III", buf, lut_off + 4 + 12 * i)
        if off + 4 > pool_size:
            raise AcdbError("device %d property 0x%x offset 0x%x outside DATAPOOL" % (dev, prop, off))
        length = struct.unpack_from("<I", buf, pool_off + off)[0]
        if off + 4 + length > pool_size:
            raise AcdbError("device %d property 0x%x length %d outside DATAPOOL" % (dev, prop, length))
        value = buf[pool_off + off + 4:pool_off + off + 4 + length]
        devices.setdefault(dev, []).append((prop, value))
    return devices


def device_name(props):
    for prop, value in props:
        if prop == PROP_DEVICE_NAME:
            return value.decode("utf-16le", "replace")
    return "?"


def afe_topology(props):
    for prop, value in props:
        if prop == PROP_AFE_TOPOLOGY and len(value) == 4:
            return struct.unpack("<I", value)[0]
    return None


def report(path, verbose, out):
    with open(path, "rb") as f:
        buf = f.read()
    chunks = parse(buf)
    out.write("%s: %s, modified %s, %d device(s)\n" % (
        path, text_chunk(buf, chunks, "SWPNAME") or "?",
        text_chunk(buf, chunks, "MODIFIED") or "?",
        len(device_properties(buf, chunks))))
    for dev, props in sorted(device_properties(buf, chunks).items()):
        topo = afe_topology(props)
        out.write("  device %3d (0x%02x) %-36s AFE topology %s\n" % (
            dev, dev, device_name(props),
            "0x%08X" % topo if topo is not None else "(none)"))
        if verbose:
            for prop, value in props:
                out.write("      property 0x%05x len %3d %s\n" % (
                    prop, len(value), value[:32].hex() + ("..." if len(value) > 32 else "")))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("acdb", nargs="+", help=".acdb file(s) to decode")
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="also list every device property as hex")
    args = ap.parse_args(argv)
    rc = 0
    for path in args.acdb:
        try:
            report(path, args.verbose, sys.stdout)
        except (OSError, AcdbError, struct.error) as e:
            sys.stderr.write("acdb-afe-topology: %s: %s\n" % (path, e))
            rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
