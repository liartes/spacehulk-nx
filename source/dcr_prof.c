/* dcr_prof.c -- where a long frame's time goes, on every thread.
 *
 * A theme change or the missions screen is ~5 s behind the screen wipe, most
 * of it one frame of ~2.5 s even at 1785 MHz (dcr_boost.c). The first profile
 * (main thread only, hardware 2026-09-24) showed the main thread SLEEPING
 * through it -- PreloadManager::WaitForAllAsyncOperationsToComplete ->
 * Thread::Sleep -- while Unity's loading thread does the work; other long
 * frames wait on the render thread compiling shaders (SubProgram::Compile ->
 * CreateGpuProgram) or on garbage collection. So every game thread is
 * sampled: in a frame that passes 100 ms, every 10 ms a thread of its own
 * pauses each registered thread for a moment (as the watchdog does, under
 * b_pause_lock, one thread at a time, so the GC bridge and this never both
 * pause one), classifies it by the syscall it sits in (none: running;
 * SendSyncRequest: file/IPC; anything else: waiting) and, when it is not
 * waiting, reads its pc and the code addresses on the mapped top of its stack.
 * Off by default (it costs time in exactly the frames it measures):
 * [debug] profile_long_frames = true in config.ini turns it on. When a frame of 200 ms or more
 * ends, the log gets, for the busiest threads (named as Unity names them):
 *   run/io/wait shares, then
 *   self    the busiest code (pc, 64-byte buckets), module+offset;
 *   incl    the call sites most often on the stack, module+offset;
 *   managed the C# methods on the stack, by name (Mono's tables);
 * and the frame's GC stop-the-world time. libunity/libmono offsets are named
 * offline against symbols. MIT.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "bionic_pthread.h"
#include "dcr_config.h"
#include "jit_arena.h"
#include "util.h"

const char *dcr_addr_name(uint32_t a, char *buf, size_t cap); /* exc_handler.c */
int dcr_is_code_addr(uint32_t a);                               /* exc_handler.c */
size_t dcr_readable(uint32_t p, size_t want);                   /* exc_handler.c */
int dcr_mono_method_name(uint32_t addr, char *buf, size_t cap); /* mono_rt.c */
uint64_t dcr_gc_stopped_ns(void);                               /* mono_rt.c */

#define START_MS 100
#define WINDOW_MS 10000        /* [debug] profile_continuous: one report per window */
#define WINDOW_PERIOD_NS 5000000ll
#define REPORT_MS 200
#define PERIOD_NS 10000000ll
#define MAX_SAMPLES 16384
#define MAX_THREADS 64
#define DEPTH 24
#define SHOW_THREADS 4

typedef struct {
  uint64_t r[29];
  uint64_t fp, lr, sp, pc;
  uint32_t psr, _pad;
  uint8_t v[32][16];
  uint32_t fpcr, fpsr;
  uint64_t tpidr;
} KCtx;

static Result get_ctx(KCtx *ctx, Handle h) {
  register uint32_t r0 __asm__("r0") = (uint32_t)(uintptr_t)ctx;
  register uint32_t r1 __asm__("r1") = h;
  __asm__ volatile("svc 0x33" : "+r"(r0), "+r"(r1) : : "r2", "r3", "r12", "lr", "memory");
  return r0;
}

enum { ST_RUN, ST_IO, ST_WAIT };

typedef struct {
  uint32_t pc;
  uint8_t thread, state, n, _pad;
  uint32_t ret[DEPTH];
} Sample;

typedef struct {
  BThread *t;
  int tid, main;
  char name[16];
  uint32_t n[3];
} Seen;

static Sample *g_s;
static volatile uint32_t g_n;
static Seen g_seen[MAX_THREADS];
static int g_nseen;
static volatile uint64_t g_frame_start; /* system tick; 0 between frames */
static volatile int g_busy;
static uint64_t g_gc_at_start;
static Handle g_main_handle;
static int g_window;           /* continuous mode: every frame, reports per WINDOW_MS */
static uint64_t g_window_start, g_window_frames;
static Thread g_thread;

/* The syscall a paused thread sits in. A thread blocked in the kernel is
 * reported with its pc ON the svc instruction (hardware 2026-09-24: every
 * waiting thread showed as running when only the instruction before it was
 * checked); the one before is checked too, for a thread just back from one. */
static uint32_t svc_at(uint32_t a, int thumb) {
  if (!dcr_is_code_addr(a & ~1u))
    return ~0u;
  if (thumb) { /* svc #imm8 = 0xDFxx */
    uint16_t op = *(const volatile uint16_t *)(uintptr_t)(a & ~1u);
    return (op & 0xFF00) == 0xDF00 ? (uint32_t)(op & 0xFF) : ~0u;
  }
  if (a & 3)
    return ~0u;
  uint32_t op = *(const volatile uint32_t *)(uintptr_t)a; /* svc #imm24 */
  return (op & 0x0F000000) == 0x0F000000 ? (op & 0xFFFFFF) : ~0u;
}

static int state_of(const KCtx *c) {
  uint32_t pc = (uint32_t)c->pc;
  int thumb = (c->psr & 0x20) != 0;
  uint32_t svc = svc_at(pc, thumb);
  if (svc == ~0u)
    svc = svc_at(pc - (thumb ? 2 : 4), thumb);
  if (svc == ~0u)
    return ST_RUN;
  if (svc == 0x21 || svc == 0x22) /* SendSyncRequest(WithUserBuffer): fs and other IPC */
    return ST_IO;
  return ST_WAIT; /* sleep, WaitSynchronization, condvars, arbitration, ... */
}

static int seen_index(BThread *t) {
  for (int i = 0; i < g_nseen; i++)
    if (g_seen[i].t == t && g_seen[i].tid == t->tid)
      return i;
  if (g_nseen == MAX_THREADS)
    return -1;
  Seen *e = &g_seen[g_nseen];
  memset(e, 0, sizeof *e);
  e->t = t;
  e->tid = t->tid;
  e->main = t->handle == g_main_handle;
  memcpy(e->name, t->name, sizeof e->name);
  e->name[sizeof e->name - 1] = 0;
  return g_nseen++;
}

static void sample_one(BThread *t, void *arg) {
  if (t->handle == INVALID_HANDLE || t->finished || g_n >= MAX_SAMPLES)
    return;
  int ti = seen_index(t);
  if (ti < 0)
    return;
  /* per thread, so the GC bridge never waits longer than one sample */
  b_pause_lock();
  int gc = t->gc_paused;
  int paused = !gc && R_SUCCEEDED(svcSetThreadActivity(t->handle, ThreadActivity_Paused));
  KCtx ctx;
  if ((gc || paused) && R_SUCCEEDED(get_ctx(&ctx, t->handle))) {
    int st = gc ? ST_WAIT : state_of(&ctx);
    g_seen[ti].n[st]++;
    if (st != ST_WAIT) {
      Sample *s = &g_s[g_n];
      s->pc = (uint32_t)ctx.pc;
      s->thread = (uint8_t)ti;
      s->state = (uint8_t)st;
      s->n = 0;
      uint32_t sp = (uint32_t)ctx.r[13];
      uintptr_t lo = (uintptr_t)t->stack_base, hi = lo + t->stack_size;
      if ((uint32_t)ctx.r[14] && dcr_is_code_addr((uint32_t)ctx.r[14] & ~1u))
        s->ret[s->n++] = (uint32_t)ctx.r[14];
      uintptr_t end = (sp & ~3u) + dcr_readable(sp & ~3u, 0x4000); /* mapped only */
      if (lo && sp >= lo && sp < hi)
        for (uintptr_t a = sp & ~3u; a + 4 <= hi && a + 4 <= end && s->n < DEPTH; a += 4) {
          uint32_t v = *(const volatile uint32_t *)a;
          if (dcr_is_code_addr(v & ~1u))
            s->ret[s->n++] = v;
        }
      g_n++;
    }
  }
  if (paused)
    svcSetThreadActivity(t->handle, ThreadActivity_Runnable);
  b_pause_unlock();
}

static void sampler(void *arg) {
  for (;;) {
    __atomic_store_n(&g_busy, 1, __ATOMIC_SEQ_CST);
    uint64_t st = __atomic_load_n(&g_frame_start, __ATOMIC_SEQ_CST);
    int long_frame = st && (g_window || armTicksToNs(armGetSystemTick() - st) >= START_MS * 1000000ull);
    if (long_frame)
      b_thread_foreach(sample_one, NULL);
    __atomic_store_n(&g_busy, 0, __ATOMIC_SEQ_CST);
    svcSleepThread(g_window ? WINDOW_PERIOD_NS : long_frame ? PERIOD_NS : 5000000ll);
  }
}

void dcr_prof_start(void) {
  g_window = dcr_config()->profile_window;
  if (!dcr_config()->profile && !g_window)
    return;
  g_main_handle = envGetMainThreadHandle();
  g_s = malloc(sizeof(Sample) * MAX_SAMPLES);
  if (!g_s)
    return;
  if (R_SUCCEEDED(threadCreate(&g_thread, sampler, NULL, NULL, 0x4000, 0x2C, -2)))
    threadStart(&g_thread);
  if (g_window)
    debugPrintf("[prof] continuous: every thread is sampled every %lld ms, one report per %d s\n",
                WINDOW_PERIOD_NS / 1000000, WINDOW_MS / 1000);
  else
    debugPrintf("[prof] every thread is sampled every %lld ms in frames over %d ms; frames of %d ms "
                "or more are reported\n", PERIOD_NS / 1000000, START_MS, REPORT_MS);
}

void dcr_prof_frame_begin(void) {
  if (!g_s)
    return;
  if (g_window && g_window_start) { /* the window goes on across frames */
    __atomic_store_n(&g_frame_start, armGetSystemTick(), __ATOMIC_SEQ_CST);
    return;
  }
  g_window_start = armGetSystemTick();
  g_window_frames = 0;
  g_n = 0;
  g_nseen = 0;
  g_gc_at_start = dcr_gc_stopped_ns();
  __atomic_store_n(&g_frame_start, armGetSystemTick(), __ATOMIC_SEQ_CST);
}

/* ---- aggregation: small open-addressing tables ---- */
#define TAB 1024
typedef struct {
  uint32_t key, count;
} Ent;

static void add(Ent *t, uint32_t key) {
  for (uint32_t h = (key * 2654435761u) % TAB, k = 0; k < TAB; k++, h = (h + 1) % TAB) {
    if (t[h].count && t[h].key != key)
      continue;
    t[h].key = key;
    t[h].count++;
    return;
  }
}

typedef struct {
  char name[96];
  uint32_t incl, stamp;
} Named;
#define MAXNAMES 128
static Named g_names[MAXNAMES];
static int g_nnames;

static Named *named(const char *n) {
  for (int i = 0; i < g_nnames; i++)
    if (!strcmp(g_names[i].name, n))
      return &g_names[i];
  if (g_nnames == MAXNAMES)
    return NULL;
  Named *e = &g_names[g_nnames++];
  memset(e, 0, sizeof *e);
  snprintf(e->name, sizeof e->name, "%s", n);
  return e;
}

/* The method name for a JIT address, cached per 64-byte bucket. */
static const char *jit_name(uint32_t a) {
  static uint32_t keys[512];
  static char names[512][96];
  uint32_t k = a & ~63u, h = (k >> 6) % 512;
  if (keys[h] != k) {
    keys[h] = k;
    if (!dcr_mono_method_name(a, names[h], sizeof names[h]))
      snprintf(names[h], sizeof names[h], "JIT code");
  }
  return names[h];
}

static void top_line(const char *label, Ent *t, uint32_t total, int want) {
  char line[900];
  int n = snprintf(line, sizeof line, "[prof]     %s:", label);
  int any = 0;
  for (int k = 0; k < want; k++) {
    int best = -1;
    for (int i = 0; i < TAB; i++)
      if (t[i].count && (best < 0 || t[i].count > t[best].count))
        best = i;
    if (best < 0 || n > (int)sizeof line - 80)
      break;
    char a[64];
    n += snprintf(line + n, sizeof line - n, " %s %u%%", dcr_addr_name(t[best].key, a, sizeof a),
                  t[best].count * 100 / total);
    t[best].count = 0;
    any = 1;
  }
  if (any)
    debugPrintf("%s\n", line);
}

static void report_thread(int ti, uint32_t ticks) {
  static Ent self[TAB], incl[TAB];
  memset(self, 0, sizeof self);
  memset(incl, 0, sizeof incl);
  g_nnames = 0;
  const Seen *e = &g_seen[ti];
  uint32_t n = g_n;
  for (uint32_t i = 0; i < n; i++) {
    const Sample *s = &g_s[i];
    if (s->thread != ti)
      continue;
    if (!jit_contains((const void *)(uintptr_t)s->pc))
      add(self, s->pc & ~63u);
    for (int k = -1; k < s->n; k++) {
      uint32_t a = (k < 0 ? s->pc : s->ret[k]) & ~1u;
      if (jit_contains((const void *)(uintptr_t)a)) {
        Named *m = named(jit_name(a));
        if (m && m->stamp != i + 1) {
          m->stamp = i + 1;
          m->incl++;
        }
      } else if (k >= 0) {
        int dup = 0;
        for (int j = 0; j < k && !dup; j++)
          dup = (s->ret[j] & ~1u) == a;
        if (!dup)
          add(incl, a);
      }
    }
  }
  debugPrintf("[prof]   %s \"%s\" (tid %d): running %u%%, file/IPC %u%%, waiting %u%% of the %s\n",
              e->main ? "main thread" : "thread", e->main ? "UnityMain" : e->name, e->tid,
              e->n[ST_RUN] * 100 / ticks, e->n[ST_IO] * 100 / ticks, e->n[ST_WAIT] * 100 / ticks,
              g_window ? "window" : "frame");
  top_line("self", self, ticks, 12);
  top_line("incl", incl, ticks, 20);
  char line[1200];
  int len = snprintf(line, sizeof line, "[prof]     managed:");
  int any = 0;
  for (int k = 0; k < 10; k++) {
    int best = -1;
    for (int i = 0; i < g_nnames; i++)
      if (g_names[i].incl && (best < 0 || g_names[i].incl > g_names[best].incl))
        best = i;
    if (best < 0 || len > (int)sizeof line - 120)
      break;
    len += snprintf(line + len, sizeof line - len, " %s %u%%;", g_names[best].name,
                    g_names[best].incl * 100 / ticks);
    g_names[best].incl = 0;
    any = 1;
  }
  if (any)
    debugPrintf("%s\n", line);
}

void dcr_prof_frame_end(uint64_t frame) {
  if (!g_s)
    return;
  uint64_t st = g_frame_start;
  __atomic_store_n(&g_frame_start, 0, __ATOMIC_SEQ_CST);
  while (__atomic_load_n(&g_busy, __ATOMIC_SEQ_CST))
    svcSleepThread(100000ll);
  uint64_t ms = st ? armTicksToNs(armGetSystemTick() - st) / 1000000ull : 0;
  if (g_window) {
    g_window_frames++;
    ms = armTicksToNs(armGetSystemTick() - g_window_start) / 1000000ull;
    if (ms < WINDOW_MS && g_n < MAX_SAMPLES)
      return;
    g_window_start = 0; /* the next frame_begin starts a new window */
    debugPrintf("[prof] window: %llu frames in %llu ms (%.1f fps, %.1f ms a frame)%s\n",
                (unsigned long long)g_window_frames, (unsigned long long)ms,
                (double)g_window_frames * 1000.0 / (double)ms, (double)ms / (double)g_window_frames,
                g_n >= MAX_SAMPLES ? " -- sample buffer full" : "");
  } else if (ms < REPORT_MS || !g_nseen)
    return;
  if (!g_nseen)
    return;
  /* ticks: the most samples any one thread got */
  uint32_t ticks = 0;
  for (int i = 0; i < g_nseen; i++) {
    uint32_t t = g_seen[i].n[0] + g_seen[i].n[1] + g_seen[i].n[2];
    if (t > ticks)
      ticks = t;
  }
  if (ticks < 5)
    return;
  debugPrintf("[prof] frame %llu: %llu ms, %lu ticks, %d threads; world stopped for GC %llu ms\n",
              (unsigned long long)frame, (unsigned long long)ms, (unsigned long)ticks, g_nseen,
              (unsigned long long)((dcr_gc_stopped_ns() - g_gc_at_start) / 1000000ull));
  /* the busiest threads (running + file/IPC), and always the main thread */
  int shown[MAX_THREADS] = {0};
  for (int k = 0; k < SHOW_THREADS; k++) {
    int best = -1;
    for (int i = 0; i < g_nseen; i++) {
      uint32_t busy = g_seen[i].n[ST_RUN] + g_seen[i].n[ST_IO];
      if (!shown[i] && busy * 20 >= ticks &&
          (best < 0 || busy > g_seen[best].n[ST_RUN] + g_seen[best].n[ST_IO]))
        best = i;
    }
    if (best < 0)
      break;
    shown[best] = 1;
    report_thread(best, ticks);
  }
  for (int i = 0; i < g_nseen; i++)
    if (g_seen[i].main && !shown[i])
      debugPrintf("[prof]   main thread: running %u%%, file/IPC %u%%, waiting %u%% of the frame\n",
                  g_seen[i].n[ST_RUN] * 100 / ticks, g_seen[i].n[ST_IO] * 100 / ticks,
                  g_seen[i].n[ST_WAIT] * 100 / ticks);
}
