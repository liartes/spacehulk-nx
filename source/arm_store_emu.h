/* arm_store_emu.h -- decode and perform one ARM-state store instruction.
 * Pure C (no libnx), so tools/test_store_emu.py can build and check it on the
 * host against an independent decoder. */
#ifndef DCR_ARM_STORE_EMU_H
#define DCR_ARM_STORE_EMU_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
  uint32_t r[16];   /* r15 = address of the instruction (the +8 is applied here) */
  uint64_t d[32];   /* VFP/NEON D registers */
  uint32_t cpsr;    /* for the carry flag of RRX */
} ArmRegs;

/* put(ctx, addr, bytes, n): perform the store; return 0 to refuse.
 * Called first with bytes == NULL for every store of the instruction (a probe:
 * may it be written?), and only if all are accepted, again to write them. */
typedef int (*ArmPutFn)(void *ctx, uint32_t addr, const void *src, size_t n);

/* 1 = emulated (registers updated for writeback, r15 untouched),
 * 0 = not a store this emulator handles (nothing written, regs unchanged). */
int arm_emulate_store(uint32_t insn, ArmRegs *regs, ArmPutFn put, void *ctx);

#endif
