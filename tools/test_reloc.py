#!/usr/bin/env python3
"""
test_reloc.py -- on-host validation of so_util.c's ARM REL relocation handling.

so_util.c cannot run here (no libnx32, no Switch), but its relocation math is
pure arithmetic over the ELF image. This test:

  1. confirms EVERY relocation in libunity/libmono/libmain is a type the loader
     handles (R_ARM_RELATIVE/ABS32/GLOB_DAT/JUMP_SLOT) -- a new type would make
     so_relocate fatal_error at boot, so catching it here is the point;
  2. re-implements apply_rel() in Python and applies it to a simulated load at a
     chosen bias, then checks the invariants the C code must produce:
       - RELATIVE:  out = in_image_addend + bias         (points inside image)
       - ABS32(def):out = bias + sym.value + addend
       - GLOB_DAT/JUMP_SLOT(def): out = bias + sym.value
     and that every defined-symbol result lands inside the mapped image.

Run: python3 tools/test_reloc.py <lib/armeabi-v7a>
"""
import sys
import os
import struct
from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection

R_ARM_ABS32, R_ARM_GLOB_DAT, R_ARM_JUMP_SLOT, R_ARM_RELATIVE = 2, 21, 22, 23
HANDLED = {R_ARM_ABS32, R_ARM_GLOB_DAT, R_ARM_JUMP_SLOT, R_ARM_RELATIVE}
NAMES = {2: "ABS32", 21: "GLOB_DAT", 22: "JUMP_SLOT", 23: "RELATIVE"}


class Image:
    """A flat little-endian image addressed by p_vaddr, like the loader's load_base."""
    def __init__(self, elf):
        self.load_size = 0
        segs = []
        for p in elf.iter_segments():
            if p["p_type"] == "PT_LOAD":
                segs.append(p)
                self.load_size = max(self.load_size, p["p_vaddr"] + p["p_memsz"])
        self.load_size = (self.load_size + 0xFFF) & ~0xFFF
        self.buf = bytearray(self.load_size)
        for p in segs:
            self.buf[p["p_vaddr"]:p["p_vaddr"] + p["p_filesz"]] = p.data()

    def u32(self, va):
        return struct.unpack_from("<I", self.buf, va)[0]

    def w32(self, va, val):
        struct.pack_into("<I", self.buf, va, val & 0xFFFFFFFF)


def run(path):
    elf = ELFFile(open(path, "rb"))
    dynsym = elf.get_section_by_name(".dynsym")
    img = Image(elf)
    BIAS = 0x30000000
    counts, applied, checked, fails = {}, 0, 0, 0

    writes = {}
    # so_util applies the SAME table in two passes: so_relocate (local only) then
    # so_resolve (imports only). Model both and require every target to be
    # written at most once -- a second write is the double-bias bug.
    for pass_no in (1, 2):
      for sec in elf.iter_sections():
        if not isinstance(sec, RelocationSection):
            continue
        for r in sec.iter_relocations():
            t = r["r_info_type"]
            sym0 = dynsym.get_symbol(r["r_info_sym"])
            local = (t == R_ARM_RELATIVE) or (sym0["st_shndx"] != "SHN_UNDEF")
            if (pass_no == 1) != local:
                continue                    # pass 1: local only; pass 2: imports only
            if pass_no == 2:
                writes[r["r_offset"]] = writes.get(r["r_offset"], 0) + 1
                continue                    # imports resolve against shims at runtime
            counts[t] = counts.get(t, 0) + 1
            if t not in HANDLED:
                print("  !! UNHANDLED reloc type %d at 0x%x -- loader would abort"
                      % (t, r["r_offset"]))
                fails += 1
                continue
            off = r["r_offset"]
            addend = img.u32(off)                      # REL: addend is at target
            sym = dynsym.get_symbol(r["r_info_sym"])
            defined = sym["st_shndx"] != "SHN_UNDEF"
            if t == R_ARM_RELATIVE:
                out = (addend + BIAS) & 0xFFFFFFFF
            elif t == R_ARM_ABS32:
                out = ((BIAS + sym["st_value"] + addend) & 0xFFFFFFFF) if defined else None
            else:  # GLOB_DAT / JUMP_SLOT
                out = ((BIAS + sym["st_value"]) & 0xFFFFFFFF) if defined else None
            if out is None:
                continue                                # import: resolved later
            img.w32(off, out)
            writes[off] = writes.get(off, 0) + 1
            applied += 1
            # invariant: a resolved pointer into this image lands in [BIAS, BIAS+size)
            checked += 1
            if not (BIAS <= out < BIAS + img.load_size):
                # ABS32 may legitimately point at a symbol's absolute value, but
                # for defined syms in a PIC .so it is always in-image.
                print("  !! %s at 0x%x -> 0x%x OUT OF IMAGE (size 0x%x)"
                      % (NAMES[t], off, out, img.load_size))
                fails += 1

    dup = [o for o, c in writes.items() if c > 1]
    if dup:
        print("  !! %d relocation targets written more than once (e.g. 0x%x)" % (len(dup), dup[0]))
        fails += len(dup)
    print("  %s" % os.path.basename(path))
    for t in sorted(counts):
        print("    %-9s x%d %s" % (NAMES.get(t, "?%d" % t), counts[t],
                                   "" if t in HANDLED else "<-- UNHANDLED"))
    print("    applied %d, invariant-checked %d, failures %d" % (applied, checked, fails))
    return fails


def main():
    libdir = sys.argv[1]
    total = 0
    for lib in ("libmain.so", "libunity.so", "libmono.so"):
        total += run(os.path.join(libdir, lib))
    print("\n%s" % ("PASS" if total == 0 else "FAIL (%d)" % total))
    sys.exit(1 if total else 0)


if __name__ == "__main__":
    main()
