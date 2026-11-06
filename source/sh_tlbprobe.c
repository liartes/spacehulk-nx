/* sh_tlbprobe.c -- what a TLB miss costs here, zone by zone ([debug] tlb_probe).
 *
 * The AArch32 port of torchlight-nx's src/torchlight_tlb.cpp (2026-11-04),
 * to check sh_bigpages.c on the console. The Cortex-A57 has 1024 TLB entries
 * for code and data together: a 4 KB page takes one, a 2 MB block one for 512
 * pages.
 *
 * Method: with the same number of cache lines (so the caches behave alike),
 * two chains of dependent loads in random order:
 *   - grouped: every line of a few consecutive pages (the TLB holds them);
 *   - scattered: one random line in each of n pages spread over the zone (more
 *     pages than the TLB holds if the zone is in 4 KB pages, a few entries if
 *     it is in blocks).
 * Scattered minus grouped is the price of the misses: near 0 in blocks. Code
 * is also measured executing: calls to `bx lr` words (0xE12FFF1E) already in
 * its pages. In ARM state that word is `bx lr` wherever it sits, so a call only
 * returns. Pages and lines are random: a regular pattern measured the L2
 * cache, not the TLB (torchlight's first version).
 *
 * Witness: the same physical pages mapped twice by svcMapProcessMemory, at a
 * 2 MB-aligned address (blocks possible) and 4 KB further (blocks
 * impossible). Same lines on both sides, so a gap can only be the TLB. It
 * also shows whether the heap is VA = PA modulo 2 MB in this process, which
 * sh_bigpages.c relies on: if it is not, both sides cost the same.
 *
 * Zones: the witness, a heap buffer of its own, libunity's and libmono's code
 * (read and execute), this program's code (read and execute) and data, and
 * the JIT arena's RX view (read). Nothing of the game's is written.
 * svcQueryPhysicalAddress proves nothing (it refuses code) and is not used.
 *
 * Output: "[tlb]" lines in debug.log, a minute after the start, then twice
 * more 30 s apart; then the 32 MB it took are given back. Times are the
 * median ns per access of 12 passes. MIT.
 */
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "jit_arena.h"
#include "selfproc.h"
#include "sh_bigpages.h"
#include "so_util.h"
#include "util.h"

#define PAGE 0x1000u
#define LINES_PER_PAGE 64u
#define MAX_LINES 4096u
#define MAX_RET_PAGES 8192u
#define HEAP_BYTES (16u << 20)
#define WITNESS_BYTES (16u << 20)
#define BLOCKS 3 /* grouped / scattered alternations */
#define PASSES 5 /* per block; the first one fills the caches and is not counted */
#define SAMPLES (BLOCKS * (PASSES - 1))
#define CYCLES 3
#define BX_LR 0xE12FFF1Eu

static uint32_t g_grouped[MAX_LINES] __attribute__((aligned(64)));
static uint32_t g_scattered[MAX_LINES] __attribute__((aligned(64)));
static uint32_t g_ret[MAX_RET_PAGES]; /* one `bx lr` per page that has one */
static uint32_t g_rng = 0x9E3779B9u;
static Thread g_thread;

static uint32_t rnd(void) {
  g_rng ^= g_rng << 13;
  g_rng ^= g_rng >> 17;
  g_rng ^= g_rng << 5;
  return g_rng;
}

static void shuffle(uint32_t *t, size_t n) {
  for (size_t i = n - 1; i > 0; i--) {
    size_t j = rnd() % (i + 1);
    uint32_t x = t[i];
    t[i] = t[j];
    t[j] = x;
  }
}

/* Dependent loads: each table entry is read at an offset made from the byte
 * loaded just before (eor of a register with itself: 0, but after it). */
__attribute__((noinline)) static uint64_t pass_read(const uint32_t *t, size_t n) {
  const uint32_t *end = t + n;
  uint32_t v = 0, zero, a;
  const uint64_t t0 = armGetSystemTick();
  __asm__ volatile("1:\n"
                   "  eor %[zero], %[v], %[v]\n"
                   "  ldr %[a], [%[p], %[zero]]\n"
                   "  ldrb %[v], [%[a]]\n"
                   "  add %[p], %[p], #4\n"
                   "  cmp %[p], %[end]\n"
                   "  bne 1b\n"
                   : [p] "+r"(t), [v] "+r"(v), [zero] "=&r"(zero), [a] "=&r"(a)
                   : [end] "r"(end)
                   : "cc", "memory");
  return armGetSystemTick() - t0;
}

/* Calls to `bx lr` words in code: the instruction-side price. */
__attribute__((noinline)) static uint64_t pass_exec(const uint32_t *t, size_t n) {
  const uint32_t *end = t + n;
  const uint64_t t0 = armGetSystemTick();
  __asm__ volatile("1:\n"
                   "  ldr r12, [%[p]], #4\n"
                   "  blx r12\n"
                   "  cmp %[p], %[end]\n"
                   "  bne 1b\n"
                   : [p] "+r"(t)
                   : [end] "r"(end)
                   : "r12", "lr", "cc", "memory");
  return armGetSystemTick() - t0;
}

typedef uint64_t (*Pass)(const uint32_t *, size_t);

/* tenths of a ns per access */
static int tenths(uint64_t ticks, size_t n) { return (int)(armTicksToNs(ticks) * 10 / n); }

static int median(int *v, int n) {
  for (int i = 1; i < n; i++)
    for (int j = i; j > 0 && v[j - 1] > v[j]; j--) {
      int x = v[j];
      v[j] = v[j - 1];
      v[j - 1] = x;
    }
  return v[n / 2];
}

/* "12.3", "-0.4" */
static const char *ns(char *buf, int t) {
  snprintf(buf, 16, "%s%d.%d", t < 0 ? "-" : "", (t < 0 ? -t : t) / 10, (t < 0 ? -t : t) % 10);
  return buf;
}

/* The two tables in alternated blocks: medians in tenths of ns. */
static void compare(Pass pass, size_t n, int *grouped, int *scattered) {
  int g[SAMPLES], s[SAMPLES], ng = 0, ns_ = 0;
  for (int b = 0; b < BLOCKS; b++) {
    for (int p = 0; p < PASSES; p++) {
      uint64_t t = pass(g_grouped, n);
      if (p)
        g[ng++] = tenths(t, n);
    }
    for (int p = 0; p < PASSES; p++) {
      uint64_t t = pass(g_scattered, n);
      if (p)
        s[ns_++] = tenths(t, n);
    }
    svcSleepThread(1000000); /* the game's threads on this core get their turn */
  }
  *grouped = median(g, ng);
  *scattered = median(s, ns_);
}

static void report(const char *zone, const char *what, size_t n, int g, int s, const char *extra) {
  char a[16], b[16], c[16];
  debugPrintf("[tlb] %s %s, %u lines: grouped %s ns, scattered %s ns, gap %s%s ns%s\n", zone, what,
              (unsigned)n, ns(a, g), ns(b, s), s >= g ? "+" : "", ns(c, s - g), extra);
}

/* Line counts for a zone of `pages` pages: 2048 and 4096 where they fit (as
 * torchlight), else 1024 -- never fewer: 512 lines are 32 KB, the size of the
 * L1 data cache, which the grouped set then fits and the scattered one does
 * not (libmono read +9 ns in 2 MB blocks, hardware 2026-11-06). */
static int line_counts(size_t pages, size_t *out) {
  int k = 0;
  if (pages >= 2048)
    out[k++] = 2048;
  if (pages >= 4096)
    out[k++] = 4096;
  if (!k && pages >= 2 * 1024 / LINES_PER_PAGE)
    out[k++] = 1024;
  return k;
}

static void measure_read(const char *zone, uintptr_t start, size_t size) {
  const size_t pages = size / PAGE;
  size_t counts[2];
  const int nc = line_counts(pages, counts);
  for (int c = 0; c < nc; c++) {
    const size_t n = counts[c];
    /* scattered: line k in a random page of the k-th n-th of the zone (a
     * zone of fewer than n pages gives some pages two lines) */
    for (size_t k = 0; k < n; k++) {
      const size_t lo = k * pages / n, hi = (k + 1) * pages / n;
      const size_t page = lo + (hi > lo ? rnd() % (hi - lo) : 0);
      g_scattered[k] = start + page * PAGE + (rnd() % LINES_PER_PAGE) * 64;
    }
    const size_t first = pages / 2;
    for (size_t k = 0; k < n; k++)
      g_grouped[k] = start + (first + k / LINES_PER_PAGE) * PAGE + (k % LINES_PER_PAGE) * 64;
    shuffle(g_scattered, n);
    shuffle(g_grouped, n);
    int g, s;
    compare(pass_read, n, &g, &s);
    char extra[48];
    snprintf(extra, sizeof extra, " (%u pages)", (unsigned)pages);
    report(zone, "read", n, g, s, extra);
  }
}

static void measure_exec(const char *zone, uintptr_t start, size_t size) {
  const size_t pages = size / PAGE;
  size_t nret = 0;
  for (size_t p = 0; p < pages && nret < MAX_RET_PAGES; p++) {
    const uint32_t *w = (const uint32_t *)(start + p * PAGE);
    unsigned seen = 0, pick = 0;
    for (unsigned i = 0; i < PAGE / 4; i++)
      if (w[i] == BX_LR && rnd() % ++seen == 0)
        pick = i;
    if (seen)
      g_ret[nret++] = start + p * PAGE + pick * 4;
  }
  size_t n = 256;
  while (n * 2 <= nret && n * 2 <= 2048)
    n *= 2;
  if (n > nret) {
    debugPrintf("[tlb] %s execute: only %u pages with a bx lr, skipped\n", zone, (unsigned)nret);
    return;
  }
  /* scattered: n of those pages, at random */
  for (size_t k = 0; k < n; k++) {
    size_t j = k + rnd() % (nret - k);
    uint32_t x = g_ret[k];
    g_ret[k] = g_ret[j];
    g_ret[j] = x;
    g_scattered[k] = g_ret[k];
  }
  /* grouped: every bx lr of consecutive pages from the middle, one per line */
  size_t filled = 0, gpages = 0;
  for (size_t i = 0; i < pages && filled < n; i++) {
    const uintptr_t page = start + ((pages / 2 + i) % pages) * PAGE;
    const uint32_t *w = (const uint32_t *)page;
    gpages++;
    for (unsigned line = 0; line < LINES_PER_PAGE && filled < n; line++)
      for (unsigned m = 0; m < 16; m++)
        if (w[line * 16 + m] == BX_LR) {
          g_grouped[filled++] = page + (line * 16 + m) * 4;
          break;
        }
  }
  if (filled < n) {
    debugPrintf("[tlb] %s execute: only %u bx lr lines, skipped\n", zone, (unsigned)filled);
    return;
  }
  shuffle(g_scattered, n);
  shuffle(g_grouped, n);
  int g, s;
  compare(pass_exec, n, &g, &s);
  char extra[64];
  snprintf(extra, sizeof extra, " (grouped in %u pages, scattered in %u)", (unsigned)gpages, (unsigned)n);
  report(zone, "execute", n, g, s, extra);
}

/* ----------------------------------------------------------------- witness */
static void *g_w_src, *g_w_shadow;
static uintptr_t g_w_aligned, g_w_off;
static VirtmemReservation *g_w_rv[2];
static int g_w_mapped; /* 0 none, 1 shadow, 2 shadow RW, 3 + aligned, 4 + offset */

/* virtmemFindAslr's second argument is a guard, not an alignment. Under
 * virtmemLock. */
static uintptr_t find_aligned(size_t size, VirtmemReservation **rv) {
  uintptr_t z = (uintptr_t)virtmemFindAslr(size + SH_BLOCK, PAGE);
  if (!z)
    return 0;
  uintptr_t a = (z + SH_BLOCK - 1) & ~(uintptr_t)(SH_BLOCK - 1);
  *rv = virtmemAddReservation((void *)a, size);
  return *rv ? a : 0;
}

static int witness_make(void) {
  const Handle proc = dcr_self_process();
  g_w_src = memalign(SH_BLOCK, WITNESS_BYTES);
  if (!g_w_src)
    return 0;
  memset(g_w_src, 1, WITNESS_BYTES);
  Result rc = 1;
  virtmemLock();
  g_w_shadow = virtmemFindCodeMemory(WITNESS_BYTES, PAGE);
  if (g_w_shadow)
    rc = svcMapProcessCodeMemory(proc, (u64)(uintptr_t)g_w_shadow, (u64)(uintptr_t)g_w_src, WITNESS_BYTES);
  if (R_SUCCEEDED(rc)) {
    g_w_mapped = 1;
    g_w_aligned = find_aligned(WITNESS_BYTES, &g_w_rv[0]);
    g_w_off = find_aligned(WITNESS_BYTES + SH_BLOCK, &g_w_rv[1]);
    if (g_w_off)
      g_w_off += PAGE;
  }
  virtmemUnlock();
  if (R_SUCCEEDED(rc) && (!g_w_aligned || !g_w_off))
    rc = 2;
  if (R_SUCCEEDED(rc) &&
      R_SUCCEEDED(rc = svcSetProcessMemoryPermission(proc, (u64)(uintptr_t)g_w_shadow, WITNESS_BYTES, Perm_Rw)))
    g_w_mapped = 2;
  if (R_SUCCEEDED(rc) &&
      R_SUCCEEDED(rc = svcMapProcessMemory((void *)g_w_aligned, proc, (u64)(uintptr_t)g_w_shadow, WITNESS_BYTES)))
    g_w_mapped = 3;
  if (R_SUCCEEDED(rc) &&
      R_SUCCEEDED(rc = svcMapProcessMemory((void *)g_w_off, proc, (u64)(uintptr_t)g_w_shadow, WITNESS_BYTES)))
    g_w_mapped = 4;
  debugPrintf("[tlb] witness: source 0x%08x, shadow 0x%08x, aligned 0x%08x, 4 KB off 0x%08x: 0x%x\n",
              (unsigned)(uintptr_t)g_w_src, (unsigned)(uintptr_t)g_w_shadow, (unsigned)g_w_aligned,
              (unsigned)g_w_off, rc);
  return g_w_mapped == 4;
}

static void witness_free(void) {
  const Handle proc = dcr_self_process();
  int ok = 1;
  if (g_w_mapped >= 4)
    ok &= R_SUCCEEDED(svcUnmapProcessMemory((void *)g_w_off, proc, (u64)(uintptr_t)g_w_shadow, WITNESS_BYTES));
  if (g_w_mapped >= 3)
    ok &= R_SUCCEEDED(svcUnmapProcessMemory((void *)g_w_aligned, proc, (u64)(uintptr_t)g_w_shadow, WITNESS_BYTES));
  if (ok && g_w_mapped >= 1)
    ok = R_SUCCEEDED(svcUnmapProcessCodeMemory(proc, (u64)(uintptr_t)g_w_shadow, (u64)(uintptr_t)g_w_src,
                                               WITNESS_BYTES));
  virtmemLock();
  for (int i = 0; i < 2; i++)
    if (ok && g_w_rv[i])
      virtmemRemoveReservation(g_w_rv[i]);
  virtmemUnlock();
  if (ok && g_w_src)
    free(g_w_src); /* the pages are heap again; otherwise they stay given away */
  if (!ok)
    debugPrintf("[tlb] witness: could not unmap it (16 MB stay taken)\n");
  g_w_mapped = 0;
}

static void measure_witness(void) {
  const size_t pages = WITNESS_BYTES / PAGE;
  for (size_t n = 2048; n <= 4096; n *= 2) {
    const size_t step = pages / n;
    for (size_t k = 0; k < n; k++) {
      const uint32_t off = (k * step + rnd() % step) * PAGE + (rnd() % LINES_PER_PAGE) * 64;
      g_grouped[k] = g_w_aligned + off;
      g_scattered[k] = g_w_off + off;
    }
    for (size_t i = n - 1; i > 0; i--) { /* the same order on both sides */
      size_t j = rnd() % (i + 1);
      uint32_t x = g_grouped[i];
      g_grouped[i] = g_grouped[j];
      g_grouped[j] = x;
      x = g_scattered[i];
      g_scattered[i] = g_scattered[j];
      g_scattered[j] = x;
    }
    int a, d;
    compare(pass_read, n, &a, &d);
    char x[16], y[16], z[16];
    debugPrintf("[tlb] witness, %u lines: 2 MB-aligned %s ns, 4 KB off %s ns, gap %s%s ns\n", (unsigned)n,
                ns(x, a), ns(y, d), d >= a ? "+" : "", ns(z, d - a));
  }
}

/* -------------------------------------------------------------------- zones */
/* The executable PT_LOAD of a loaded module, whole pages. */
static int module_code(const char *name, uintptr_t *start, size_t *size) {
  so_module *m = so_find_module_by_name(name);
  if (!m || m->state != SO_SEALED)
    return 0;
  for (int i = 0; i < m->phnum; i++) {
    const Elf32_Phdr *p = &m->phdr[i];
    if (p->p_type == PT_LOAD && (p->p_flags & PF_X)) {
      *start = ((uintptr_t)m->load_virtbase + p->p_vaddr + PAGE - 1) & ~(uintptr_t)(PAGE - 1);
      *size = ((uintptr_t)m->load_virtbase + p->p_vaddr + p->p_memsz - *start) & ~(size_t)(PAGE - 1);
      return 1;
    }
  }
  return 0;
}

/* This program's blocks from its code on: logged; code and the first
 * writable block returned. */
static void own_segments(uintptr_t *code, size_t *code_size, uintptr_t *data, size_t *data_size) {
  MemoryInfo mi;
  u32 pi;
  *code = *data = 0;
  *code_size = *data_size = 0;
  uintptr_t a = (uintptr_t)&pass_read;
  for (int i = 0; i < 6; i++) {
    if (R_FAILED(svcQueryMemory(&mi, &pi, a)) || !mi.size)
      break;
    debugPrintf("[tlb] program block 0x%08x +%u KB: type 0x%x, perm %x, rem 0x%x\n", (unsigned)mi.addr,
                (unsigned)(mi.size >> 10), (unsigned)(mi.type & MemState_Type), (unsigned)mi.perm,
                (unsigned)(mi.addr & (SH_BLOCK - 1)));
    if (i == 0 && mi.perm == Perm_Rx) {
      *code = mi.addr;
      *code_size = mi.size;
    } else if (!*data && mi.perm == Perm_Rw) {
      *data = mi.addr;
      *data_size = mi.size;
    }
    a = mi.addr + mi.size;
  }
}

static void probe(void *arg) {
  svcSleepThread(60000000000ll); /* the game is at its menu by then */
  debugPrintf("[tlb] probe start (large_pages %s)\n", sh_bigpages_on() ? "on" : "off");
  uintptr_t unity = 0, mono = 0, code, data, jit;
  size_t unity_size = 0, mono_size = 0, code_size, data_size, jit_size;
  module_code("libunity.so", &unity, &unity_size);
  module_code("libmono.so", &mono, &mono_size);
  own_segments(&code, &code_size, &data, &data_size);
  jit = jit_arena_rx(&jit_size);
  debugPrintf("[tlb] libunity code 0x%08x +%u KB, libmono code 0x%08x +%u KB, JIT rx 0x%08x +%u KB\n",
              (unsigned)unity, (unsigned)(unity_size >> 10), (unsigned)mono, (unsigned)(mono_size >> 10),
              (unsigned)jit, (unsigned)(jit_size >> 10));

  void *heap = memalign(SH_BLOCK, HEAP_BYTES);
  if (heap)
    memset(heap, 1, HEAP_BYTES);
  else
    debugPrintf("[tlb] no heap buffer\n");
  const int witness = witness_make();

  for (int cycle = 1; cycle <= CYCLES; cycle++) {
    debugPrintf("[tlb] === cycle %d of %d ===\n", cycle, CYCLES);
    if (witness)
      measure_witness();
    if (heap)
      measure_read("heap", (uintptr_t)heap, HEAP_BYTES);
    if (unity_size) {
      measure_read("libunity", unity, unity_size);
      measure_exec("libunity", unity, unity_size);
    }
    if (mono_size) {
      measure_read("libmono", mono, mono_size);
      measure_exec("libmono", mono, mono_size);
    }
    if (code_size) {
      measure_read("program code", code, code_size);
      measure_exec("program code", code, code_size);
    }
    if (data_size)
      measure_read("program data", data, data_size);
    if (jit_size)
      measure_read("JIT", jit, jit_size);
    if (cycle < CYCLES)
      svcSleepThread(30000000000ll);
  }
  if (witness || g_w_mapped)
    witness_free();
  free(heap);
  debugPrintf("[tlb] probe done\n");
}

void sh_tlbprobe_start(void) {
  /* core 2 (the game's other threads share 0 and 2; main 0, render 1), above
   * the game's threads so a pass is not cut by them */
  if (R_SUCCEEDED(threadCreate(&g_thread, probe, NULL, NULL, 0x10000, 0x2C, 2)) &&
      R_SUCCEEDED(threadStart(&g_thread)))
    debugPrintf("[tlb] probe in 60 s\n");
  else
    debugPrintf("[tlb] probe: no thread\n");
}
