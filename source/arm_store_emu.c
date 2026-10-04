/* arm_store_emu.c -- ARM (A32) store emulation for the JIT data-abort path.
 *
 * Covers every store form the C compiler emits in libmono's code emitters and
 * patchers: STR/STRB (immediate and scaled-register offsets, pre/post-index,
 * writeback), STRH/STRD (immediate and register), STM (IA/IB/DA/DB, with
 * writeback), STREX/STREXB/STREXH/STREXD (performed, status = success), and
 * VSTR/VSTM (single and double precision). Anything else returns 0 and the
 * fault is reported as a crash, never guessed at.
 *
 * All-or-nothing: the stores are collected first and only performed when the
 * whole instruction decoded, so a refusal leaves memory and registers as the
 * faulting instruction found them. Checked on the host against Capstone by
 * tools/test_store_emu.py. MIT.
 */
#include "arm_store_emu.h"

#include <string.h>

typedef struct {
  uint32_t addr;
  uint8_t n;
  uint8_t bytes[8];
} Pending;

typedef struct {
  Pending p[32];
  int n;
} Batch;

static void add(Batch *b, uint32_t addr, const void *src, size_t n) {
  if (b->n < 32) {
    b->p[b->n].addr = addr;
    b->p[b->n].n = (uint8_t)n;
    memcpy(b->p[b->n].bytes, src, n);
    b->n++;
  }
}
static void add32(Batch *b, uint32_t addr, uint32_t v) { add(b, addr, &v, 4); }

static uint32_t R(const ArmRegs *s, unsigned n) { return n == 15 ? s->r[15] + 8 : s->r[n]; }
static uint32_t S(const ArmRegs *s, unsigned n) {
  uint64_t d = s->d[n >> 1];
  return (n & 1) ? (uint32_t)(d >> 32) : (uint32_t)d;
}

static uint32_t shifted(uint32_t v, unsigned type, unsigned imm, unsigned carry) {
  switch (type) {
  case 0: return v << imm;
  case 1: return imm ? v >> imm : 0;                       /* LSR #32 */
  case 2: return imm ? (uint32_t)((int32_t)v >> imm) : ((int32_t)v < 0 ? 0xffffffffu : 0);
  default: return imm ? (v >> imm) | (v << (32 - imm)) : ((carry << 31) | (v >> 1)); /* RRX */
  }
}

int arm_emulate_store(uint32_t insn, ArmRegs *s, ArmPutFn put, void *ctx) {
  const unsigned rn = (insn >> 16) & 15, rt = (insn >> 12) & 15;
  const unsigned P = (insn >> 24) & 1, U = (insn >> 23) & 1, W = (insn >> 21) & 1;
  const unsigned carry = (s->cpsr >> 29) & 1;
  Batch b = {.n = 0};
  int wb = 0;
  uint32_t wb_val = 0;

  if ((insn >> 28) == 0xF)
    return 0; /* unconditional space: NEON element stores etc. */

  if ((insn & 0x0C100000) == 0x04000000) {
    /* STR / STRB */
    const unsigned B = (insn >> 22) & 1;
    uint32_t off;
    if (insn & (1u << 25)) {
      if (insn & 0x10)
        return 0; /* media instruction space */
      off = shifted(R(s, insn & 15), (insn >> 5) & 3, (insn >> 7) & 31, carry);
    } else {
      off = insn & 0xFFF;
    }
    uint32_t base = R(s, rn), moved = U ? base + off : base - off;
    uint32_t addr = P ? moved : base, v = R(s, rt);
    if (B) {
      uint8_t v8 = (uint8_t)v;
      add(&b, addr, &v8, 1);
    } else {
      add32(&b, addr, v);
    }
    if (!P || W) {
      wb = 1;
      wb_val = moved;
    }
  } else if ((insn & 0x0F900FF0) == 0x01800F90) {
    /* STREX / STREXD / STREXB / STREXH */
    unsigned rs = insn & 15, op = (insn >> 21) & 3;
    if (rt == 15 || rs == 15 || rn == 15 || rt == rn || rt == rs ||
        (op == 1 && ((rs & 1) || rs == 14 || rt == rs + 1)))
      return 0; /* UNPREDICTABLE forms */
    uint32_t addr = R(s, rn), v = R(s, rs);
    if (op == 0) add32(&b, addr, v);
    else if (op == 1) { add32(&b, addr, v); add32(&b, addr + 4, R(s, rs + 1)); }
    else if (op == 2) { uint8_t v8 = (uint8_t)v; add(&b, addr, &v8, 1); }
    else { uint16_t v16 = (uint16_t)v; add(&b, addr, &v16, 2); }
    for (int k = 0; k < b.n; k++)
      if (!put(ctx, b.p[k].addr, NULL, b.p[k].n))
        return 0;
    for (int k = 0; k < b.n; k++)
      put(ctx, b.p[k].addr, b.p[k].bytes, b.p[k].n);
    s->r[rt] = 0; /* Rd: the exclusive store succeeded */
    return 1;
  } else if ((insn & 0x0E000090) == 0x00000090 && (insn & 0x60) && !(insn & (1u << 20))) {
    /* STRH / STRD (op2 = 1 / 3; op2 = 2 is LDRD) */
    unsigned op2 = (insn >> 5) & 3;
    if (op2 == 2 || (!P && W) || rt == 15)
      return 0; /* LDRD, the unprivileged STRHT, or an UNPREDICTABLE Rt */
    uint32_t off = (insn & (1u << 22)) ? (((insn >> 4) & 0xF0) | (insn & 0xF)) : R(s, insn & 15);
    uint32_t base = R(s, rn), moved = U ? base + off : base - off;
    uint32_t addr = P ? moved : base;
    if (op2 == 1) {
      uint16_t v16 = (uint16_t)R(s, rt);
      add(&b, addr, &v16, 2);
    } else {
      if (rt & 1)
        return 0; /* STRD needs an even Rt */
      add32(&b, addr, R(s, rt));
      add32(&b, addr + 4, R(s, rt + 1));
    }
    if (!P || W) {
      wb = 1;
      wb_val = moved;
    }
  } else if ((insn & 0x0E100000) == 0x08000000) {
    /* STM */
    unsigned list = insn & 0xFFFF, n = (unsigned)__builtin_popcount(list);
    if (!n || (insn & (1u << 22)))
      return 0; /* empty list, or the user-bank form */
    uint32_t base = R(s, rn);
    uint32_t a = U ? (P ? base + 4 : base) : (P ? base - 4 * n : base - 4 * n + 4);
    for (unsigned r = 0; r < 16; r++)
      if (list & (1u << r)) {
        add32(&b, a, R(s, r));
        a += 4;
      }
    if (W) {
      wb = 1;
      wb_val = U ? base + 4 * n : base - 4 * n;
    }
  } else if ((insn & 0x0E100E00) == 0x0C000A00 && (insn & 0x01A00000)) {
    /* VSTR / VSTM (cp10 single, cp11 double) */
    const int dbl = (insn >> 8) & 1;
    const unsigned D = (insn >> 22) & 1, vd = (insn >> 12) & 15, imm8 = insn & 0xFF;
    uint32_t base = R(s, rn);
    if ((insn & 0x0F200000) == 0x0D000000) {
      /* VSTR */
      uint32_t addr = U ? base + imm8 * 4 : base - imm8 * 4;
      if (dbl) {
        uint64_t v = s->d[(D << 4) | vd];
        add(&b, addr, &v, 8);
      } else {
        add32(&b, addr, S(s, (vd << 1) | D));
      }
    } else {
      /* VSTM: IA (P=0 U=1) or DB (P=1 U=0 W=1) */
      if (P == U || (P && !W))
        return 0;
      uint32_t addr = P ? base - imm8 * 4 : base;
      if (dbl) {
        if (imm8 & 1)
          return 0; /* FSTMX: not emitted by modern compilers */
        unsigned first = (D << 4) | vd;
        if (!imm8 || first + imm8 / 2 > 32 || imm8 / 2 > 16)
          return 0;
        for (unsigned k = 0; k < imm8 / 2; k++) {
          uint64_t v = s->d[first + k];
          add(&b, addr + 8 * k, &v, 8);
        }
      } else {
        unsigned first = (vd << 1) | D;
        if (!imm8 || first + imm8 > 32)
          return 0;
        for (unsigned k = 0; k < imm8; k++)
          add32(&b, addr + 4 * k, S(s, first + k));
      }
      if (W) {
        wb = 1;
        wb_val = U ? base + imm8 * 4 : base - imm8 * 4;
      }
    }
  } else {
    return 0;
  }

  if (b.n == 0)
    return 0;
  for (int k = 0; k < b.n; k++)
    if (!put(ctx, b.p[k].addr, NULL, b.p[k].n))
      return 0;
  for (int k = 0; k < b.n; k++)
    put(ctx, b.p[k].addr, b.p[k].bytes, b.p[k].n);
  if (wb && rn != 15)
    s->r[rn] = wb_val;
  return 1;
}
