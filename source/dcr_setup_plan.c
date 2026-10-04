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
 *   960- 990  the OBB looked for, and its zip headers indexed (once)
 *        1000 the game starts
 *
 * .setup keys, which must not change (or every player unpacks again once):
 * libmain.so, libunity.so, libmono.so, classes.txt. MIT.
 */
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "dcr_path.h"
#include "dcr_setup.h"
#include "rt_settings.h"
#include "util.h"

static const char *const k_libs[] = {"libmain.so", "libunity.so", "libmono.so"};

/* ------------------------------------------------------------------ OBB
 * The levels, models and sounds are in the expansion file, which Google Play
 * put in Android/obb/<package>/ next to the APK. Here it goes in <root>/obb/
 * (dcr_path.c maps the Android folder there); dcr_boot.c hands it to Unity.
 * Looked for (the game cannot start without it, and saying so here is
 * clearer than Unity's missing-level errors), and its zip headers indexed
 * the first time (sh_obbindex.c). */
static void obb_check(RtSetupCtx *ctx) {
  char obb[300];
  rt_root_path(obb, sizeof obb, "obb/" SH_OBB_NAME);
  long sz = rt_file_size(obb); /* 1.48 GB: under 2 GB, a 32-bit long holds it */
  if (sz < 0)
    debugPrintf("[setup] MISSING %s: copy main.7.com.hoplite.spacehulk.obb (about 1.5 GB) there,\n"
                "[setup] from Android/obb/com.hoplite.spacehulk/ on the phone\n", obb);
  else {
    debugPrintf("[setup] OBB: %s (%ld MB)\n", obb, sz >> 20);
    void sh_obbindex_build(const char *obb, int64_t size, int p0, int p1);
    sh_obbindex_build(obb, sz, ctx->p0, ctx->p1); /* its zip headers, once (sh_obbindex.c) */
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
