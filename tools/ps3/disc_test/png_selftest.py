#!/usr/bin/env python3
"""PS3 port: reads png_selftest's PNGs with Pillow and OpenCV (libpng) and compares them with the pictures.

    python3 png_selftest.py OUT_DIR
"""
import glob
import os
import sys

import numpy as np
from PIL import Image

try:
    import cv2
except ImportError:
    cv2 = None

bad = 0
files = sorted(glob.glob(os.path.join(sys.argv[1], "selftest_*.png")))
for path in files:
    want = np.fromfile(path[:-4] + ".rgba", np.uint8).reshape(176, 320, 4)
    im = Image.open(path)
    im.load()
    if im.mode != "RGBA" or not np.array_equal(np.asarray(im), want):
        print("Pillow differs:", path)
        bad += 1
    if cv2 is not None:
        bgra = cv2.imread(path, cv2.IMREAD_UNCHANGED)
        if bgra is None or not np.array_equal(bgra[..., [2, 1, 0, 3]], want):
            print("OpenCV differs:", path)
            bad += 1
print(f"{len(files)} PNGs read with Pillow{' and OpenCV' if cv2 else ''}: {bad} wrong")
sys.exit(1 if bad or not files else 0)
