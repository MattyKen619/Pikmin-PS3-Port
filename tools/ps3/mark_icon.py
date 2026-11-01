#!/usr/bin/env python3
"""PS3 port: marks a PNG as a stand-in XMB icon (build 54). Used by Makefile.ps3 for a package built without an
icon of its own: on start, the game replaces a marked icon with one made from the disc's own banner
(platform/ps3/include/ps3/banner_icon.h). Adds a text chunk "pikmin-ps3: icon-from-disc"; standard library only.

    python3 tools/ps3/mark_icon.py IN.png OUT.png
"""
import struct
import sys
import zlib

MARK = b"pikmin-ps3\0icon-from-disc"


def main():
    data = open(sys.argv[1], "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n" or data[12:16] != b"IHDR":
        sys.exit(f"{sys.argv[1]}: not a PNG")
    after_ihdr = 8 + 8 + struct.unpack(">I", data[8:12])[0] + 4
    chunk = struct.pack(">I", len(MARK)) + b"tEXt" + MARK + struct.pack(">I", zlib.crc32(b"tEXt" + MARK) & 0xFFFFFFFF)
    open(sys.argv[2], "wb").write(data[:after_ihdr] + chunk + data[after_ihdr:])


if __name__ == "__main__":
    main()
