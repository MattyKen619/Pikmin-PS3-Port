#!/usr/bin/env python3
"""
PS3 port: turn the addresses in a pikmin_log.txt into function names.

Stall reports and heartbeats print each game thread's call stack as "FN<SITE" pairs, innermost first:
FN is the function (its ELFv1 function descriptor) and SITE the return address inside its caller.
This script needs the unstripped ELF of the exact build that wrote the log (build/ps3/pikmin.elf, or
the copy archived per test build).

Usage: python3 tools/ps3/symbolize_log.py build/ps3/pikmin.elf pikmin_log.txt > pikmin_log.sym.txt
"""
import bisect
import re
import struct
import subprocess
import sys


def read_elf(path):
    data = open(path, "rb").read()
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 2:
        sys.exit(f"{path}: not a big-endian ELF64 file")
    shoff, = struct.unpack_from(">Q", data, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from(">HHH", data, 0x3A)
    secs = []
    for i in range(shnum):
        name, typ, flags, addr, off, size, link, info, align, entsize = struct.unpack_from(">IIQQQQIIQQ", data, shoff + i * shentsize)
        secs.append(dict(name=name, type=typ, addr=addr, off=off, size=size, link=link, entsize=entsize))
    shstr = secs[shstrndx]["off"]

    def cstr(off):
        return data[off:data.index(b"\0", off)].decode(errors="replace")

    for s in secs:
        s["name"] = cstr(shstr + s["name"])
    byname = {s["name"]: s for s in secs}
    symtab = byname.get(".symtab")
    if not symtab:
        sys.exit(f"{path}: no symbol table (use the unstripped ELF)")
    strtab = secs[symtab["link"]]["off"]
    opd = byname.get(".opd")
    descr = {}   # descriptor address -> name
    code = []    # (code address, name)
    for i in range(symtab["size"] // 24):
        st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from(">IBBHQQ", data, symtab["off"] + i * 24)
        if st_info & 0xF != 2 or not st_value:  # STT_FUNC
            continue
        name = cstr(strtab + st_name)
        descr[st_value] = name
        if opd and opd["addr"] <= st_value < opd["addr"] + opd["size"]:
            entry, = struct.unpack_from(">Q", data, opd["off"] + st_value - opd["addr"])
            code.append((entry, name))
        else:
            code.append((st_value, name))
    code.sort()
    return descr, code


def demangle(names):
    names = sorted(set(names))
    if not names:
        return {}
    try:
        out = subprocess.run(["c++filt"], input="\n".join(names), capture_output=True, text=True).stdout.splitlines()
        return dict(zip(names, out))
    except OSError:
        return {n: n for n in names}


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    descr, code = read_elf(sys.argv[1])
    starts = [c[0] for c in code]
    lines = open(sys.argv[2], errors="replace").read().splitlines()
    used = []

    def fn_name(a):
        return descr.get(a)

    def site_name(a):
        i = bisect.bisect_right(starts, a) - 1
        if i < 0:
            return None
        return code[i][1], a - code[i][0]

    out = []
    for line in lines:
        if not line.startswith("[stack]"):
            out.append(line)
            continue
        head, _, frames = line.partition(":")
        parts = []
        for m in re.finditer(r"(0x[0-9a-f]+)<(0x[0-9a-f]+)", frames):
            fn, site = int(m.group(1), 16), int(m.group(2), 16)
            f = fn_name(fn)
            s = site_name(site)
            used += [x for x in (f, s and s[0]) if x]
            parts.append((f or hex(fn), s))
        out.append((head, parts))
    dm = demangle(used)
    for item in out:
        if isinstance(item, str):
            print(item)
            continue
        head, parts = item
        print(head + ":")
        for f, s in parts:
            caller = f"  (called from {dm.get(s[0], s[0])}+0x{s[1]:x})" if s else ""
            print(f"[stack]       {dm.get(f, f)}{caller}")


if __name__ == "__main__":
    main()
