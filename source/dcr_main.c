/* dcr_main.c -- Space Hulk's part of the boot (the runtime's main.c
 * runs the rest: runtime/source/main.c).
 *
 * port_load() goes from the APK to the game's first code: setup, the three
 * modules loaded and bound, the patches and hooks. port_run() runs the
 * constructors in Android's order and then the player. The order here
 * matters; each step says why it is where it is. MIT.
 */
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "config.h"
#include "dcr_config.h"
#include "dcr_patches.h"
#include "dcr_path.h"
#include "dcr_setup.h"
#include "emu_fixups.h"
#include "error.h"
#include "imports.h"
#include "jit_arena.h"
#include "rt_boot.h"
#include "so_util.h"
#include "util.h"

int dcr_time_install(void);
int dcr_vsync_init(void);
int dcr_mono_install(void);
int dcr_boot_run(void);
void dcr_fmod_patch_voices(void);
void dcr_fmod_patch_rate(void);
void dcr_mono_hook_exceptions(void);
void dcr_mod_jitlog_install(void);
void dcr_audio_selftest(void);

so_module main_mod, unity_mod, mono_mod;

/* ------------------------------------------------------------- boot report */
extern volatile uint32_t __dcr_reloc_diag[16] __attribute__((visibility("hidden")));

void port_report_boot(void) {
  if (__dcr_reloc_diag[1]) /* test build (DCR_TEST_ALIAS_RELOC) only */
    debugPrintf("[reloc] alias test: result 0x%lx (a11a5 = OK) handle 0x%lx fail rc 0x%lx at %08lx"
                " | blk %08lx+%lx -> %08lx | blk %08lx+%lx -> %08lx | blk %08lx+%lx -> %08lx"
                " | RX view sees patch: %lu ok, %lu stale\n",
                (unsigned long)__dcr_reloc_diag[0], (unsigned long)__dcr_reloc_diag[1],
                (unsigned long)__dcr_reloc_diag[2], (unsigned long)__dcr_reloc_diag[3],
                (unsigned long)__dcr_reloc_diag[4], (unsigned long)__dcr_reloc_diag[5],
                (unsigned long)__dcr_reloc_diag[6], (unsigned long)__dcr_reloc_diag[7],
                (unsigned long)__dcr_reloc_diag[8], (unsigned long)__dcr_reloc_diag[9],
                (unsigned long)__dcr_reloc_diag[10], (unsigned long)__dcr_reloc_diag[11],
                (unsigned long)__dcr_reloc_diag[12], (unsigned long)__dcr_reloc_diag[13],
                (unsigned long)__dcr_reloc_diag[14]);
}

const char *port_apk_help(void) {
  return "Copy the APK of your own Space Hulk (Hoplite Research, armeabi-v7a,\n"
         "versionCode 7) into that folder, under any name, and its expansion file\n"
         "main.7.com.hoplite.spacehulk.obb into the obb folder inside it: the game\n"
         "reads its data from them, and the libraries are unpacked from the APK on\n"
         "the first launch.";
}

/* The paths worth a log line: Unity's asset bundles and its reads inside the
 * APK. Its routine probes under assets/bin/Data are not news. */
int dcr_path_traced(const char *p) {
  if (!p || strstr(p, "/assets/bin/Data/"))
    return 0;
  return strstr(p, "AssetBundles") || strstr(p, "jar:") || strstr(p, ".apk/") ||
         strstr(p, ".apk!") || strstr(p, "StreamingAssets");
}

/* The runtime's default would run the shared audout test; dcr_audio.c has
 * its own output (FMOD's pump), tested the same way. */
void port_audio_selftest(void) { dcr_audio_selftest(); }

/* ----------------------------------------------------------------- modules */
/* Under an emulator a module runs where it is staged; libunity is staged with
 * room behind it for the instruction stubs of emu_fixups.c (branch range). */
#define EMU_POOL_BYTES 0x10000u
static uint32_t *g_emu_pool;

static int load_module(so_module *mod, const char *name) {
  char path[512];
  snprintf(path, sizeof path, "%s/%s", dcr_game_root(), name);
  void *base = NULL;
  size_t max = PORT_SO_REGION_BYTES;
  if (dcr_is_emulator() && mod == &unity_mod && (base = memalign(0x1000, PORT_SO_REGION_BYTES)))
    max = PORT_SO_REGION_BYTES - EMU_POOL_BYTES;
  int rc = so_load(mod, path, base, max);
  if (rc < 0) {
    const char *why = rc == -1 ? "cannot open it, or it is not a 32-bit ARM ELF"
                    : rc == -2 ? "out of memory"
                    : rc == -3 ? "larger than PORT_SO_REGION_BYTES"
                    : rc == -4 ? "too many program headers" : "?";
    debugPrintf("[boot] so_load(%s) failed rc=%d: %s\n", path, rc, why);
    return -1;
  }
  if (base) {
    g_emu_pool = (uint32_t *)((uint8_t *)base + mod->load_size);
    memset(g_emu_pool, 0, EMU_POOL_BYTES);
  }
  so_relocate(mod);
  return 0;
}

/* Imports are bound once every module is loaded: Unity 5.6's libunity
 * imports libmono's exports directly (DT_NEEDED libmono.so). */
static void resolve_module(so_module *mod, const char *name) {
  int missing = so_resolve(mod, dcr_imports, dcr_imports_count, 1);
  debugPrintf("[boot] %-12s %6u KB  staged %p -> code %p  (%d unresolved imports)\n", name,
              (unsigned)(mod->load_size >> 10), mod->load_base, mod->load_virtbase, missing);
}

int port_load(const char *apk) {
  /* libmain/libunity/libmono and classes.txt, from game.apk when they are
   * missing or it has changed; the OBB check (dcr_setup_plan.c) */
  dcr_setup_from_apk(apk);
  if (load_module(&main_mod, DCR_LIB_MAIN) < 0 ||
      load_module(&unity_mod, DCR_LIB_UNITY) < 0 ||
      load_module(&mono_mod, DCR_LIB_MONO) < 0)
    fatal_error("Could not load the game libraries from %s.\n\n"
                "They are unpacked from the APK (lib/armeabi-v7a/) on launch: delete\n"
                "libmain.so, libunity.so, libmono.so and .setup there to unpack them again.",
                dcr_game_root());

  resolve_module(&main_mod, DCR_LIB_MAIN);
  resolve_module(&unity_mod, DCR_LIB_UNITY);
  resolve_module(&mono_mod, DCR_LIB_MONO);
  if (dcr_is_emulator()) {
    /* EMULATOR ONLY: instructions Ryujinx's A32 decoder lacks (emu_fixups.c);
     * libunity first, then this program (RT_EMU_FIXUPS 0 keeps this order) */
    if (g_emu_pool)
      dcr_emu_fix_module(&unity_mod, g_emu_pool, EMU_POOL_BYTES / 4);
    dcr_emu_fix_self();
  }

  so_finalize(&main_mod);
  so_finalize(&unity_mod);
  so_finalize(&mono_mod);
  so_flush_caches(&main_mod);
  so_flush_caches(&unity_mod);
  so_flush_caches(&mono_mod);
  debugPrintf("[boot] modules mapped RX/RW\n");

  /* Patches and hooks go in before any engine code runs (init_array first). */
  dcr_patches_apply(); /* Choreographer VSYNC enable/stop -> bx lr */
  void sh_threaded_apply(void);
  sh_threaded_apply(); /* Unity's multithreaded rendering on (sh_threaded.c) */
  if (dcr_config()->voices64)
    dcr_fmod_patch_voices(); /* FMOD: 64 real voices, not 32 (dcr_fmod.c) */
  if (dcr_config()->mix_48k)
    dcr_fmod_patch_rate(); /* FMOD mixes at 48 kHz, as on a phone (dcr_fmod.c) */

  dcr_time_install(); /* 11 Time icalls + 4 TimeManager entries */
  dcr_vsync_init();   /* WaitVSync's mutex/cond/counter */
  dcr_mono_install(); /* icache flush hook + Boehm GC bridge */

  if (jit_arena_init() != 0)
    fatal_error("Could not reserve executable memory for Mono's JIT.");
  dcr_mono_hook_exceptions(); /* names the scripts behind "NullReferenceException" lines */
  dcr_mod_jitlog_install();   /* a debug aid: <root>/jitlog maps the JIT code (dcr_mod.c) */
  return 0;
}

/* Android runs a library's constructors when it is loaded: libmain first
 * (System.loadLibrary), then libunity (dlopen from libmain), then libmono
 * (dlopen from libunity during player start-up). */
void port_run(void) {
  so_execute_init_array(&main_mod);
  so_execute_init_array(&unity_mod);
  so_execute_init_array(&mono_mod);
  debugPrintf("[boot] module constructors done\n");
  dcr_boot_run();
}
