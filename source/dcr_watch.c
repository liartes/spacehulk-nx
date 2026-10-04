/* dcr_watch.c -- Disney Crossy Road's part of the hang reports (the
 * runtime's watchdog.c does the rest).
 *
 * The counters the watchdog prints with each report (rising counters mean
 * slow, not stuck): Mono's JIT flushes, the JIT stores and signals the fault
 * handler served (dcr_exc_mono.c), and the GC's thread suspensions
 * (mono_rt.c). Under an emulator, which cannot pause a spinning thread for
 * its registers, each report also samples the main thread and writes its
 * stack and the JIT code to files for tools/stackdump.py. MIT.
 */
#include <stdio.h>
#include <switch.h>

#include "bionic_pthread.h"
#include "dcr_path.h"
#include "exc_handler.h"
#include "so_util.h"
#include "util.h"
#include "watchdog.h"

uint32_t dcr_jit_flushes(void);    /* jit_arena.c */
uint32_t dcr_exc_jit_stores(void); /* dcr_exc_mono.c */
uint32_t dcr_signal_count(void);   /* dcr_exc_mono.c */
uint32_t dcr_gc_suspends(void);    /* mono_rt.c */
uint32_t jit_arena_base(void);     /* jit_arena.c */
uint32_t jit_arena_size(void);
extern so_module main_mod, unity_mod, mono_mod;

void dcr_watch_counters(void) {
  rt_watchdog_add_counter("JIT flushes", dcr_jit_flushes);
  rt_watchdog_add_counter("emulated JIT stores", dcr_exc_jit_stores);
  rt_watchdog_add_counter("signals", dcr_signal_count);
  rt_watchdog_add_counter("GC suspends", dcr_gc_suspends);
}

static void dump_jit_arena(uint32_t size) {
  char path[300];
  snprintf(path, sizeof path, "%s/jit_arena.bin", dcr_game_root());
  FILE *f = fopen(path, "wb");
  if (!f)
    return;
  uint32_t base = jit_arena_base();
  fwrite(&base, 4, 1, f);
  fwrite((const void *)(uintptr_t)base, 1, dcr_readable(base, size), f);
  fclose(f);
}

/* Emulator: the main thread's stack as a file (stack_main.bin: a header of
 * "DCRS", the stack's top address, the module bases, then the words from the
 * lowest mapped page to the top), and the JIT code in use (up to 4 MB), for
 * the managed frames. */
static Handle g_main_thread;

static void dump_main_stack(BThread *t, void *arg) {
  (void)arg;
  if (t->handle != g_main_thread || !t->stack_base)
    return;
  uintptr_t lo = (uintptr_t)t->stack_base, hi = lo + t->stack_size, start = hi;
  while (start > lo && dcr_readable(start - 0x1000, 0x1000) == 0x1000 && hi - start < 0x80000)
    start -= 0x1000;
  char path[300];
  snprintf(path, sizeof path, "%s/stack_main.bin", dcr_game_root());
  FILE *f = fopen(path, "wb");
  if (!f)
    return;
  extern char _start[];
  const uint32_t hdr[8] = {0x53524344u, (uint32_t)hi, (uint32_t)start, (uint32_t)(uintptr_t)_start,
                           (uint32_t)(uintptr_t)main_mod.load_virtbase, (uint32_t)(uintptr_t)unity_mod.load_virtbase,
                           (uint32_t)(uintptr_t)mono_mod.load_virtbase, g_sample_sp};
  fwrite(hdr, 4, 8, f);
  fwrite((const void *)start, 1, hi - start, f);
  fclose(f);
  uint32_t size = jit_arena_size();
  dump_jit_arena(size > (4u << 20) ? 4u << 20 : size);
}

void port_watchdog_emulator_report(unsigned secs) {
  if (secs >= 20)
    return;
  if (!g_main_thread)
    g_main_thread = envGetMainThreadHandle();
  /* ask the main thread where it is, next time it calls a sampled shim */
  uint32_t n0 = g_sample_n;
  g_sample_want = g_main_thread;
  for (int i = 0; i < 20 && g_sample_n == n0; i++)
    svcSleepThread(50000000ll);
  g_sample_want = 0;
  debugPrintf("[watchdog] main thread sample: %s sp %08lx, shim called from %08lx\n",
              g_sample_n != n0 ? "taken:" : "NOT taken (it calls no sampled shim):",
              (unsigned long)g_sample_sp, (unsigned long)g_sample_lr);
  b_thread_foreach(dump_main_stack, NULL);
}

/* Emulator: a jump into JIT memory Mono never wrote (jit_arena.c fills it
 * with calls to a stub that lands here). regs: r0-r12 and lr as pushed; the
 * stray address is lr - 4, and the stack pointer at the jump is just above
 * them -- so this stack dump holds the live frames only. No locks. */
void dcr_emu_stray(uint32_t *regs) {
  char buf[400];
  const uint32_t sp = (uint32_t)(uintptr_t)(regs + 14);
  int n = snprintf(buf, sizeof buf, "[jit] STRAY JUMP into unwritten JIT memory at %08lx (thread 0x%x): "
                   "r0 %08lx r1 %08lx r2 %08lx r3 %08lx r4 %08lx r11 %08lx r12 %08lx sp %08lx",
                   (unsigned long)(regs[13] - 4), (unsigned)threadGetCurHandle(), (unsigned long)regs[0],
                   (unsigned long)regs[1], (unsigned long)regs[2], (unsigned long)regs[3], (unsigned long)regs[4],
                   (unsigned long)regs[11], (unsigned long)regs[12], (unsigned long)sp);
  svcOutputDebugString(buf, n > 0 ? (size_t)n : 0);
  BThread *t = b_thread_self();
  if (t && t->stack_base) {
    char path[300];
    snprintf(path, sizeof path, "%s/stack_stray.bin", dcr_game_root());
    FILE *f = fopen(path, "wb");
    if (f) {
      extern char _start[];
      uint32_t hi = (uint32_t)(uintptr_t)t->stack_base + t->stack_size;
      const uint32_t hdr[8] = {0x53524344u, hi, sp, (uint32_t)(uintptr_t)_start,
                               (uint32_t)(uintptr_t)main_mod.load_virtbase, (uint32_t)(uintptr_t)unity_mod.load_virtbase,
                               (uint32_t)(uintptr_t)mono_mod.load_virtbase, regs[13] - 4};
      fwrite(hdr, 4, 8, f);
      fwrite((const void *)(uintptr_t)sp, 1, hi - sp, f);
      fclose(f);
      dump_jit_arena(4u << 20);
    }
  }
  svcOutputDebugString("[jit] stray-jump report written; this thread stops here", 55);
  for (;;)
    svcSleepThread(1000000000ll);
}
