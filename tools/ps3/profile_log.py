#!/usr/bin/env python3
"""
PS3 port: turn the sampling profiler's "[prof]" blocks in a pikmin_log.txt into tables with function names
(platform/ps3/sdk/profiler.cpp writes them at every game section change, every minute and on exit).

  python3 tools/ps3/profile_log.py build/ps3/pikmin.elf pikmin_log.txt [--top N]

"self": samples whose last game code ran in the function (including SDK and GX library code it called).
"incl": samples with the function anywhere on the game thread's call stack.
Percentages are of the samples where a game thread was running (not waiting).
"""
import bisect
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from symbolize_log import read_elf, demangle  # noqa: E402


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
        return code[i][1] if i >= 0 else "?"

    blocks = []
    cur = None
    for line in open(log, errors="replace"):
        m = re.match(r'\[prof\] begin "(.*)" \((.*)\): (\d+) samples over (\d+) s; running:(.*?) waiting (\d+)%, no thread (\d+)%', line)
        if m:
            cur = dict(section=m.group(1), why=m.group(2), samples=int(m.group(3)), secs=int(m.group(4)),
                       threads=m.group(5).strip().rstrip(","), waiting=int(m.group(6)), nothread=int(m.group(7)), self={}, incl={})
            continue
        m = re.match(r"\[prof\] (self|incl) (\d+) (?:0x)?([0-9a-fA-F]+)", line)
        if m and cur is not None:
            count, addr = int(m.group(2)), int(m.group(3), 16)
            if m.group(1) == "self":
                name = fn_of_pc(addr)
                cur["self"][name] = cur["self"].get(name, 0) + count
            else:
                name = descr.get(addr) or fn_of_pc(addr)
                cur["incl"][name] = cur["incl"].get(name, 0) + count
            continue
        if line.startswith("[prof] end") and cur is not None:
            blocks.append(cur)
            cur = None

    allnames = set()
    for b in blocks:
        allnames.update(b["self"].keys())
        allnames.update(b["incl"].keys())
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
        print()


if __name__ == "__main__":
    main()
