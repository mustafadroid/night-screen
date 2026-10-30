#!/usr/bin/env python3
"""Generate assets/icon.ico: amber crescent moon, 16/32/48px 32bpp BMP-based ICO.

No dependencies (pure stdlib) so it runs anywhere. Re-run after editing.
"""
import math
import struct
import sys
from pathlib import Path

OUT = Path(__file__).resolve().parent.parent / "assets" / "icon.ico"


def render(size: int) -> tuple[bytes, bytes]:
    px = bytearray(size * size * 4)
    row_bytes = ((size + 31) // 32) * 4
    mask = bytearray(row_bytes * size)
    for y in range(size):
        for x in range(size):
            u = (x + 0.5) / size * 2 - 1
            v = (y + 0.5) / size * 2 - 1
            d = (u + 0.12) ** 2 + (v + 0.05) ** 2
            s = (u - 0.42) ** 2 + (v - 0.30) ** 2
            if d <= 0.72 and s > 0.42:
                shade = 1.0 - 0.28 * ((x + y) / (2 * size))
                # BGRA amber
                px[(y * size + x) * 4:(y * size + x) * 4 + 4] = bytes(
                    (max(0, min(255, int(c * shade))) for c in (92, 186, 255, 255))
                )
            else:
                mask[y * row_bytes + x // 8] |= 0x80 >> (x % 8)
    return bytes(px), bytes(mask)


def main() -> None:
    OUT.parent.mkdir(parents=True, exist_ok=True)
    entries = []
    blobs = []
    for size in (16, 32, 48):
        xor, and_mask = render(size)
        info = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0,
                           len(xor) + len(and_mask), 0, 0, 0, 0)
        blob = info + xor + and_mask
        blobs.append(blob)
        entries.append((size, blob))
    header = struct.pack("<HHH", 0, 1, len(entries))
    offset = 6 + 16 * len(entries)
    out = bytearray(header)
    for size, blob in entries:
        w = size if size < 256 else 0
        out += struct.pack("<BBBBHHII", w, w, 0, 0, 1, 32, len(blob), offset)
        offset += len(blob)
    for _, blob in entries:
        out += blob
    OUT.write_bytes(bytes(out))
    print(f"wrote {OUT} ({len(out)} bytes)")


if __name__ == "__main__":
    sys.exit(main())
