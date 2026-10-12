#!/usr/bin/env python3
"""
PS3 port: compile the code we intend to keep with GCC and collect diagnostics.

This is a compile-only check (no linking). It mirrors the defines of the matching
build, adds TARGET_PS3, and uses an LP64, unsigned-char configuration like the
PS3 PPU toolchain (ps3dev GCC 13.2.0).

Usage:
  python3 tools/ps3/gcc_check.py [--cxx g++] [--cc gcc] [--target TRIPLE] [--out DIR] [-j N]

Writes DIR/results.json (per-file error/warning counts) and DIR/logs/<unit>.log.
"""
import argparse
import json
import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# Groups of code that the port keeps (the Nintendo SDK layer is replaced, so it is not checked).
KEEP_GROUPS = {
    "game": ["plugPikiColin", "plugPikiKando", "plugPikiNakata", "plugPikiNishimura",
             "plugPikiOgawa", "plugPikiYamashita", "sysCommon", "sysDolphin", "sysBootup.cpp"],
    "jaudio": ["jaudio"],
    "libs": ["mtx", "hvqm4dec"],
}

DEFINES = [
    "-DBUILD_VERSION=5",          # GPIE01_01
    "-DVERSION_GPIE01_01",
    "-DDTK_CONFIG_NONMATCHING=1",
    "-DNDEBUG=1",
    "-DTARGET_PS3=1",
]


def group_of(src: str):
    rel = src[len("src/"):] if src.startswith("src/") else src
    top = rel.split("/")[0]
    for g, tops in KEEP_GROUPS.items():
        if top in tops:
            return g
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cxx", default="g++")
    ap.add_argument("--cc", default="gcc")
    ap.add_argument("--target", default=None, help="clang --target triple (optional)")
    ap.add_argument("--out", default="build/ps3check")
    ap.add_argument("-j", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--extra", action="append", default=[], help="extra flag (repeatable)")
    ap.add_argument("--warnings", action="store_true", help="report warnings instead of suppressing them")
    args = ap.parse_args()

    objdiff = json.loads((ROOT / "objdiff.json").read_text())
    units = []
    for u in objdiff["units"]:
        src = u.get("metadata", {}).get("source_path")
        if src and group_of(src):
            units.append((u["name"], src, group_of(src)))

    out = ROOT / args.out
    (out / "logs").mkdir(parents=True, exist_ok=True)

    common = [
        "-fsyntax-only", "-funsigned-char", "-fno-exceptions",
        "-nostdinc", "-isystem", "include/stl", "-I", "include", "-I", "build/GPIE01_01/include", "-I", "platform/ps3/include",
        "-fmax-errors=0",
        *DEFINES, *args.extra,
    ]
    if not args.warnings:
        common.append("-w")
    if args.target:
        common = [f"--target={args.target}"] + common

    def run(item):
        name, src, group = item
        # jaudio's .c files are compiled as C++ by the matching build ("-lang c++" in cflags_jaudio).
        is_c = src.endswith(".c") and group != "jaudio"
        if src.endswith(".c") and not is_c:
            cmd_lang = ["-x", "c++"]
        else:
            cmd_lang = []
        cmd = [args.cc if is_c else args.cxx] + (["-std=gnu99"] if is_c else ["-std=gnu++14"]) + common + cmd_lang + [src]
        p = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
        log = p.stderr
        safe = name.replace("/", "__")
        (out / "logs" / f"{safe}.log").write_text(log)
        errors = [l for l in log.splitlines() if re.search(r"\berror\b:", l)]
        warnings = [l for l in log.splitlines() if re.search(r"\bwarning\b:", l)]
        return {"unit": name, "source": src, "group": group, "ok": p.returncode == 0,
                "errors": len(errors), "warnings": len(warnings), "first_error": errors[0] if errors else ""}

    with ThreadPoolExecutor(max_workers=args.j) as ex:
        results = list(ex.map(run, units))

    (out / "results.json").write_text(json.dumps(results, indent=1))
    total = len(results)
    ok = sum(r["ok"] for r in results)
    print(f"files: {total}  compile clean: {ok}  with errors: {total - ok}  total errors: {sum(r['errors'] for r in results)}")
    for g in KEEP_GROUPS:
        rs = [r for r in results if r["group"] == g]
        print(f"  {g:7s} files={len(rs):4d} clean={sum(r['ok'] for r in rs):4d} errors={sum(r['errors'] for r in rs)}")


if __name__ == "__main__":
    sys.exit(main())
