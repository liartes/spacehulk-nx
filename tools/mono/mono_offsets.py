#!/usr/bin/env python3
"""mono_offsets.py -- the libmono addresses source/mono_rt.c needs, read from
the symbol table of a libmono built by tools/mono/build_libmono.sh (unstripped).

    PYTHONPATH=.pydeps python3 tools/mono/mono_offsets.py <libmono.so> [name]

The game's libmono is stripped, so tools/offsets/derive_offsets.py finds these
by the shape of its -O0 code; a rebuilt one carries its symbols, and an
optimized one has another shape. Prints a MonoOffsets initializer (C) with each
function's first four instruction words as its guard. MIT.
"""
import struct
import sys

from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection


def main():
    path = sys.argv[1]
    name = sys.argv[2] if len(sys.argv) > 2 else "rebuilt"
    data = open(path, "rb").read()
    e = ELFFile(open(path, "rb"))

    def vread(va, n=4):
        for s in e.iter_segments():
            if s["p_type"] == "PT_LOAD" and s["p_vaddr"] <= va < s["p_vaddr"] + s["p_filesz"]:
                o = s["p_offset"] + va - s["p_vaddr"]
                return data[o:o + n]
        raise KeyError(hex(va))

    syms = {}
    for sec in e.iter_sections():
        if sec["sh_type"] in ("SHT_SYMTAB", "SHT_DYNSYM"):
            for s in sec.iter_symbols():
                if s.name and s["st_value"]:
                    syms.setdefault(s.name, s["st_value"])

    def fn(n):
        a = syms[n] & ~1
        return a, struct.unpack("<4I", vread(a, 16))

    # GOT slots holding a variable's address: R_ARM_GLOB_DAT against it, or
    # R_ARM_RELATIVE whose stored addend is its address
    def got_slot(var):
        want = syms[var]
        for sec in e.iter_sections():
            if not isinstance(sec, RelocationSection):
                continue
            symtab = e.get_section(sec["sh_link"])
            for r in sec.iter_relocations():
                t, off = r["r_info_type"], r["r_offset"]
                if t == 21 and r["r_info_sym"] and symtab.get_symbol(r["r_info_sym"]).name == var:
                    return off
                if t == 23 and struct.unpack("<I", vread(off))[0] == want:
                    return off
        raise KeyError(var)

    flush, flush_g = fn("mono_arch_flush_icache")
    # the reference derive_offsets.py checks: the handler body (_GC_suspend_handler;
    # an optimized build may inline it into GC_suspend_handler)
    handler, handler_g = fn("_GC_suspend_handler" if "_GC_suspend_handler" in syms else "GC_suspend_handler")
    lookup, lookup_g = fn("GC_lookup_thread")
    stop = got_slot("GC_stop_count")
    ack = got_slot("GC_suspend_ack_sem")

    def g(w):
        return "{" + ", ".join("0x%08xu" % x for x in w) + "}"

    print("    {\"%s\", 0x%07xu, %s, 0x%07xu, %s, 0x%07xu, %s, 0x%07xu, 0x%07xu}," %
          (name, flush, g(flush_g), handler, g(handler_g), lookup, g(lookup_g), stop, ack))


if __name__ == "__main__":
    main()
