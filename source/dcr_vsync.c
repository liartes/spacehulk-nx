/* dcr_vsync.c -- the frame pulse Unity 2017.4's Android player waits for.
 *
 * WaitVSync(target) (libunity+DCR_RVA_WaitVSync) is:
 *     lock(&mutex); while (counter < target) cond_wait(&cond, &mutex); unlock
 * On a phone a Choreographer FrameCallback bumps `counter` every display
 * refresh. Choreographer is patched off (dcr_patches.c), so without this pump
 * the first frame would park in WaitVSync for ever. The game asks for
 * targetFrameRate 60 with vSyncCount 0, so this pulse is ALL of its pacing:
 * TimeManager::Sync waits for last + 1 each frame.
 *
 * PUMP THE COUNTER, DO NOT PATCH THE WAIT AWAY: the engine keeps its own
 * pacing logic and just gets the pulse, from a dedicated thread (the render
 * call is what waits, so the pulse cannot come from the thread that makes it).
 * The pulse is the DISPLAY's vsync (vi's vsync event on the default display),
 * so each frame starts just after a refresh and is shown at the next one; a
 * free-running 60 Hz timer drifts against the panel, and a frame could then
 * start anywhere in the refresh and wait in the swap queue. The timer remains
 * as the fallback, and as the 10 Hz trickle while the applet is suspended (the
 * engine may be inside WaitVSync when focus goes; a counter that stops dead
 * there never lets it out again).
 *
 * INPUT AT THE START OF THE FRAME: nativeRender runs InputProcess() -- which
 * drains the events dcr_input.c injected -- and only then PlayerLoop(), whose
 * first step (TimeManager::Sync) sleeps in WaitVSync. So input polled before
 * nativeRender was up to a whole refresh old by the time scripts read it. The
 * frame loop now calls dcr_vsync_frame_start() first: it waits for the very
 * tick WaitVSync is about to wait for (the engine's last one + 1, recorded by
 * the WaitVSync replacement below), THEN input is polled and nativeRender
 * called, and the engine's own wait finds its tick already there.
 *
 * The mutex and cond are bionic objects -- one word each -- in libunity's .bss,
 * locked by the engine through the pthread shims. This file locks them through
 * the SAME shims (looked up in the import table): two lock implementations on
 * one word would be two locks. MIT.
 */
#include <stdint.h>
#include <switch.h>

#include "config.h"
#include "dcr_offsets.h"
#include "dcr_patches.h"
#include "imports.h"
#include "util.h"

extern so_module unity_mod;
void dcr_boost_poll(void); /* dcr_boost.c */

#define PERIOD_NS 16666667ull   /* 60 Hz */

typedef int (*pt_fn)(void *);
typedef int (*pt_wait_fn)(void *, void *);
static pt_fn s_lock, s_unlock, s_bcast;
static pt_wait_fn s_wait;
static void *s_mutex, *s_cond;
static volatile int32_t *s_counter;
static int s_ok, s_hooked;
static Thread s_thread;
static volatile int s_run, s_paused;
static uint64_t s_ticks, s_display_ticks, s_timeouts;

/* the counter as the engine last saw it on leaving WaitVSync */
static volatile int32_t s_engine_seen;
static volatile uint32_t s_engine_waits;
static uint64_t s_prewait_ns, s_prewaits;

ViDisplay *dcr_vi_display(void); /* nx_init.c */
static Event s_vsync_ev;
static int s_have_ev;

static uint64_t now_ns(void) { return armTicksToNs(armGetSystemTick()); }

/* The engine's WaitVSync, same wait on the same objects, plus a note of the
 * counter it leaves with (what TimeManager::Sync stores as "last" next). */
static __attribute__((unused)) void w_wait_vsync(int target) {
  s_lock(s_mutex);
  while (*s_counter < target)
    s_wait(s_cond, s_mutex);
  s_engine_seen = *s_counter;
  s_unlock(s_mutex);
  s_engine_waits++;
}

/* The vsync event of the display the default window is on: the one open
 * handle to it (nx_init.c -- a second OpenDisplay is refused). */
static __attribute__((unused)) void open_vsync_event(void) {
  ViDisplay *d = dcr_vi_display();
  Result rc = d ? viGetDisplayVsyncEvent(d, &s_vsync_ev) : MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
  s_have_ev = R_SUCCEEDED(rc);
  if (!s_have_ev) {
    debugPrintf("[vsync] no display vsync event (0x%x): pulsing from a 60 Hz timer\n", (unsigned)rc);
    return;
  }
  /* A few refreshes, timed: the display's period, and proof the event fires. */
  eventWait(&s_vsync_ev, 50000000ull);
  eventClear(&s_vsync_ev);
  uint64_t t0 = now_ns();
  int n = 0;
  for (; n < 6; n++) {
    if (R_FAILED(eventWait(&s_vsync_ev, 50000000ull)))
      break;
    eventClear(&s_vsync_ev);
  }
  uint64_t dt = now_ns() - t0;
  if (n < 6) {
    debugPrintf("[vsync] the display vsync event did not fire (%d of 6): pulsing from a 60 Hz timer\n", n);
    eventClose(&s_vsync_ev);
    s_have_ev = 0;
    return;
  }
  debugPrintf("[vsync] display vsync event: %.2f ms per refresh (%.2f Hz)\n", (double)dt / 6e6,
              6e9 / (double)dt);
}

int dcr_vsync_init(void) {
#if !DCR_PATCH_VSYNC
  return -1;
#else
  if (!dcr_guard_ok(&unity_mod, DCR_RVA_WaitVSync, DCR_GUARD_WaitVSync, "WaitVSync")) {
    debugPrintf("[vsync] NOT ARMED: the first frame will park in WaitVSync\n");
    return -1;
  }
  s_lock = (pt_fn)dcr_import_lookup("pthread_mutex_lock");
  s_unlock = (pt_fn)dcr_import_lookup("pthread_mutex_unlock");
  s_bcast = (pt_fn)dcr_import_lookup("pthread_cond_broadcast");
  s_wait = (pt_wait_fn)dcr_import_lookup("pthread_cond_wait");
  if (!s_lock || !s_unlock || !s_bcast)
    return -1;
  uintptr_t b = (uintptr_t)unity_mod.load_virtbase;
  s_mutex = (void *)(b + DCR_VSYNC_MUTEX);
  s_cond = (void *)(b + DCR_VSYNC_COND);
  s_counter = (volatile int32_t *)(b + DCR_VSYNC_COUNTER);
  s_ok = 1;
  if (s_wait)
    s_hooked = dcr_hook(&unity_mod, DCR_RVA_WaitVSync, DCR_GUARD_WaitVSync, (void *)w_wait_vsync,
                        "WaitVSync");
  debugPrintf("[vsync] armed: mutex +0x%x cond +0x%x counter +0x%x; WaitVSync %s\n", DCR_VSYNC_MUTEX,
              DCR_VSYNC_COND, DCR_VSYNC_COUNTER,
              s_hooked ? "replaced (input is read at the start of each frame)" : "left alone");
  open_vsync_event();
  return 0;
#endif
}

static void tick(void) {
  s_lock(s_mutex);
  (*s_counter)++;
  s_bcast(s_cond);
  s_unlock(s_mutex);
  s_ticks++;
  dcr_boost_poll();
}

static void pump_main(void *arg) {
  uint64_t next = now_ns() + PERIOD_NS;
  while (s_run) {
    if (s_paused) {
      svcSleepThread(100000000ll);
      tick();
      next = now_ns() + PERIOD_NS;
      continue;
    }
    if (s_have_ev) {
      /* One pulse per refresh. A display that stops sending them (off,
       * reconfiguring) still gets 20 pulses a second. */
      Result rc = eventWait(&s_vsync_ev, 50000000ull);
      eventClear(&s_vsync_ev);
      if (R_SUCCEEDED(rc))
        s_display_ticks++;
      else
        s_timeouts++;
      tick();
      continue;
    }
    uint64_t t = now_ns();
    if (t < next) {
      svcSleepThread((s64)(next - t));
      continue;
    }
    tick();
    next += PERIOD_NS;
    /* Never catch up in a burst; re-base when far behind. */
    if (now_ns() > next + 8 * PERIOD_NS)
      next = now_ns() + PERIOD_NS;
  }
}

int dcr_vsync_start(void) {
  if (!s_ok)
    return -1;
  s_run = 1;
  /* Above the engine's threads (priority 59, dcr_sched.c): a late pulse is a
   * dropped frame. */
  Result rc = threadCreate(&s_thread, pump_main, NULL, NULL, 0x8000, 0x2B, -2);
  if (R_SUCCEEDED(rc))
    rc = threadStart(&s_thread);
  if (R_FAILED(rc)) {
    s_run = 0;
    debugPrintf("[vsync] pump thread failed: 0x%x\n", (unsigned)rc);
    return -1;
  }
  debugPrintf("[vsync] pump started (%s)\n", s_have_ev ? "display vsync" : "60 Hz timer");
  return 0;
}

/* Frame loop, before input: wait for the tick the engine's WaitVSync will
 * want (its last + 1). No-op until the engine has waited once, while
 * suspended, or without the WaitVSync replacement. */
void dcr_vsync_frame_start(void) {
  if (!s_hooked || !s_engine_waits || s_paused)
    return;
  int32_t want = s_engine_seen + 1;
  uint64_t t0 = now_ns();
  s_lock(s_mutex);
  while (*s_counter < want)
    s_wait(s_cond, s_mutex);
  s_unlock(s_mutex);
  s_prewait_ns += now_ns() - t0;
  s_prewaits++;
}

/* For the periodic frame log. */
void dcr_vsync_report(void) {
  debugPrintf("[vsync] %llu pulses (%llu display vsyncs, %llu timeouts); frames waited %.1f ms on "
              "average for their vsync before reading input\n",
              (unsigned long long)s_ticks, (unsigned long long)s_display_ticks,
              (unsigned long long)s_timeouts,
              s_prewaits ? (double)s_prewait_ns / (double)s_prewaits / 1e6 : 0.0);
  s_prewait_ns = s_prewaits = 0;
}

void dcr_vsync_set_paused(int paused) { s_paused = paused ? 1 : 0; }

void dcr_vsync_stop(void) {
  if (!s_run)
    return;
  s_run = 0;
  threadWaitForExit(&s_thread);
  threadClose(&s_thread);
  if (s_have_ev) {
    eventClose(&s_vsync_ev);
    s_have_ev = 0;
  }
  debugPrintf("[vsync] pump stopped after %llu ticks\n", (unsigned long long)s_ticks);
}
