#!/usr/bin/env python3
"""PS3 port: makes the XMB icon (ICON0.PNG: 320 x 176, with transparency) from a picture, such as a logo
on a white background.

    python3 tools/ps3/make_icon.py PICTURE platform/ps3/pkg/ICON0.local.png [--keep-background]

The plain light background around the picture (white, and the light grey of a drop shadow), as far as it
reaches in from the edges, is made see-through: the XMB's own background shows behind the icon, and a
shadow stays as a shadow. Light areas inside the picture are kept. The picture is then scaled to fit
320 x 176 and centred.

Makefile.ps3 puts platform/ps3/pkg/ICON0.local.png in the package when it exists. That file is not in the
repository: a game's logo is its publisher's artwork, so each player makes their own.
"""
import argparse

import numpy as np
from PIL import Image
from scipy import ndimage

W, H = 320, 176


def remove_background(rgb):
    """RGBA with the light background reaching in from the edges made transparent."""
    f = rgb.astype(np.float32)
    lo, hi = f.min(axis=2), f.max(axis=2)
    light = (lo > 140) & (hi - lo < 40)  # white or a light, colourless grey
    labels, _ = ndimage.label(light)
    edge = np.unique(np.concatenate([labels[0], labels[-1], labels[:, 0], labels[:, -1]]))
    background = np.isin(labels, edge[edge > 0])
    # The pixels just around it are partly background (smoothed edges): treat them the same way.
    near = ndimage.binary_dilation(background, iterations=2)
    # "Colour to alpha" against white: a pixel is (colour * alpha) over white.
    alpha = np.where(near, (255.0 - lo) / 255.0, 1.0)
    alpha = np.clip(alpha, 0.0, 1.0)
    safe = np.maximum(alpha, 1e-6)[..., None]
    colour = np.where(near[..., None], (f - (1.0 - alpha[..., None]) * 255.0) / safe, f)
    colour = np.clip(colour, 0, 255)
    out = np.dstack([colour, alpha * 255.0]).round().astype(np.uint8)
    out[alpha < 1.0 / 255.0] = 0
    return out


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("picture")
    p.add_argument("out")
    p.add_argument("--keep-background", action="store_true", help="keep the background as it is")
    a = p.parse_args()

    rgb = np.asarray(Image.open(a.picture).convert("RGB"))
    if a.keep_background:
        rgba = np.dstack([rgb, np.full(rgb.shape[:2], 255, np.uint8)])
    else:
        rgba = remove_background(rgb)
    im = Image.fromarray(rgba, "RGBA")
    box = im.getchannel("A").point(lambda v: 255 if v > 8 else 0).getbbox()
    if box:
        im = im.crop(box)
    # Scale in premultiplied form, so that see-through pixels don't darken or lighten the edges.
    scale = min(W / im.width, H / im.height)
    size = (max(1, round(im.width * scale)), max(1, round(im.height * scale)))
    im = im.convert("RGBa").resize(size, Image.LANCZOS).convert("RGBA")
    icon = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    icon.paste(im, ((W - size[0]) // 2, (H - size[1]) // 2))
    icon.save(a.out, optimize=True)
    print(f"{a.out}: {W} x {H}, picture {size[0]} x {size[1]}")


if __name__ == "__main__":
    main()
