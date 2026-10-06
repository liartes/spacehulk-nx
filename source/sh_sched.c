/* sh_sched.c -- the render thread on a core of its own.
 *
 * With threaded rendering (sh_threaded.c) a mission is two busy threads: the
 * main thread (60-69% of a core) and UnityGfxDeviceWorker (82-93%, the
 * frame-rate limit), while the rest -- Unity's two job workers, its loading
 * threads, Mono's -- use little (hardware 2026-10-04, profile_continuous).
 * dcr_sched.c lets every guest thread run on cores 0-2 (priority 59, time-
 * sliced every 10 ms), so the render thread can find another thread on its
 * core and wait out a slice.
 *
 * [performance] pin_threads: the render thread gets core 1 to itself; the
 * main thread runs on core 0; every other guest thread on cores 0 and 2.
 * Threads appear as the game goes, so the placement is applied again once a
 * second from the frame loop (a thread placed once is not touched again).
 * MIT.
 */
#include <string.h>
#include <switch.h>

#include "bionic_pthread.h"
#include "dcr_config.h"
#include "dcr_sched.h"
#include "util.h"

#define CORE_MAIN 0x1ull
#define CORE_RENDER 0x2ull
#define CORE_OTHERS 0x5ull /* cores 0 and 2 */
#define MAX_PLACED 128

static int g_placed[MAX_PLACED];
static int g_nplaced;
static Handle g_main;

static int placed(int tid) {
  for (int i = 0; i < g_nplaced; i++)
    if (g_placed[i] == tid)
      return 1;
  return 0;
}

static void place(BThread *t, void *arg) {
  if (t->handle == INVALID_HANDLE || t->finished || placed(t->tid) || g_nplaced == MAX_PLACED)
    return;
  int render = !strncmp(t->name, "UnityGfxDevice", 14);
  int main = t->handle == g_main;
  u64 mask = render ? CORE_RENDER : main ? CORE_MAIN : CORE_OTHERS;
  s32 ideal = render ? 1 : main ? 0 : 2;
  Result rc = dcr_thread_set_cores(t->handle, ideal, mask);
  g_placed[g_nplaced++] = t->tid;
  if (render || main || R_FAILED(rc))
    debugPrintf("[sched] \"%s\" (tid %d): cores 0x%llx%s\n", main ? "UnityMain" : t->name, t->tid,
                (unsigned long long)mask, R_FAILED(rc) ? " FAILED" : "");
}

/* dcr_boot.c, every frame; works once a second */
void sh_sched_frame(uint64_t frame) {
  if (!dcr_config()->pin_threads || frame % 60 != 1)
    return;
  if (!g_main)
    g_main = envGetMainThreadHandle();
  b_thread_foreach(place, NULL);
}
