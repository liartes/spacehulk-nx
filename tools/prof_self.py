#!/usr/bin/env python3
"""prof_self.py -- where a thread's time really goes, by function.

The port's continuous profiler ([debug] profile_continuous) appends every
10 s window's running samples to <root>/prof_self.txt, per busy thread: each
pc with its count, as module+offset (or JIT:<C# method>). This sums them by
function -- spacehulk_nx (the port, newlib, libnx, Mesa) with the build's
ELF and addr2line, libunity / libmono by function start (and the nearest
exported name) -- and by category: allocator, locks and waits, Mesa, Unity,
Mono, the game's C#.

    python3 tools/prof_self.py prof_self.txt --elf logs/elf/spacehulk_nx-<build>.elf \\
        --libs <dir with libunity.so libmono.so> [--windows 5-12] [--top 25]

addr2line runs in the toolchain image (docker).
"""
import argparse
import collections
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "offsets"))

CATS = [
    ("allocator", r"^(_?malloc|_?free|_?realloc|_?calloc|_?memalign|_malloc_r|_free_r|_realloc_r|_calloc_r|_memalign_r|malloc_|__malloc_|_malloc_trim|mallinfo)"),
    ("locks/waits", r"(lock|Lock|mutex|Mutex|svcArbitrate|svcWait|condvar|cond_|_GetTag|_LoadExclusive|_StoreExclusive|futex|sem_)"),
    ("memcpy/memset", r"^(memcpy|memmove|memset|memcmp|__aeabi_mem)"),
    ("mesa/nouveau", r"^(_mesa|st_|nvc0|nv50|nouveau|pipe_|util_|cso_|tgsi|u_|vbo_|draw_|_glapi|bo_|glsl|ir_|nir_|ralloc|hash_table|_hash|lp_|translate_|update_|bind_|map_buffer|validate_|vtn|gl[A-Z])"),
    ("libnx/svc", r"^(svc|serviceDispatch|ipc|armDCache|nvMap|nvFence|nvGpu|nwindow|eventWait)"),
]


def addr2line(elf, offs):
    """offset (hex str) -> function name, for the port's ELF (docker toolchain)."""
    if not offs:
        return {}
    d = os.path.dirname(os.path.abspath(elf))
    cmd = ["docker", "run", "--rm", "-i", "-v", f"{d}:/e", "ghcr.io/vita2hos/devcontainer/vita2hos", "bash", "-lc",
           f"arm-none-eabi-addr2line -f -C -e /e/{os.path.basename(elf)}"]
    out = subprocess.run(cmd, input="\n".join(offs), capture_output=True, text=True).stdout.splitlines()
    return {o: out[2 * i] for i, o in enumerate(offs) if 2 * i < len(out)}


class Lib:
    def __init__(self, path):
        from arm32 import Arm32
        self.u = Arm32(path)
        self.syms = sorted((v, n) for n, v in self.u.dynsym_def.items() if v)
        self.cache = {}

    def name(self, off):
        if off in self.cache:
            return self.cache[off]
        fn = self.u.func_start(off) or off
        import bisect
        i = bisect.bisect_right(self.syms, (fn, "\xff")) - 1
        n = f"fn@0x{fn:x}"
        if i >= 0 and self.syms[i][0] == fn:
            n = self.syms[i][1]
        self.cache[off] = n
        return n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--elf", required=True)
    ap.add_argument("--libs", required=True)
    ap.add_argument("--windows", default=None, help="e.g. 5-12 (1-based, inclusive)")
    ap.add_argument("--top", type=int, default=25)
    a = ap.parse_args()
    lo, hi = (1, 10 ** 9)
    if a.windows:
        p = a.windows.split("-")
        lo, hi = int(p[0]), int(p[-1])

    per = collections.defaultdict(collections.Counter)  # thread -> raw key -> samples
    totals = collections.Counter()                       # thread -> running+io+wait samples
    run = collections.Counter()
    w = 0
    thread = None
    for line in open(a.file, errors="replace"):
        line = line.rstrip("\n")
        if line.startswith("W "):
            w = int(line.split()[1])
            continue
        if not (lo <= w <= hi):
            continue
        if line.startswith("T "):
            f = line.split()
            thread = f[1]
            run[thread] += int(f[2]) + int(f[3])
            totals[thread] += int(f[2]) + int(f[3]) + int(f[4])
            continue
        m = re.match(r"(\d+) (.*)$", line)
        if m and thread:
            per[thread][m.group(2).split(" (")[0]] += int(m.group(1))

    offs = sorted({k.split("+")[1] for t in per.values() for k in t if k.startswith("spacehulk_nx+")})
    port = addr2line(a.elf, offs)
    libs = {n: Lib(os.path.join(a.libs, n)) for n in ("libunity.so", "libmono.so") if os.path.exists(os.path.join(a.libs, n))}

    def fname(k):
        if k.startswith("JIT:"):
            return "C# " + k[4:], "game C# (JIT)"
        mod, _, off = k.partition("+")
        if mod == "spacehulk_nx":
            n = port.get(off, k)
            for cat, rx in CATS:
                if re.search(rx, n):
                    return n, cat
            return n, "port/other"
        if mod in libs:
            return f"{mod}:{libs[mod].name(int(off, 16))}", "unity" if mod == "libunity.so" else "mono"
        return k, "other"

    for t in sorted(per, key=lambda t: -run[t]):
        funcs = collections.Counter()
        cats = collections.Counter()
        for k, c in per[t].items():
            n, cat = fname(k)
            funcs[n] += c
            cats[cat] += c
        tot = totals[t] or 1
        busy = sum(per[t].values()) or 1
        print(f"\n=== {t}: busy {100 * run[t] / tot:.0f}% of the samples ({sum(per[t].values())} counted)")
        print("  by category (% of the thread's busy samples):")
        for cat, c in cats.most_common():
            print(f"    {cat:16s} {100 * c / busy:5.1f}%")
        print("  top functions:")
        for n, c in funcs.most_common(a.top):
            print(f"    {100 * c / busy:5.1f}%  {n}")


if __name__ == "__main__":
    main()
