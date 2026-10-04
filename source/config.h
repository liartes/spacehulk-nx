/* config.h -- Space Hulk's own build-wide constants (the runtime's
 * settings are in port_config.h).
 *
 * Space Hulk (versionCode 7): Unity 5.3.4f1, Mono backend, armeabi-v7a.
 * MIT.
 */
#ifndef DCR_CONFIG_H
#define DCR_CONFIG_H

#include "rt_settings.h"

/* The game's modules, and where Unity's data is inside the APK. */
#define DCR_LIB_MAIN    "libmain.so"
#define DCR_LIB_UNITY   "libunity.so"
#define DCR_LIB_MONO    "libmono.so"
#define DCR_DATA_DIR    "assets/bin/Data"
#define DCR_MANAGED_DIR "assets/bin/Data/Managed"
#define DCR_PKG_NAME    PORT_PACKAGE
/* The expansion file Unity's Java side hands to nativeFile() after the APK
 * (UnityPlayer.j(): settings.xml useObb). Android's name for it; dcr_path.c
 * maps /storage/emulated/0/Android/obb/<pkg>/ to <root>/obb/. */
#define SH_OBB_NAME     "main.7." PORT_PACKAGE ".obb"
#define SH_ANDROID_OBB  "/storage/emulated/0/Android/obb/" PORT_PACKAGE "/" SH_OBB_NAME

/* Feature switches wired against dcr_offsets.h. */
/* Unity 5.3 has no Choreographer VSYNC path (no WaitVSync to pump): it paces
 * with eglSwapInterval / targetFrameRate (derive_offsets.py finds nothing). */
#define DCR_PATCH_CHOREOGRAPHER 0   /* patch ChoreographerEnableVSync -> ret */
#define DCR_PATCH_VSYNC         0   /* pump the vsync counter from a thread   */
#define DCR_TIME_ICALL_HOOKS    1   /* hook the 11 Time icalls                */

/* Mono runtime policy (see mono_rt.c): avoid the signal paths Horizon can't
 * service. Coop suspend removes the tkill dependency; explicit null checks
 * remove the SIGSEGV dependency. */
#define DCR_MONO_COOP_SUSPEND        1
#define DCR_MONO_EXPLICIT_NULLCHECKS 1

#endif /* DCR_CONFIG_H */
