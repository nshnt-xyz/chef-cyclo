#!/usr/bin/env python3
"""Host tests for tools/sns-idl-dump.py and tools/sns-reg-map.py, on
synthetic inputs built here (no stock binaries needed):

 - a minimal aarch64-shaped ELF64 with a .dynsym, a .rela.dyn of
   R_AARCH64_RELATIVE pointers and one made-up QMI service object whose
   type table references a second table by symbol, like the real ones;
 - a byte blob holding an item table and a group table in the layout
   sns-reg-map.py documents, surrounded by filler it must skip.
"""
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.normpath(os.path.join(HERE, "..", ".."))
FAILS = []


def check(cond, what):
    if not cond:
        FAILS.append(what)
        print("FAIL: %s" % what, file=sys.stderr)


def run(*args):
    p = subprocess.run([sys.executable] + list(args), capture_output=True, text=True)
    return p.returncode, p.stdout, p.stderr


class Blob:
    """Data image at vaddr == file offset 0x1000.., with pointer slots
    recorded for RELATIVE relocations."""

    def __init__(self, base):
        self.base = base
        self.data = bytearray()
        self.relocs = []    # (slot vaddr, target vaddr)

    def here(self):
        return self.base + len(self.data)

    def align(self, n):
        while len(self.data) % n:
            self.data.append(0)

    def put(self, b):
        self.align(1)
        at = self.here()
        self.data += b
        return at

    def ptr(self, target):
        self.align(8)
        at = self.here()
        self.data += b"\0" * 8          # addend lives in the reloc, as on Android
        if target:
            self.relocs.append((at, target))
        return at


def build_elf(path):
    base = 0x1000
    b = Blob(base)
    # common table: one type {u8 @0, u8 @1}
    ctype = b.put(bytes([0x00, 0x00, 0x00, 0x01, 0x20]))
    # own type 0: {u8 @0, u32 @4}
    otype = b.put(bytes([0x00, 0x00, 0x02, 0x04, 0x20]))
    # msg 0 (req): TLV1 u16 @0; last optional TLV 0x10 u8[<=8] var @8,
    # len at +4, valid byte at 2 (delta 6)
    m0 = b.put(bytes([0x01, 0x01, 0x00, 0xc6, 0x10, 0x50, 0x08, 0x08, 0x04]))
    # msg 1 (resp): TLV2 struct (common #0) @0; last TLV3 own#0[<=3] var @8
    m1 = b.put(bytes([0x02, 0x07, 0x00, 0x00, 0x01, 0x83, 0x57, 0x08, 0x03, 0x04, 0x00, 0x00]))

    b.align(8)
    ctypes = b.here()
    b.data += struct.pack("<I4x", 2)
    b.ptr(ctype)
    otypes = b.here()
    b.data += struct.pack("<I4x", 8)
    b.ptr(otype)
    omsgs = b.here()
    b.data += struct.pack("<I4x", 12)
    b.ptr(m0)
    b.data += struct.pack("<I4x", 40)
    b.ptr(m1)

    # tables: {u16 n_types, u16 n_msgs, u8 n_refs, ptr types, ptr msgs, ptr refs, ptr ranges}
    common_tt = b.here()
    b.data += struct.pack("<HHB3x", 1, 0, 0)
    b.ptr(ctypes)
    b.ptr(0)
    crefs_slot = b.ptr(0)
    b.ptr(0)
    own_tt = b.here()
    b.data += struct.pack("<HHB3x", 1, 2, 1)
    b.ptr(otypes)
    b.ptr(omsgs)
    orefs_slot = b.ptr(0)
    b.ptr(0)
    crefs = b.here()
    b.ptr(common_tt)
    orefs = b.here()
    b.ptr(own_tt)
    b.ptr(common_tt)
    b.data += b"\x06\x00\x00\x00\x01\x00\x00\x00"   # junk after the list, not relocated
    b.relocs.append((crefs_slot, crefs))
    b.relocs.append((orefs_slot, orefs))

    # message directories: {u16 qmi id, u16 table<<12|index, u16 max len}
    dreq = b.put(struct.pack("<HHH", 0x02, 0x0000, 10))
    dresp = b.put(struct.pack("<HHH", 0x02, 0x0001, 20))
    b.align(8)
    svc = b.here()
    b.data += struct.pack("<IIII3H2x", 6, 1, 0x1ab, 100, 1, 1, 0)
    b.ptr(dreq)
    b.ptr(dresp)
    b.ptr(0)
    b.ptr(own_tt)
    b.data += struct.pack("<I4x", 7)
    b.ptr(0)

    # dynstr / dynsym
    names = [b"", b"SNS_TEST_SVC_qmi_idl_service_object_v01",
             b"sns_common_qmi_idl_type_table_object_v01"]
    dynstr = bytearray()
    offs = []
    for n in names:
        offs.append(len(dynstr))
        dynstr += n + b"\0"
    dynsym = bytearray(24)
    dynsym += struct.pack("<IBBHQQ", offs[1], 0x11, 0, 1, svc, 72)
    dynsym += struct.pack("<IBBHQQ", offs[2], 0x11, 0, 1, common_tt, 40)
    rela = bytearray()
    for slot, target in b.relocs:
        rela += struct.pack("<QQq", slot, 1027, target)
    shstr = b"\0.dynsym\0.dynstr\0.rela.dyn\0.shstrtab\0.data\0"

    img = bytearray(base) + b.data
    secs = []

    def add(name, typ, content, entsize=0):
        while len(img) % 8:
            img.append(0)
        off = len(img)
        img.extend(content)
        secs.append((shstr.index(name), typ, off, len(content), entsize))

    add(b".dynsym", 11, dynsym, 24)
    add(b".dynstr", 3, dynstr)
    add(b".rela.dyn", 4, rela, 24)
    add(b".shstrtab", 3, shstr)
    while len(img) % 8:
        img.append(0)
    shoff = len(img)
    img += bytes(64)                                    # null section
    for name, typ, off, size, ent in secs:
        img += struct.pack("<IIQQQQIIQQ", name, typ, 0, 0, off, size, 0, 0, 8, ent)
    ehdr = b"\x7fELF\x02\x01\x01" + bytes(9)
    ehdr += struct.pack("<HHIQQQIHHHHHH", 3, 183, 1, 0, 64, shoff, 0, 64, 56, 1, 64,
                        len(secs) + 1, len(secs))
    phdr = struct.pack("<IIQQQQQQ", 1, 4, 0, 0, 0, len(img), len(img), 0x1000)
    img[0:64] = ehdr
    img[64:64 + 56] = phdr
    with open(path, "wb") as f:
        f.write(img)


def test_idl_dump(tmp):
    elf = os.path.join(tmp, "fake.so")
    build_elf(elf)
    rc, out, err = run(os.path.join(TOOLS, "sns-idl-dump.py"), elf, "TEST")
    check(rc == 0, "sns-idl-dump rc %d: %s" % (rc, err))
    for want in [
        "service 0x1ab idl v1.7 (lib 6) max_msg_len 100, 1 req / 1 resp / 0 ind",
        "msg 0x02 req  max_len 10    c_size 12    (self msg 0)",
        "    TLV 0x01 mandatory @0    u16",
        "    TLV 0x10 optional  @8    u8[<=8] (len u32 at +4, wire len u8) (valid u8 at 2)",
        "msg 0x02 resp max_len 20    c_size 40    (self msg 1)",
        "    TLV 0x02 mandatory @0    struct sns_common#0",
        "    TLV 0x03 mandatory @8    struct table_0x",
        "struct sns_common#0 (c_size 2)",
        "    @1    u8",
        "    @4    u32",
    ]:
        check(want in out, "sns-idl-dump output lacks %r:\n%s" % (want, out))
    rc, out, err = run(os.path.join(TOOLS, "sns-idl-dump.py"), elf, "0x1ab")
    check(rc == 0 and "service 0x1ab" in out, "select by ID")
    rc, out, err = run(os.path.join(TOOLS, "sns-idl-dump.py"), elf, "SMGR")
    check(rc == 1 and "no service object matches" in err, "unknown selector")


def build_reg_blob(path):
    items = []
    groups = [(2000, 0, 64), (2695, 256, 40), (10007, 512, 34)]
    iid = 100
    for gid, off, size in groups:
        o = off
        while o + 4 <= off + size:
            items.append((2, o, 2, 7, iid))     # u32 items
            iid += 1
            o += 4
    while len(items) < 120:                     # a real table has thousands
        items.append((0, 512 + 32 + len(items) % 2, 2, 7, iid))   # u8 items
        iid += 1
    blob = b"\xff" * 1004                       # 4- but not 20-aligned: alignment search
    for t in items:
        blob += struct.pack("<5I", *t)
    for gid, off, size in groups:
        blob += struct.pack("<3H", size, off, gid)
    blob += bytes(6) + b"\x01\x02\x04\x08" * 10
    with open(path, "wb") as f:
        f.write(blob)
    return items, groups


def test_reg_map(tmp):
    blob = os.path.join(tmp, "fake-sensors.qti")
    items, groups = build_reg_blob(blob)
    rc, out, err = run(os.path.join(TOOLS, "sns-reg-map.py"), blob)
    check(rc == 0, "sns-reg-map rc %d: %s" % (rc, err))
    lines = [l for l in out.splitlines() if not l.startswith("#")]
    check(lines[0] == "size 546", "map size line: %r" % lines[:1])
    for gid, off, size in groups:
        check("group %d %d %d" % (gid, off, size) in lines, "group %d in map" % gid)
    check(sum(1 for l in lines if l.startswith("item ")) == len(items), "item count")
    for t, off, _a, _b, iid in items:
        want = "item %d %d %d" % (iid, off, 4 if t == 2 else 1)
        check(want in lines, "map lacks %r" % want)

    # --show reads values through the map
    reg = os.path.join(tmp, "sns.reg")
    with open(reg, "wb") as f:
        f.write(bytes((i * 7 + 3) & 0xff for i in range(546)))
    rc, out, err = run(os.path.join(TOOLS, "sns-reg-map.py"), blob, "--show", reg, "101", "9")
    check(rc == 0 and "item 101 @0x4 size 4: 1f 26 2d 34" in out and "item 9: not in map" in out,
          "--show output: %s %s" % (out, err))

    # an item outside every group is rejected
    bad = os.path.join(tmp, "bad.qti")
    with open(blob, "rb") as f:
        data = bytearray(f.read())
    struct.pack_into("<5I", data, 1004, 2, 400, 2, 7, 100)   # offset 400: in no group
    with open(bad, "wb") as f:
        f.write(data)
    rc, out, err = run(os.path.join(TOOLS, "sns-reg-map.py"), bad)
    check(rc == 1 and "outside every group" in err, "item outside groups: %s" % err)

    rc, out, err = run(os.path.join(TOOLS, "sns-reg-map.py"), os.path.join(tmp, "nope"))
    check(rc == 1, "missing input")


def main():
    with tempfile.TemporaryDirectory() as tmp:
        test_idl_dump(tmp)
        test_reg_map(tmp)
    if FAILS:
        print("test_sns_tools.py: %d failure(s)" % len(FAILS))
        return 1
    print("test_sns_tools.py: all passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
