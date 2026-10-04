/* port_config.h -- Space Hulk's settings for the android32 runtime.
 *
 * Macros only: the runtime's C files, its assembly and the launcher all read
 * this (runtime/source/rt_settings.h). What each setting does is next to its
 * default in the runtime; runtime/docs/ lists them all. MIT.
 */
#ifndef PORT_CONFIG_H
#define PORT_CONFIG_H

/* ------------------------------------------------------------------ the game */
#define PORT_TITLE    "Space Hulk"
#define PORT_NAME     "spacehulk_nx"
#define PORT_PACKAGE  "com.hoplite.spacehulk"
#define PORT_BANNER   "spacehulk_nx: Space Hulk (Hoplite, versionCode 7; Unity 5.3.4f1 / Mono, armeabi-v7a)"
/* libmain + libunity + libmono end to end (~22 MB mapped), with room for a
 * content update; under-reserving shows up as so_load() -3 */
#define PORT_SO_REGION_BYTES (48u * 1024 * 1024)

/* The APK, by what is in it: becomes game.apk. Its expansion file
 * (main.7.com.hoplite.spacehulk.obb, ~1.5 GB) goes in <root>/obb/ (dcr_path.c). */
#define PORT_APK_DESC "Space Hulk by Hoplite Research (versionCode 7, armeabi-v7a)"
#define PORT_APK_ROLES \
  {.what = "Space Hulk", .name = "game.apk", .package = "com.hoplite.spacehulk", .flags = RT_APK_ADOPT}
#define PORT_LAUNCHER_START_NOTE "(the first start unpacks the game's libraries from the APK)"

/* ------------------------------------------------------------------ libc */
#define RT_PROC_COMM        "hoplite.spaceh"
#define RT_EXTRA_ENV        "MONO_DEBUG=explicit-null-checks" /* load-bearing: Mono without SIGSEGV */
#define RT_DL_HAS_OPENSLES  0 /* FMOD falls back to its Java output (dcr_audio.c) */
#define RT_ZLIB_HOLDBACK    0

/* ------------------------------------------------------------------ JNI, NDK */
#define RT_JNI_UNHANDLED_INSTANCE_SINGLETON   1 /* Unity's C# plugins */
#define RT_JNI_UNHANDLED_BUILDER_RETURNS_SELF 1
#define RT_LOOPER_MAIN_IMPLICIT               0

/* ------------------------------------------------------------------ frames */
#define RT_GL_SWAP_ENDS_FRAME 0 /* the frame is nativeRender (dcr_boot.c) */
#define RT_CAPTURE_TRIES      300
#define RT_BOOST_LAUNCH_AFTER_PICTURE_MS 25000
#define RT_MAIN_THREAD_NAME   "UnityMain"
#define RT_PAD_MAX_PLAYERS    8

/* ------------------------------------------------------------------ watchdog */
#define RT_WATCHDOG_PRIO 0x1C
#define RT_WATCHDOG_CORE 2
#define RT_WATCHDOG_BOOT 1

/* ------------------------------------------------------------------ files */
#define RT_PATH_CWD_RELATIVE   0
#define RT_DIRCACHE            0 /* Mono probes and writes under data/ */
#define RT_MANIFEST_LOG_PREFIX "unity"
#define RT_EMU_FIXUPS          0 /* its self-fix runs after the module fix (dcr_main.c) */

#endif
