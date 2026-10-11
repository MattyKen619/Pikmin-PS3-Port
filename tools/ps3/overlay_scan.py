#!/usr/bin/env python3
"""
PS3 port: find places where raw memory (byte buffers, void*, integers) is cast to a pointer to a
struct whose layout depends on pointer size. Those are candidates for "struct laid over file data",
which breaks on 64-bit targets unless the struct uses PTR32 fields.

Needs libclang + its Python bindings:
  python3 tools/ps3/overlay_scan.py --bindings /path/to/clang/bindings/python \
      --libclang /usr/lib/llvm-18/lib/libclang-18.so.1 [--out build/ps3check/overlay_scan.json]
"""
import argparse
import json
import os
import sys
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from gcc_check import KEEP_GROUPS, DEFINES, group_of  # noqa: E402

BYTE_KINDS = None  # filled after import of cindex
ARGS = None


def base_args(target):
    return [f"--target={target}", "-std=gnu++14", "-funsigned-char", "-nostdinc", "-isystem", "include/stl",
            "-I", "include", "-w", *DEFINES]


def init(bindings, libclang):
    sys.path.insert(0, bindings)
    import clang.cindex as ci
    if not ci.Config.loaded:
        ci.Config.set_library_file(libclang)
    return ci


def scan_unit(task):
    src, bindings, libclang = task
    ci = init(bindings, libclang)
    K = ci.CursorKind
    T = ci.TypeKind
    byte_kinds = {T.CHAR_U, T.UCHAR, T.CHAR_S, T.SCHAR, T.VOID}
    int_kinds = {T.UINT, T.INT, T.ULONG, T.LONG, T.ULONGLONG, T.LONGLONG, T.USHORT, T.SHORT}
    cast_kinds = {K.CSTYLE_CAST_EXPR, K.CXX_REINTERPRET_CAST_EXPR, K.CXX_STATIC_CAST_EXPR}
    results = {}
    sizes = {}
    for target in ("powerpc64-unknown-elf", "powerpc-unknown-elf"):
        idx = ci.Index.create()
        lang = ["-x", "c++"]
        tu = idx.parse(str(ROOT / src), args=lang + base_args(target))
        for cur in tu.cursor.walk_preorder():
            if cur.kind not in cast_kinds:
                continue
            loc = cur.location
            if not loc.file:
                continue
            fpath = os.path.relpath(loc.file.name, ROOT)
            if fpath.startswith(".."):
                continue
            dst = cur.type.get_canonical()
            if dst.kind != T.POINTER:
                continue
            pointee = dst.get_pointee().get_canonical()
            if pointee.kind != T.RECORD:
                continue
            kids = [c for c in cur.get_children() if c.kind.is_expression()]
            if not kids:
                continue
            st = kids[-1].type.get_canonical()
            if st.kind == T.POINTER:
                sp = st.get_pointee().get_canonical()
                if sp.kind not in byte_kinds and sp.kind != T.INT and sp.kind != T.UINT:
                    continue
                src_desc = sp.spelling + "*"
            elif st.kind in int_kinds:
                src_desc = st.spelling
            else:
                continue
            name = pointee.spelling
            key = (fpath, loc.line, name)
            sizes.setdefault(name, {})[target] = pointee.get_size()
            if target == "powerpc64-unknown-elf":
                results[key] = src_desc
    out = []
    for (fpath, line, name), src_desc in results.items():
        s = sizes.get(name, {})
        out.append({"file": fpath, "line": line, "struct": name, "from": src_desc,
                    "size32": s.get("powerpc-unknown-elf"), "size64": s.get("powerpc64-unknown-elf"),
                    "unit": src})
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bindings", required=True)
    ap.add_argument("--libclang", required=True)
    ap.add_argument("--out", default="build/ps3check/overlay_scan.json")
    ap.add_argument("-j", type=int, default=os.cpu_count() or 2)
    args = ap.parse_args()
    objdiff = json.loads((ROOT / "objdiff.json").read_text())
    srcs = []
    for u in objdiff["units"]:
        s = u.get("metadata", {}).get("source_path")
        if s and group_of(s) and group_of(s) != "libs":  # libs (mtx, hvqm4dec) are C; checked by hand
            srcs.append(s)
    os.chdir(ROOT)
    found = {}
    with ProcessPoolExecutor(max_workers=args.j) as ex:
        for res in ex.map(scan_unit, [(s, args.bindings, args.libclang) for s in srcs]):
            for r in res:
                found.setdefault((r["file"], r["line"], r["struct"]), r)
    rows = sorted(found.values(), key=lambda r: (r["file"], r["line"]))
    (ROOT / args.out).parent.mkdir(parents=True, exist_ok=True)
    (ROOT / args.out).write_text(json.dumps(rows, indent=1))
    risky = [r for r in rows if r["size32"] != r["size64"]]
    print(f"casts from raw memory to struct pointers: {len(rows)}; to pointer-size-dependent structs: {len(risky)}")
    for r in risky:
        print(f"  {r['file']}:{r['line']}: ({r['struct']}*) from {r['from']}  size {r['size32']} -> {r['size64']}")


if __name__ == "__main__":
    main()
