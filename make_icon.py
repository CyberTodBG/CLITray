#!/usr/bin/env python3
"""Generate a simple tray icon (blue circle + white play triangle) as src/icon.ico."""
import struct
import os

OUT = os.path.join(os.path.dirname(__file__), "src", "icon.ico")


def sign(p1, p2, p3):
    return (p1[0] - p3[0]) * (p2[1] - p3[1]) - (p2[0] - p3[0]) * (p1[1] - p3[1])


def point_in_tri(pt, t0, t1, t2):
    d1 = sign(pt, t0, t1)
    d2 = sign(pt, t1, t2)
    d3 = sign(pt, t2, t0)
    has_neg = (d1 < 0) or (d2 < 0) or (d3 < 0)
    has_pos = (d1 > 0) or (d2 > 0) or (d3 > 0)
    return not (has_neg and has_pos)


def draw(size):
    half = size / 2.0
    r = size * 0.40
    px = [[(0, 0, 0, 0) for _ in range(size)] for _ in range(size)]

    for y in range(size):
        for x in range(size):
            dx = (x + 0.5) - half
            dy = (y + 0.5) - half
            if dx * dx + dy * dy <= r * r:
                shade = 0xE0 if size < 32 else 0xCC
                px[y][x] = (0x2D, 0x6C - (shade - 0xC0), 0xDF, 255)

    h = size * 0.20
    w = size * 0.21
    cx = half + size * 0.05
    cy = half
    t0 = (cx - w, cy - h)
    t1 = (cx - w, cy + h)
    t2 = (cx + w, cy)
    for y in range(size):
        for x in range(size):
            if px[y][x][3] == 0:
                continue
            if point_in_tri((x + 0.5, y + 0.5), t0, t1, t2):
                px[y][x] = (255, 255, 255, 255)
    return px


def ico_entry(w, h, bpp, data_len, offset):
    return struct.pack("<BBBBHHII", w & 0xFF, h & 0xFF, 0, 0, 1, bpp, data_len, offset)


def bmp_data(px, size):
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0,
                         size * size * 4, 0, 0, 0, 0)
    rows = []
    for y in range(size - 1, -1, -1):
        for x in range(size):
            r, g, b, a = px[y][x]
            rows.append(struct.pack("<BBBB", b, g, r, a))
    # AND mask (all zero)
    mask_row = b"\x00" * (((size + 31) // 32) * 4)
    mask = mask_row * size
    return header + b"".join(rows) + mask


def main():
    sizes = [16, 32, 48]
    images = []
    for s in sizes:
        images.append(bmp_data(draw(s), s))

    entries = b""
    offset = 6 + 16 * len(images)
    data = b""
    for s, img in zip(sizes, images):
        entries += ico_entry(s, s, 32, len(img), offset)
        data += img
        offset += len(img)

    ico = struct.pack("<HHH", 0, 1, len(images)) + entries + data
    with open(OUT, "wb") as f:
        f.write(ico)
    print("wrote", OUT, len(ico), "bytes")


if __name__ == "__main__":
    main()