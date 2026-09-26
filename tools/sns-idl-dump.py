#!/usr/bin/env python3
"""Dump the QMI IDL service objects and type tables compiled into a stock
Qualcomm sensors binary (libsensor1.so, sensors.qti), read-only, standard
library only.

Why: tools/sensord speaks the Sensors1 QMI services (SMGR 0x100, REG2
0x10f, TIME2 0x118, SAM_*) to the ADSP. No public headers describe them,
but every QMI client/server binary carries the IDL tables its generated
code was built from. This decoder prints them as plain text so sensord's
own C message definitions (tools/sensord/sns_msgs.h) can be written from,
and re-checked against, the stock binaries. Run:

    debugfs -R "dump /lib64/libsensor1.so /tmp/libsensor1.so" \\
        stock/partitions/vendor_a.img
    python3 tools/sns-idl-dump.py /tmp/libsensor1.so SMGR REG2 TIME2

Service selectors match the symbol name (SNS_<NAME>_SVC_...) or a service
ID (0x100). With no selector every service object in the file is listed.

Layout recovered from the aarch64 binaries (little endian, LP64):

  service object (symbol *_qmi_idl_service_object_vNN):
    u32 library_version, u32 idl_version, u32 service_id, u32 max_msg_len,
    u16 n_msgs[3] (req, resp, ind), pad, ptr msg_dir[3], ptr type_table,
    u32 idl_minor_version, ptr parent
  msg_dir entry: u16 qmi_msg_id, u16 ref (table << 12 | index), u16 max_len
  type table: u16 n_types, u16 n_messages, u8 n_refs, ptr types,
    ptr messages, ptr ref_tables (ref_tables[0] is the table itself)
  types[] / messages[]: u32 c_struct_size, ptr encoded_data

  encoded message: a TLV list. First byte bit7 = last TLV, bit6 =
    optional. Optional: low 6 bits = offset of the field minus offset of
    its *_valid byte, then a TLV type byte. Mandatory: low 6 bits are the
    TLV type. One field descriptor follows.
  encoded type (struct): field descriptors, terminated by 0x20.
  field descriptor: flags byte (bit7 offset is u16, bit6 array, bit5 array
    length is u16, bit4 variable length, bit3 extended (one more flags
    byte follows; not seen in the sensor services), low 3 bits = kind:
    0..3 = 1/2/4/8 byte integer, 4/5 = 1/2 byte enum, 6 = string,
    7 = aggregate), C offset (u8 or u16), then for arrays/strings the max
    length (u8 or u16), for variable arrays the distance from the array to
    its u32 *_len field (always u8), and for aggregates a type
    reference as two bytes (index, ref_table).

Relocations: pointers are resolved through .rela.dyn (RELATIVE and
symbol relocations), so the stripped, PIC stock builds decode as is.
Nothing is written; no vendor source or header text is reproduced.
"""
import argparse
import hashlib
import re
import struct
import sys

R_AARCH64_ABS64 = 257
R_AARCH64_GLOB_DAT = 1025
R_AARCH64_RELATIVE = 1027

KINDS = {0: "u8", 1: "u16", 2: "u32", 3: "u64", 4: "enum8", 5: "enum16",
         6: "string", 7: "struct"}
KIND_SIZE = {0: 1, 1: 2, 2: 4, 3: 8, 4: 1, 5: 2}
F_OFF16 = 0x80
F_ARRAY = 0x40
F_SZ16 = 0x20
F_VAR = 0x10
F_EXT = 0x08
TYPE_END = 0x20
TLV_LAST = 0x80
TLV_OPT = 0x40


class IdlError(Exception):
    pass


class Elf:
    """Just enough of a little-endian ELF64 reader for data lookups."""

    def __init__(self, path):
        d = self.d = open(path, "rb").read()
        if d[:4] != b"\x7fELF" or d[4] != 2 or d[5] != 1:
            raise IdlError("%s: not a little-endian ELF64 file" % path)
        phoff, shoff = struct.unpack_from("<QQ", d, 0x20)
        phentsize, phnum, shentsize, shnum, shstrndx = \
            struct.unpack_from("<HHHHH", d, 0x36)
        self.segs = []
        for i in range(phnum):
            p = struct.unpack_from("<IIQQQQQQ", d, phoff + i * phentsize)
            if p[0] == 1:
                self.segs.append((p[3], p[2], p[5]))
        shs = [struct.unpack_from("<IIQQQQIIQQ", d, shoff + i * shentsize)
               for i in range(shnum)]
        stroff = shs[shstrndx][4]
        self.secs = {}
        for sh in shs:
            self.secs[self._str(stroff + sh[0])] = sh
        dynsym, dynstr = self.secs[".dynsym"], self.secs[".dynstr"]
        self.syms = {}
        symtab = []
        for i in range(dynsym[5] // 24):
            name, _info, _other, _shndx, value, _size = \
                struct.unpack_from("<IBBHQQ", d, dynsym[4] + i * 24)
            nm = self._str(dynstr[4] + name)
            symtab.append((nm, value))
            if value:
                self.syms[nm] = value
        self.names = {v: k for k, v in self.syms.items()}
        self.relocs = {}
        for sec in (".rela.dyn",):
            sh = self.secs.get(sec)
            if not sh:
                continue
            for i in range(sh[5] // 24):
                off, info, add = struct.unpack_from("<QQq", d, sh[4] + i * 24)
                rtype, sym = info & 0xffffffff, info >> 32
                if rtype == R_AARCH64_RELATIVE:
                    self.relocs[off] = add
                elif rtype in (R_AARCH64_ABS64, R_AARCH64_GLOB_DAT):
                    nm, value = symtab[sym]
                    # An undefined symbol is another library's table;
                    # keep its name so the reference is still printable.
                    self.relocs[off] = value + add if value else nm

    def _str(self, off):
        return self.d[off:self.d.index(b"\0", off)].decode("latin1")

    def off(self, va):
        for base, foff, size in self.segs:
            if base <= va < base + size:
                return foff + va - base
        raise IdlError("address 0x%x outside file-backed segments" % va)

    def u8(self, va):
        return self.d[self.off(va)]

    def u16(self, va):
        return struct.unpack_from("<H", self.d, self.off(va))[0]

    def u32(self, va):
        return struct.unpack_from("<I", self.d, self.off(va))[0]

    def ptr(self, va):
        if va in self.relocs:
            return self.relocs[va]
        return struct.unpack_from("<Q", self.d, self.off(va))[0]


class Field:
    def __init__(self):
        self.kind = 0
        self.offset = 0
        self.array = False
        self.var = False
        self.sz16 = False
        self.maxlen = None
        self.len_delta = None
        self.ref = None      # (table address or name, index)
        self.ext = None

    def describe(self, elf, name_of):
        if self.kind == 7:
            base = "struct %s" % name_of(self.ref)
        else:
            base = KINDS[self.kind]
        if self.kind == 6:
            s = "%s[%d]" % (base, self.maxlen)
        elif self.array and self.var:
            s = "%s[<=%d] (len u32 at +%d, wire len %s)" % (
                base, self.maxlen, self.offset - self.len_delta,
                "u16" if self.sz16 else "u8")
        elif self.array:
            s = "%s[%d]" % (base, self.maxlen)
        else:
            s = base
        s = "@%-4d %s" % (self.offset, s)
        if self.ext is not None:
            s += " ext=0x%02x" % self.ext
        return s


def parse_field(elf, va, table, refs):
    """Decode one field descriptor at va. Returns (Field, next va)."""
    f = Field()
    flags = elf.u8(va)
    va += 1
    f.kind = flags & 0x07
    f.array = bool(flags & F_ARRAY)
    f.var = bool(flags & F_VAR)
    f.sz16 = bool(flags & F_SZ16)
    if flags & F_EXT:
        f.ext = elf.u8(va)
        va += 1
    if flags & F_OFF16:
        f.offset = elf.u16(va)
        va += 2
    else:
        f.offset = elf.u8(va)
        va += 1
    if f.array or f.kind == 6:
        if f.sz16:
            f.maxlen = elf.u16(va)
            va += 2
        else:
            f.maxlen = elf.u8(va)
            va += 1
    if f.array and f.var:
        f.len_delta = elf.u8(va)
        va += 1
    if f.kind == 7:
        idx, tno = elf.u8(va), elf.u8(va + 1)
        va += 2
        if tno >= len(refs):
            raise IdlError("type ref table %d out of range at 0x%x" % (tno, va))
        f.ref = (refs[tno], idx)
    return f, va


def table_refs(elf, table):
    """ref_tables[0] is the table itself. The u8 count in the header does
    not bound the list (SMGR's says 1 but references three tables), so read
    slots while they are relocated pointers, i.e. real addresses."""
    pr = elf.ptr(table + 24)
    refs = []
    while pr + 8 * len(refs) in elf.relocs and len(refs) < 64:
        refs.append(elf.ptr(pr + 8 * len(refs)))
    return refs


class Dumper:
    def __init__(self, elf):
        self.elf = elf
        self.types_seen = []
        self.types_done = set()

    def table_name(self, table):
        if isinstance(table, str):
            return table
        nm = self.elf.names.get(table)
        if nm:
            return re.sub(r"_qmi_idl_type_table_object_v\d+$", "", nm)
        return "table_0x%x" % table

    def name_of(self, ref):
        table, idx = ref
        if ref not in self.types_done and ref not in self.types_seen:
            self.types_seen.append(ref)
        return "%s#%d" % (self.table_name(table), idx)

    def decode_type(self, table, idx):
        elf = self.elf
        if isinstance(table, str):
            return None, ["(defined in another library)"]
        nt = elf.u16(table)
        if idx >= nt:
            raise IdlError("type %d beyond table 0x%x (%d types)" % (idx, table, nt))
        refs = table_refs(elf, table)
        entry = elf.ptr(table + 8) + 16 * idx
        size = elf.u32(entry)
        va = elf.ptr(entry + 8)
        lines = []
        while elf.u8(va) != TYPE_END:
            f, va = parse_field(elf, va, table, refs)
            lines.append(f.describe(elf, self.name_of))
        return size, lines

    def decode_msg(self, table, idx):
        elf = self.elf
        refs = table_refs(elf, table)
        entry = elf.ptr(table + 16) + 16 * idx
        size = elf.u32(entry)
        va = elf.ptr(entry + 8)
        lines = []
        if not va:
            return size, lines
        while True:
            b = elf.u8(va)
            va += 1
            if b & TLV_OPT:
                valid_delta = b & 0x3f
                tlv = elf.u8(va)
                va += 1
            else:
                valid_delta = None
                tlv = b & 0x3f
            f, va = parse_field(elf, va, table, refs)
            desc = f.describe(elf, self.name_of)
            if valid_delta is None:
                lines.append("TLV 0x%02x mandatory %s" % (tlv, desc))
            else:
                lines.append("TLV 0x%02x optional  %s (valid u8 at %d)" % (
                    tlv, desc, f.offset - valid_delta))
            if b & TLV_LAST:
                break
        return size, lines

    def service(self, sym):
        elf = self.elf
        va = elf.syms[sym]
        libver, idlver, sid, maxlen = struct.unpack_from(
            "<4I", elf.d, elf.off(va))
        nmsgs = struct.unpack_from("<3H", elf.d, elf.off(va + 16))
        table = elf.ptr(va + 48)
        minor = elf.u32(va + 56)
        refs = table_refs(elf, table)
        out = ["=" * 72,
               "%s" % sym,
               "service 0x%x idl v%d.%d (lib %d) max_msg_len %d, %d req / %d resp / %d ind" % (
                   sid, idlver, minor, libver, maxlen, *nmsgs)]
        kinds = ("req", "resp", "ind")
        rows = {}
        for k in range(3):
            d = elf.ptr(va + 24 + 8 * k)
            for j in range(nmsgs[k]):
                mid, ref, mlen = struct.unpack_from("<3H", elf.d, elf.off(d + 6 * j))
                rows.setdefault(mid, []).append((kinds[k], ref, mlen))
        for mid in sorted(rows):
            for kind, ref, mlen in rows[mid]:
                tno, idx = ref >> 12, ref & 0xfff
                t = refs[tno]
                where = "self" if tno == 0 else self.table_name(t)
                if isinstance(t, str):
                    out.append("msg 0x%02x %-4s max_len %-5d -> %s msg %d (external)" % (
                        mid, kind, mlen, where, idx))
                    continue
                size, lines = self.decode_msg(t, idx)
                out.append("msg 0x%02x %-4s max_len %-5d c_size %-5d (%s msg %d)%s" % (
                    mid, kind, mlen, size, where, idx, "" if lines else " no TLVs"))
                out += ["    " + l for l in lines]
        out.append("")
        return out

    def types(self):
        out = []
        while self.types_seen:
            ref = self.types_seen.pop(0)
            if ref in self.types_done:
                continue
            self.types_done.add(ref)
            size, lines = self.decode_type(*ref)
            label = "%s#%d" % (self.table_name(ref[0]), ref[1])
            out.append("struct %s%s" % (label, "" if size is None else " (c_size %d)" % size))
            out += ["    " + l for l in lines]
        return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("elf", help="stock libsensor1.so or sensors.qti (aarch64)")
    ap.add_argument("select", nargs="*",
                    help="service names (SMGR, REG2, SAM_AMD) or IDs (0x100)")
    args = ap.parse_args()
    try:
        elf = Elf(args.elf)
        svcs = []
        for sym, va in elf.syms.items():
            m = re.match(r"SNS_(\w+)_SVC_qmi_idl_service_object_v(\d+)$", sym)
            if not m:
                continue
            sid = elf.u32(va + 8)
            svcs.append((sid, sym, m.group(1)))
        svcs.sort()
        if args.select:
            want = []
            for sel in args.select:
                hit = [s for s in svcs if s[2] == sel.upper()
                       or (sel.lower().startswith("0x") and s[0] == int(sel, 16))]
                if not hit:
                    raise IdlError("no service object matches %r" % sel)
                want += [h for h in hit if h not in want]
            svcs = want
        dumper = Dumper(elf)
        out = ["# sns-idl-dump of %s (sha256 %s)" % (
                   args.elf.rsplit("/", 1)[-1], hashlib.sha256(elf.d).hexdigest()),
               "# @N = C struct offset in the generated vendor struct (only"
               " meaningful for", "# *_len/*_valid bookkeeping); wire order is"
               " TLV order, fields packed little endian.", ""]
        for _sid, sym, _name in svcs:
            out += dumper.service(sym)
        types = dumper.types()
        if types:
            out += ["=" * 72, "Referenced structs", ""] + types
        print("\n".join(out))
    except (IdlError, OSError, KeyError) as e:
        print("sns-idl-dump: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
