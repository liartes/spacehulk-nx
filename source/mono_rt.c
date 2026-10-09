/* mono_rt.c -- what Mono's JIT and garbage collector need from the OS, and do
 * not get from Horizon.
 *
 * 1. CACHE MAINTENANCE. mono_arch_flush_icache() issues the Linux cacheflush
 *    syscall inline (`svc #0`, r7 = 0xF0002). On Horizon `svc 0` is not a
 *    system call at all, so the function is hooked to jit_flush(), which cleans
 *    the D-cache and invalidates the I-cache over the freshly written code
 *    (through the arena's RW alias on hardware).
 *
 * 2. STOP-THE-WORLD. Mono's Boehm collector stops every other thread by
 *    tkill(tid, 30) and restarts them with tkill(tid, 24); each thread's signal
 *    handler records where its stack is, acknowledges on GC_suspend_ack_sem,
 *    and sleeps in sigsuspend until the restart signal. Horizon has no signals,
 *    so the handler's work is done FOR the thread, from the collector, exactly
 *    as the handler in this libmono does it (read out of GC_suspend_handler by
 *    tools/offsets/derive_offsets.py):
 *
 *      suspend (30):  if me->stop_info.last_stop_count == GC_stop_count: done
 *                     (a retry for a thread already stopped); otherwise pause
 *                     the thread (svcSetThreadActivity), read its registers
 *                     (svcGetThreadContext3), push r0-r12 and lr below its sp
 *                     so the conservative scan sees them, publish that as
 *                     me->stop_info.stack_ptr, set last_stop_count, post ack.
 *      restart (24):  me->stop_info.signal = 24, resume the thread, post ack
 *                     (GC_restart_all waits for one ack per thread).
 *
 *    Nothing in this path may log or allocate: another thread may be paused
 *    while holding the log's or the allocator's lock. Counters are reported
 *    later, from the main loop.
 * MIT.
 */
#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "bionic_pthread.h"
#include "dcr_offsets.h"
#include "dcr_patches.h"
#include "imports.h"
#include "jit_arena.h"
#include "so_util.h"
#include "util.h"

extern so_module mono_mod;

typedef void *(*fn_lookup)(void *pthread_id);
typedef int (*fn_sem_post)(void *sem);

static fn_lookup g_lookup;
static fn_sem_post g_sem_post;
static volatile uint32_t *g_stop_count;
static void *g_ack_sem;
static int g_gc_ready;

static volatile uint32_t g_n_suspend, g_n_restart, g_n_retry, g_n_ctx_fail, g_n_unknown;
uint32_t dcr_gc_suspends(void) { return g_n_suspend; }

/* svcGetThreadContext3 with the kernel's own 0x320-byte ThreadContext. For an
 * AArch32 thread the kernel fills r[0..14] (sp = r[13], lr = r[14]). */
typedef struct {
  uint64_t r[29];
  uint64_t fp, lr, sp, pc;
  uint32_t psr, _pad;
  uint8_t v[32][16];
  uint32_t fpcr, fpsr;
  uint64_t tpidr;
} KThreadContext;
_Static_assert(sizeof(KThreadContext) == 0x320, "kernel ThreadContext is 0x320 bytes");

static Result get_thread_context(KThreadContext *ctx, Handle h) {
  register uint32_t r0 __asm__("r0") = (uint32_t)(uintptr_t)ctx;
  register uint32_t r1 __asm__("r1") = h;
  /* The 32-bit SVC ABI returns with r1-r3 zeroed: they must be clobbers, or
   * the compiler keeps live values there (the watchdog's first report died on
   * a pointer it had parked in r3). */
  __asm__ volatile("svc 0x33" : "+r"(r0), "+r"(r1) : : "r2", "r3", "r12", "lr", "memory");
  return r0;
}

static uintptr_t mono_base(void) { return (uintptr_t)mono_mod.load_virtbase; }

int dcr_mono_install(void) {
  int ok = 1;
  /* 1. cache maintenance */
  if (!dcr_hook(&mono_mod, DCR_MONO_RVA_FlushIcache, DCR_MONO_GUARD_FlushIcache, (void *)jit_flush,
                "mono_arch_flush_icache")) {
    debugPrintf("[mono] WARNING: JIT'd code will hit `svc 0` on its first flush\n");
    ok = 0;
  }

  /* 2. GC bridge */
  if (dcr_guard_ok(&mono_mod, DCR_MONO_RVA_SuspendHandler, DCR_MONO_GUARD_SuspendHandler,
                   "GC_suspend_handler") &&
      dcr_guard_ok(&mono_mod, DCR_MONO_RVA_LookupThread, DCR_MONO_GUARD_LookupThread,
                   "GC_lookup_thread")) {
    g_lookup = (fn_lookup)(mono_base() + DCR_MONO_RVA_LookupThread);
    g_stop_count = (volatile uint32_t *)*(uint32_t *)(mono_base() + DCR_MONO_GOT_StopCount);
    g_ack_sem = (void *)*(uint32_t *)(mono_base() + DCR_MONO_GOT_AckSem);
    g_sem_post = (fn_sem_post)dcr_import_lookup("sem_post");
    g_gc_ready = g_sem_post && g_stop_count && g_ack_sem;
  }
  debugPrintf("[mono] GC bridge %s (stop_count %p, ack sem %p)\n",
              g_gc_ready ? "armed" : "NOT ARMED -- the first collection will hang",
              (void *)g_stop_count, g_ack_sem);
  return ok && g_gc_ready;
}

static int in_stack(const BThread *t, uintptr_t p) {
  uintptr_t lo = (uintptr_t)t->stack_base;
  return lo && p >= lo && p <= lo + t->stack_size;
}

/* Stop-the-world time: from the first thread a collection pauses to the last
 * it resumes (the collection is keyed by Boehm's stop_count). */
static uint32_t g_gc_cur = ~0u;
static uint64_t g_gc_t0, g_gc_t1, g_gc_ns, g_gc_max_ns;
static uint32_t g_gc_n;

static void gc_close(void) {
  if (g_gc_cur != ~0u && g_gc_t1 > g_gc_t0) {
    uint64_t ns = armTicksToNs(g_gc_t1 - g_gc_t0);
    g_gc_ns += ns;
    if (ns > g_gc_max_ns)
      g_gc_max_ns = ns;
    g_gc_n++;
  }
  g_gc_cur = ~0u;
}

/* Total stop-the-world time so far (dcr_prof.c), the running one included. */
uint64_t dcr_gc_stopped_ns(void) {
  uint64_t open = (g_gc_cur != ~0u && g_gc_t1 > g_gc_t0) ? armTicksToNs(g_gc_t1 - g_gc_t0) : 0;
  return g_gc_ns + open;
}

#define GCSTEP(lit) do { if (dcr_is_emulator()) svcOutputDebugString(lit, sizeof lit - 1); } while (0)
static void gc_suspend(BThread *t) {
  GCSTEP("[gc] suspend");
  uint8_t *me = g_lookup((void *)t);
  if (!me) {
    g_n_unknown++;
    return; /* not a GC thread: the collector does not wait for it */
  }
  uint32_t stop = *g_stop_count;
  if (*(volatile uint32_t *)(me + DCR_MONO_GCT_LAST_STOP) == stop) {
    g_n_retry++;
    return;
  }
  KThreadContext ctx;
  uintptr_t sp = 0;
  b_pause_lock();
  svcSetThreadActivity(t->handle, ThreadActivity_Paused);
  t->gc_paused = 1;
  if (R_SUCCEEDED(get_thread_context(&ctx, t->handle)))
    sp = (uintptr_t)ctx.r[13];
  b_pause_unlock();
  if (!in_stack(t, sp)) {
    /* Unknown sp: scan the whole stack. Conservative, never unsafe. */
    g_n_ctx_fail++;
    *(void **)(me + DCR_MONO_GCT_STACK_PTR) = t->stack_base;
  } else {
    uint32_t *save = (uint32_t *)((sp - 16 * 4) & ~7u);
    for (int i = 0; i < 13; i++)
      save[i] = (uint32_t)ctx.r[i];
    save[13] = (uint32_t)ctx.r[14];
    *(void **)(me + DCR_MONO_GCT_STACK_PTR) = save;
  }
  *(volatile uint32_t *)(me + DCR_MONO_GCT_LAST_STOP) = stop;
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  if (stop != g_gc_cur) {
    gc_close();
    g_gc_cur = stop;
    g_gc_t0 = g_gc_t1 = armGetSystemTick();
  }
  g_n_suspend++;
  g_sem_post(g_ack_sem);
  GCSTEP("[gc] suspended");
}

static void gc_restart(BThread *t) {
  GCSTEP("[gc] restart");
  uint8_t *me = g_lookup((void *)t);
  if (me)
    *(volatile uint32_t *)(me + DCR_MONO_GCT_SIGNAL) = DCR_MONO_GC_RESTART_SIG;
  b_pause_lock();
  if (t->gc_paused) {
    t->gc_paused = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    svcSetThreadActivity(t->handle, ThreadActivity_Runnable);
  }
  b_pause_unlock();
  g_gc_t1 = armGetSystemTick();
  g_n_restart++;
  g_sem_post(g_ack_sem);
}

/* Called by bionic_signal.c for every tkill/pthread_kill. */
int port_gc_signal(BThread *t, int sig) {
  if (!g_gc_ready || !t)
    return 0;
  if (sig == (int)DCR_MONO_GC_SUSPEND_SIG) {
    gc_suspend(t);
    return 1;
  }
  if (sig == (int)DCR_MONO_GC_RESTART_SIG) {
    gc_restart(t);
    return 1;
  }
  return 0;
}

/* ------------------------------------------------------ managed frames
 * Names for JIT code addresses, from Mono's own tables. A runtime assertion
 * (g_assert -> raise) reaches bionic_signal.c as a fatal signal with only
 * libmono and "JIT code" on the stack; this says which C# methods were
 * running. Calls into Mono (and its allocator): not for use while other
 * threads are paused. */
typedef void *(*fn_root_domain)(void);
typedef void *(*fn_jit_find)(void *domain, char *addr);
typedef void *(*fn_ji_method)(void *ji);
typedef char *(*fn_full_name)(void *method, int signature);
typedef void (*fn_mono_free)(void *p);

int dcr_mono_method_name(uint32_t addr, char *buf, size_t cap) {
  static fn_root_domain root;
  static fn_jit_find find;
  static fn_ji_method method_of;
  static fn_full_name full_name;
  static fn_mono_free mfree;
  static int tried;
  if (!tried) {
    tried = 1;
    root = (fn_root_domain)so_try_find_addr_rx(&mono_mod, "mono_get_root_domain");
    find = (fn_jit_find)so_try_find_addr_rx(&mono_mod, "mono_jit_info_table_find");
    method_of = (fn_ji_method)so_try_find_addr_rx(&mono_mod, "mono_jit_info_get_method");
    full_name = (fn_full_name)so_try_find_addr_rx(&mono_mod, "mono_method_full_name");
    mfree = (fn_mono_free)so_try_find_addr_rx(&mono_mod, "mono_free");
  }
  if (!root || !find || !method_of || !full_name || !jit_contains((const void *)(uintptr_t)addr))
    return 0;
  void *dom = root();
  void *ji = dom ? find(dom, (char *)(uintptr_t)addr) : NULL;
  void *m = ji ? method_of(ji) : NULL;
  char *n = m ? full_name(m, 1) : NULL;
  if (!n)
    return 0;
  snprintf(buf, cap, "%s", n);
  if (mfree)
    mfree(n);
  return 1;
}

void dcr_mono_log_managed_stack(const char *tag) {
  BThread *t = b_thread_self();
  uintptr_t sp = (uintptr_t)__builtin_frame_address(0);
  uintptr_t hi = (t && t->stack_base) ? (uintptr_t)t->stack_base + t->stack_size : sp + 0x4000;
  char name[256], last[256] = "";
  int n = 0;
  for (uintptr_t a = sp & ~3u; a + 4 <= hi && a < sp + 0x20000 && n < 16; a += 4) {
    uint32_t v = *(const volatile uint32_t *)a;
    if (!dcr_mono_method_name(v & ~1u, name, sizeof name) || !strcmp(name, last))
      continue;
    snprintf(last, sizeof last, "%s", name);
    debugPrintf("%s managed frame: %s\n", tag, name);
    n++;
  }
  if (!n)
    debugPrintf("%s no managed frames on this thread's stack\n", tag);
}

/* The runtime's signal delivery ends the process for a fatal signal (abort()
 * in the game ends there); first, what Mono was running, by name. */
void port_on_fatal_signal(int sig, BThread *t) {
  (void)sig, (void)t;
  dcr_mono_log_managed_stack("[fatal]");
}

/* The nearest `max` managed callers on this thread's stack, outside
 * UnityEngine, as "Type:Method < Type:Method" (signatures dropped), into out.
 * The number found. For diagnostics: a stale return address in a caller's
 * frame can show up as an extra name. */
int dcr_mono_callers(char *out, size_t cap, int max) {
  BThread *t = b_thread_self();
  uintptr_t sp = (uintptr_t)__builtin_frame_address(0);
  uintptr_t hi = (t && t->stack_base) ? (uintptr_t)t->stack_base + t->stack_size : sp + 0x4000;
  char name[256], last[256] = "";
  int n = 0;
  size_t o = 0;
  out[0] = 0;
  for (uintptr_t a = sp & ~3u; a + 4 <= hi && a < sp + 0x10000 && n < max; a += 4) {
    uint32_t v = *(const volatile uint32_t *)a;
    if (!dcr_mono_method_name(v & ~1u, name, sizeof name))
      continue;
    char *paren = strstr(name, " (");
    if (paren)
      *paren = 0;
    if (!strcmp(name, last) || !strncmp(name, "UnityEngine.", 12))
      continue;
    snprintf(last, sizeof last, "%s", name);
    int k = snprintf(out + o, cap - o, "%s%s", n ? " < " : "", name);
    if (k < 0 || (size_t)k >= cap - o)
      break;
    o += (size_t)k;
    n++;
  }
  return n;
}

/* ------------------------------------------------ managed exceptions
 * Unity's release player logs a script exception as its message alone
 * ("NullReferenceException: ..."), no stack. With explicit null checks the
 * JIT creates corlib exceptions through mono_exception_from_token while the
 * throwing method is still on the stack: an entry hook there logs the
 * managed callers of the first ones (and one in 500 after), which names the
 * script. The hook: the function's two-word prologue `push {fp, lr}; add
 * fp, sp, #4` moves into a trampoline in the JIT arena, the entry becomes
 * `ldr pc, [pc, #-4]` + the wrapper's address. */
typedef void *(*fn_ex_from_token)(void *image, uint32_t token);
typedef void *(*fn_ex_null)(void);
static fn_ex_from_token g_ex_orig;
static fn_ex_null g_exnull_orig;
static volatile uint32_t g_ex_count;

static void ex_log(const char *what, uint32_t token) {
  uint32_t n = __atomic_add_fetch(&g_ex_count, 1, __ATOMIC_RELAXED);
  if (n <= 12 || n % 500 == 0) {
    char callers[600];
    if (dcr_mono_callers(callers, sizeof callers, 6) == 0)
      snprintf(callers, sizeof callers, "(no managed frames)");
    debugPrintf("[exc] #%lu %s 0x%lx thrown in: %s\n", (unsigned long)n, what, (unsigned long)token, callers);
  }
}
static void *ex_from_token(void *image, uint32_t token) {
  ex_log("corlib exception token", token);
  return g_ex_orig(image, token);
}
static void *ex_null(void) {
  ex_log("NullReferenceException", 0);
  return g_exnull_orig();
}

/* Entry hook: the function's two-word prologue -- `push {..., fp, lr}; add
 * fp, sp, #n`, position independent -- moves into a trampoline in the JIT
 * arena, and the entry becomes `ldr pc, [pc, #-4]` + the wrapper's address.
 * Returns the trampoline (the original function), or NULL. */
static void *hook_entry(const char *sym, void *wrapper) {
  uint32_t *fn = (uint32_t *)so_try_find_addr_rx(&mono_mod, sym);
  if (!fn || (fn[0] & 0xFFFF4800u) != 0xE92D4800u || (fn[1] & 0xFFFFFF00u) != 0xE28DB000u) {
    debugPrintf("[exc] %s is not as expected: not traced\n", sym);
    return NULL;
  }
  uint32_t *tr = jit_alloc(16);
  if (!tr)
    return NULL;
  uint32_t *w = jit_rw(tr);
  w[0] = fn[0];
  w[1] = fn[1];
  w[2] = 0xE51FF004u; /* ldr pc, [pc, #-4] */
  w[3] = (uint32_t)(uintptr_t)(fn + 2);
  jit_flush(tr, 16);
  const uint32_t entry[2] = {0xE51FF004u, (uint32_t)(uintptr_t)wrapper};
  if (so_patch_code(fn, entry, sizeof entry) != 0)
    return NULL;
  return tr;
}

void dcr_mono_hook_exceptions(void) {
  g_ex_orig = (fn_ex_from_token)hook_entry("mono_exception_from_token", (void *)ex_from_token);
  g_exnull_orig = (fn_ex_null)hook_entry("mono_get_exception_null_reference", (void *)ex_null);
  debugPrintf("[exc] tracing managed exceptions:%s%s\n", g_ex_orig ? " corlib-by-token" : "",
              g_exnull_orig ? " null-reference" : "");
}

void dcr_mono_report(void) {
  if (!g_n_suspend && !g_n_unknown)
    return;
  /* the managed heap: how big Boehm made it, how much of it is in use */
  static int64_t (*heap_size)(void), (*used_size)(void);
  static int looked;
  if (!looked) {
    looked = 1;
    heap_size = (int64_t(*)(void))so_try_find_addr_rx(&mono_mod, "mono_gc_get_heap_size");
    used_size = (int64_t(*)(void))so_try_find_addr_rx(&mono_mod, "mono_gc_get_used_size");
  }
  debugPrintf("[mono] GC bridge: %lu suspends, %lu restarts, %lu retries, %lu full-stack scans, "
              "%lu non-GC threads; %lu collections stopped the world %llu ms in all (longest %llu ms); "
              "managed heap %lld MB, %lld MB in use\n",
              (unsigned long)g_n_suspend, (unsigned long)g_n_restart, (unsigned long)g_n_retry,
              (unsigned long)g_n_ctx_fail, (unsigned long)g_n_unknown, (unsigned long)g_gc_n,
              (unsigned long long)(g_gc_ns / 1000000ull), (unsigned long long)(g_gc_max_ns / 1000000ull),
              heap_size ? (long long)(heap_size() >> 20) : -1LL, used_size ? (long long)(used_size() >> 20) : -1LL);
}
