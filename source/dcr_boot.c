/* dcr_boot.c -- plays the part of the APK's Java side, for Unity 5.6.4f1
 * (Disney Crossy Road); Space Hulk's Unity 5.3.4f1 UnityPlayer runs the same
 * sequence, plus nativeFile(<the OBB>) after nativeFile(<the APK>).
 *
 * The order is the APK's own (UnityPlayer.smali of Disney Crossy Road: SEA):
 *
 *   UnityPlayer.<clinit>     System.loadLibrary("main")   -> libmain JNI_OnLoad
 *   UnityPlayer.<init>       NativeLoader.load(nativeLibraryDir)
 *                              libmain: dlopen("<dir>/libunity.so") and call
 *                              its JNI_OnLoad, which RegisterNatives the
 *                              UnityPlayer natives (captured by jni_core.c)
 *                            initJni(context)
 *                            nativeFile(getPackageCodePath())  -- 5.6: the
 *                              APK the engine reads its data from (2017.4
 *                              asks for it itself); OBB files would follow,
 *                              this game has none (settings.xml useObb off)
 *                            nativeInitWWW(WWW.class)       -- 5.6 only: the
 *                              Java WWW downloader (jni_www.c), which is also
 *                              how the engine reads jar:file:// URLs
 *                            nativeInitWebRequest(UnityWebRequest.class)
 *   surfaceChanged           nativeRecreateGfxState(0, surface)
 *   onResume / focus         nativeResume(), nativeFocusChanged(true)
 *   UnityMain thread         nativeRender() while it returns true
 *
 * The 5.6 natives, as registered (the same as 2017.4's for everything the
 * lifecycle uses): initJni(Landroid/content/Context;)V -- three arguments
 * with env and thiz; nativeFile(Ljava/lang/String;)V;
 * nativeInitWWW(Ljava/lang/Class;)V; nativeRecreateGfxState(ILandroid/view/Surface;)V;
 * nativeRender()Z; nativeResume()V; nativePause()Z; nativeFocusChanged(Z)V;
 * nativeDone()V. There is no nativeSendSurfaceChangedEvent in this version.
 *
 * Applet lifecycle: on losing focus (HOME, sleep) the engine is paused, the
 * port's clock stops (bionic_time) and the vsync pump drops to a trickle; on
 * regaining focus that reverses. MIT.
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <switch.h>

#include "config.h"
#include "dcr_config.h"
#include "dcr_input.h"
#include "dcr_path.h"
#include "dcr_time.h"
#include "error.h"
#include "dcr_boost.h"
#include "dcr_jni_unity.h"
#include "gl_layer.h"
#include "rt_applet.h"
#include "so_util.h"
#include "util.h"
#include "watchdog.h"

extern so_module main_mod, unity_mod, mono_mod;
extern volatile int g_dcr_quit_requested;

int dcr_vsync_start(void);
void dcr_vsync_stop(void);
void dcr_vsync_set_paused(int paused);
void dcr_vsync_frame_start(void);
void dcr_vsync_report(void);
void dcr_boost_frame_begin(void);
void dcr_boost_frame_end(uint64_t frame);
void dcr_boost_report(void);
void dcr_boost_first_picture(void);
void dcr_boost_launch_tick(void);
void dcr_boost_cpu_report(void);
void dcr_prof_start(void);
void dcr_apkcache_report(void);
void dcr_prof_frame_begin(void);
void dcr_prof_frame_end(uint64_t frame);
void dcr_time_tick(void);
void dcr_mono_report(void);

#define C_PLAYER "com/unity3d/player/UnityPlayer"
#define C_ACTIVITY "com/unity3d/player/UnityPlayerActivity"
#define LIB_DIR "/data/app/" DCR_PKG_NAME "-1/lib/arm"

typedef jint (*fn_onload)(void *vm, void *reserved);
typedef jboolean (*fn_nl_load)(void *env, void *clazz, void *path);
typedef void (*fn_ctx)(void *env, void *thiz, void *obj);
typedef void (*fn_recreate)(void *env, void *thiz, jint flag, void *surface);
typedef void (*fn_v)(void *env, void *thiz);
typedef jboolean (*fn_z)(void *env, void *thiz);
typedef void (*fn_vz)(void *env, void *thiz, jboolean b);

static struct {
  fn_ctx initJni, initWebRequest, initWWW, file;
  fn_recreate recreateGfxState;
  fn_z render, pause;
  fn_v resume, done, lowMemory;
  fn_vz focusChanged;
} U;

static void *g_thiz, *g_ctx, *g_surface;
static volatile uint64_t g_frames_done;

/* For the watchdog (the runtime's watchdog.c): frames completed. Whether a
 * pause is expected is the runtime's (rt_applet.c: focus, system screens). */
uint64_t dcr_boot_frames(void) { return g_frames_done; }
void dcr_watch_counters(void); /* dcr_watch.c */

static void *native(const char *name, int required) {
  void *p = jni_native(C_PLAYER, name);
  if (!p && required)
    debugPrintf("[boot] MISSING required native %s\n", name);
  else if (p)
    debugPrintf("[boot]   %-26s libunity+0x%lx\n", name,
                (unsigned long)((uintptr_t)p - (uintptr_t)unity_mod.load_virtbase));
  return p;
}

/* ---------------------------------------------------------- applet focus
 * The runtime (rt_applet.c) takes the HOME menu and sleep messages, flushes
 * the log and holds the clocks; the player is paused and resumed here. Both
 * run from rt_applet_poll() in the frame loop, on this thread. */
void port_focus_lost(void) {
  if (U.focusChanged) U.focusChanged(g_jni_env, g_thiz, 0);
  if (U.pause) U.pause(g_jni_env, g_thiz);
  dcr_vsync_set_paused(1);
}

void port_focus_gained(void) {
  dcr_vsync_set_paused(0);
  if (U.resume) U.resume(g_jni_env, g_thiz);
  if (U.focusChanged) U.focusChanged(g_jni_env, g_thiz, 1);
}

/* ------------------------------------------------------------------ boot */
static int load_engine(void) {
  fn_onload main_onload = (fn_onload)so_try_find_addr_rx(&main_mod, "JNI_OnLoad");
  if (!main_onload) {
    debugPrintf("[boot] libmain has no JNI_OnLoad\n");
    return -1;
  }
  jint ver = main_onload(g_jni_vm, NULL);
  debugPrintf("[boot] libmain JNI_OnLoad -> 0x%lx\n", (unsigned long)ver);

  fn_nl_load nl_load = (fn_nl_load)jni_native("com/unity3d/player/NativeLoader", "load");
  if (!nl_load) {
    debugPrintf("[boot] NativeLoader.load was not registered by libmain\n");
    return -1;
  }
  JObj *dir = jni_str(LIB_DIR);
  jboolean ok = nl_load(g_jni_env, jni_class("com/unity3d/player/NativeLoader")->obj, dir);
  jni_release(dir);
  debugPrintf("[boot] NativeLoader.load(\"%s\") -> %d\n", LIB_DIR, ok);
  return ok ? 0 : -1;
}

static int resolve_natives(void) {
  debugPrintf("[boot] UnityPlayer natives:\n");
  U.initJni = (fn_ctx)native("initJni", 1);
  U.recreateGfxState = (fn_recreate)native("nativeRecreateGfxState", 1);
  U.render = (fn_z)native("nativeRender", 1);
  U.resume = (fn_v)native("nativeResume", 0);
  U.pause = (fn_z)native("nativePause", 0);
  U.done = (fn_v)native("nativeDone", 0);
  U.focusChanged = (fn_vz)native("nativeFocusChanged", 0);
  U.initWebRequest = (fn_ctx)native("nativeInitWebRequest", 0);
  U.initWWW = (fn_ctx)native("nativeInitWWW", 0);
  U.file = (fn_ctx)native("nativeFile", 0);
  U.lowMemory = (fn_v)native("nativeLowMemory", 0);
  return (U.initJni && U.recreateGfxState && U.render) ? 0 : -1;
}

int dcr_boot_run(void) {
  jni_init();
  jni_looper_bind_engine(); /* this thread plays UnityMain: Handler() here posts to it */
  dcr_watchdog_start();     /* log flushing + hang reports from here on */
  dcr_watch_counters();     /* JIT, signal and GC counters in its reports (dcr_watch.c) */
  if (load_engine() != 0 || resolve_natives() != 0)
    fatal_error("The engine did not start (see debug.log).\n\n"
                "Check that libmain.so, libunity.so and libmono.so come from the same\n"
                "copy of Space Hulk as game.apk.");

  dcr_input_init();
  g_thiz = jni_singleton(C_PLAYER);
  g_ctx = jni_singleton(C_ACTIVITY);
  g_surface = jni_singleton("android/view/Surface");

  debugPrintf("[boot] initJni\n");
  U.initJni(g_jni_env, g_thiz, g_ctx);
  debugPrintf("[boot] initJni returned\n");
  if (U.file) {
    /* Context.getPackageCodePath(): the APK, as Android names it (dcr_path.c
     * maps it to game.apk). */
    JObj *apk = jni_str(DCR_ANDROID_APK);
    debugPrintf("[boot] nativeFile(%s)\n", DCR_ANDROID_APK);
    U.file(g_jni_env, g_thiz, apk);
    jni_release(apk);
    /* UnityPlayer.j(): with useObb, each main/patch OBB of this versionCode
     * that exists goes to nativeFile too; Space Hulk's levels are in it. */
    char obb[512];
    struct stat st;
    snprintf(obb, sizeof obb, "%s/obb/%s", dcr_game_root(), SH_OBB_NAME);
    if (stat(obb, &st) == 0) {
      JObj *o = jni_str(SH_ANDROID_OBB);
      debugPrintf("[boot] nativeFile(%s) (%llu MB)\n", SH_ANDROID_OBB,
                  (unsigned long long)(st.st_size >> 20));
      U.file(g_jni_env, g_thiz, o);
      jni_release(o);
    } else {
      debugPrintf("[boot] WARNING: no %s: the game's levels are in it\n", obb);
    }
  }
  if (U.initWWW)
    U.initWWW(g_jni_env, g_thiz, jni_class("com/unity3d/player/WWW")->obj);
  if (U.initWebRequest)
    U.initWebRequest(g_jni_env, g_thiz, jni_class("com/unity3d/player/UnityWebRequest")->obj);

  /* The pump runs before any graphics call: the engine can reach WaitVSync
   * from inside surface setup, and nothing else raises the counter. */
  if (dcr_vsync_start() != 0)
    debugPrintf("[boot] WARNING: no vsync pump; the first frame will wait for ever\n");

  debugPrintf("[boot] nativeRecreateGfxState(0, surface)\n");
  U.recreateGfxState(g_jni_env, g_thiz, 0, g_surface);

  /* The focus messages (HOME, sleep) are the runtime's from boot on
   * (rt_applet.c); they reach the player from the frame loop. */
  if (U.resume) U.resume(g_jni_env, g_thiz);
  if (U.focusChanged) U.focusChanged(g_jni_env, g_thiz, 1);

  dcr_prof_start();
  debugPrintf("[boot] entering the frame loop\n");
  uint64_t frames = 0;
  while (appletMainLoop() && !g_dcr_quit_requested) {
    rt_applet_poll();
    if (!rt_focused()) {
      svcSleepThread(50000000ll);
      continue;
    }
    /* The vsync this frame will start on, THEN input: the engine drains its
     * input queue before it waits for vsync (dcr_vsync.c), so polling after
     * the wait is what makes a press count from the frame it happened in. */
    dcr_vsync_frame_start();
    dcr_input_frame();
    jni_looper_run_engine(); /* what the engine posted to its own looper */
    dcr_boost_frame_begin();
    dcr_prof_frame_begin();
    jboolean alive = U.render(g_jni_env, g_thiz);
    /* Handheld.PlayFullScreenMovie, as the Android VideoView does it: the
     * player paused and its surface taken away, the movie, then the surface
     * back and the player resumed (sh_video.c) */
    int sh_video_pending(void);
    if (sh_video_pending()) {
      void sh_video_play_pending(void);
      debugPrintf("[video] pausing the player and releasing its surface\n");
      if (U.pause) U.pause(g_jni_env, g_thiz);
      U.recreateGfxState(g_jni_env, g_thiz, 0, NULL);
      sh_video_play_pending();
      U.recreateGfxState(g_jni_env, g_thiz, 0, g_surface);
      if (U.resume) U.resume(g_jni_env, g_thiz);
      debugPrintf("[video] the player has its surface back\n");
    }
    void sh_quality_boot(void);
    sh_quality_boot(); /* the texture limit, as soon as QualitySettings answers (sh_quality.c) */
    void sh_sched_frame(uint64_t frame);
    sh_sched_frame(frames + 1); /* [performance] pin_threads (sh_sched.c) */
    void sh_padprobe_frame(void);
    sh_padprobe_frame(); /* [debug] log_buttons (sh_padprobe.c) */
    void dcr_mod_frame(uint64_t frame);
    dcr_mod_frame(frames + 1); /* the port's C#, once the game's assemblies are in (dcr_mod.c) */
    void dcr_jit_emu_frame_end(void);
    dcr_jit_emu_frame_end(); /* emulator only: JIT code patched this frame (jit_arena.c) */
    dcr_prof_frame_end(frames + 1);
    dcr_boost_frame_end(frames + 1);
    dcr_time_tick();
    frames++;
    /* the start-up boost goes on through the logos until the title menu (dcr_boost.c) */
    static int s_started;
    if (!s_started && (dcr_gl_frames() > 0 || frames >= 600)) {
      s_started = 1;
      dcr_boost_first_picture();
    }
    dcr_boost_launch_tick();
    g_frames_done = frames;
    if (frames % 60 == 0)
      log_console_update();
    if (frames <= 3 || frames % 600 == 0) {
      /* Presented frames per second of wall time since the last report. */
      static u64 last_tick, first_tick;
      static unsigned long last_presented;
      u64 tick = armGetSystemTick();
      if (!first_tick)
        first_tick = tick;
      unsigned long presented = (unsigned long)dcr_gl_frames();
      double fps = last_tick ? (double)(presented - last_presented) * 1e9 /
                                   (double)armTicksToNs(tick - last_tick)
                             : 0.0;
      last_tick = tick;
      last_presented = presented;
      /* the RTC, independent of the tick counter every other clock here uses */
      static u64 rtc0;
      u64 rtc = 0;
      timeGetCurrentTime(TimeType_LocalSystemClock, &rtc);
      if (!rtc0)
        rtc0 = rtc;
      debugPrintf("[boot] frame %llu (alive=%d, presented %lu, %.1f fps; RTC +%llu s, ticks +%llu s)\n",
                  (unsigned long long)frames, alive, presented, fps,
                  (unsigned long long)(rtc - rtc0),
                  (unsigned long long)(armTicksToNs(tick - first_tick) / 1000000000ull));
      dcr_mono_report();
      if (frames % 600 == 0) {
        if (dcr_config()->log_sounds) {
          void dcr_fmod_census(char *out, size_t cap);
          static char census[300];
          dcr_fmod_census(census, sizeof census);
          if (census[0])
            debugPrintf("[fmod] %s\n", census);
        }
        dcr_vsync_report();
        dcr_boost_report();
        if (frames == 600 || frames % 3600 == 0) /* the first call only takes a snapshot */
          dcr_boost_cpu_report();
        dcr_apkcache_report();
      }
    }
    /* From here the log goes to a RAM ring, written out every 300 frames (and
     * on HOME / exit / crash): a line flushed to the SD card per call, every
     * frame, costs real frame time. */
    if (frames == 3)
      log_set_quiet(1);
    else if (frames % 300 == 0)
      log_flush_ring();
    if (!alive && frames > 1) {
      debugPrintf("[boot] nativeRender reported the player finished\n");
      break;
    }
  }
  debugPrintf("[boot] leaving the frame loop after %llu frames\n", (unsigned long long)frames);
  log_set_quiet(0);
  log_flush_ring();
  rt_applet_stop();
  if (U.pause) U.pause(g_jni_env, g_thiz);
  dcr_vsync_stop();
  if (U.done) U.done(g_jni_env, g_thiz);
  return 0;
}
