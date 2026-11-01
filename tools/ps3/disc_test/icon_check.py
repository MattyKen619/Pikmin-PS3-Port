#!/usr/bin/env python3
"""PS3 port: checks icon_test's XMB icon (the disc banner scaled to 320 x 176, ps3/banner_icon.h) against the
same picture made independently here (numpy) from the banner read by tools/ps3/extract_disc.py, and that the PNG
reads back correctly (Pillow).

    python3 icon_check.py IMAGE icon.png
"""
import os
import struct
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from extract_disc import DiscReader  # noqa: E402

disc = DiscReader(sys.argv[1])
header = disc.read(0, 0x440)
fst_offset, fst_size = struct.unpack(">II", header[0x424:0x42C])
fst = disc.read(fst_offset, fst_size)
count = struct.unpack(">I", fst[8:12])[0]
strings = fst[count * 12:]
banner = None
for i in range(1, count):
    w0, w1, w2 = struct.unpack(">III", fst[i * 12:i * 12 + 12])
    name = strings[w0 & 0xFFFFFF:strings.index(b"\0", w0 & 0xFFFFFF)]
    if not (w0 >> 24) and name.lower() == b"opening.bnr":  # (it is in the root directory)
        banner = disc.read(w1, w2)
assert banner and banner[:3] == b"BNR"

px = np.zeros((32, 96, 4), np.uint8)
p = 0x20
for ty in range(0, 32, 4):
    for tx in range(0, 96, 4):
        for y in range(ty, ty + 4):
            for x in range(tx, tx + 4):
                v = struct.unpack(">H", banner[p:p + 2])[0]
                p += 2
                if v & 0x8000:
                    r, g, b = (v >> 10) & 31, (v >> 5) & 31, v & 31
                    px[y, x] = ((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2), 255)
                else:
                    a = (v >> 12) & 7
                    px[y, x] = (((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17, (a << 5) | (a << 2) | (a >> 1))
# Lanczos-3, premultiplied, outside the banner transparent (in floating point, the same maths written again).
def taps(out_size, scale):
    w = np.zeros((out_size, 6))
    first = np.zeros(out_size, int)
    for i in range(out_size):
        center = (i + 0.5) / scale - 0.5
        first[i] = int(np.floor(center)) - 2
        x = center - (first[i] + np.arange(6))
        k = np.where(np.abs(x) < 1e-6, 1.0, 3 * np.sin(np.pi * x) * np.sin(np.pi * x / 3) / np.maximum((np.pi * x) ** 2, 1e-12))
        k[np.abs(x) >= 3] = 0
        w[i] = k / k.sum()
    return first, w


def resample(src, axis, out_size, scale):
    first, w = taps(out_size, scale)
    src = np.moveaxis(src, axis, 0)
    out = np.zeros((out_size,) + src.shape[1:])
    for i in range(out_size):
        for k in range(6):
            j = first[i] + k
            if 0 <= j < src.shape[0]:
                out[i] += w[i, k] * src[j]
    return np.moveaxis(out, 0, axis)


f = px.astype(np.float64)
pre = np.dstack([f[..., :3] * f[..., 3:] / 255.0, f[..., 3]])
scale = 320 / 96
big = resample(resample(pre, 1, 320, scale), 0, 107, scale)
a = np.clip(big[..., 3], 0, 255)
rgb = np.where(a[..., None] > 0.5, np.clip(big[..., :3] * 255.0 / np.maximum(a[..., None], 1e-9), 0, 255), 0)
small = np.dstack([rgb, a]).round().astype(np.uint8)
small[small[..., 3] == 0] = 0
want = np.zeros((176, 320, 4), np.uint8)
want[(176 - 107) // 2:(176 - 107) // 2 + 107] = small
want = Image.fromarray(want, "RGBA")
# And Pillow's own scaling, as a looser cross-check (it rounds and clamps between its two passes).
pillow = Image.new("RGBA", (320, 176), (0, 0, 0, 0))
pillow.paste(Image.fromarray(px, "RGBA").convert("RGBa").resize((320, 107), Image.LANCZOS).convert("RGBA"), (0, (176 - 107) // 2))

got = Image.open(sys.argv[2])
got.load()  # (checks the PNG's chunks and compressed data)
assert got.size == (320, 176) and got.mode == "RGBA", (got.size, got.mode)


def premultiplied(im):
    a = np.asarray(im, np.float32)
    return np.dstack([a[..., :3] * a[..., 3:] / 255.0, a[..., 3]])


diff = np.abs(premultiplied(got) - premultiplied(want))
loose = np.abs(premultiplied(got) - premultiplied(pillow))
print(f"icon {got.size[0]} x {got.size[1]} {got.mode}: largest difference {diff.max():.1f} of 255 "
      f"(from Pillow's scaling: mean {loose.mean():.2f})")
sys.exit(0 if diff.max() <= 2.0 else 1)
