#!/usr/bin/env python3
"""
verify_offsets.py -- independent check that source/dcr_offsets.h still describes
the shipped binaries. Parses the generated header, re-reads each guard from the
.so, and confirms it matches -- the on-host half of the "cross-checked twice"
bar (the generator finds addresses structurally; this confirms them literally).

Exits non-zero on any mismatch, so it can gate a build / CI.

    python3 verify_offsets.py <lib/armeabi-v7a> [--header source/dcr_offsets.h]
"""
import sys
import os
import re
import argparse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from arm32 import Arm32


def parse(header):
    txt = open(header).read()
    rvas = {m.group(1): int(m.group(2), 16)
            for m in re.finditer(r"#define\s+DCR_RVA_(\w+)\s+0x([0-9a-f]+)u", txt)}
    mono_rvas = {m.group(1): int(m.group(2), 16)
                 for m in re.finditer(r"#define\s+DCR_MONO_RVA_(\w+)\s+0x([0-9a-f]+)u", txt)}
    mono_guards = {m.group(1): [int(x, 16) for x in re.findall(r"0x([0-9a-f]+)u", m.group(2))]
                   for m in re.finditer(r"DCR_MONO_GUARD_(\w+)\[4\]\s*=\s*\{([^}]*)\}", txt)}
    guards = {m.group(1): [int(x, 16) for x in re.findall(r"0x([0-9a-f]+)u", m.group(2))]
              for m in re.finditer(r"DCR_GUARD_(\w+)\[4\]\s*=\s*\{([^}]*)\}", txt)}
    sites = []
    for block in re.finditer(r"DCR_(\w+)_SITES\[\]\s*=\s*\{(.*?)\n\};", txt, re.S):
        for row in re.finditer(r'\{\s*"([^"]+)",\s*0x([0-9a-f]+)u,\s*\{([^}]*)\}', block.group(2)):
            sites.append((row.group(1), int(row.group(2), 16),
                          [int(x, 16) for x in re.findall(r"0x([0-9a-f]+)u", row.group(3))]))
    return rvas, guards, sites, mono_rvas, mono_guards


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("libdir")
    ap.add_argument("--header", default=os.path.join(
        os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
        "source", "dcr_offsets.h"))
    a = ap.parse_args()
    u = Arm32(os.path.join(a.libdir, "libunity.so"))
    mo = Arm32(os.path.join(a.libdir, "libmono.so"))
    rvas, guards, sites, mono_rvas, mono_guards = parse(a.header)

    ok = bad = 0
    # RVA + matching guard-by-name
    for name, rva in sorted(rvas.items()):
        g = guards.get(name)
        if g is None:
            continue
        actual = u.guard(rva)
        good = actual == g
        ok += good
        bad += not good
        print("  %-28s 0x%07x  %s" % (name, rva, "OK" if good else
              "MISMATCH got %s" % [hex(x) for x in actual]))
    # icall site tables
    for name, rva, g in sites:
        actual = u.guard(rva)
        good = actual == g
        ok += good
        bad += not good
        print("  site %-24s 0x%07x  %s" % (name, rva, "OK" if good else
              "MISMATCH got %s" % [hex(x) for x in actual]))
    for name, rva in sorted(mono_rvas.items()):
        g = mono_guards.get(name)
        if g is None:
            continue
        actual = mo.guard(rva)
        good = actual == g
        ok += good
        bad += not good
        print("  mono %-23s 0x%07x  %s" % (name, rva, "OK" if good else
              "MISMATCH got %s" % [hex(x) for x in actual]))
    print("\n%d ok, %d mismatched" % (ok, bad))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
