/* dcr_time_hooks.c -- the time offsets: 10-11 Time icall bindings + 4
 * TimeManager entry points (libunity RVAs in dcr_offsets.h).
 *
 * The engine clock in 2017.4 is GetTimeSinceStartup -> clock_gettime(
 * CLOCK_MONOTONIC), and bionic_time.c already answers that with a clock that
 * does not count time spent suspended. So the engine is not broken while
 * awake, and would not jump after a sleep either. The hooks exist for the
 * managed side's consistency and for the clamp:
 *
 *   - one per-frame sample (dcr_time_tick) feeds every Time.* getter, so C#
 *     sees a single coherent clock within a frame;
 *   - deltaTime is clamped to 100 ms, so a synchronous scene load (seconds of
 *     one frame) is a hitch, not a teleport of every moving object;
 *   - the two SETTERS are forwarded to TimeManager::SetTimeScale /
 *     SetFixedDeltaTime, so Time.timeScale = 0 from a pause menu stops
 *     Animator, particles and physics too, not just what C# reads;
 *   - fixedDeltaTime / maximumParticleDeltaTime are read out of TimeManager,
 *     because physics and particles use the field, and the getter must agree.
 *
 * These are libunity icall BINDINGS called by Mono-JIT'd code with the plain
 * AAPCS softfp convention (float in r0, as the stock `vmov r0, s0` shows), no
 * hidden argument. The host is built softfp, so a C float function matches.
 *
 * Hooks are all-or-nothing per the guards: a mixed clock (some getters ours,
 * some the engine's) is worse than none, so if any guard fails, none of the
 * icall hooks are installed. MIT.
 */
#include <stdint.h>
#include <string.h>
#include <switch.h>

#include "config.h"
#include "dcr_offsets.h"
#include "dcr_patches.h"
#include "dcr_time.h"
#include "util.h"

extern so_module unity_mod;

#define FIXED_DELTA (1.0f / 60.0f)
#define MIN_DELTA 0.001f
#define MAX_DELTA 0.1f

static uint64_t g_last_ns;
static double g_scaled_s, g_unscaled_s, g_realtime_origin;
static float g_delta = FIXED_DELTA;
static float g_time_scale = 1.0f;
static float g_fixed_delta = FIXED_DELTA;
static uint32_t g_frames;
static int g_ready;

typedef void *(*fn_gtm)(void);
typedef double (*fn_rt)(void *tm);
typedef void (*fn_setf)(void *tm, float v);
static fn_gtm g_gtm;
static fn_rt g_rt;
static fn_setf g_set_timescale, g_set_fixeddelta;

static void *tm(void) { return g_gtm ? g_gtm() : NULL; }
static double mono_s(void) { return (double)dcr_monotonic_ns() * 1e-9; }

void dcr_time_tick(void) {
  if (!g_ready)
    return;
  uint64_t t = dcr_monotonic_ns();
  float d = (float)((double)(t - g_last_ns) * 1e-9);
  g_last_ns = t;
  if (d < MIN_DELTA) d = MIN_DELTA;
  if (d > MAX_DELTA) d = MAX_DELTA;
  g_delta = d;
  g_unscaled_s += (double)d;
  g_scaled_s += (double)d * (double)g_time_scale;
  g_frames++;
  if ((g_frames % 600u) == 0u) {
    void *m = tm();
    double eng = (m && g_rt) ? g_rt(m) : -1.0;
    debugPrintf("[time] frame %lu t=%.2fs real=%.2fs dt=%.2fms scale=%.2f engine=%.2fs\n",
                (unsigned long)g_frames, g_scaled_s, g_unscaled_s, (double)g_delta * 1000.0,
                (double)g_time_scale, eng);
  }
}

/* ---------------------------------------------------------------- hooks */
static float h_time(void) { return (float)g_scaled_s; }
static float h_deltaTime(void) { return g_delta * g_time_scale; }
static float h_unscaledTime(void) { return (float)g_unscaled_s; }
static float h_unscaledDeltaTime(void) { return g_delta; }
static float h_timeScale(void) { return g_time_scale; }
static int h_frameCount(void) { return (int)g_frames; }
static float h_realtimeSinceStartup(void) { return (float)(mono_s() - g_realtime_origin); }
static float h_fixedDeltaTime(void) {
  void *m = tm();
  return m ? *(const float *)((const char *)m + DCR_TM_FIELD_FIXEDDELTA) : g_fixed_delta;
}
#ifdef DCR_TM_FIELD_MAXPARTICLEDELTA /* Unity 5.5+ */
static float h_maximumParticleDeltaTime(void) {
  void *m = tm();
  return m ? *(const float *)((const char *)m + DCR_TM_FIELD_MAXPARTICLEDELTA) : 0.03f;
}
#endif
static void h_set_timeScale(float v) {
  if (v < 0.0f)
    v = 0.0f;
  g_time_scale = v;
  void *m = tm();
  if (m && g_set_timescale)
    g_set_timescale(m, v);
}
static void h_set_fixedDeltaTime(float v) {
  if (v > 0.0f)
    g_fixed_delta = v;
  void *m = tm();
  if (m && g_set_fixeddelta)
    g_set_fixeddelta(m, v);
}

static const struct { const char *name; void *fn; } k_hooks[] = {
    {"get_time", (void *)h_time},
    {"get_deltaTime", (void *)h_deltaTime},
    {"get_unscaledTime", (void *)h_unscaledTime},
    {"get_unscaledDeltaTime", (void *)h_unscaledDeltaTime},
    {"get_fixedDeltaTime", (void *)h_fixedDeltaTime},
    {"set_fixedDeltaTime", (void *)h_set_fixedDeltaTime},
#ifdef DCR_TM_FIELD_MAXPARTICLEDELTA
    {"get_maximumParticleDeltaTime", (void *)h_maximumParticleDeltaTime},
#endif
    {"get_timeScale", (void *)h_timeScale},
    {"set_timeScale", (void *)h_set_timeScale},
    {"get_frameCount", (void *)h_frameCount},
    {"get_realtimeSinceStartup", (void *)h_realtimeSinceStartup},
};

int dcr_time_install(void) {
  uintptr_t b = (uintptr_t)unity_mod.load_virtbase;
  int engine = 0;
  if (dcr_guard_ok(&unity_mod, DCR_RVA_GetTimeManager, DCR_GUARD_GetTimeManager, "GetTimeManager"))
    g_gtm = (fn_gtm)(b + DCR_RVA_GetTimeManager), engine++;
  if (dcr_guard_ok(&unity_mod, DCR_RVA_GetRealtimeSinceStartup, DCR_GUARD_GetRealtimeSinceStartup,
                   "TimeManager::GetRealtimeSinceStartup"))
    g_rt = (fn_rt)(b + DCR_RVA_GetRealtimeSinceStartup), engine++;
  if (dcr_guard_ok(&unity_mod, DCR_RVA_SetTimeScale, DCR_GUARD_SetTimeScale, "TimeManager::SetTimeScale"))
    g_set_timescale = (fn_setf)(b + DCR_RVA_SetTimeScale), engine++;
  if (dcr_guard_ok(&unity_mod, DCR_RVA_SetFixedDeltaTime, DCR_GUARD_SetFixedDeltaTime,
                   "TimeManager::SetFixedDeltaTime"))
    g_set_fixeddelta = (fn_setf)(b + DCR_RVA_SetFixedDeltaTime), engine++;
  debugPrintf("[time] %d/4 TimeManager entry points bound\n", engine);

#if !DCR_TIME_ICALL_HOOKS
  return engine;
#else
  /* Every guard first; install only if every one matches. */
  const int N = DCR_NSITES(DCR_TIME_SITES);
  for (int i = 0; i < N; i++)
    if (!dcr_guard_ok(&unity_mod, DCR_TIME_SITES[i].rva, DCR_TIME_SITES[i].guard, DCR_TIME_SITES[i].name)) {
      debugPrintf("[time] icall hooks NOT installed (a mixed clock is worse than none)\n");
      return engine;
    }
  g_last_ns = dcr_monotonic_ns();
  g_realtime_origin = mono_s();
  g_ready = 1;
  int n = 0;
  for (int i = 0; i < N; i++)
    for (unsigned k = 0; k < sizeof k_hooks / sizeof k_hooks[0]; k++)
      if (!strcmp(k_hooks[k].name, DCR_TIME_SITES[i].name)) {
        hook_arm(b + DCR_TIME_SITES[i].rva, (uintptr_t)k_hooks[k].fn);
        n++;
      }
  debugPrintf("[time] %d/%d Time icalls hooked; %d time offsets live\n", n, N, n + engine);
  return n + engine;
#endif
}
