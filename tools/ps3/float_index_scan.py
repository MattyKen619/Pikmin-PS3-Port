#!/usr/bin/env python3
"""
PS3 port: find array accesses whose index comes from a float-to-integer conversion.

Pikmin computes NaN values in normal play (for example the height of a vertical wall triangle, an
x/0 that later meets 0). Converting a NaN to int gives 0x80000000. On the GameCube, table[0x80000000]
wraps around the 32-bit address space and lands on (or next to) the table, so nothing visible
happens. On the PS3 (64-bit pointers) the same index points 2-8 GB away and crashes. Tests 9 and 10
crashed this way in GetAtanTable and sinf3.

This lists every subscript whose index expression contains a float->int conversion, directly or
through a local integer variable initialised or assigned from one. Each hit needs a look: a clamp in
between makes it safe.

  python3 tools/ps3/float_index_scan.py --bindings /path/to/clang/bindings/python \\
      --libclang /usr/lib/llvm-18/lib/libclang.so.1 [--out build/ps3check/float_index_scan.json]
"""
import argparse
import json
import os
import sys
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from gcc_check import DEFINES, group_of  # noqa: E402


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
    float_kinds = {T.FLOAT, T.DOUBLE, T.LONGDOUBLE}
    int_kinds = {T.INT, T.UINT, T.SHORT, T.USHORT, T.LONG, T.ULONG, T.LONGLONG, T.ULONGLONG, T.CHAR_S, T.CHAR_U,
                 T.SCHAR, T.UCHAR, T.BOOL}
    cast_kinds = {K.CSTYLE_CAST_EXPR, K.CXX_FUNCTIONAL_CAST_EXPR, K.CXX_STATIC_CAST_EXPR, K.UNEXPOSED_EXPR}
    func_kinds = {K.FUNCTION_DECL, K.CXX_METHOD, K.CONSTRUCTOR, K.DESTRUCTOR, K.FUNCTION_TEMPLATE}

    args = ["-x", "c++", "--target=powerpc64-unknown-elf", "-std=gnu++14", "-funsigned-char", "-nostdinc", "-isystem",
            "include/stl", "-I", "include", "-w", *DEFINES]
    tu = ci.Index.create().parse(str(ROOT / src), args=args)

    def exprs(c):
        return [k for k in c.get_children() if k.kind.is_expression()]

    def is_f2i(c):
        if c.kind not in cast_kinds:
            return False
        if c.type.get_canonical().kind not in int_kinds:
            return False
        kids = exprs(c)
        return bool(kids) and kids[-1].type.get_canonical().kind in float_kinds

    def has_f2i(c, tainted):
        stack = [c]
        while stack:
            n = stack.pop()
            if is_f2i(n):
                return True
            if n.kind == K.DECL_REF_EXPR and n.referenced is not None and n.referenced.hash in tainted:
                return True
            stack.extend(n.get_children())
        return False

    out = []
    for fn in tu.cursor.walk_preorder():
        if fn.kind not in func_kinds or not fn.is_definition():
            continue
        loc = fn.location
        if not loc.file:
            continue
        fpath = os.path.relpath(loc.file.name, ROOT)
        if fpath.startswith("..") or not (fpath.startswith("src/") or fpath.startswith("include/")):
            continue
        nodes = list(fn.walk_preorder())
        tainted = set()
        # Two passes so that a variable assigned from another tainted variable is caught too.
        for _ in range(2):
            for n in nodes:
                if n.kind == K.VAR_DECL and n.type.get_canonical().kind in int_kinds:
                    init_kids = exprs(n)
                    if init_kids and has_f2i(init_kids[-1], tainted):
                        tainted.add(n.hash)
                elif n.kind in (K.BINARY_OPERATOR, K.COMPOUND_ASSIGNMENT_OPERATOR):
                    kids = exprs(n)
                    if len(kids) == 2 and kids[0].kind == K.DECL_REF_EXPR and kids[0].referenced is not None:
                        ref = kids[0].referenced
                        if ref.kind == K.VAR_DECL and ref.type.get_canonical().kind in int_kinds:
                            toks = [t.spelling for t in n.get_tokens()]
                            lhs_toks = len(list(kids[0].get_tokens()))
                            op = toks[lhs_toks] if lhs_toks < len(toks) else ""
                            if op.endswith("=") and op not in ("==", "!=", "<=", ">=") and has_f2i(kids[1], tainted):
                                tainted.add(ref.hash)
        for n in nodes:
            if n.kind != K.ARRAY_SUBSCRIPT_EXPR:
                continue
            kids = exprs(n)
            if len(kids) != 2 or not has_f2i(kids[1], tainted):
                continue
            nl = n.location
            if not nl.file:
                continue
            p = os.path.relpath(nl.file.name, ROOT)
            text = " ".join(t.spelling for t in n.get_tokens())
            out.append({"file": p, "line": nl.line, "function": fn.spelling, "expr": text[:160], "unit": src})
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bindings", required=True)
    ap.add_argument("--libclang", required=True)
    ap.add_argument("--out", default="build/ps3check/float_index_scan.json")
    ap.add_argument("-j", type=int, default=os.cpu_count() or 2)
    args = ap.parse_args()
    objdiff = json.loads((ROOT / "objdiff.json").read_text())
    srcs = []
    for u in objdiff["units"]:
        s = u.get("metadata", {}).get("source_path")
        if s and group_of(s) and group_of(s) != "libs":
            srcs.append(s)
    os.chdir(ROOT)
    found = {}
    with ProcessPoolExecutor(max_workers=args.j) as ex:
        for res in ex.map(scan_unit, [(s, args.bindings, args.libclang) for s in srcs]):
            for r in res:
                found.setdefault((r["file"], r["line"], r["expr"]), r)
    rows = sorted(found.values(), key=lambda r: (r["file"], r["line"]))
    (ROOT / args.out).parent.mkdir(parents=True, exist_ok=True)
    (ROOT / args.out).write_text(json.dumps(rows, indent=1))
    print(f"subscripts indexed by a float->int conversion: {len(rows)}")
    for r in rows:
        print(f"  {r['file']}:{r['line']} ({r['function']}): {r['expr']}")


if __name__ == "__main__":
    main()
