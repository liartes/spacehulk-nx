/* sh_egl.c -- eglMakeCurrent calls that change nothing, skipped.
 *
 * The profile of a mission with threaded rendering (hardware 2026-10-04,
 * profile_continuous, build 202610041510) had Unity's render thread
 * ("UnityGfxDeviceW", the frame-rate limit at 82-90% busy) with
 * b_eglMakeCurrent in 55-60% of its stacks, under it Mesa's
 * st_framebuffer_reuse_or_create -> _mesa_reference_framebuffer_ and its
 * locks, while the draws themselves (_mesa_DrawElements, st_draw_vbo,
 * nvc0_state_validate) were 25-30%. EGL lets a make-current of what is
 * already current be a no-op; Mesa still does the whole bind.
 *
 * So a call whose display, draw and read surfaces and context are the
 * calling thread's current ones (eglGetCurrent*: thread-local in Mesa)
 * returns EGL_TRUE without reaching it. Every 10 s with calls in it the log
 * gets "[egl] eglMakeCurrent: N calls, M skipped" -- how often the engine
 * asks, and how much of it was redundant. [graphics]
 * skip_redundant_makecurrent turns the skipping off (the count stays).
 * MIT.
 */
#include <EGL/egl.h>
#include <switch.h>

#include "dcr_config.h"
#include "util.h"

EGLBoolean b_eglMakeCurrent(EGLDisplay d, EGLSurface dr, EGLSurface rd, EGLContext c); /* gl_mesa.c */

static volatile uint32_t g_calls, g_skipped;
static uint64_t g_last;

static void report(void) {
  uint64_t now = armGetSystemTick();
  if (!g_last) {
    g_last = now;
    return;
  }
  uint64_t ns = armTicksToNs(now - g_last);
  if (ns < 10000000000ull)
    return;
  g_last = now;
  uint32_t calls = g_calls, skipped = g_skipped;
  g_calls = g_skipped = 0;
  debugPrintf("[egl] eglMakeCurrent: %lu calls in %llu s (%lu a second), %lu skipped as already current\n",
              (unsigned long)calls, (unsigned long long)(ns / 1000000000ull),
              (unsigned long)(calls * 1000000000ull / ns), (unsigned long)skipped);
}

/* The engine asks for a swap interval of 1; [display] frame_rate 30 makes it
 * 2: Mesa's Switch EGL hands it to nwindowSetSwapInterval, the swap waits for
 * every other refresh, and the threads sleep the rest of the time. */
EGLBoolean b_eglSwapInterval(EGLDisplay d, EGLint i); /* gl_mesa.c */
EGLBoolean sh_eglSwapInterval(EGLDisplay d, EGLint i) {
  const int want = dcr_config()->swap_interval;
  if (i > 0 && i < want)
    i = want;
  return b_eglSwapInterval(d, i);
}

EGLBoolean sh_eglMakeCurrent(EGLDisplay d, EGLSurface dr, EGLSurface rd, EGLContext c) {
  g_calls++;
  report();
  if (dcr_config()->skip_makecurrent && c == eglGetCurrentContext() && d == eglGetCurrentDisplay() &&
      dr == eglGetCurrentSurface(EGL_DRAW) && rd == eglGetCurrentSurface(EGL_READ)) {
    g_skipped++;
    return EGL_TRUE;
  }
  return b_eglMakeCurrent(d, dr, rd, c);
}
