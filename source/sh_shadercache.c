/* sh_shadercache.c -- Unity's shader cache emptied when the program changes.
 *
 * Unity keeps each linked GL program as a binary in UnityShaderCache
 * (external/cache/ here) and loads it back with glProgramBinary. mesa32 ties
 * a binary to the Mesa build that made it (st_shader_cache.c: a hash of its
 * compile date and time) and refuses any other. Unity then links the program
 * from source again -- and that path is broken: programs relinked after a
 * refused binary come out with shifted attribute locations and sampler
 * state (e.g. _glesNormal at 22 instead of 21), weapons flicker between
 * textured and flat grey, and Unity saves those programs, so the next start
 * loads them back even worse (hardware 2026-11-06: every session after a Mesa
 * change, and none after the cache was emptied; 62 of 179 programs differed
 * from a clean compile).
 *
 * Mesa is linked into this program: the Makefile passes a checksum of its
 * libraries (SH_MESA_ID), and when it differs from the one recorded beside
 * the cache, the cache is emptied before Unity starts (an update that keeps
 * the same Mesa keeps the cache). Unity then compiles from source as on
 * a first start (a longer first mission load), never from a refused binary.
 * MIT.
 */
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "dcr_path.h"
#include "util.h"

void sh_shadercache_check(void) {
  char dir[256], mark[256], want[32], have[32] = "";
  snprintf(dir, sizeof dir, "%s/external/cache/UnityShaderCache", dcr_game_root());
  snprintf(mark, sizeof mark, "%s/external/cache/shadercache.mesa", dcr_game_root());
  snprintf(want, sizeof want, "%s", SH_MESA_ID);
  FILE *f = fopen(mark, "r");
  if (f) {
    if (!fgets(have, sizeof have, f))
      have[0] = 0;
    fclose(f);
    have[strcspn(have, "\r\n")] = 0;
  }
  if (!strcmp(have, want))
    return;
  int n = 0, failed = 0;
  DIR *d = opendir(dir);
  if (d) {
    struct dirent *e;
    char path[512];
    while ((e = readdir(d))) {
      if (e->d_name[0] == '.')
        continue;
      snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
      if (remove(path))
        failed++;
      else
        n++;
    }
    closedir(d);
  }
  debugPrintf("[shaders] Mesa %s (cache made by %s): %d cached programs removed%s\n", want,
              have[0] ? have : "an unknown Mesa", n, failed ? ", some could not be" : "");
  if (failed)
    return; /* try again next start */
  f = fopen(mark, "w");
  if (f) {
    fprintf(f, "%s\n", want);
    fclose(f);
  }
}
