#!/usr/bin/env python3
"""
PS3 port: compile the kept code to big-endian PowerPC 64 objects (Clang, ELFv1 like the PS3 PPU),
link with ld.lld using section garbage collection (like the original linker's dead-stripping),
and report which symbols are still missing, grouped by where they must come from.

Usage:
  python3 tools/ps3/link_check.py [--extra-src FILE ...] [--out build/ps3link] [-j N]

Writes OUT/undefined.json and OUT/report.txt. Exit code 0 if the link succeeds.
"""
import argparse
import json
import os
import re
import subprocess
import sys
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from gcc_check import DEFINES, group_of  # noqa: E402

TARGET = "powerpc64-unknown-elf"
# ld.lld's support for the PS3's ELFv1 ABI (function descriptors in .opd) fails on discarded COMDAT
# copies of inline functions, so this link test uses ELFv2. The symbol inventory is ABI-independent;
# the real PS3 link uses the ps3dev toolchain's own linker.
CFLAGS = [f"--target={TARGET}", "-mabi=elfv2", "-c", "-O1", "-g0", "-ffunction-sections", "-fdata-sections",
          "-funsigned-char", "-fno-exceptions", "-nostdinc", "-isystem", "include/stl", "-I", "include",
          "-I", "platform/ps3/include",
          "-w", *DEFINES]
CXXFLAGS = ["-std=gnu++14", "-fno-rtti", "-fno-threadsafe-statics", "-fno-use-cxa-atexit"]

# Port sources compiled in addition to the kept game code.
PORT_SOURCES = [
    "platform/ps3/stubs/sdk_stubs.gen.cpp",   # generated SDK placeholders (tools/ps3/gen_stubs.py)
    "platform/ps3/stubs/sdk_globals.cpp",     # SDK globals the game touches directly
    "platform/ps3/stubs/stub_report.cpp",
    "src/MSL_C/PPCEABI/bare/H/float.c",       # MSL float constants (__float_nan etc.); not in newlib
]

# Symbols the ps3dev toolchain supplies: newlib 1.20 (checked against its libc/libm sources) and
# GCC's C++ runtime (the PPU GCC is built with --enable-languages="c,c++").
TOOLCHAIN_PROVIDED = {
    "acosf", "atan2", "atan2f", "atexit", "atof", "atoi", "cosf", "memcpy", "memmove", "memset", "printf", "rand",
    "sinf", "sprintf", "sqrt", "sscanf", "strcat", "strchr", "strcmp", "strcpy", "strlen", "strncmp", "strncpy",
    "strtol", "strtoul", "tanf", "vsprintf",
    "__cxa_pure_virtual",
}

# Symbol groups, checked in order. Each entry: (group name, regex on the demangled-or-raw name).
GROUPS = [
    ("sdk:GX (graphics)", r"^(GX|__GX|GD|__GD)"),
    ("sdk:OS (threads, time, memory, interrupts)", r"^(OS|__OS|PPC|__PPC|DC[A-Z]|IC[A-Z]|LC[A-Z]|__DC|__IC)"),
    ("sdk:DVD (disc)", r"^(DVD|__DVD)"),
    ("sdk:CARD (memory card)", r"^(CARD|__CARD)"),
    ("sdk:PAD (controller)", r"^(PAD|__PAD|SI[A-Z]|__SI)"),
    ("sdk:VI (video)", r"^(VI[A-Z]|__VI)"),
    ("sdk:AR/ARQ (ARAM)", r"^(AR[A-Z]|ARQ|__AR)"),
    ("sdk:AI (audio interface)", r"^(AI[A-Z]|__AI)"),
    ("sdk:DSP (audio DSP)", r"^(DSP|__DSP)"),
    ("sdk:EXI/HIO/DB/debug", r"^(EXI|__EXI|HIO|DB[A-Z]|__DB|Hu|AMC|ODEMU)"),
]


def demangle(names):
    if not names:
        return {}
    p = subprocess.run(["c++filt"], input="\n".join(names), capture_output=True, text=True)
    return dict(zip(names, p.stdout.splitlines()))


def libc_names():
    """Functions and objects declared by the C library headers the game uses (include/stl)."""
    names = set()
    for h in (ROOT / "include/stl").glob("*.h"):
        for m in re.finditer(r"\b([A-Za-z_]\w*)\s*\(", h.read_text(errors="ignore")):
            names.add(m.group(1))
    return names


def classify(sym, dem, libc):
    for name, rx in GROUPS:
        if re.match(rx, sym):
            return name
    if sym in libc or re.match(r"^(mem|str|v?s?n?printf|sin|cos|tan|atan|sqrt|pow|floor|ceil|fmod|abs|rand|srand|exit|abort)", sym):
        return "libc (newlib provides)"
    if sym.startswith("__cxa") or sym in ("__dso_handle", "__gxx_personality_v0") or "operator new" in dem or "operator delete" in dem:
        return "C++ runtime"
    if sym.startswith("_Z"):
        return "game C++ code (declared, never defined)"
    return "other C symbol"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="build/ps3link")
    ap.add_argument("--extra-src", action="append", default=[], help="additional source")
    ap.add_argument("--no-port-sources", action="store_true", help="link only the game code (shows the full SDK inventory)")
    ap.add_argument("-j", type=int, default=os.cpu_count() or 2)
    ap.add_argument("--ppu-bin", help="use the ps3dev toolchain in this bin dir (ppu-gcc, ELFv1, newlib) instead of Clang/ld.lld")
    args = ap.parse_args()
    os.chdir(ROOT)
    out = ROOT / args.out
    (out / "obj").mkdir(parents=True, exist_ok=True)

    objdiff = json.loads((ROOT / "objdiff.json").read_text())
    units = [(u["name"], u["metadata"]["source_path"], group_of(u["metadata"]["source_path"]))
             for u in objdiff["units"]
             if u.get("metadata", {}).get("source_path") and group_of(u["metadata"]["source_path"])]
    extra = ([] if args.no_port_sources else PORT_SOURCES) + args.extra_src
    units += [("port__" + Path(s).stem, s, "extra") for s in extra]

    if args.ppu_bin:
        cc, cxx = os.path.join(args.ppu_bin, "ppu-gcc"), os.path.join(args.ppu_bin, "ppu-g++")
        cflags = CFLAGS[2:]  # drop --target / -mabi: ppu-gcc is natively powerpc64-ps3-elf, ELFv1
    else:
        cc, cxx, cflags = "clang", "clang++", CFLAGS

    def build(item):
        name, src, group = item
        o = out / "obj" / (name.replace("/", "__") + ".o")
        is_c = src.endswith(".c") and group not in ("jaudio",)
        if src.endswith((".cpp", ".cc")) or not is_c:
            cmd = [cxx, *cflags, *CXXFLAGS] + (["-x", "c++"] if src.endswith(".c") else []) + [src, "-o", str(o)]
        else:
            cmd = [cc, *cflags, "-std=gnu99", src, "-o", str(o)]
        p = subprocess.run(cmd, capture_output=True, text=True)
        return (src, p.returncode == 0, p.stderr[-2000:], str(o))

    with ThreadPoolExecutor(max_workers=args.j) as ex:
        built = list(ex.map(build, units))
    failed = [b for b in built if not b[1]]
    objs = [b[3] for b in built if b[1]]
    print(f"compiled: {len(objs)}/{len(built)}")
    for src, _, err, _ in failed:
        print(f"  FAILED {src}: {err.strip().splitlines()[0] if err.strip() else ''}")

    elf = str(out / "pikmin_ps3_linktest.elf")
    if args.ppu_bin:
        # Real PS3 link (ELFv1): PSL1GHT's startup code (which calls main), newlib, the lv2 system-call
        # layer (-lsysbase -lrt -llv2, added by the ppu-gcc driver specs) and GCC's C++ runtime.
        # Requires $PSL1GHT to point at an installed PSL1GHT.
        link_cmd = [cxx, "-Wl,--gc-sections", "-o", elf, *objs, "-lsupc++", "-lm"]
    else:
        link_cmd = ["ld.lld", "-m", "elf64ppc", "--gc-sections", "-e", "main", "--error-limit=0", "-o", elf, *objs]
    link = subprocess.run(link_cmd, capture_output=True, text=True)
    undef = defaultdict(set)
    cur = None
    for line in link.stderr.splitlines():
        m = re.match(r"^ld\.lld: error: undefined symbol: (.*)$", line)
        if m:
            cur = m.group(1).strip()
            undef.setdefault(cur, set())
            continue
        m = re.match(r"^>>> referenced by (.*)$", line)
        if m and cur:
            undef[cur].add(m.group(1).strip())
            continue
        m = re.match(r"^(.*?):\(.*\): undefined reference to [`'](.*)'$", line)
        if m:  # GNU ld format
            undef[m.group(2).strip()].add(os.path.basename(m.group(1)))
    other_errors = [l for l in link.stderr.splitlines()
                    if ("error" in l.lower() and "undefined" not in l and "collect2" not in l and "ld returned" not in l)]

    # lld prints demangled names; recover raw names via the objects' symbol tables for grouping.
    raw = set()
    for o in objs:
        p = subprocess.run(["llvm-nm", "-u", o], capture_output=True, text=True)
        raw.update(l.split()[-1] for l in p.stdout.splitlines() if l.strip())
    dm = demangle(sorted(raw))
    by_dem = {v: k for k, v in dm.items()}
    libc = libc_names()
    groups = defaultdict(list)
    rows = []
    for dem_name, refs in sorted(undef.items()):
        sym = by_dem.get(dem_name, dem_name)
        g = classify(sym, dem_name, libc)
        groups[g].append(dem_name)
        rows.append({"symbol": sym, "demangled": dem_name, "group": g, "refs": sorted(refs)[:5], "nrefs": len(refs)})
    (out / "undefined.json").write_text(json.dumps(rows, indent=1))
    outside = sorted(r["symbol"] for r in rows if r["symbol"] not in TOOLCHAIN_PROVIDED)
    complete = not other_errors and not outside
    if complete:
        # Produce the ELF anyway, leaving the toolchain-provided symbols unresolved, for inspection.
        if args.ppu_bin:
            subprocess.run(link_cmd[:1] + ["-Wl,--unresolved-symbols=ignore-all"] + link_cmd[1:], check=True)
        else:
            subprocess.run(["ld.lld", "-m", "elf64ppc", "--gc-sections", "-e", "main", "--unresolved-symbols=ignore-all",
                            "-o", elf, *objs], check=True)
    lines = [f"link: {'OK' if link.returncode == 0 else 'FAILED'}; undefined symbols: {len(undef)}"]
    lines.append(f"complete apart from toolchain-provided symbols: {'YES' if complete else 'NO'}"
                 + ("" if complete else f" ({len(outside)} other symbols missing)"))
    for g in sorted(groups, key=lambda k: (-len(groups[k]), k)):
        lines.append(f"  {len(groups[g]):4d}  {g}")
    if other_errors:
        lines.append(f"other link errors: {len(other_errors)}")
        lines += ["  " + e for e in other_errors[:10]]
    (out / "report.txt").write_text("\n".join(lines) + "\n")
    print("\n".join(lines))
    return 0 if (link.returncode == 0 or complete) else 1


if __name__ == "__main__":
    sys.exit(main())
