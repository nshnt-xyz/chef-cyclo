#!/usr/bin/env python3
"""Turn Image.gz-dtb into the uncompressed kernel Chef's abl accepts.

    mkkernel-uncompressed.py IN OUT

IN is the gzip kernel with its appended DTB(s) (out/kernel/arch/arm64/boot/
Image.gz-dtb, the file the build stamp names). OUT gets the layout abl calls
a "patched kernel" (confirmed in the disassembly of Chef's abl, see
docs/research/chef-loader-kernel-budget.md):

    "UNCOMPRESSED_IMG" | LE32 len(Image) | raw Image | the same DTB(s)

abl loads the raw Image from offset 20 and looks for the DTBs at 20 + the
length field. The Image must be ARM64 (magic at 0x38), or abl would take the
32-bit load path. Nothing but the encoding changes: the Image is the inflated
gzip member and the DTB bytes are copied unchanged, so the result is a pure
function of IN.
"""

import importlib.util
import struct
import sys
import zlib
from pathlib import Path

_spec = importlib.util.spec_from_file_location(
    "budget", Path(__file__).resolve().parent / "check-chef-loader-budget.py")
budget = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(budget)


def inflate(data):
    """The first gzip member (CRC and trailer checked) and what follows it."""
    decoder = zlib.decompressobj(16 + zlib.MAX_WBITS)
    try:
        image = decoder.decompress(data)
    except zlib.error as error:
        raise ValueError(f"invalid gzip stream or CRC/trailer: {error}") from error
    if not decoder.eof:
        raise ValueError("truncated gzip stream (missing end/trailer)")
    return image, decoder.unused_data


def pack(data):
    if not data.startswith(budget.GZIP_MAGIC):
        raise ValueError("not a gzip Image.gz-dtb")
    image, dtbs = inflate(data)
    budget.arm64_image_size(image)
    budget.split_fdts(dtbs)
    if len(image) > 0xFFFFFFFF:
        raise ValueError("Image too large for the 32-bit length field")
    return budget.UNCOMPRESSED_MAGIC + struct.pack("<I", len(image)) + image + dtbs


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__.split("\n\n")[1])
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    try:
        packed = pack(src.read_bytes())
    except (OSError, ValueError) as error:
        print(f"mkkernel-uncompressed: {src}: {error}", file=sys.stderr)
        return 1
    tmp = out.with_name(out.name + ".tmp")
    tmp.write_bytes(packed)
    tmp.replace(out)
    print(f"mkkernel-uncompressed: {out}: {len(packed)} bytes "
          f"(Image {struct.unpack_from('<I', packed, 16)[0]} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
