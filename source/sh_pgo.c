/* sh_pgo.c -- the training run of a profile-guided Mesa (make SH_PGO=gen).
 *
 * Mesa is half of the render thread, which limits the frame rate (hardware
 * 2026-10-06: 49% of UnityGfxDeviceWorker's samples). A profile-guided build
 * lays its branches, inlining and hot/cold code out for what the game really
 * runs. mesa32's build_mesa32.sh builds it in two passes (MESA_PGO=gen, then
 * MESA_PGO=use); this file is the program's half of the first one.
 *
 * devkitARM's libgcov is built without a C library: __gcov_dump writes
 * nothing. So the instrumented Mesa registers its counters in the gcov_info
 * section (-fprofile-info-section=gcov_info; a C-identifier name, so the
 * linker gives it __start_/__stop_ symbols without a linker script), and here
 * a thread serializes them every 60 s with __gcov_info_to_gcda, the GCC 12+
 * interface for freestanding programs, into one gcov stream file:
 * <game folder>/mesa-pgo.gcov, written beside and renamed over the last one.
 * The counters keep running totals, so the last file holds the whole session.
 * On the PC, `arm-none-eabi-gcov-tool merge-stream mesa-pgo.gcov` (in the
 * toolchain container, mesa32 at /work) writes the .gcda files of
 * mesa32/pgo-profile/.
 *
 * The normal build compiles none of this: sh_pgo_start() does nothing. MIT.
 */
#include <stdio.h>
#include <stdlib.h>
#include <switch.h>

#include "dcr_path.h"
#include "util.h"

#if SH_PGO_GEN
#include <gcov.h>

#define PERIOD_NS 60000000000ll

extern const struct gcov_info *const __start_gcov_info[];
extern const struct gcov_info *const __stop_gcov_info[];

static Thread g_thread;
static FILE *g_out;
static size_t g_bytes;
static int g_err;

static void out(const void *d, unsigned n, void *arg) {
  if (fwrite(d, 1, n, g_out) != n)
    g_err = 1;
  g_bytes += n;
}

static void filename(const char *f, void *arg) { __gcov_filename_to_gcfn(f, out, arg); }

/* only for value-profile counters, which this build has none of */
static void *allocate(unsigned n, void *arg) { return malloc(n); }

static void write_profile(int round) {
  char path[256], tmp[260];
  snprintf(path, sizeof path, "%s/mesa-pgo.gcov", dcr_game_root());
  snprintf(tmp, sizeof tmp, "%s.tmp", path);
  const uint64_t t0 = armGetSystemTick();
  g_out = fopen(tmp, "wb");
  if (!g_out) {
    debugPrintf("[pgo] cannot write %s\n", tmp);
    return;
  }
  static char buf[256 * 1024];
  setvbuf(g_out, buf, _IOFBF, sizeof buf);
  g_bytes = 0;
  g_err = 0;
  const struct gcov_info *const *info = __start_gcov_info;
  const struct gcov_info *const *end = __stop_gcov_info;
  __asm__("" : "+r"(info)); /* as GCC's manual: keep the loop */
  int n = 0;
  for (; info != end; info++, n++)
    __gcov_info_to_gcda(*info, filename, out, allocate, NULL);
  if (fclose(g_out))
    g_err = 1;
  g_out = NULL;
  if (!g_err) {
    remove(path);
    if (rename(tmp, path))
      g_err = 1;
  }
  debugPrintf("[pgo] round %d: %d objects, %u KB written in %u ms%s\n", round, n, (unsigned)(g_bytes >> 10),
              (unsigned)(armTicksToNs(armGetSystemTick() - t0) / 1000000), g_err ? " -- WRITE FAILED" : "");
}

static void pgo_thread(void *arg) {
  for (int round = 1;; round++) {
    svcSleepThread(PERIOD_NS);
    write_profile(round);
  }
}

void sh_pgo_start(void) {
  debugPrintf("[pgo] instrumented Mesa (%d objects): profile to mesa-pgo.gcov every 60 s\n",
              (int)(__stop_gcov_info - __start_gcov_info));
  /* low priority, core 2 (the game's other threads use 0 and 2) */
  if (R_FAILED(threadCreate(&g_thread, pgo_thread, NULL, NULL, 0x8000, 0x3B, 2)) ||
      R_FAILED(threadStart(&g_thread)))
    debugPrintf("[pgo] no thread: no profile\n");
}
#else
void sh_pgo_start(void) {}
#endif
