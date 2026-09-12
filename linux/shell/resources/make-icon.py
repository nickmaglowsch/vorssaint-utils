#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Vorssaint
#
# Generates com.vorssaint.VorssaintLinux.png, the shell's app and tray icon.
# A generator rather than an opaque binary, so the committed PNG can be
# regenerated and reviewed:
#
#   python3 linux/shell/resources/make-icon.py \
#       linux/shell/resources/com.vorssaint.VorssaintLinux.png
#
# The mark is the WP-01 spike's: a rounded square with a rising trace, which
# is what the panel shows. TRADEMARKS.md reserves the macOS product's icon
# for official builds, so this is deliberately not it (PLAN.md section 10.1).
import struct
import sys
import zlib

SIZE = 256
BG = (0x1E, 0x22, 0x2B, 0x00)   # transparent outside the rounded square:
                                # a tray icon sits on panels of every colour
FG = (0x4A, 0x90, 0xD9, 0xFF)
INK = (0xFF, 0xFF, 0xFF, 0xFF)
RADIUS = 44
MARGIN = 20

# The trace, in 0..1 of the inner box, left to right.
TRACE = [(0.10, 0.72), (0.33, 0.36), (0.55, 0.58), (0.90, 0.18)]
STROKE = 13.0


def rounded(x, y):
    lo, hi = MARGIN, SIZE - MARGIN
    if not (lo <= x < hi and lo <= y < hi):
        return False
    for cx, cy in ((lo + RADIUS, lo + RADIUS), (hi - RADIUS, lo + RADIUS),
                   (lo + RADIUS, hi - RADIUS), (hi - RADIUS, hi - RADIUS)):
        if (x < cx) == (cx == lo + RADIUS) and (y < cy) == (cy == lo + RADIUS):
            if (x - cx) ** 2 + (y - cy) ** 2 > RADIUS ** 2:
                return False
    return True


def distance_to_segment(px, py, ax, ay, bx, by):
    dx, dy = bx - ax, by - ay
    length = dx * dx + dy * dy
    t = 0.0 if length == 0 else max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / length))
    return ((px - (ax + t * dx)) ** 2 + (py - (ay + t * dy)) ** 2) ** 0.5


def main(path):
    lo, hi = MARGIN, SIZE - MARGIN
    span = hi - lo
    points = [(lo + fx * span, lo + fy * span) for fx, fy in TRACE]
    rows = bytearray()
    for y in range(SIZE):
        rows.append(0)  # PNG filter: none
        for x in range(SIZE):
            if not rounded(x, y):
                rows += bytes(BG)
                continue
            near = min(distance_to_segment(x + 0.5, y + 0.5, *points[i], *points[i + 1])
                       for i in range(len(points) - 1))
            rows += bytes(INK if near <= STROKE / 2 else FG)

    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))

    with open(path, "wb") as out:
        out.write(b"\x89PNG\r\n\x1a\n"
                  + chunk(b"IHDR", struct.pack(">IIBBBBB", SIZE, SIZE, 8, 6, 0, 0, 0))
                  + chunk(b"IDAT", zlib.compress(bytes(rows), 9))
                  + chunk(b"IEND", b""))
    print("wrote %s" % path)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "com.vorssaint.VorssaintLinux.png")
