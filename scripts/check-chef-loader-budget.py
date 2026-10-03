#!/usr/bin/env python3
"""Check the ARM64 kernel budget observed in Chef's extracted ABL loader.

This validates the first gzip member and reads its Image header in bounded memory.
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
    if len(header) < 64:
        raise ValueError("truncated ARM64 Image header")
    if header[56:60] != b"ARM\x64":
        raise ValueError("invalid ARM64 Image magic at header offset 0x38")
    image_size = struct.unpack_from("<Q", header, 16)[0]
    if image_size < 64:
        raise ValueError("unsupported ARM64 Image.image_size: zero/legacy or smaller than its header")
    return image_size


def kernel_capacity(ramdisk_size):
    rounded = (ramdisk_size + PAGE_SIZE - 1) // PAGE_SIZE * PAGE_SIZE
    return REGION_SIZE - KERNEL_START_OFFSET - rounded - LOADER_RESERVE


def check_budget(kernel, ramdisk):
    image_size = read_image_size(kernel)
    ramdisk_size = Path(ramdisk).stat().st_size
    capacity = kernel_capacity(ramdisk_size)
    margin = capacity - image_size
    if margin < 0:
        raise ValueError(
            f"Chef loader budget exceeded: Image.image_size={image_size} bytes, "
            f"ramdisk={ramdisk_size} bytes, capacity={capacity} bytes "
            f"(overflow={-margin} bytes). Shrink the ramdisk; an alternative "
            "compression such as LZMA requires CONFIG_RD_LZMA=y. "
            "This is a Chef-specific observed loader constraint, not a universal ARM64 limit."
        )
    return image_size, ramdisk_size, capacity, margin


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kernel", type=Path, help="Image.gz-dtb input")
    parser.add_argument("ramdisk", type=Path, help="encoded initramfs input")
    args = parser.parse_args()
    try:
        image_size, ramdisk_size, capacity, margin = check_budget(args.kernel, args.ramdisk)
    except (OSError, ValueError) as error:
        print(f"Chef loader preflight: {error}", file=sys.stderr)
        return 1
    print(
        f"Chef loader preflight: Image.image_size={image_size}, "
        f"ramdisk={ramdisk_size}, capacity={capacity}, margin={margin} bytes "
        "(Chef-specific observed bound; first gzip member validated)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
