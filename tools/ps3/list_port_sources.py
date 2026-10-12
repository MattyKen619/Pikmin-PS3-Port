#!/usr/bin/env python3
"""
PS3 port: write tools/ps3/port_sources.txt, the list of decompiled sources the PS3 build compiles
("<group> <path>" per line). Generated from objdiff.json (created by configure.py); the list is
committed so the PS3 build does not need the matching-build setup.
"""
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from gcc_check import group_of  # noqa: E402

rows = []
for u in json.loads((ROOT / "objdiff.json").read_text())["units"]:
    src = u.get("metadata", {}).get("source_path")
    if src and group_of(src):
        rows.append(f"{group_of(src)} {src}")
rows.append("libs src/MSL_C/PPCEABI/bare/H/float.c")  # MSL float constants (__float_nan etc.)
(ROOT / "tools/ps3/port_sources.txt").write_text("\n".join(rows) + "\n")
print(f"{len(rows)} sources -> tools/ps3/port_sources.txt")
