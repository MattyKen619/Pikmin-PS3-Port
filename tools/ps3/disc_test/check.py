#!/usr/bin/env python3
"""PS3 port: reads a disc image with tools/ps3/extract_disc.py's reader (independent of disc_image.h) and
compares every file's size and CRC-32 with disc_test's list.  python3 check.py IMAGE list.txt"""
import os, struct, sys, zlib
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from extract_disc import DiscReader

disc = DiscReader(sys.argv[1])
header = disc.read(0, 0x440)
fst_offset, fst_size = struct.unpack(">II", header[0x424:0x42C])
fst = disc.read(fst_offset, fst_size)
count = struct.unpack(">I", fst[8:12])[0]
strings = fst[count * 12:]
expected = {}
dir_stack = [(count, "")]
for i in range(1, count):
    while dir_stack and i >= dir_stack[-1][0]:
        dir_stack.pop()
    prefix = dir_stack[-1][1] if dir_stack else ""
    w0, w1, w2 = struct.unpack(">III", fst[i * 12:i * 12 + 12])
    name = strings[w0 & 0xFFFFFF:strings.index(b"\0", w0 & 0xFFFFFF)].decode("shift_jis", "replace")
    path = f"{prefix}/{name}"
    if w0 >> 24:
        dir_stack.append((w2, path))
        expected[i] = ("d", path)
    else:
        crc, pos, left = 0, w1, w2
        while left:
            chunk = disc.read(pos, min(left, 4 << 20))
            crc = zlib.crc32(chunk, crc)
            pos += len(chunk)
            left -= len(chunk)
        expected[i] = ("f", path, w2, "%08x" % crc)
bad = 0
seen = 0
for raw in open(sys.argv[2], "rb"):
    # (disc_test prints names as the disc stores them: Shift-JIS)
    p = raw.rstrip(b"\n").decode("shift_jis", "replace").split("\t")
    i = int(p[0])
    seen += 1
    want = expected.get(i)
    got = ("d", p[2]) if p[1] == "d" else ("f", p[2], int(p[3]), p[4])
    if want != got:
        bad += 1
        if bad <= 5:
            print("differs:", want, got)
print(f"{seen} entries compared with {len(expected)}, {bad} differ")
sys.exit(1 if bad or seen != len(expected) else 0)
