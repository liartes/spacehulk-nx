/* dcr_patches.c -- the engine patches, each guard-checked against the game's
 * own code before a single word is written.
 *
 * CHOREOGRAPHER (the "swappy/vsync offsets").
 * Unity 2017.4 predates Swappy; its frame pacing on Android is Choreographer
 * VSYNC: a Java FrameCallback bumps a counter that WaitVSync() sleeps on. Two
 * engine functions set that up, and both first build a Choreographer
 * singleton, which needs a Java Looper thread that does not exist here -- the
 * construction would wait on it forever:
 *
 *   ChoreographerEnableVSync  ("Choreographer available: Enabling VSYNC timing")
 *   ChoreographerStopVSync    its twin: same API gate, same singleton
 *
 * Both become `bx lr`. The counter they would have fed is pumped by
 * dcr_vsync.c at the display rate instead, so the engine keeps its own pacing
 * logic intact and simply gets its pulse from us.
 * MIT.
 */
#include <string.h>

#include "config.h"
#include "dcr_offsets.h"
#include "dcr_patches.h"
#include "util.h"

extern so_module unity_mod;

#define ARM_BX_LR 0xE12FFF1Eu

int dcr_guard_ok(so_module *mod, uint32_t rva, const uint32_t guard[4], const char *what) {
  const uint32_t *p = (const uint32_t *)((uintptr_t)mod->load_virtbase + rva);
  if (!memcmp(p, guard, 16))
    return 1;
  debugPrintf("[patch] %s at +0x%lx: GUARD MISMATCH (found %08lx %08lx %08lx %08lx) -- "
              "not touched; regenerate dcr_offsets.h for this build\n",
              what, (unsigned long)rva, (unsigned long)p[0], (unsigned long)p[1],
              (unsigned long)p[2], (unsigned long)p[3]);
  return 0;
}

int dcr_hook(so_module *mod, uint32_t rva, const uint32_t guard[4], void *dst, const char *what) {
  if (!dcr_guard_ok(mod, rva, guard, what))
    return 0;
  hook_arm((uintptr_t)mod->load_virtbase + rva, (uintptr_t)dst);
  return 1;
}

static __attribute__((unused)) int patch_ret(uint32_t rva, const uint32_t guard[4], const char *what) {
  if (!dcr_guard_ok(&unity_mod, rva, guard, what))
    return 0;
  const uint32_t insn = ARM_BX_LR;
  if (so_patch_code((void *)((uintptr_t)unity_mod.load_virtbase + rva), &insn, 4) != 0) {
    debugPrintf("[patch] %s: write failed\n", what);
    return 0;
  }
  debugPrintf("[patch] %s -> bx lr\n", what);
  return 1;
}

void dcr_patches_apply(void) {
#if DCR_PATCH_CHOREOGRAPHER
  int n = 0;
  n += patch_ret(DCR_RVA_ChoreographerEnableVSync, DCR_GUARD_ChoreographerEnableVSync,
                 "ChoreographerEnableVSync");
  n += patch_ret(DCR_RVA_ChoreographerStopVSync, DCR_GUARD_ChoreographerStopVSync,
                 "ChoreographerStopVSync");
  debugPrintf("[patch] Choreographer: %d/2 patched\n", n);
#endif
}
