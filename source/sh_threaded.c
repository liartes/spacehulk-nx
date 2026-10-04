/* sh_threaded.c -- Unity's multithreaded rendering, which the game leaves off.
 *
 * The profile of a mission (hardware 2026-10-04, profile_continuous) had the
 * main thread busy 95% of the time at ~30 fps, a third of it inside Mesa
 * (map_buffer_range, st_draw_vbo, nvc0_state_validate), while Unity's two
 * worker threads idled at 10% and the third core had nothing to do.
 * PlayerSettings has m_MTRendering on but m_MobileMTRendering OFF, so on
 * Android the engine renders on the main thread.
 *
 * With it on, the engine records its draw calls into a command buffer and a
 * thread of its own ("UnityGfxDeviceWorker") makes the GL calls: Mesa's CPU
 * work moves to another core. The decision is made at device creation
 * (libunity+0x33d55c, InitializeGfxDevice):
 *     threaded = UseThreadedDevice() && processorCount > 1
 * and, before that, PlayerSettings::GetMobileMTRendering (libunity+0x389a48,
 * `ldrb r0, [r0, #0x85]` -- the byte the PlayerSettings reader stores
 * m_MobileMTRendering in): 0 forces the single-threaded device whatever
 * follows. UseThreadedDevice (libunity+0x4550a4) reads the player's flags.
 * Both become `return 1` (the first build patched only UseThreadedDevice: no
 * worker thread appeared, hardware 2026-10-04). Guard-checked: another build
 * is left alone.
 *
 * [graphics] threaded_rendering (default true) turns it off. MIT.
 */
#include <stdint.h>

#include "dcr_config.h"
#include "dcr_patches.h"
#include "so_util.h"
#include "util.h"

extern so_module unity_mod;

static const struct {
  const char *name;
  uint32_t rva;
  uint32_t guard[4];
} k_sites[] = {
    {"PlayerSettings::GetMobileMTRendering", 0x389a48u, {0xe5d00085u, 0xe12fff1eu, 0xe5d000a4u, 0xe12fff1eu}},
    {"UseThreadedDevice", 0x4550a4u, {0xe59f0034u, 0xe59f1034u, 0xe08f0000u, 0xe7d12000u}},
};

void sh_threaded_apply(void) {
  if (!dcr_config()->threaded) {
    debugPrintf("[gfx] threaded rendering off (config.ini): the engine renders on its main thread\n");
    return;
  }
  /* all guards first: half of it would change nothing */
  for (unsigned i = 0; i < sizeof k_sites / sizeof k_sites[0]; i++)
    if (!dcr_guard_ok(&unity_mod, k_sites[i].rva, k_sites[i].guard, k_sites[i].name))
      return;
  static const uint32_t ret1[2] = {0xE3A00001u /* mov r0, #1 */, 0xE12FFF1Eu /* bx lr */};
  for (unsigned i = 0; i < sizeof k_sites / sizeof k_sites[0]; i++)
    if (so_patch_code((void *)((uintptr_t)unity_mod.load_virtbase + k_sites[i].rva), ret1, sizeof ret1)) {
      debugPrintf("[gfx] %s: write failed\n", k_sites[i].name);
      return;
    }
  debugPrintf("[gfx] threaded rendering on: GL calls move to UnityGfxDeviceWorker\n");
}
