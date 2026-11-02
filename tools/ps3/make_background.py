#!/usr/bin/env python3
"""PS3 port: makes the XMB background (PIC1.PNG: 1920 x 1080, shown behind the XMB while the game is
selected) from a picture.

    python3 tools/ps3/make_background.py PICTURE platform/ps3/pkg/PIC1.local.png

The picture is scaled to cover 1920 x 1080 and cropped to it, centred. Makefile.ps3 puts
platform/ps3/pkg/PIC1.local.png in the package when it exists. That file is not in the repository: like the
icon (make_icon.py), a picture from the game is its publisher's artwork, so each player makes their own.
"""
import argparse

from PIL import Image

W, H = 1920, 1080


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("picture")
    p.add_argument("out")
    a = p.parse_args()

    im = Image.open(a.picture).convert("RGB")
    scale = max(W / im.width, H / im.height)
    size = (max(W, round(im.width * scale)), max(H, round(im.height * scale)))
    if size != im.size:
        im = im.resize(size, Image.LANCZOS)
    left, top = (im.width - W) // 2, (im.height - H) // 2
    im = im.crop((left, top, left + W, top + H))
    im.save(a.out, optimize=True)
    print(f"{a.out}: {W} x {H}")


if __name__ == "__main__":
    main()
