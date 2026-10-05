#!/usr/bin/env python3
"""Check the ARM64 kernel budget observed in Chef's extracted ABL loader.

Two kernel forms are accepted. Image.gz-dtb: this validates the first gzip
member and reads its Image header in bounded memory. The uncompressed form
(scripts/mkkernel-uncompressed.py): "UNCOMPRESSED_IMG", LE32 len(Image), the
raw Image, then the appended FDTs, which must start exactly at the length
field and fill the rest. abl copies the whole kernel section (the boot
header's kernel_size) to the kernel start before it checks Image.image_size,
so both must fit there.
The fixed region assumptions are specific to Chef, not an ARM64 boot rule.
"""

import argparse
from pathlib import Path
import struct
import sys
import zlib

PAGE_SIZE = 4096
REGION_SIZE = 0x5600000
KERNEL_START_OFFSET = 0x80000
LOADER_RESERVE = 2 * PAGE_SIZE + 0x200000
GZIP_MAGIC = b"\x1f\x8b\x08"
UNCOMPRESSED_MAGIC = b"UNCOMPRESSED_IMG"
UNCOMPRESSED_HEADER = len(UNCOMPRESSED_MAGIC) + 4
FDT_MAGIC = 0xD00DFEED
FDT_HEADER_SIZE = 0x28


def read_image_size(kernel):
    """Validate the first gzip member, retaining only the ARM64 Image header."""
    decoder = zlib.decompressobj(16 + zlib.MAX_WBITS)
    header = bytearray()
    with Path(kernel).open("rb") as stream:
        while not decoder.eof:
            pending = stream.read(4096)
            if not pending:
                raise ValueError("truncated gzip stream (missing end/trailer)")
            while pending and not decoder.eof:
                try:
                    output = decoder.decompress(pending, 65536)
                except zlib.error as error:
                    raise ValueError(f"invalid gzip stream or CRC/trailer: {error}") from error
                header.extend(output[:max(0, 64 - len(header))])
                pending = decoder.unconsumed_tail
        # unused_data can contain an appended DTB; do not parse another member.
    return arm64_image_size(header)


def arm64_image_size(header):
    if len(header) < 64:
        raise ValueError("truncated ARM64 Image header")
    if header[56:60] != b"ARM\x64":
        raise ValueError("invalid ARM64 Image magic at header offset 0x38")
    image_size = struct.unpack_from("<Q", header, 16)[0]
    if image_size < 64:
        raise ValueError("unsupported ARM64 Image.image_size: zero/legacy or smaller than its header")
    return image_size


def split_fdts(blob):
    """Return the sizes of the FDTs that make up BLOB exactly."""
    sizes = []
    offset = 0
    while offset < len(blob):
        if len(blob) - offset < FDT_HEADER_SIZE:
            raise ValueError(f"truncated FDT header at DTB offset {offset}")
        magic, total = struct.unpack_from(">II", blob, offset)
        if magic != FDT_MAGIC:
            raise ValueError(f"no FDT magic at DTB offset {offset}")
        if total < FDT_HEADER_SIZE or offset + total > len(blob):
            raise ValueError(f"FDT at DTB offset {offset} has a bad totalsize {total}")
        sizes.append(total)
        offset += total
    if not sizes:
        raise ValueError("no appended DTB")
    return sizes


def read_uncompressed(kernel):
    """Image.image_size and the kernel section size of the uncompressed form."""
    data = Path(kernel).read_bytes()
    if not data.startswith(UNCOMPRESSED_MAGIC) or len(data) < UNCOMPRESSED_HEADER:
        raise ValueError("not an UNCOMPRESSED_IMG kernel")
    length = struct.unpack_from("<I", data, len(UNCOMPRESSED_MAGIC))[0]
    image = data[UNCOMPRESSED_HEADER:UNCOMPRESSED_HEADER + length]
    if len(image) != length:
        raise ValueError(f"length field {length} runs past the end of the kernel")
    image_size = arm64_image_size(image)
    try:
        split_fdts(data[UNCOMPRESSED_HEADER + length:])
    except ValueError as error:
        raise ValueError(f"length field {length} does not end at the appended DTBs: {error}") from error
    return image_size, len(data)


def kernel_form(kernel):
    with Path(kernel).open("rb") as stream:
        head = stream.read(len(UNCOMPRESSED_MAGIC))
    if head.startswith(GZIP_MAGIC):
        return "gzip"
    if head == UNCOMPRESSED_MAGIC:
        return "uncompressed"
    raise ValueError("kernel is neither gzip Image.gz-dtb nor an UNCOMPRESSED_IMG kernel")


def kernel_capacity(ramdisk_size):
    rounded = (ramdisk_size + PAGE_SIZE - 1) // PAGE_SIZE * PAGE_SIZE
    return REGION_SIZE - KERNEL_START_OFFSET - rounded - LOADER_RESERVE


def check_budget(kernel, ramdisk):
    return check_kernel(kernel, ramdisk)[1:]


def check_kernel(kernel, ramdisk):
    """(form, image_size, ramdisk_size, capacity, margin); raises on overflow."""
    form = kernel_form(kernel)
    if form == "gzip":
        image_size = read_image_size(kernel)
        need = image_size
        copied = ""
    else:
        image_size, kernel_size = read_uncompressed(kernel)
        need = max(image_size, kernel_size)
        copied = f"kernel_size={kernel_size} bytes, "
    ramdisk_size = Path(ramdisk).stat().st_size
    capacity = kernel_capacity(ramdisk_size)
    margin = capacity - need
    if margin < 0:
        raise ValueError(
            f"Chef loader budget exceeded: Image.image_size={image_size} bytes, {copied}"
            f"ramdisk={ramdisk_size} bytes, capacity={capacity} bytes "
            f"(overflow={-margin} bytes). Shrink the ramdisk; an alternative "
            "compression such as LZMA requires CONFIG_RD_LZMA=y. "
            "This is a Chef-specific observed loader constraint, not a universal ARM64 limit."
        )
    return form, image_size, ramdisk_size, capacity, margin


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kernel", type=Path, help="Image.gz-dtb or UNCOMPRESSED_IMG kernel input")
    parser.add_argument("ramdisk", type=Path, help="encoded initramfs input")
    args = parser.parse_args()
    try:
        form, image_size, ramdisk_size, capacity, margin = check_kernel(args.kernel, args.ramdisk)
    except (OSError, ValueError) as error:
        print(f"Chef loader preflight: {error}", file=sys.stderr)
        return 1
    if form == "uncompressed":
        kernel_size = args.kernel.stat().st_size
        print(
            f"Chef loader preflight: uncompressed kernel: Image.image_size={image_size}, "
            f"kernel_size={kernel_size}, ramdisk={ramdisk_size}, capacity={capacity}, "
            f"margin={margin} bytes (Chef-specific observed bound; abl copies kernel_size "
            "bytes before it checks image_size)"
        )
        return 0
    print(
        f"Chef loader preflight: Image.image_size={image_size}, "
        f"ramdisk={ramdisk_size}, capacity={capacity}, margin={margin} bytes "
        "(Chef-specific observed bound; first gzip member validated)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
