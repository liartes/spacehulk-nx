#!/usr/bin/env python3
"""
arm32.py -- AArch32 ELF analysis helpers for deriving offsets from the game's
own stripped .so files (Unity 2017.4.17f1, armeabi-v7a, Mono backend).

This is the 32-bit counterpart of the arm64 tooling the Drive Ahead port uses.
Everything here is READ-ONLY analysis of a shared object: it never writes to the
binary. derive_offsets.py builds on it to emit source/dcr_offsets.h.

The one idea that makes the whole approach work: the game ships stripped .so
files, but the functions we need to hook or patch have a fixed shape (a Time
getter is "call GetTimeManager, load one field, return"; WaitVSync is
"lock / while(counter<target) cond_wait / unlock"). We locate them structurally
-- by the strings, PLT calls and constants they carry -- rather than by symbol,
so the same script re-derives correct addresses after a game update.

Requires: pyelftools, capstone.
"""
import struct
import re
import io
from elftools.elf.elffile import ELFFile
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB
from capstone.arm import ARM_OP_IMM, ARM_OP_MEM, ARM_REG_PC


class Arm32:
    """A loaded AArch32 shared object, addressed by virtual address (RVA)."""

    def __init__(self, path):
        self.path = path
        self.raw = open(path, "rb").read()
        self.e = ELFFile(io.BytesIO(self.raw))
        self.segs = [
            (p["p_vaddr"], p["p_offset"], p["p_filesz"], p["p_memsz"])
            for p in self.e.iter_segments()
            if p["p_type"] == "PT_LOAD"
        ]
        self.arm = Cs(CS_ARCH_ARM, CS_MODE_ARM)
        self.arm.detail = True
        self.thumb = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
        self.thumb.detail = True
        self.plt = {}          # PLT stub VA -> imported symbol name
        self.dynsym_undef = {} # imported (UND) name -> index
        self.dynsym_def = {}   # exported name -> value (RVA)
        self._load_dynsym()
        self._build_plt()

    # -- address translation ------------------------------------------------
    def off(self, va):
        for v, o, fs, _ in self.segs:
            if v <= va < v + fs:
                return o + va - v
        return None

    def u32(self, va):
        return struct.unpack_from("<I", self.raw, self.off(va))[0]

    def u16(self, va):
        return struct.unpack_from("<H", self.raw, self.off(va))[0]

    def u8(self, va):
        return self.raw[self.off(va)]

    def cstr(self, va):
        o = self.off(va)
        end = self.raw.index(b"\0", o)
        return self.raw[o:end].decode("latin1")

    def guard(self, va, n=4):
        """The n leading 32-bit words at va -- the fingerprint a consumer checks
        before it writes, so a stale offset SKIPS instead of corrupting code."""
        return [self.u32(va + 4 * i) for i in range(n)]

    # -- dynamic symbols ----------------------------------------------------
    def _load_dynsym(self):
        ds = self.e.get_section_by_name(".dynsym")
        if not ds:
            return
        for i, s in enumerate(ds.iter_symbols()):
            if not s.name:
                continue
            if s["st_shndx"] == "SHN_UNDEF":
                self.dynsym_undef[s.name] = i
            else:
                self.dynsym_def[s.name] = s["st_value"]

    # -- PLT ----------------------------------------------------------------
    def _build_plt(self):
        e = self.e
        relplt = e.get_section_by_name(".rel.plt")
        dsym = e.get_section_by_name(".dynsym")
        if not relplt or not dsym:
            return
        got2name = {
            r["r_offset"]: dsym.get_symbol(r["r_info_sym"]).name
            for r in relplt.iter_relocations()
        }
        plt = e.get_section_by_name(".plt")
        if not plt:
            return
        pb, pd = plt["sh_addr"], plt.data()

        def dpimm(w):  # ARM data-processing rotated immediate
            imm8, rot = w & 0xFF, ((w >> 8) & 0xF) * 2
            return ((imm8 >> rot) | (imm8 << (32 - rot))) & 0xFFFFFFFF if rot else imm8

        a = pb + 20  # skip the 20-byte PLT header
        while a + 12 <= pb + len(pd):
            w0, w1, w2 = struct.unpack_from("<III", pd, a - pb)
            # add ip, pc, #x ; add ip, ip, #y ; ldr pc, [ip, #z]!
            if (w0 & 0xFFFFF000) == 0xE28FC000 and \
               (w1 & 0xFFFFF000) == 0xE28CC000 and \
               (w2 & 0xFFFFF000) == 0xE5BCF000:
                got = (a + 8 + dpimm(w0) + dpimm(w1) + (w2 & 0xFFF)) & 0xFFFFFFFF
                if got in got2name:
                    self.plt[a] = got2name[got]
            a += 12

    def plt_sites(self, name):
        """VAs of PLT stubs for imported `name`."""
        return [a for a, n in self.plt.items() if n == name]

    def bl_targets_to(self, targets):
        """Every ARM bl/b whose target is in the set `targets` -> [(site, target)]."""
        base, d = self.text()
        out = []
        tset = set(targets)
        for off in range(0, len(d) - 3, 4):
            w = struct.unpack_from("<I", d, off)[0]
            if (w & 0x0E000000) == 0x0A000000 and (w >> 28) != 0xF:
                pc = base + off
                imm = w & 0xFFFFFF
                if imm & 0x800000:
                    imm -= 0x1000000
                t = (pc + 8 + imm * 4) & 0xFFFFFFFF
                if t in tset:
                    out.append((pc, t))
        return out

    # -- text / disasm ------------------------------------------------------
    def text(self):
        t = self.e.get_section_by_name(".text")
        return t["sh_addr"], t.data()

    def func_start(self, va, maxback=0x2000):
        """Walk back to the prologue: push {..,lr} or str fp,[sp,#-4]!."""
        a = va
        while a > va - maxback:
            w = self.u32(a)
            if (w & 0xFFFF0000) == 0xE92D0000 and (w & 0x4000):  # push {..,lr}
                return a
            if w == 0xE52DB004:  # str fp,[sp,#-4]!
                return a
            a -= 4
        return None

    def dis(self, va, n=48, thumb=False, stop=True, show=False):
        """Disassemble up to n instructions from va. Returns list of (insn, note)
        where note annotates PLT calls and pc-relative literals."""
        md = self.thumb if thumb else self.arm
        code = self.raw[self.off(va):self.off(va) + 4 * n]
        res = []
        for i in md.disasm(code, va):
            note = ""
            if i.mnemonic in ("bl", "b", "blx") and len(i.operands) == 1 \
               and i.operands[0].type == ARM_OP_IMM:
                note = self.plt.get(i.operands[0].imm, "")
            elif i.mnemonic.startswith("ldr") and len(i.operands) == 2 \
                    and i.operands[1].type == ARM_OP_MEM \
                    and i.operands[1].mem.base == ARM_REG_PC:
                lit = ((i.address + 4) & ~3) if thumb else i.address + 8
                try:
                    note = "=0x%x" % self.u32(lit + i.operands[1].mem.disp)
                except Exception:
                    pass
            res.append((i, note))
            if show:
                print("  %08x: %-10s %s %s  %s" %
                      (i.address, i.bytes.hex(), i.mnemonic, i.op_str, note))
            if stop and ((i.mnemonic in ("pop", "ldm", "ldmia") and "pc" in i.op_str)
                         or (i.mnemonic == "bx" and i.op_str == "lr")):
                break
        return res

    # -- searching ----------------------------------------------------------
    def find_str(self, text):
        """VAs where the exact NUL-terminated C string `text` is stored."""
        out = []
        pat = b"\0" + text.encode() + b"\0"
        for m in re.finditer(re.escape(pat), self.raw):
            o = m.start() + 1
            for v, oo, fs, _ in self.segs:
                if oo <= o < oo + fs:
                    out.append(v + o - oo)
        return out

    def words_equal(self, val):
        """Aligned data words equal to val (function-pointer / address tables)."""
        out = []
        b = struct.pack("<I", val & 0xFFFFFFFF)
        for m in re.finditer(re.escape(b), self.raw):
            o = m.start()
            if o % 4:
                continue
            for v, oo, fs, _ in self.segs:
                if oo <= o < oo + fs:
                    out.append(v + o - oo)
        return out

    def arm_pcrel_refs(self, target):
        """ARM-mode sites that materialise `target` via ldr rX,[pc,#i] + add rY,pc,rX.
        Used to find the code that references a string/table by address."""
        base, d = self.text()
        res = []
        for off in range(0, len(d) - 4, 4):
            w = struct.unpack_from("<I", d, off)[0]
            if (w & 0x0FFF0FF0) == 0x008F0000 and (w >> 28) == 0xE:  # add Rd, pc, Rm
                rm = w & 0xF
                for k in range(1, 17):
                    if off - 4 * k < 0:
                        break
                    w2 = struct.unpack_from("<I", d, off - 4 * k)[0]
                    if (w2 & 0x0F7F0000) == 0x051F0000 and ((w2 >> 12) & 0xF) == rm \
                       and (w2 >> 28) == 0xE:  # ldr rm,[pc,#imm]
                        imm, up = w2 & 0xFFF, (w2 >> 23) & 1
                        la = (base + off - 4 * k) + 8 + (imm if up else -imm)
                        try:
                            v = self.u32(la)
                        except Exception:
                            break
                        if (base + off + 8 + v) & 0xFFFFFFFF == target:
                            res.append(base + off)
                        break
        return res

    # -- GOT-relative code (Unity 5.6's gcc build) -----------------------------
    # Unity 2017.4's libunity reaches data pc-relative (ldr lit; add rX,pc,rX).
    # Unity 5.6's reaches it through the GOT: rG = pc + lit (= the GOT base,
    # DT_PLTGOT), then address = rG + GOTOFF literal (add rX, rOff, rG).
    def got_base(self):
        if not hasattr(self, "_got"):
            self._got = None
            dyn = self.e.get_section_by_name(".dynamic")
            for t in dyn.iter_tags():
                if t.entry.d_tag == "DT_PLTGOT":
                    self._got = t.entry.d_val
        return self._got

    def gotoff_refs(self, target):
        """ARM sites whose literal pool holds target - GOT (a GOTOFF), as the
        `ldr rX, [pc, #imm]` that loads it. Empty when the module is pc-relative."""
        got = self.got_base()
        if got is None:
            return []
        base, d = self.text()
        want = (target - got) & 0xFFFFFFFF
        pools = set()
        for m in re.finditer(re.escape(struct.pack("<I", want)), d):
            if m.start() % 4 == 0:
                pools.add(base + m.start())
        res = []
        for la in pools:
            for k in range(1, 1024):  # ldr rt,[pc,#+imm] reaches 4 KB back
                pc = la - 4 * k
                if pc < base:
                    break
                w = self.u32(pc)
                if (w & 0x0FFF0000) == 0x059F0000 and pc + 8 + (w & 0xFFF) == la:
                    res.append(pc)
                    break
        return sorted(res)

    def data_refs(self, target):
        """pc-relative or GOT-relative references to target (a string, a global)."""
        return self.arm_pcrel_refs(target) or self.gotoff_refs(target)

    def track_globals(self, fn, n=24):
        """Walk fn's first n instructions tracking registers that hold absolute
        addresses (pc-relative or GOT-relative). Returns a list of events:
        ("call", site, target, {reg: abs}) at each bl, and ("load", site, rt,
        abs) for `ldr rt, [rX, #imm]` from a known address. Both address
        styles come out the same, so a caller need not know which one it has."""
        lit, absr, ev = {}, {}, []
        for k in range(n):
            pc = fn + 4 * k
            w = self.u32(pc)
            if (w >> 28) != 0xE:
                continue
            rd, rn, rm = (w >> 12) & 0xF, (w >> 16) & 0xF, w & 0xF
            if (w & 0x0F7F0000) == 0x051F0000:                 # ldr rd,[pc,#imm]
                imm, up = w & 0xFFF, (w >> 23) & 1
                lit[rd] = self.u32(pc + 8 + (imm if up else -imm))
                absr.pop(rd, None)
            elif (w & 0x0FE00FF0) == 0x00800000:               # add rd, rn, rm
                if rn == 15 and rm in lit:
                    absr[rd] = (pc + 8 + lit[rm]) & 0xFFFFFFFF
                elif rm == 15 and rn in lit:
                    absr[rd] = (pc + 8 + lit[rn]) & 0xFFFFFFFF
                elif rn in absr and rm in lit:
                    absr[rd] = (absr[rn] + lit[rm]) & 0xFFFFFFFF
                elif rm in absr and rn in lit:
                    absr[rd] = (absr[rm] + lit[rn]) & 0xFFFFFFFF
                else:
                    absr.pop(rd, None)
                lit.pop(rd, None)
            elif (w & 0x0FF00000) == 0x02800000 and rn in absr:  # add rd, rn, #imm
                rot = ((w >> 8) & 0xF) * 2
                imm = w & 0xFF
                imm = ((imm >> rot) | (imm << (32 - rot))) & 0xFFFFFFFF if rot else imm
                absr[rd] = (absr[rn] + imm) & 0xFFFFFFFF
                lit.pop(rd, None)
            elif (w & 0x0FF00FF0) == 0x07900000 and rm in lit and rn == 15:  # ldr rd,[pc,rm]
                ev.append(("load", pc, rd, (pc + 8 + lit[rm]) & 0xFFFFFFFF))
                lit.pop(rd, None)
                absr.pop(rd, None)
            elif (w & 0x0FF00000) == 0x05900000 and rn in absr:  # ldr rd,[rn,#imm]
                ev.append(("load", pc, rd, (absr[rn] + (w & 0xFFF)) & 0xFFFFFFFF))
                lit.pop(rd, None)
                absr.pop(rd, None)
            elif (w & 0x0F000000) in (0x0B000000, 0x0A000000):  # bl / b
                imm = w & 0xFFFFFF
                if imm & 0x800000:
                    imm -= 0x1000000
                ev.append(("call", pc, (pc + 8 + imm * 4) & 0xFFFFFFFF, dict(absr)))
        return ev


def hexguard(words):
    return "{ " + ", ".join("0x%08xu" % w for w in words) + " }"
