/* dcr_config.c -- Space Hulk's settings: config.ini's options, on the
 * runtime's INI engine (runtime/source/rt_cfg.c). The file header is the
 * runtime's default, "# Space Hulk for Switch -- settings." (PORT_TITLE).
 * MIT.
 */
#include <stdio.h>
#include <sys/stat.h>
#include <switch.h>

#include "dcr_config.h"
#include "dcr_path.h"
#include "rt_cfg.h"
#include "util.h"

/* Space Hulk: the Disney Crossy Road features these fields drive (IL
 * patches, the C# mod, FMOD patches at that build's offsets) stay off. */
static DcrConfig g_cfg = {.res_w = 1280, .res_h = 720, .boost = 1, .tex_limit = 1, .msaa = 0, .threaded = 1, .read_buf_kb = 32, .obb_shared = 1, .log_buttons = 0, .mix_48k = 1, .voices64 = 1};
static int g_aa_choice; /* [graphics] antialiasing: index in "game,0,2,4" */

const DcrConfig *dcr_config(void) { return &g_cfg; }

static const CfgOpt k_opts[] = {
    CFG_ROW_RESOLUTION("auto",
                       "Rendering resolution: auto (1080 if docked when the game starts, 720 in\n"
                       "# handheld), 1080 or 720. The Switch scales the picture to the screen either\n"
                       "# way."),
    {"graphics", "texture_resolution", "half",
     "Texture resolution: full, half or quarter. Full is the game as on the\n"
     "# NVIDIA Shield, and does not fit in a mission: the 32-bit Switch program\n"
     "# has 1 GB, the Shield had 3 (sh_quality.c).",
     CFG_CHOICE, "full,half,quarter", &g_cfg.tex_limit},
    {"graphics", "antialiasing", "0",
     "Multisample antialiasing: 0, 2 or 4 samples, or game (the quality level's\n"
     "# own: 4 at the top level). Each step costs GPU time and memory.",
     CFG_CHOICE, "game,0,2,4", &g_aa_choice},
    {"graphics", "threaded_rendering", "true",
     "Unity renders on a thread of its own, on another core (the game ships\n"
     "# with it off on Android). Off: everything on the main thread, as on a phone.",
     CFG_BOOL, NULL, &g_cfg.threaded},
    CFG_ROW_BOOST("CPU at 1785 MHz while the game starts and inside loading frames (those\n"
                  "# over 50 ms), normal otherwise.",
                  &g_cfg.boost),
    {"performance", "obb_one_handle", "true",
     "Open the OBB once and share it: Unity opens it again for every asset it\n"
     "# reads (80+ times a second while loading), each open slow on the SD card.",
     CFG_BOOL, NULL, &g_cfg.obb_shared},
    {"performance", "read_buffer_kb", "32",
     "Read buffer of the game's other files, in KB (each refill is one request\n"
     "# to the SD card). 0: the C library's own, 1 KB.",
     CFG_INT, "0,16,32,64,128,256,512", &g_cfg.read_buf_kb},
    {"audio", "mix_at_48khz", "true",
     "Mix the game's audio at 48 kHz, as phones do. Off: 24 kHz, the Android\n"
     "# fallback's rate (muffled music, harsh effects).",
     CFG_BOOL, NULL, &g_cfg.mix_48k},
    {"audio", "real_voices_64", "true",
     "64 sounds heard at once instead of the game's 32 (past that, FMOD silences\n"
     "# the quietest).",
     CFG_BOOL, NULL, &g_cfg.voices64},
    CFG_ROW_GL_SELFTEST(&g_cfg.gl_selftest),
    {"debug", "profile_long_frames", "false",
     "Write where the time goes in frames over 100 ms (loading) to debug.log.\n"
     "# It slows those frames down a little: leave off unless asked for a log.",
     CFG_BOOL, NULL, &g_cfg.profile},
    {"debug", "profile_continuous", "false",
     "Sample every thread every 5 ms, all the time, and write where the time\n"
     "# went to debug.log every 10 s (native code, call sites, C# methods). For\n"
     "# finding what limits the frame rate; costs a little of it.",
     CFG_BOOL, NULL, &g_cfg.profile_window},
    CFG_ROW_BOOT_LOG("Show the start-up log on screen at every launch. Off: the screen stays\n"
                     "# dark until the game draws, and the log appears only while something is\n"
                     "# being set up or updated (first launch, a new game.apk or NRO).",
                     &g_cfg.boot_log),
    {"debug", "log_buttons", "false",
     "Write to debug.log which Unity joystick button each controller button\n"
     "# becomes (for mapping the controls).",
     CFG_BOOL, NULL, &g_cfg.log_buttons},
    {"debug", "minus_screenshot", "false",
     "The - button saves a screenshot of the game's picture (capture-NNN.bmp\n"
     "# here) instead of being the game's Select (the strategic view).",
     CFG_BOOL, NULL, &g_cfg.minus_capture},
    {"debug", "log_sounds", "false",
     "Write every sound the game plays (and its volume faders) to debug.log\n"
     "# (for a bug report about audio).",
     CFG_BOOL, NULL, &g_cfg.log_sounds},
    /* [config] version = 1: the engine's row, last (CfgTable.version) */
};

static void apply(void) {
  const RtConfig *rt = rt_config();
  g_cfg.res_w = rt->res_w;
  g_cfg.res_h = rt->res_h;
  g_cfg.msaa = g_aa_choice == 0 ? -1 : g_aa_choice == 1 ? 0 : g_aa_choice == 2 ? 2 : 4;
  int docked = appletGetOperationMode() == AppletOperationMode_Console;
  debugPrintf("[config] %dx%d (%s, %s), textures 1/%d, MSAA %s, CPU boost %s, profiler %s, sound log %s\n",
              g_cfg.res_w, g_cfg.res_h, rt_config_get("display", "resolution"), docked ? "docked" : "handheld",
              1 << g_cfg.tex_limit, rt_config_get("graphics", "antialiasing"), g_cfg.boost ? "on" : "off",
              g_cfg.profile ? "on" : "off", g_cfg.log_sounds ? "on" : "off");
}

static const CfgTable k_table = {
    .opts = k_opts,
    .nopts = CFG_COUNT(k_opts),
    .version = 1,
    .apply = apply,
};

void dcr_config_load(void) { rt_config_load(&k_table); }

void port_config_new_file(void) {}
