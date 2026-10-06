/* sh_quality.c -- Space Hulk's graphics settings, held against the game's own.
 *
 * THE MEMORY. The game was made for the NVIDIA Shield (3 GB). Its textures
 * are RGBA32 (a 1024x1024 one with its mips is 5.6 MB), and a mission loads
 * them by the hundred: on the 1 GB a 32-bit Switch process has, the first
 * mission ran the heap to 718 MB and died with "Could not allocate memory"
 * on a 5.6 MB texture while 326 MB were free in pieces (hardware
 * 2026-10-04). QualitySettings.masterTextureLimit = 1 uploads every texture
 * from its second mip (a quarter of the memory), as the game's own "Fast"
 * level does -- without that level's other cuts (no shadows).
 *
 * THE LEVEL. QualityHandlerAndroid picks the top level ("Fantastic": full
 * textures, 4x MSAA) on a Tegra K1 or newer, read through libnvsyscaps --
 * which this port does not load, so the check throws and the project's
 * default stands (also "Fantastic"). The level is left alone; only the two
 * settings below are held, set again after every SetQualityLevel (the game's
 * options menu calls it, and a level change resets them).
 *
 *   [graphics] texture_resolution  full / half / quarter -> limit 0 / 1 / 2
 *   [graphics] antialiasing        game / 0 / 2 / 4      -> MSAA samples
 *
 * The icalls are captured as libunity registers them (dcr_icall_hooks.c).
 * MIT.
 */
#include <stdint.h>

#include "dcr_config.h"
#include "util.h"

typedef void (*set_level_fn)(int index, int apply_expensive);
typedef void (*set_int_fn)(int v);
typedef int (*get_int_fn)(void);

typedef void (*set_float_fn)(float v);
set_int_fn sh_i_set_pixelLightCount;
set_float_fn sh_i_set_shadowDistance, sh_i_set_lodBias;
set_level_fn sh_o_SetQualityLevel;
set_int_fn sh_i_set_masterTextureLimit, sh_i_set_antiAliasing;
get_int_fn sh_i_GetQualityLevel;

static int g_applied;

int sh_obbindex_mips_stripped(void); /* sh_obbindex.c */

/* The limit asked for, less the mip the OBB has already dropped
 * (tools/obb/strip_mips.py): the same picture, a quarter of the reads. */
static int tex_limit(void) {
  int l = dcr_config()->tex_limit - (sh_obbindex_mips_stripped() ? 1 : 0);
  return l < 0 ? 0 : l;
}

void sh_quality_apply(const char *why) {
  const DcrConfig *c = dcr_config();
  if (sh_i_set_masterTextureLimit)
    sh_i_set_masterTextureLimit(tex_limit());
  if (sh_i_set_antiAliasing && c->msaa >= 0)
    sh_i_set_antiAliasing(c->msaa);
  /* the top level's per-pixel lights (4: each one draws what it touches
   * again), shadow distance (300) and LOD bias (10), for the render thread */
  if (sh_i_set_pixelLightCount && c->pixel_lights >= 0)
    sh_i_set_pixelLightCount(c->pixel_lights);
  if (sh_i_set_shadowDistance && c->shadow_dist >= 0)
    sh_i_set_shadowDistance((float)c->shadow_dist);
  if (sh_i_set_lodBias && c->lod_bias10 >= 0)
    sh_i_set_lodBias((float)c->lod_bias10 / 10.0f);
  debugPrintf("[quality] pixel lights %d, shadow distance %d, LOD bias x10 %d (-1: the level's)\n",
              c->pixel_lights, c->shadow_dist, c->lod_bias10);
  debugPrintf("[quality] %s: level %d, textures 1/%d (masterTextureLimit %d%s), MSAA %s%d\n", why,
              sh_i_GetQualityLevel ? sh_i_GetQualityLevel() : -1, 1 << c->tex_limit, tex_limit(),
              sh_obbindex_mips_stripped() ? ", the OBB's mips already stripped" : "",
              c->msaa < 0 ? "the level's, not " : "", c->msaa < 0 ? 0 : c->msaa);
  g_applied = 1;
}

void sh_w_SetQualityLevel(int index, int apply_expensive) {
  sh_o_SetQualityLevel(index, apply_expensive);
  sh_quality_apply("SetQualityLevel");
}

/* dcr_boot.c, after each of the first frames: as soon as the icalls work. */
void sh_quality_boot(void) {
  if (!g_applied && sh_i_set_masterTextureLimit)
    sh_quality_apply("start-up");
}
