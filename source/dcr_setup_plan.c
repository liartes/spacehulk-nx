/* dcr_setup_plan.c -- Space Hulk's part of the first launch: its setup plan
 * for the runtime's dcr_setup.c, and its own step.
 *
 * The runtime's dcr_setup.c does the common work from this plan:
 *   game.apk rewritten stored when its assets/ are compressed (Unity would
 *   inflate them at every read; the APK is 24 MB), then the three libraries,
 *   then classes.txt, then the OBB check (below), then "Starting the game".
 *
 * The bar, in permille of the whole first launch:
 *     0- 800  game.apk rewritten uncompressed (by bytes written)
 *   800- 820  the rewritten copy checked
 *   820- 940  libmain / libunity / libmono unpacked (by bytes)
 *   940- 960  the Java class list
 *   960- 985  the OBB looked for, and made Switch-sized (once: sh_obbstrip.c)
 *   985- 990  its zip headers indexed (once)
 *        1000 the game starts
 *
 * .setup keys, which must not change (or every player unpacks again once):
 * libmain.so, libunity.so, libmono.so, classes.txt. MIT.
 */
#include <stdio.h>
#include <string.h>

#include <switch.h>

#include "config.h"
#include "dcr_config.h"
#include "dcr_path.h"
#include "dcr_setup.h"
#include "rt_settings.h"
#include "sh_obbstrip.h"
#include "util.h"

static const char *const k_libs[] = {"libmain.so", "libunity.so", "libmono.so"};

/* ------------------------------------------------------------------ OBB
 * The levels, models and sounds are in the expansion file, which Google Play
 * put in Android/obb/<package>/ next to the APK. Here it goes in <root>/obb/
 * (dcr_path.c maps the Android folder there); dcr_boot.c hands it to Unity.
 * Looked for (the game cannot start without it, and saying so here is
 * clearer than Unity's missing-level errors), and its zip headers indexed
 * the first time (sh_obbindex.c). */
/* ------------------------------------------------------------ the OBB, made
 * Switch-sized once (sh_obbstrip.c): the copy written next to it, then put in
 * its place. Kept as it is when the SD card has no room for the copy, or the
 * copy fails (the game plays the same either way, only its loads are
 * slower). [setup] optimize_obb. */
void dcr_boost_hold(int on); /* dcr_boost.c */

typedef struct {
  int p0, p1;
} StripBar;

static void strip_progress(uint64_t done, uint64_t total, void *ctx) {
  const StripBar *b = ctx;
  rt_setup_progress_in("Optimizing the OBB for the Switch (once, a few minutes)", b->p0, b->p1, done, total);
}

static void obb_optimize(const char *obb, long size, int p0, int p1) {
  if (!dcr_config()->optimize_obb)
    return;
  int done = sh_obbstrip_is_done(obb);
  if (done != 0) {
    if (done < 0)
      debugPrintf("[setup] %s: not a zip -- left as it is\n", obb);
    return;
  }
  s64 free_bytes = 0;
  FsFileSystem *fs = fsdevGetDeviceFileSystem("sdmc");
  if (fs && R_SUCCEEDED(fsFsGetFreeSpace(fs, "/", &free_bytes)) && free_bytes < (s64)size * 3 / 4 + (64ll << 20)) {
    debugPrintf("[setup] OBB optimization skipped: %lld MB free on the SD card, about %ld MB needed\n",
                (long long)(free_bytes >> 20), (size * 3 / 4 >> 20) + 64);
    return;
  }
  char part[320];
  snprintf(part, sizeof part, "%s.part", obb);
  remove(part);
  debugPrintf("[setup] optimizing the OBB for the Switch (its textures from their second mip)...\n");
  u64 t0 = armGetSystemTick();
  dcr_boost_hold(1);
  StripBar bar = {p0, p1};
  ShObbStripStats st;
  int r = sh_obbstrip_run(obb, part, strip_progress, &bar, &st);
  dcr_boost_hold(0);
  long out = rt_file_size(part);
  if (r != 1 || out <= 0 || sh_obbstrip_is_done(part) != 1) {
    debugPrintf("[setup] OBB optimization failed: the original stays\n");
    remove(part);
    return;
  }
  if (remove(obb) != 0 || rename(part, obb) != 0) {
    debugPrintf("[setup] could not put the optimized OBB in place (%s): check the obb folder\n", part);
    return;
  }
  char idx[300];
  rt_root_path(idx, sizeof idx, "obb/.headers");
  remove(idx); /* another OBB: the header index is made again */
  debugPrintf("[setup] OBB optimized in %llu s: %u textures, %u files, %llu MB less to read; %ld -> %ld MB\n",
              (unsigned long long)(armTicksToNs(armGetSystemTick() - t0) / 1000000000ull), st.textures, st.files,
              (unsigned long long)(st.bytes_saved >> 20), size >> 20, out >> 20);
}

static void obb_check(RtSetupCtx *ctx) {
  char obb[300];
  rt_root_path(obb, sizeof obb, "obb/" SH_OBB_NAME);
  long sz = rt_file_size(obb); /* 1.48 GB: under 2 GB, a 32-bit long holds it */
  if (sz < 0)
    debugPrintf("[setup] MISSING %s: copy main.7.com.hoplite.spacehulk.obb (about 1.5 GB) there,\n"
                "[setup] from Android/obb/com.hoplite.spacehulk/ on the phone\n", obb);
  else {
    debugPrintf("[setup] OBB: %s (%ld MB)\n", obb, sz >> 20);
    const int mid = ctx->p0 + (ctx->p1 - ctx->p0) * 5 / 6;
    obb_optimize(obb, sz, ctx->p0, mid); /* once (sh_obbstrip.c) */
    sz = rt_file_size(obb);
    void sh_obbindex_build(const char *obb, int64_t size, int p0, int p1);
    if (sz > 0)
      sh_obbindex_build(obb, sz, mid, ctx->p1); /* its zip headers, once (sh_obbindex.c) */
  }
}

static const RtSetupStep k_steps[] = {
    {"OBB", RT_STEP_AFTER_ZIP, 960, 990, obb_check},
};

const RtSetupPlan port_setup_plan = {
    .libs = k_libs,
    .nlibs = sizeof k_libs / sizeof k_libs[0],
    .libs_what = "Unpacking the game's libraries",
    .apk_requirement = "This port needs the 32-bit ARM (armeabi-v7a) build of Space Hulk by\n"
                       "Hoplite Research (versionCode 7): use an APK of that version.",
    .libs_p0 = 820,
    .libs_p1 = 940,
    .classes_p0 = 940,
    .classes_p1 = 960,
    .store_apk_prefix = "assets/",
    .store_p0 = 0,
    .store_p1 = 800,
    .steps = k_steps,
    .nsteps = sizeof k_steps / sizeof k_steps[0],
};
