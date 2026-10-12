#!/usr/bin/env python3
"""
PS3 port: list the system libraries a PS3 ELF imports and fail if one of them lives in a module that is
not loaded when a process starts.

Calling into such a library without loading its module first (sysModuleLoad / cellSysmoduleLoadModule)
jumps through an unresolved import and crashes the process. Test build 1 crashed that way: it used
cellFs ("sys_fs") for its log file without loading SYSMODULE_FS, before anything reached the screen.

Usage: python3 tools/ps3/check_imports.py build/ps3/pikmin.elf [--allow LIB ...]
"""
import argparse
import struct
import sys

# Libraries available without loading a module. These are what PSL1GHT's own display sample imports,
# and that sample runs on the user's console.
PRELOADED = {"sysPrxForUser", "cellSysutil", "cellGcmSys", "sys_io"}


def sections(data):
    shoff, = struct.unpack_from(">Q", data, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from(">HHH", data, 0x3A)
    hdrs = []
    for i in range(shnum):
        name, typ, flags, addr, off, size = struct.unpack_from(">IIQQQQ", data, shoff + i * shentsize)
        hdrs.append((name, addr, off, size))
    stroff = hdrs[shstrndx][2]

    def cstr(off):
        return data[off:data.index(b"\0", off)].decode()

    return {cstr(stroff + h[0]): h for h in hdrs}


def read_addr(secs, data, addr):
    for name, (_, saddr, off, size) in secs.items():
        if saddr and saddr <= addr < saddr + size:
            return data[off + addr - saddr:data.index(b"\0", off + addr - saddr)].decode()
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("--allow", action="append", default=[], help="library whose module the code loads itself")
    args = ap.parse_args()
    data = open(args.elf, "rb").read()
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 2:
        sys.exit(f"{args.elf}: not a big-endian ELF64 file")
    secs = sections(data)
    if ".lib.stub" not in secs:
        sys.exit(f"{args.elf}: no .lib.stub section")
    _, _, off, size = secs[".lib.stub"]
    libs = []
    for i in range(size // 0x2C):  # PSL1GHT's Stub struct (tools/sprxlinker/linker.c)
        name_ptr, = struct.unpack_from(">I", data, off + i * 0x2C + 0x10)
        libs.append(read_addr(secs, data, name_ptr) or f"<0x{name_ptr:x}>")
    allowed = PRELOADED | set(args.allow)
    bad = [l for l in libs if l not in allowed]
    for lib in libs:
        print(f"  imports {lib}{'' if lib in allowed else '   <-- module not loaded at start-up'}")
    if bad:
        print(f"ERROR: {', '.join(bad)}: load the module first (sysModuleLoad) and pass --allow, or avoid the library")
        return 1
    print("imports OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
