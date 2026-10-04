/* jit_arena.c -- executable memory for Mono's JIT.
 *
 * WHY AN ARENA WITH TWO VIEWS
 * ---------------------------
 * Mono's code manager mmap()s PROT_READ|PROT_WRITE|PROT_EXEC chunks and then
 * writes code into them in place: emitted methods, trampolines, patched call
 * sites. Horizon never maps a page writable and executable at once, and
 * Mesosphere lets a code page go RW->RX exactly once (KPageTableBase::
 * SetProcessMemoryPermission only accepts a write permission from the Code /
 * AliasCode states), so there is no per-allocation permission toggling either.
 *
 * So: one CodeMemory object (libnx jitCreate; Atmosphere permits mapping one's
 * own code memory) gives the SAME physical pages at two addresses:
 *     rx  -- what Mono is given, executes and reads;
 *     rw  -- where writes actually land.
 * Writes aimed at rx are redirected to rw by:
 *   - the memcpy / memmove / memset family (bionic_string.c), which covers the
 *     bulk copy of every compiled method into its final place;
 *   - the data-abort handler (exc_handler.c), which emulates the remaining
 *     individual stores (call-site patching, trampolines) against rw.
 * And Mono's instruction-cache flush -- an inline Linux `cacheflush` svc at
 * libmono+0x10eed4 that Horizon would reject -- is hooked to jit_flush(),
 * which cleans rw from the D-cache and invalidates rx in the I-cache.
 *
 * Under an emulator (dcr_is_emulator()) guest page permissions are not
 * enforced, so the arena is one view (rx == rw). But Ryujinx 1.1.1098 keeps
 * running its OLD translation of code that is rewritten in place (Mono
 * patches call sites, trampolines...), and gives an AArch32 guest no cache
 * maintenance that reaches its translator: FlushProcessDataCache is a stub,
 * MCR c7 is not decoded. What it does honour is an UNMAP: every unmap of
 * guest memory invalidates the translations in that range. So there the
 * arena is an svcMapMemory alias of a heap block, and a flush unmaps and maps
 * back the pages it covers (same backing, same contents). Emulator only.
 * MIT.
 */
#include <malloc.h>
#include <string.h>
#include <switch.h>

#include "bionic.h"
#include "bionic_pthread.h"
#include "jit_arena.h"
#include "selfproc.h"
#include "util.h"

int b_sigaction(int sig, const struct b_sigaction *act, struct b_sigaction *old); /* bionic_signal.c */

/* Self-test 4's SIGSEGV handler: does what Mono's does -- edits the Linux ARM
 * ucontext (uc_mcontext at +20: r0 at +32, pc at +92) and returns. */
static volatile int g_sig_test_seen;
static void sig_test_handler(int sig, void *info, void *uc) {
  uint32_t *u = uc;
  const uint32_t *si = info;
  g_sig_test_seen = sig == 11 && si[0] == 11 && si[3] == 0; /* si_signo, si_addr */
  u[92 / 4] += 4;     /* skip the faulting load */
  u[32 / 4] = 1234;   /* r0 */
}

static void jit_self_test(void);

#define JIT_ARENA_BYTES (48u * 1024 * 1024)
#define PAGE 0x1000u

static Jit g_jit;
static uintptr_t g_rx, g_rw;
static uintptr_t g_emu_backing; /* emulator: the heap block the arena aliases */
static uint32_t *g_emu_flushed;  /* emulator: words flushed before (dcr_code_flush) */
static void emu_book_init(void);
static void emu_reuse_pages(size_t first, size_t n);
void dcr_jit_emu_frame_end(void);
static Mutex g_emu_lock;
static size_t g_size;
static int g_ready;
static Mutex g_lock;

/* Emulator: arena pages nobody has been given hold calls to the stray-jump
 * stub (page 0), so execution that runs off the end of Mono's code -- zero
 * words decode as `andeq r0, r0, r0` and slide on silently -- stops at the
 * first free page and reports its stack (watchdog.c dcr_emu_stray). */
static void emu_sentinel_fill(size_t first_page, size_t pages) {
  uint32_t *w = (uint32_t *)(g_rw + first_page * PAGE);
  for (size_t k = 0; k < pages * PAGE / 4; k++) {
    uint32_t at = (uint32_t)(g_rx + first_page * PAGE + k * 4);
    w[k] = 0xEB000000u | (((g_rx - (at + 8)) >> 2) & 0x00FFFFFFu); /* bl stub */
  }
}

/* Page-granular first-fit allocator over the arena. Mono rarely frees code
 * (only dynamic methods and domain unload), so a simple map is enough. */
static uint8_t *g_used;   /* 1 byte per page: 0 free, 1 used, 2 used+block-start */
static size_t g_pages;

uint32_t dcr_exc_jit_stores(void); /* exc_handler.c */

/* Hardware only: prove the two things Mono depends on before Mono runs, so a
 * failure is one clear log line instead of a crash deep inside the JIT.
 *   1. a plain store to the rx view traps and is emulated onto rw
 *      (exc32.S / exc_handler.c; needs Atmosphere's user exception handlers);
 *   2. code written through rw executes from rx after jit_flush;
 *   3. code REwritten after it ran executes in its new form (the I-cache);
 *   4. a fault in JIT code is delivered to a SIGSEGV handler (exc_handler.c). */
static void jit_self_test(void) {
  volatile uint32_t *rx = (volatile uint32_t *)(g_rx + g_size - PAGE);
  volatile uint32_t *rw = (volatile uint32_t *)(g_rw + g_size - PAGE);
  debugPrintf("[jit] self-test 1: store to the execute-only view (expect one emulated fault)\n");
  log_flush_ring();
  uint32_t before = dcr_exc_jit_stores();
  rx[0] = 0xE3A0002Au; /* mov r0, #42 -- via the data-abort handler */
  rw[1] = 0xE12FFF1Eu; /* bx lr       -- directly */
  int t1 = rw[0] == 0xE3A0002Au && dcr_exc_jit_stores() == before + 1;
  debugPrintf("[jit] self-test 1: %s\n", t1 ? "OK (store emulated)" : "FAILED");
  jit_flush((void *)rx, 8);
  int (*fn)(void) = (int (*)(void))(uintptr_t)rx;
  int r = fn();
  debugPrintf("[jit] self-test 2: generated code returned %d: %s\n", r, r == 42 ? "OK" : "FAILED");
  /* 3. Rewrite code that has just run (so the instruction cache holds it) and
   *    run it again: without a working I-cache invalidation this returns 42. */
  rw[0] = 0xE3A00007u; /* mov r0, #7 */
  jit_flush((void *)rx, 8);
  r = fn();
  debugPrintf("[jit] self-test 3: rewritten code returned %d: %s\n", r,
              r == 7 ? "OK (instruction cache invalidated)" : "FAILED (stale instruction cache)");
  /* 4. A fault in JIT code reaches a SIGSEGV handler, which can rewrite the
   *    context (Mono turns managed null dereferences into exceptions so). */
  struct b_sigaction act = {(void *)sig_test_handler, 0, L_SA_SIGINFO, NULL}, old;
  b_sigaction(L_SIGSEGV, &act, &old);
  rw[0] = 0xE5900000u; /* ldr r0, [r0] */
  rw[1] = 0xE12FFF1Eu; /* bx lr */
  jit_flush((void *)rx, 8);
  int (*load)(int) = (int (*)(int))(uintptr_t)rx;
  r = load(0);
  b_sigaction(L_SIGSEGV, &old, NULL);
  debugPrintf("[jit] self-test 4: null load in JIT code -> handler -> resumed with %d: %s\n", r,
              r == 1234 && g_sig_test_seen ? "OK (signal delivered)" : "FAILED");
  rw[0] = rw[1] = 0;
  jit_flush((void *)rx, 8);
}

int jit_arena_init(void) {
  if (g_ready)
    return 0;
  g_size = JIT_ARENA_BYTES;
  g_pages = g_size / PAGE;
  g_used = calloc(g_pages, 1);
  if (!g_used)
    return -1;
  if (dcr_is_emulator()) {
    void *p = memalign(PAGE, g_size);
    if (!p)
      return -1;
    memset(p, 0, g_size);
    g_rx = g_rw = (uintptr_t)p;
    /* the alias that flushes can unmap and map back (see the top) */
    virtmemLock();
    void *a = virtmemFindStack(g_size, PAGE);
    Result rc = a ? svcMapMemory(a, p, g_size) : MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    virtmemUnlock();
    if (R_SUCCEEDED(rc)) {
      g_emu_backing = (uintptr_t)p;
      g_rx = g_rw = (uintptr_t)a;
      /* page 0: the stray-jump stub (see jit_alloc) */
      void dcr_emu_stray(uint32_t *regs);
      uint32_t *st = (uint32_t *)g_rx;
      st[0] = 0xE92D5FFFu; /* push {r0-r12, lr} */
      st[1] = 0xE1A0000Du; /* mov r0, sp */
      st[2] = 0xE51FF004u; /* ldr pc, [pc, #-4] */
      st[3] = (uint32_t)(uintptr_t)dcr_emu_stray;
    } else {
      debugPrintf("[jit] emulator: no alias for the arena (0x%x): rewritten JIT code may run stale\n", rc);
    }
  } else {
    Result rc = jitCreate(&g_jit, g_size);
    if (R_FAILED(rc)) {
      debugPrintf("[jit] jitCreate(%u MB) failed 0x%x\n", (unsigned)(g_size >> 20), rc);
      return -1;
    }
    rc = jitTransitionToExecutable(&g_jit);
    if (R_FAILED(rc)) {
      debugPrintf("[jit] jitTransitionToExecutable failed 0x%x\n", rc);
      return -1;
    }
    g_rx = (uintptr_t)jitGetRxAddr(&g_jit);
    g_rw = (uintptr_t)jitGetRwAddr(&g_jit);
  }
  if (g_emu_backing) {
    emu_book_init();
    g_used[0] = 2; /* the stray-jump stub's page */
    emu_sentinel_fill(1, g_pages - 1);
  }
  g_ready = 1;
  debugPrintf("[jit] arena %u MB: rx %p rw %p%s\n", (unsigned)(g_size >> 20), (void *)g_rx,
              (void *)g_rw, g_rx == g_rw ? " (emulator: single view)" : "");
  if (g_rx != g_rw) {
    jit_self_test();
  } else {
    /* Emulator: does rewritten code run in its new form? Mono patches code it
     * has already run (call sites, trampolines); an emulator's translation
     * cache must notice. */
    volatile uint32_t *w = (volatile uint32_t *)(g_rx + g_size - PAGE);
    w[0] = 0xE3A0002Au; /* mov r0, #42 */
    w[1] = 0xE12FFF1Eu; /* bx lr */
    jit_flush((void *)w, 8);
    int (*fn)(void) = (int (*)(void))(uintptr_t)w;
    int r1 = fn();
    w[0] = 0xE3A00007u; /* mov r0, #7 */
    jit_flush((void *)w, 8);
    dcr_jit_emu_frame_end(); /* as the frame loop does */
    int r2 = fn();
    debugPrintf("[jit] emulator self-test: code ran (%d), rewritten code ran as %d: %s\n", r1, r2,
                r2 == 7 ? "OK" : "STALE -- the emulator runs the old translation");
    w[0] = w[1] = 0;
  }
  return 0;
}

void *jit_alloc(size_t len) {
  if (!g_ready && jit_arena_init() < 0)
    return NULL;
  size_t need = (len + PAGE - 1) / PAGE;
  mutexLock(&g_lock);
  for (size_t i = 0; i + need <= g_pages;) {
    size_t j = 0;
    while (j < need && !g_used[i + j])
      j++;
    if (j == need) {
      g_used[i] = 2;
      for (size_t k = 1; k < need; k++)
        g_used[i + k] = 1;
      mutexUnlock(&g_lock);
      void *rx = (void *)(g_rx + i * PAGE);
      if (g_emu_backing)
        emu_reuse_pages(i, need);
      /* zeroed, as mmap's pages are: Mono relies on it (this Mono's specific
       * trampolines branch to the word BEFORE the generic trampoline, a zero
       * word -- `andeq r0, r0, r0`, a no-op -- on a phone as here) */
      memset((void *)(g_rw + i * PAGE), 0, need * PAGE);
      return rx;
    }
    i += j + 1;
  }
  mutexUnlock(&g_lock);
  debugPrintf("[jit] arena exhausted (%u KB requested)\n", (unsigned)(len >> 10));
  return NULL;
}

int jit_free(void *rx, size_t len) {
  if (!jit_contains(rx))
    return -1;
  size_t i = ((uintptr_t)rx - g_rx) / PAGE;
  size_t n = (len + PAGE - 1) / PAGE;
  mutexLock(&g_lock);
  for (size_t k = 0; k < n && i + k < g_pages; k++)
    g_used[i + k] = 0;
  mutexUnlock(&g_lock);
  if (g_emu_backing)
    emu_sentinel_fill(i, n);
  return 0;
}

uint32_t jit_arena_base(void) { return (uint32_t)g_rx; }
uint32_t jit_arena_size(void) { return (uint32_t)g_size; }

int jit_contains(const void *p) {
  uintptr_t a = (uintptr_t)p;
  return g_ready && a >= g_rx && a < g_rx + g_size;
}

int jit_range_contains(const void *p, size_t n) {
  uintptr_t a = (uintptr_t)p;
  return g_ready && n && a < g_rx + g_size && a + n > g_rx;
}

void *jit_rw(const void *rx) {
  if (!jit_contains(rx))
    return (void *)rx;
  return (void *)(g_rw + ((uintptr_t)rx - g_rx));
}

int jit_is_split(void) { return g_ready && g_rx != g_rw; }

/* Instruction-cache maintenance is the runtime's (code_flush.c: the flip
 * page, proven here on hardware 2026-09-23); the JIT arena's ranges come back
 * here through port_code_flush below: their writable view is elsewhere, and
 * under an emulator the arena pages are remapped instead. */
/* Make [code, code+size) -- just written -- safe to execute on every core. */
/* Emulator: unmap and map back the arena pages [code, code+size) touches --
 * the one thing that makes Ryujinx drop its translations of them. A thread
 * running code on those pages in that instant would fault: the window is two
 * SVCs, and on the pages just written (a new method, a patched call site). */
static volatile uint32_t g_emu_remaps, g_emu_remap_fail, g_emu_fresh, g_emu_rewrites;
/* Emulator bookkeeping of flushed code: one bit per word (flushed before) and
 * a shadow copy of what was flushed (64 KB blocks, made when first used).
 *   - a word flushed for the first time was never run: nothing to do;
 *   - a flush that CHANGES words flushed before is Mono patching a call site
 *     (the bl now goes straight to the compiled method) or dropping a
 *     class-init call (a nop). Making the emulator see it takes a remap,
 *     which leaves the page unmapped for a moment (another thread translating
 *     code on it right then kills the process). So the patch is recorded and
 *     UNDONE -- memory keeps matching the emulator's translation, and the old
 *     bl still reaches the method through its trampoline -- and between
 *     frames, with the other threads paused, the frame loop applies the
 *     frame's patches and remaps their pages (dcr_jit_emu_frame_end). */
#define SHADOW_SHIFT 16
static uint32_t **g_emu_shadow;
static uint32_t *g_emu_pending;      /* one bit per arena page: remap between frames */
static volatile int g_emu_any_pending;
static void emu_book_init(void) {
  g_emu_flushed = calloc(g_size / 4 / 32, 4);
  g_emu_shadow = calloc(g_size >> SHADOW_SHIFT, sizeof *g_emu_shadow);
  g_emu_pending = calloc(g_pages / 32 + 1, 4);
}
static uint32_t *shadow_word(size_t word) {
  size_t blk = (word * 4) >> SHADOW_SHIFT;
  if (!g_emu_shadow[blk] && !(g_emu_shadow[blk] = calloc(1u << SHADOW_SHIFT, 1)))
    return NULL;
  return g_emu_shadow[blk] + (word & ((1u << (SHADOW_SHIFT - 2)) - 1));
}

static Result g_emu_fail_rc;
static uintptr_t g_emu_fail_at;
/* Patches waiting for the frame end: arena word index and the value Mono
 * wrote. Until then the word holds its old value again (undone), so memory
 * always matches what the emulator runs. */
typedef struct { uint32_t word, value; } EmuPatch;
static EmuPatch *g_emu_patches;
static size_t g_emu_npatch, g_emu_cappatch;

static void emu_remap(uintptr_t lo, uintptr_t hi) { /* no logging here: see ep_pause */
  void *src = (void *)(g_emu_backing + (lo - g_rx));
  Result rc = svcUnmapMemory((void *)lo, src, hi - lo);
  if (R_SUCCEEDED(rc))
    rc = svcMapMemory((void *)lo, src, hi - lo);
  if (R_FAILED(rc) && !g_emu_remap_fail++) {
    g_emu_fail_rc = rc;
    g_emu_fail_at = lo;
  }
  g_emu_remaps++;
}

/* Pages handed out again after a free may still have translations of the
 * code that was there: remap them now, while nothing can run on them, and
 * forget what was flushed on them. */
static void emu_reuse_pages(size_t first, size_t n) {
  int used = 0;
  for (size_t w = first * PAGE / 4; w < (first + n) * PAGE / 4 && !used; w += 32)
    used = g_emu_flushed[w >> 5] != 0;
  if (!used)
    return;
  mutexLock(&g_emu_lock);
  for (size_t w = first * PAGE / 4; w < (first + n) * PAGE / 4; w += 32)
    g_emu_flushed[w >> 5] = 0;
  size_t keep = 0; /* patches recorded for the old code there are void */
  for (size_t i = 0; i < g_emu_npatch; i++)
    if (g_emu_patches[i].word < first * PAGE / 4 || g_emu_patches[i].word >= (first + n) * PAGE / 4)
      g_emu_patches[keep++] = g_emu_patches[i];
  g_emu_npatch = keep;
  emu_remap(g_rx + first * PAGE, g_rx + (first + n) * PAGE);
  mutexUnlock(&g_emu_lock);
}

static void ep_pause(BThread *t, void *arg);
#define EP_MAX 128
static BThread *g_ep[EP_MAX];
static int g_ep_n;
static uint32_t g_emu_reused;

/* A flush of more than a call site (Mono's call-site patches are one or two
 * words) that changes words flushed before is NEW code written where old code
 * was: Mono's dynamic-code allocator (dlmalloc inside its 64 KB exec
 * segments) reuses what it freed without an munmap, so jit_free never hears
 * of it. Undoing that as a patch ran the old bytes -- an old literal pool --
 * as the new method (Ryujinx 2026-09-29: "Unknown MRC 0x9E0D5FF0" on
 * UnityMain, a heap pointer executed). So it is kept, and its pages are
 * remapped at once with the other threads paused (as at a frame end): no
 * translation of the old code survives, and nothing translates meanwhile. */
static void emu_fresh_over_old(size_t lo, size_t hi) {
  uint32_t *mem = (uint32_t *)g_rx;
  for (size_t k = lo; k < hi; k++) {
    uint32_t *sh = shadow_word(k);
    if (!sh)
      continue;
    g_emu_flushed[k >> 5] |= 1u << (k & 31);
    *sh = mem[k];
  }
  size_t keep = 0; /* patches recorded for the old code there are void */
  for (size_t i = 0; i < g_emu_npatch; i++)
    if (g_emu_patches[i].word < lo || g_emu_patches[i].word >= hi)
      g_emu_patches[keep++] = g_emu_patches[i];
  g_emu_npatch = keep;
  g_ep_n = 0;
  b_thread_foreach(ep_pause, b_thread_self());
  const uintptr_t first = g_rx + ((lo * 4) & ~(uintptr_t)(PAGE - 1));
  const uintptr_t last = g_rx + ((hi * 4 + PAGE - 1) & ~(uintptr_t)(PAGE - 1));
  emu_remap(first, last);
  for (int i = 0; i < g_ep_n; i++) {
    b_pause_lock();
    if (!g_ep[i]->gc_paused)
      svcSetThreadActivity(g_ep[i]->handle, ThreadActivity_Runnable);
    b_pause_unlock();
  }
  g_emu_reused++;
}

/* Mono's specific trampolines are three words:
 *     push {r0-r12, lr} ; bl <generic trampoline> ; .word <MonoMethod *>
 * and the generic trampoline finds the method at lr. Ryujinx decodes a whole
 * function at once and treats the word after a bl as the call's return
 * point: it decodes the MonoMethod pointer as an instruction. A heap address
 * like 0x9E0D5FF0 reads as an MRC it does not implement, and the translation
 * throws (2026-09-29: "Unknown MRC 0x9E1..." on UnityMain, more often the
 * more the heap is used). So under the emulator the bl becomes a plain b to
 * a stub that sets lr itself and goes on -- nothing after it to decode:
 *     stub: ldr lr, [pc, #0] ; b <generic> ; .word <trampoline + 8>   */
#define TSTUB_BYTES (128u << 10) /* ~10900 trampolines */
static uint32_t *g_tstub, g_tstub_left;
static uint32_t g_tramps_rewritten;
static void emu_split_trampoline(uint32_t *t) {
  if (t[0] != 0xE92D5FFFu || (t[1] & 0x0F000000u) != 0x0B000000u || (t[1] >> 28) != 0xE)
    return;
  if (g_tstub_left < 3)
    return; /* out of stubs: left as Mono made it */
  const int32_t off = (int32_t)(t[1] << 8) >> 6; /* imm24 * 4, signed */
  const uintptr_t generic = (uintptr_t)(t + 1) + 8 + off;
  uint32_t *st = g_tstub;
  g_tstub += 3;
  g_tstub_left -= 3;
  const int32_t boff = (int32_t)(generic - ((uintptr_t)(st + 1) + 8)) >> 2;
  const int32_t toff = (int32_t)((uintptr_t)st - ((uintptr_t)(t + 1) + 8)) >> 2;
  st[0] = 0xE59FE000u;                          /* ldr lr, [pc, #0] */
  st[1] = 0xEA000000u | ((uint32_t)boff & 0x00FFFFFFu); /* b generic */
  st[2] = (uint32_t)(uintptr_t)(t + 2);
  for (int k = 0; k < 3; k++) { /* fresh words: flushed as they are */
    const size_t w = ((uintptr_t)(st + k) - g_rx) >> 2;
    uint32_t *sh = shadow_word(w);
    g_emu_flushed[w >> 5] |= 1u << (w & 31);
    if (sh)
      *sh = st[k];
  }
  t[1] = 0xEA000000u | ((uint32_t)toff & 0x00FFFFFFu); /* b stub */
  g_tramps_rewritten++;
}

int dcr_jitlog_on(void); /* dcr_mod.c: the "jitlog" debug aid */
static void emu_flush(uintptr_t code, size_t size) {
  size_t lo = (code - g_rx) >> 2, hi = (code + size - g_rx + 3) >> 2;
  uint32_t *mem = (uint32_t *)g_rx;
  int changed = 0;
  static int stubs_taken;
  if (!stubs_taken && size == 12) { /* jit_alloc may take g_emu_lock itself: not under it */
    stubs_taken = 1;
    g_tstub = (uint32_t *)jit_alloc(TSTUB_BYTES);
    g_tstub_left = g_tstub ? TSTUB_BYTES / 4 : 0;
  }
  if (dcr_jitlog_on()) {
    int prev = 0;
    for (size_t k = lo; k < hi; k++)
      prev += (g_emu_flushed[k >> 5] >> (k & 31)) & 1;
    debugPrintf("[jitflush] %p +%u (%d words flushed before) %08lx %08lx %08lx %08lx\n", (void *)code, (unsigned)size, prev,
                (unsigned long)mem[lo], (unsigned long)(hi > lo + 1 ? mem[lo + 1] : 0),
                (unsigned long)(hi > lo + 2 ? mem[lo + 2] : 0), (unsigned long)(hi > lo + 3 ? mem[lo + 3] : 0));
  }
  mutexLock(&g_emu_lock);
  if (size > 16) {
    int old = 0;
    for (size_t k = lo; k < hi && !old; k++) {
      uint32_t *sh = shadow_word(k);
      old = sh && (g_emu_flushed[k >> 5] & (1u << (k & 31))) && *sh != mem[k];
    }
    if (old) {
      emu_fresh_over_old(lo, hi);
      mutexUnlock(&g_emu_lock);
      return;
    }
  }
  if (size == 12 && !(g_emu_flushed[lo >> 5] & (1u << (lo & 31))))
    emu_split_trampoline(mem + lo); /* g_tstub: taken before the lock, below */
  for (size_t k = lo; k < hi; k++) {
    uint32_t bit = 1u << (k & 31), *sh = shadow_word(k);
    if (!sh)
      continue;
    if (!(g_emu_flushed[k >> 5] & bit)) {
      g_emu_flushed[k >> 5] |= bit;
      *sh = mem[k];
    } else if (*sh != mem[k]) {
      if (g_emu_npatch == g_emu_cappatch) {
        size_t cap = g_emu_cappatch ? g_emu_cappatch * 2 : 256;
        EmuPatch *n = realloc(g_emu_patches, cap * sizeof *n);
        if (!n)
          continue; /* no room: the patch stays in memory only (stale until reused) */
        g_emu_patches = n;
        g_emu_cappatch = cap;
      }
      g_emu_patches[g_emu_npatch].word = (uint32_t)k;
      g_emu_patches[g_emu_npatch].value = mem[k];
      g_emu_npatch++;
      mem[k] = *sh; /* undone until the frame end */
      changed = 1;
    }
  }
  if (changed) {
    g_emu_any_pending = 1;
    g_emu_rewrites++;
  } else {
    g_emu_fresh++;
  }
  mutexUnlock(&g_emu_lock);
}

/* The other guest threads are paused around the remap -- Unity's loading
 * thread runs managed code too -- as the GC bridge pauses them (mono_rt.c):
 * the emulator stops a thread only between translated blocks, so none is
 * translating code on the page when it goes. Nothing is logged or allocated
 * while they are paused (they may hold those locks). */
static void ep_pause(BThread *t, void *arg) {
  if (t == arg || t->finished || t->handle == INVALID_HANDLE || g_ep_n >= EP_MAX)
    return;
  b_pause_lock();
  if (!t->gc_paused && R_SUCCEEDED(svcSetThreadActivity(t->handle, ThreadActivity_Paused)))
    g_ep[g_ep_n++] = t;
  b_pause_unlock();
}

/* The frame loop, between frames (dcr_boot.c): make the emulator see the
 * code patched since the last frame (see g_emu_pending). */
void dcr_jit_emu_frame_end(void) {
  static uint32_t reused_logged, tramps_logged;
  if (g_tramps_rewritten && !tramps_logged) {
    tramps_logged = 1;
    debugPrintf("[jit] emulator: Mono's specific trampolines reach the generic one through a stub (no data "
                "after a bl for the emulator to decode)\n");
  }
  if (g_emu_reused != reused_logged && reused_logged < 16) {
    reused_logged = g_emu_reused;
    debugPrintf("[jit] emulator: new code over reused JIT memory %lu time(s): kept, pages remapped at once\n",
                (unsigned long)g_emu_reused);
  }
  if (!g_emu_any_pending)
    return;
  mutexLock(&g_emu_lock); /* first: no thread is inside emu_flush once paused */
  g_ep_n = 0;
  b_thread_foreach(ep_pause, b_thread_self());
  g_emu_any_pending = 0;
  /* the frame's patches, the last one for a word winning */
  uint32_t *mem = (uint32_t *)g_rx;
  for (size_t i = 0; i < g_emu_npatch; i++) {
    const uint32_t k = g_emu_patches[i].word;
    mem[k] = g_emu_patches[i].value;
    *shadow_word(k) = g_emu_patches[i].value;
    const size_t pg = ((size_t)k * 4) / PAGE;
    g_emu_pending[pg >> 5] |= 1u << (pg & 31);
  }
  g_emu_npatch = 0;
  for (size_t w = 0; w <= g_pages / 32; w++) {
    uint32_t bits = g_emu_pending[w];
    if (!bits)
      continue;
    g_emu_pending[w] = 0;
    for (int b = 0; b < 32; b++)
      if (bits >> b & 1) {
        uintptr_t pg = g_rx + (w * 32 + b) * PAGE;
        emu_remap(pg, pg + PAGE);
      }
  }
  for (int i = 0; i < g_ep_n; i++) {
    b_pause_lock();
    if (!g_ep[i]->gc_paused) /* a collection that began meanwhile keeps it */
      svcSetThreadActivity(g_ep[i]->handle, ThreadActivity_Runnable);
    b_pause_unlock();
  }
  mutexUnlock(&g_emu_lock);
  if (g_emu_remap_fail == 1) {
    g_emu_remap_fail++;
    debugPrintf("[jit] emulator remap failed (0x%x at %p): patched JIT code may run stale\n", g_emu_fail_rc,
                (void *)g_emu_fail_at);
  }
}
uint32_t dcr_emu_jit_remaps(void) { return g_emu_remaps; }

/* The runtime's dcr_code_flush asks here first: the arena's ranges are
 * cleaned through their writable view (hardware) or remapped (emulator). */
int port_code_flush(void *code, size_t size) {
  if (!jit_contains(code))
    return 0;
  if (g_emu_backing) {
    emu_flush((uintptr_t)code, size);
    return 1;
  }
  void *d = jit_rw(code);
  Result rc = svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u64)(uintptr_t)d, size);
  if (R_FAILED(rc)) {
    static int warned;
    if (!warned++)
      debugPrintf("[jit] data-cache flush of %p+0x%x failed: 0x%x\n", d, (unsigned)size, rc);
  }
  dcr_icache_invalidate();
  return 1;
}

static volatile uint32_t g_jit_flushes;
uint32_t dcr_jit_flushes(void) { return g_jit_flushes; }

/* Replacement for Mono's mono_arch_flush_icache(code, size). */
void jit_flush(void *code, int size) {
  __atomic_add_fetch(&g_jit_flushes, 1, __ATOMIC_RELAXED);
  if (size > 0)
    dcr_code_flush(code, (size_t)size);
}
