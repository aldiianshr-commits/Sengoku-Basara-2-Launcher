#!/usr/bin/env python3
"""Check pixels of a PNG/BMP screenshot written by the game front-end.

usage: check_png.py IMAGE --expect-not-black [--min-colors N]
The front-end writes 8-bit RGB PNGs with stored (uncompressed) deflate blocks,
so this reader only needs zlib from the standard library.
"""
import argparse, struct, sys, zlib


def read_png(path):
    data = open(path, "rb").read()
    if data[:2] == b"BM":
        off = struct.unpack_from("<I", data, 10)[0]
        w, h = struct.unpack_from("<ii", data, 18)
        row = (w * 3 + 3) & ~3
        px = []
        for y in range(abs(h)):
            r0 = off + (abs(h) - 1 - y) * row
            for x in range(w):
                b, g, r = data[r0 + x * 3:r0 + x * 3 + 3]
                px.append((r, g, b))
        return w, abs(h), px
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
    pos = 8
    idat = b""
    w = h = 0
    while pos < len(data):
        n = struct.unpack(">I", data[pos:pos + 4])[0]
        t = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + n]
        if t == b"IHDR":
            w, h = struct.unpack(">II", body[:8])
        elif t == b"IDAT":
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    px = []
    stride = w * 3 + 1
    for y in range(h):
        row = raw[y * stride + 1:(y + 1) * stride]
        for x in range(w):
            px.append(tuple(row[x * 3:x * 3 + 3]))
    return w, h, px


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--expect-not-black", action="store_true")
    ap.add_argument("--min-colors", type=int, default=0)
    a = ap.parse_args()
    w, h, px = read_png(a.image)
    colors = set(px[::7])
    nonblack = sum(1 for p in px[::7] if max(p) > 16)
    print(f"{a.image}: {w}x{h}, {len(colors)} sampled colours, {nonblack} non-black samples")
    ok = True
    if a.expect_not_black and nonblack == 0:
        print("FAIL: image is black")
        ok = False
    if len(colors) < a.min_colors:
        print(f"FAIL: expected at least {a.min_colors} colours")
        ok = False
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
