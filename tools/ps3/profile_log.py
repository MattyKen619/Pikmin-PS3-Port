#!/usr/bin/env python3
"""
PS3 port: turn the sampling profiler's "[prof]" blocks in a pikmin_log.txt into tables with function names
(platform/ps3/sdk/profiler.cpp writes them at every game section change, every minute and on exit).

  python3 tools/ps3/profile_log.py build/ps3/pikmin.elf pikmin_log.txt [--top N]

"self": samples whose last game code ran in the function (including SDK and GX library code it called).
"incl": samples with the function anywhere on the game thread's call stack. (Test build 45 on: return
addresses walked from the stack, so SDK and library functions that call others show up too; a function
that calls itself, or is called from two places on the same stack, can count more than once: over 100%.)
Percentages are of the samples where a game thread was running (not waiting).
"waiting in" (test build 45 on): while no game thread ran, the last game code of the one waiting (GXDrawDone
from DGXGraphics::doneRender, the retrace, ...); percentages are of the waiting samples.
"gfx self" / "gfx incl" (test build 41 on): the graphics thread, in the samples where it was interpreting
commands; percentages are of those samples.
"""
import bisect
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from symbolize_log import read_elf, demangle, SIZES  # noqa: E402


def main():
    argv = sys.argv[1:]
    top = 25
    if "--top" in argv:
        i = argv.index("--top")
        top = int(argv[i + 1])
        del argv[i:i + 2]
    args = argv
    if len(args) != 2:
        sys.exit(__doc__)
    elf, log = args
    descr, code = read_elf(elf)
    starts = [a for a, _ in code]
    names = {}

    def fn_of_pc(pc):
        i = bisect.bisect_right(starts, pc) - 1
        if i < 0:
            return "?"
        size = SIZES.get(code[i][0], 0)
        if size and pc >= code[i][0] + size:
            return "(code without a symbol: start-up, system library)"
        return code[i][1]

    blocks = []
    cur = None
    for line in open(log, errors="replace"):
        m = re.match(r'\[prof\] begin "(.*)" \((.*)\): (\d+) samples over (\d+) s; running:(.*?) waiting (\d+)%, no thread (\d+)%', line)
        if m:
            cur = dict(section=m.group(1), why=m.group(2), samples=int(m.group(3)), secs=int(m.group(4)),
                       threads=m.group(5).strip().rstrip(","), waiting=int(m.group(6)), nothread=int(m.group(7)), self={}, incl={},
                       gfxbusy=0, gfx_self={}, gfx_incl={}, wait={})
            continue
        m = re.match(r"\[prof\] gfx busy (\d+) of (\d+) samples", line)
        if m and cur is not None:
            cur["gfxbusy"] = int(m.group(1))
            continue
        m = re.match(r"\[prof\] (gfx self|gfx incl|self|incl|wait) (\d+) (?:0x)?([0-9a-fA-F]+)", line)
        if m and cur is not None:
            kind = m.group(1).replace(" ", "_")
            count, addr = int(m.group(2)), int(m.group(3), 16)
            # (a function's descriptor where it had just been entered, else a code address: a call site
            # returned to, or a return address on the stack; up to test build 44 the stacks held descriptors)
            name = descr.get(addr) or fn_of_pc(addr)
            cur[kind][name] = cur[kind].get(name, 0) + count
            continue
        if line.startswith("[prof] end") and cur is not None:
            blocks.append(cur)
            cur = None

    allnames = set()
    for b in blocks:
        for kind in ("self", "incl", "gfx_self", "gfx_incl", "wait"):
            allnames.update(b[kind].keys())
    pretty = demangle(list(allnames))

    for b in blocks:
        running = b["samples"] * (100 - b["waiting"] - b["nothread"]) // 100 or 1
        print(f'== "{b["section"]}" ({b["why"]}): {b["samples"]} samples over {b["secs"]} s; '
              f'running: {b["threads"]}; waiting {b["waiting"]}%')
        for kind in ("self", "incl"):
            print(f"  {kind}:")
            items = sorted(b[kind].items(), key=lambda kv: -kv[1])[:top]
            for name, count in items:
                print(f"    {100.0 * count / running:5.1f}%  {pretty.get(name, name)}")
        if b["wait"]:
            waiting = sum(b["wait"].values()) or 1
            print("  waiting in:")
            for name, count in sorted(b["wait"].items(), key=lambda kv: -kv[1])[:top]:
                print(f"    {100.0 * count / waiting:5.1f}%  {pretty.get(name, name)}")
        if b["gfxbusy"]:
            print(f'  graphics thread: interpreting commands in {b["gfxbusy"]} samples ({100 * b["gfxbusy"] // max(b["samples"], 1)}%)')
            for kind in ("gfx_self", "gfx_incl"):
                print(f"  {kind.replace('_', ' ')}:")
                items = sorted(b[kind].items(), key=lambda kv: -kv[1])[:top]
                for name, count in items:
                    print(f"    {100.0 * count / b['gfxbusy']:5.1f}%  {pretty.get(name, name)}")
        print()


if __name__ == "__main__":
    main()
