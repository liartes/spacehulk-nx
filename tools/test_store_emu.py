#!/usr/bin/env python3
"""test_store_emu.py -- differential test of source/arm_store_emu.c.

The JIT store emulator only ever runs on real hardware (Ryujinx does not
enforce page permissions, so nothing faults there). So it is checked here, on
the host: arm_store_emu.c is built as a shared library, fed a large random
sample of A32 words from the store encoding spaces (and a sample of words that
are not stores), and its behaviour -- which bytes it writes where, which base
register it updates -- is compared with a model driven purely by Capstone's
independent decoding of the same word.

UNPREDICTABLE encodings (writeback with Rn == Rt, Rn in an STM list with
writeback, PC as a writeback base, odd STRD pairs, ...) are skipped: the
emulator may do anything reasonable there and so may a CPU.

    python3 tools/test_store_emu.py [--n 200000] [--seed 1]
"""
import argparse
import ctypes
import os
import random
import struct
import subprocess
import sys
import tempfile

from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM
from capstone.arm import ARM_OP_IMM, ARM_OP_MEM, ARM_OP_REG

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(os.path.dirname(HERE), "source", "arm_store_emu.c")


class ArmRegs(ctypes.Structure):
    _fields_ = [("r", ctypes.c_uint32 * 16), ("d", ctypes.c_uint64 * 32), ("cpsr", ctypes.c_uint32)]


PUTFN = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_size_t)


def build():
    out = os.path.join(tempfile.mkdtemp(), "libstoreemu.so")
    subprocess.check_call(["cc", "-O1", "-shared", "-fPIC", "-o", out, SRC])
    lib = ctypes.CDLL(out)
    lib.arm_emulate_store.argtypes = [ctypes.c_uint32, ctypes.POINTER(ArmRegs), PUTFN, ctypes.c_void_p]
    lib.arm_emulate_store.restype = ctypes.c_int
    return lib


M32 = 0xFFFFFFFF
# capstone ARM shift types
SFT = {1: "asr", 2: "lsl", 3: "lsr", 4: "ror", 5: "rrx"}


def shift(v, kind, amt, carry):
    if kind == 0:
        return v
    k = SFT.get(kind)
    if k == "lsl":
        return (v << amt) & M32
    if k == "lsr":
        return (v >> amt) & M32 if amt < 32 else 0
    if k == "asr":
        if amt >= 32:
            return M32 if v & 0x80000000 else 0
        sv = v - (1 << 32) if v & 0x80000000 else v
        return (sv >> amt) & M32
    if k == "ror":
        amt %= 32
        return ((v >> amt) | (v << (32 - amt))) & M32 if amt else v
    if k == "rrx":
        return ((carry << 31) | (v >> 1)) & M32
    raise ValueError(kind)


class Model:
    """Expected stores + writeback from Capstone's decode, or None to skip."""

    def __init__(self, md):
        self.md = md

    def regnum(self, i, reg):
        name = i.reg_name(reg)
        alias = {"sb": 9, "sl": 10, "fp": 11, "ip": 12, "sp": 13, "lr": 14, "pc": 15}
        if name in alias:
            return ("r", alias[name])
        if name[0] == "r":
            return ("r", int(name[1:]))
        if name[0] == "d":
            return ("d", int(name[1:]))
        if name[0] == "s":
            return ("s", int(name[1:]))
        return (name, -1)

    def rv(self, regs, n):
        return (regs["pc"] + 8) & M32 if n == 15 else regs["r"][n]

    def val(self, regs, kind, n):
        if kind == "r":
            return self.rv(regs, n), 4
        if kind == "d":
            return regs["d"][n], 8
        if kind == "s":
            d = regs["d"][n >> 1]
            return ((d >> 32) if n & 1 else d) & M32, 4
        raise ValueError(kind)

    def expect(self, word, regs):
        ins = list(self.md.disasm(struct.pack("<I", word), regs["pc"]))
        if not ins:
            return "notstore"
        i = ins[0]
        if i.mnemonic.endswith(".16"):
            return "skip"  # ARMv8.2 half-precision; not on a Cortex-A57
        mn = i.mnemonic.split(".")[0]
        if (word >> 28) != 0xE:
            return "skip"
        stores = []
        carry = (regs["cpsr"] >> 29) & 1
        ops = i.operands

        def mem_addr(op, post_op):
            base_n = self.regnum(i, op.mem.base)[1]
            base = self.rv(regs, base_n)
            if post_op is not None:
                if post_op.type == ARM_OP_IMM:
                    off = post_op.imm & M32
                else:
                    off = shift(self.rv(regs, self.regnum(i, post_op.reg)[1]), post_op.shift.type,
                                post_op.shift.value, carry)
                moved = (base - off if post_op.subtracted else base + off) & M32
                return base_n, base, moved, True
            if op.mem.index:
                off = shift(self.rv(regs, self.regnum(i, op.mem.index)[1]), op.shift.type, op.shift.value, carry)
                moved = (base - off if op.subtracted else base + off) & M32
            else:
                d = op.mem.disp  # Capstone reports a negative offset already negated
                moved = (base + d if d < 0 else (base - d if op.subtracted else base + d)) & M32
            return base_n, moved, moved, i.writeback

        if mn in ("strt", "strbt"):
            mn = mn[:-1]  # user-mode STRT == STR (post-indexed, writeback) at EL0
        if mn in ("str", "strb", "strh", "strd"):
            regs_ops = [o for o in ops if o.type == ARM_OP_REG]
            mems = [k for k, o in enumerate(ops) if o.type == ARM_OP_MEM]
            if not mems:
                return "skip"
            mi = mems[0]
            post = ops[mi + 1] if mi + 1 < len(ops) else None
            rn, addr, moved, wb = mem_addr(ops[mi], post)
            vals = [self.regnum(i, o.reg) for o in regs_ops]
            if any(k != "r" for k, _ in vals):
                return "skip"
            if wb and (rn == 15 or rn in [n for _, n in vals]):
                return "skip"  # UNPREDICTABLE
            if mn == "strd" and (vals[0][1] & 1 or vals[0][1] == 14):
                return "skip"
            if mn in ("strh", "strd") and vals[0][1] == 15:
                return "skip"
            if mn == "str":
                stores.append((addr, struct.pack("<I", self.rv(regs, vals[0][1]))))
            elif mn == "strb":
                stores.append((addr, struct.pack("<B", self.rv(regs, vals[0][1]) & 0xFF)))
            elif mn == "strh":
                stores.append((addr, struct.pack("<H", self.rv(regs, vals[0][1]) & 0xFFFF)))
            else:
                stores.append((addr, struct.pack("<I", self.rv(regs, vals[0][1]))))
                stores.append(((addr + 4) & M32, struct.pack("<I", self.rv(regs, vals[1][1]))))
            return stores, (rn, moved) if wb else None
        if mn in ("strex", "strexb", "strexh", "strexd"):
            regs_ops = [self.regnum(i, o.reg)[1] for o in ops if o.type == ARM_OP_REG]
            mem = [o for o in ops if o.type == ARM_OP_MEM][0]
            rd, rn = regs_ops[0], self.regnum(i, mem.mem.base)[1]
            vals = regs_ops[1:]
            if rd in vals or rd == rn or rd == 15 or rn == 15 or 15 in vals:
                return "skip"
            if mn == "strexd" and ((word & 15) & 1 or (word & 15) == 14 or rd == (word & 15) + 1):
                return "skip"  # odd Rt is UNPREDICTABLE (Capstone rounds it down)
            a = self.rv(regs, rn)
            v = self.rv(regs, vals[0])
            if mn == "strex":
                stores.append((a, struct.pack("<I", v)))
            elif mn == "strexb":
                stores.append((a, struct.pack("<B", v & 0xFF)))
            elif mn == "strexh":
                stores.append((a, struct.pack("<H", v & 0xFFFF)))
            else:
                stores.append((a, struct.pack("<I", v)))
                stores.append(((a + 4) & M32, struct.pack("<I", self.rv(regs, vals[1]))))
            return stores, ("status", rd)
        if mn in ("stm", "stmia", "stmib", "stmda", "stmdb", "push"):
            if "^" in i.op_str:
                return "skip"
            if mn == "push":
                rn, lst, wb, mode = 13, [self.regnum(i, o.reg)[1] for o in ops], True, "db"
            else:
                rn = self.regnum(i, ops[0].reg)[1]
                lst = [self.regnum(i, o.reg)[1] for o in ops[1:]]
                wb = i.writeback
                mode = {"stm": "ia", "stmia": "ia"}.get(mn, mn[3:])
            if wb and (rn == 15 or rn in lst):
                return "skip"
            if 15 in lst or rn == 15:
                return "skip"
            n = len(lst)
            base = self.rv(regs, rn)
            start = {"ia": base, "ib": base + 4, "da": base - 4 * n + 4, "db": base - 4 * n}[mode] & M32
            for k, r in enumerate(sorted(lst)):
                stores.append(((start + 4 * k) & M32, struct.pack("<I", self.rv(regs, r))))
            end = (base + 4 * n if mode in ("ia", "ib") else base - 4 * n) & M32
            return stores, (rn, end) if wb else None
        if mn == "vstr":
            k, n = self.regnum(i, ops[0].reg)
            mem = ops[1]
            rn = self.regnum(i, mem.mem.base)[1]
            base = self.rv(regs, rn)
            if rn == 15:
                base &= ~3  # VSTR's PC base is Align(PC, 4)
            d = mem.mem.disp
            a = (base + d if d < 0 else (base - d if mem.subtracted else base + d)) & M32
            v, size = self.val(regs, k, n)
            stores.append((a, struct.pack("<Q" if size == 8 else "<I", v)))
            return stores, None
        if mn in ("vstmia", "vstmdb", "vpush", "vstm"):
            if mn == "vpush":
                rn, lst, wb, mode = 13, [self.regnum(i, o.reg) for o in ops], True, "db"
            else:
                rn = self.regnum(i, ops[0].reg)[1]
                lst = [self.regnum(i, o.reg) for o in ops[1:]]
                wb = i.writeback
                mode = "db" if mn == "vstmdb" else "ia"
            if rn == 15 or not lst:
                return "skip"
            size = 8 if lst[0][0] == "d" else 4
            imm8 = word & 0xFF
            if imm8 != (len(lst) * size) // 4 or (size == 8 and len(lst) > 16):
                return "skip"  # an invalid count Capstone clamped
            total = size * len(lst)
            base = self.rv(regs, rn)
            a = (base - total if mode == "db" else base) & M32
            for k, (kind, n) in enumerate(lst):
                v, sz = self.val(regs, kind, n)
                stores.append(((a + sz * k) & M32, struct.pack("<Q" if sz == 8 else "<I", v)))
            end = (base - total if mode == "db" else base + total) & M32
            return stores, (rn, end) if wb else None
        return "notstore"


def rand_word(rng):
    """Random words biased toward the store encoding spaces, cond = AL."""
    space = rng.randrange(7)
    w = rng.getrandbits(32)
    w = (w & 0x0FFFFFFF) | 0xE0000000
    if space == 0:        # single data transfer, store
        w = (w & ~0x0C100000) | 0x04000000
    elif space == 1:      # extra load/store halfword/dual, store
        w = (w & ~0x0E100090) | 0x00000090
        w |= rng.choice([0x20, 0x60])
    elif space == 2:      # block transfer, store
        w = (w & ~0x0E500000) | 0x08000000
    elif space == 3:      # exclusive stores
        w = (w & ~0x0F900FF0) | 0x01800F90
    elif space == 4:      # VSTR / VSTM
        w = (w & ~0x0E100E00) | 0x0C000A00 | (rng.getrandbits(1) << 8)
    # 5, 6: fully random (mostly not stores)
    return w


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, default=200000)
    ap.add_argument("--seed", type=int, default=1)
    a = ap.parse_args()
    rng = random.Random(a.seed)
    lib = build()
    md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
    md.detail = True
    model = Model(md)

    counts = {"match": 0, "skip": 0, "notstore_ok": 0}
    fails = []
    kinds = {}
    for _ in range(a.n):
        w = rand_word(rng)
        regs = {"r": [rng.getrandbits(32) for _ in range(16)], "pc": 0x40001000,
                "d": [rng.getrandbits(64) for _ in range(32)], "cpsr": rng.getrandbits(32) & 0xF0000000}
        exp = model.expect(w, regs)
        if exp == "skip":
            counts["skip"] += 1
            continue

        s = ArmRegs()
        for k in range(15):
            s.r[k] = regs["r"][k]
        s.r[15] = regs["pc"]
        for k in range(32):
            s.d[k] = regs["d"][k]
        s.cpsr = regs["cpsr"]
        got = []

        def put(ctx, addr, src, n):
            if src:
                got.append((addr, ctypes.string_at(src, n)))
            return 1

        cb = PUTFN(put)
        before = list(s.r)
        ok = lib.arm_emulate_store(w, ctypes.byref(s), cb, None)

        if exp == "notstore":
            if ok:
                fails.append((w, "emulated something Capstone calls %s" %
                              (next(iter(md.disasm(struct.pack("<I", w), 0)), None) and
                               next(md.disasm(struct.pack("<I", w), 0)).mnemonic)))
            else:
                counts["notstore_ok"] += 1
            continue

        stores, wb = exp
        mn = next(md.disasm(struct.pack("<I", w), 0)).mnemonic
        kinds[mn] = kinds.get(mn, 0) + 1
        if not ok:
            fails.append((w, "refused a %s" % mn))
            continue
        want_regs = before[:15]
        if wb and wb[0] == "status":
            want_regs[wb[1]] = 0
        elif wb:
            want_regs[wb[0]] = wb[1]
        if [(x, bytes(y)) for x, y in got] != stores:
            fails.append((w, "%s stores %s, expected %s" % (mn, got, stores)))
        elif list(s.r)[:15] != want_regs:
            diff = [(k, hex(list(s.r)[k]), hex(want_regs[k])) for k in range(15) if list(s.r)[k] != want_regs[k]]
            fails.append((w, "%s registers differ %s" % (mn, diff)))
        else:
            counts["match"] += 1

    print("stores matched: %d   non-stores refused: %d   UNPREDICTABLE skipped: %d   FAILED: %d"
          % (counts["match"], counts["notstore_ok"], counts["skip"], len(fails)))
    print("coverage:", ", ".join("%s %d" % kv for kv in sorted(kinds.items())))
    for w, why in fails[:25]:
        ins = next(md.disasm(struct.pack("<I", w), 0x40001000), None)
        print("  %08x  %-32s %s" % (w, (ins.mnemonic + " " + ins.op_str) if ins else "?", why))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
