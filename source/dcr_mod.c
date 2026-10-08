/* dcr_mod.c -- the port's own C# (mod/src, built into mod/dcrmod.dll by
 * mod/build_mod.sh and carried inside this program: mod_blob.S), loaded into
 * the game's Mono domain, and the native services it calls.
 *
 * LOADING: once Unity has loaded the game's assemblies (the first frame), the
 * DLL is opened from memory (mono_image_open_from_data_with_name, the real
 * one -- not the IL patcher's wrapper) and DcrMod.Loader.Init() is invoked on
 * this thread, between two frames: the engine's main thread, where Unity's
 * API may be used. Init adds its MonoBehaviour (DontDestroyOnLoad) and hooks.
 * The mod references Assembly-CSharp by name; Mono binds it to the loaded one.
 *
 * HOOKS ("detours"): DcrMod.Native.Detour(target, replacement, original)
 * takes three MonoMethod pointers (MethodBase.MethodHandle.Value). The target
 * and replacement are compiled (mono_compile_method: the JIT's code for them,
 * in the JIT arena) and the target's first two instructions become
 *     ldr pc, [pc, #-4] ; .word <replacement>
 * so every caller -- direct calls, vtables, delegates, Unity's messages --
 * reaches the replacement, which has the same signature (an instance
 * method's `this` as its first parameter: the same registers). To call the
 * target's own code, `original` (a stub of the same signature, never
 * inlined) is pointed at a trampoline: the two instructions that were
 * overwritten (Mono's ARM prologue, `mov ip, sp; push {...}`: checked to be
 * free of pc-relative forms), then a jump to the rest of the target.
 * A method the JIT inlines into its callers cannot be hooked this way: hooks
 * are for methods with real bodies; tiny ones are IL-patched (dcr_ilpatch.c).
 * The writes go through the arena's writable view and jit_flush, which on the
 * emulator applies them between frames like any JIT patch (jit_arena.c).
 *
 * NATIVE SERVICES (icalls, "DcrMod.Native::<name>"): logging, controllers per
 * player slot (dcr_input.c), where an entry lies inside game.apk (stored
 * entries only: the setup rewrote the APK uncompressed), config.ini values,
 * the controller applet, the monotonic clock. MIT.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "dcr_config.h"
#include "jit_arena.h"
#include "so_util.h"
#include "util.h"

extern so_module mono_mod;
extern const uint8_t dcrmod_dll[];
extern const uint32_t dcrmod_dll_size;
const char *dcr_game_root(void);

typedef void *(*fn_v)(void);
static struct {
  void *(*root_domain)(void);
  void *(*open_named)(char *data, uint32_t len, int need_copy, int *status, int refonly, const char *name);
  void *(*load_from_full)(void *image, const char *fname, int *status, int refonly);
  void *(*get_image)(void *assembly);
  void *(*class_from_name)(void *image, const char *ns, const char *name);
  void *(*method_from_name)(void *klass, const char *name, int nargs);
  void *(*invoke)(void *method, void *obj, void **params, void **exc);
  void (*add_icall)(const char *name, const void *fn);
  void *(*compile)(void *method);
  char *(*to_utf8)(void *mono_string);
  void (*mfree)(void *p);
  void *(*string_new)(void *domain, const char *text);
  char *(*full_name)(void *method, int signature);
} M;

static int g_state; /* 0 waiting, 1 loaded, -1 failed */
static int g_game_loaded;

/* dcr_ilpatch.c: Assembly-CSharp.dll has been handed to Mono */
void dcr_mod_note_image(const char *name) {
  if (name && strstr(name, "Assembly-CSharp.dll") && !strstr(name, "firstpass"))
    g_game_loaded = 1;
}

/* ------------------------------------------------------------ strings */
static void mstr(void *s, char *out, size_t cap) {
  out[0] = 0;
  if (!s)
    return;
  char *u = M.to_utf8(s);
  if (u) {
    snprintf(out, cap, "%s", u);
    M.mfree(u);
  }
}

/* ------------------------------------------------------------- icalls */
static void i_Log(void *s) {
  char *u = s ? M.to_utf8(s) : NULL;
  debugPrintf("[mod] %s\n", u ? u : "(null)");
  if (u)
    M.mfree(u);
}

/* An ARM instruction that may run at another address: no pc operand, no
 * branch, not a load/store of pc. */
static int movable(uint32_t ins) {
  const uint32_t cond = ins >> 28;
  if (cond == 0xF)
    return 0;
  if (((ins >> 25) & 7) == 5) /* b / bl */
    return 0;
  if ((ins & 0x0FFFFFF0) == 0x012FFF10 || (ins & 0x0FFFFFF0) == 0x012FFF30) /* bx / blx reg */
    return 0;
  const uint32_t rn = (ins >> 16) & 15, rd = (ins >> 12) & 15, rm = ins & 15;
  const uint32_t op = (ins >> 25) & 7;
  if (op == 4) /* ldm/stm: pc in the list is a branch (ldm) or odd (stm) */
    return rn != 15 && !(ins & (1u << 15));
  if (op <= 3) {
    if (rn == 15 || rd == 15)
      return 0;
    if (op == 0 && rm == 15)
      return 0;
    return 1;
  }
  return 0; /* coprocessor / anything else: do not guess */
}

static uint32_t *g_tramp; /* one arena page of trampolines */
static int g_tramp_used;

static void write_jump(uint32_t *at, const void *to) {
  uint32_t *w = (uint32_t *)jit_rw(at);
  w[0] = 0xE51FF004; /* ldr pc, [pc, #-4] */
  w[1] = (uint32_t)(uintptr_t)to;
  jit_flush(at, 8);
}

static char *mname(void *m, char *buf, size_t cap) {
  char *n = M.full_name ? M.full_name(m, 0) : NULL;
  snprintf(buf, cap, "%s", n ? n : "?");
  if (n)
    M.mfree(n);
  return buf;
}

static int i_Detour(void *target, void *repl, void *orig) {
  char tn[200], rn[200];
  if (!target || !repl)
    return -1;
  mname(target, tn, sizeof tn);
  mname(repl, rn, sizeof rn);
  uint32_t *t = (uint32_t *)M.compile(target);
  void *r = M.compile(repl);
  if (!t || !r || !jit_contains(t)) {
    debugPrintf("[mod] detour %s: no code (%p -> %p)\n", tn, (void *)t, r);
    return -2;
  }
  if (t[0] == 0xE51FF004) {
    debugPrintf("[mod] detour %s: already hooked\n", tn);
    return -3;
  }
  if (orig) {
    if (!movable(t[0]) || !movable(t[1])) {
      debugPrintf("[mod] detour %s: prologue %08lx %08lx cannot move: not hooked\n", tn,
                  (unsigned long)t[0], (unsigned long)t[1]);
      return -4;
    }
    uint32_t *o = (uint32_t *)M.compile(orig);
    if (!g_tramp)
      g_tramp = (uint32_t *)jit_alloc(0x1000);
    if (!o || !g_tramp || !jit_contains(o) || g_tramp_used + 4 > 0x1000 / 4) {
      debugPrintf("[mod] detour %s: no room for the original's trampoline\n", tn);
      return -5;
    }
    uint32_t *tr = g_tramp + g_tramp_used;
    uint32_t *w = (uint32_t *)jit_rw(tr);
    w[0] = t[0];
    w[1] = t[1];
    w[2] = 0xE51FF004;
    w[3] = (uint32_t)(uintptr_t)(t + 2);
    jit_flush(tr, 16);
    g_tramp_used += 4;
    write_jump(o, tr);
  }
  debugPrintf("[mod] hook %s -> %s (code %p: %08lx %08lx)\n", tn, rn, (void *)t, (unsigned long)t[0],
              (unsigned long)t[1]);
  write_jump(t, r);
  return 0;
}

/* Controllers: slot 0-7 = players 1-8, 8 = the attached Joy-Cons. Returns the
 * style set (0: not connected); buttons are HidNpadButton bits (the test
 * script's included), sticks -1..1 with y up. dcr_input.c */
int dcr_input_slot(int slot, uint64_t *buttons, float *sticks);
static int i_Pad(int slot, uint32_t *buttons_lo, float *lx, float *ly, float *rx, float *ry) {
  uint64_t b = 0;
  float s[4] = {0};
  int st = dcr_input_slot(slot, &b, s);
  *buttons_lo = (uint32_t)b;
  *lx = s[0], *ly = s[1], *rx = s[2], *ry = s[3];
  return st;
}

/* Which slot drives the game's own gamepad (InControl): -1 = the usual rule
 * (player 1, else the attached Joy-Cons, else the first connected). */
void dcr_input_set_game_slot(int slot);
static void i_SetGameSlot(int slot) { dcr_input_set_game_slot(slot); }

/* An entry of game.apk: its data's file offset and size, if stored. */
int dcr_apk_stored_entry(const char *name, uint64_t *offset, uint64_t *size); /* jni_www.c */
static int i_ApkEntry(void *name, int64_t *offset, int64_t *size) {
  char n[300];
  mstr(name, n, sizeof n);
  uint64_t o = 0, s = 0;
  int r = dcr_apk_stored_entry(n, &o, &s);
  *offset = (int64_t)o;
  *size = (int64_t)s;
  return r;
}

/* "section.key" from config.ini, as the port parsed it (dcr_config.c) */
int dcr_config_value(const char *key, int dflt);
static int i_Config(void *key, int dflt) {
  char k[100];
  mstr(key, k, sizeof k);
  return dcr_config_value(k, dflt);
}

/* A reply for the game's next WWW request to exactly this URL (jni_www.c). */
void dcr_www_queue(const char *url, const uint8_t *body, size_t len);
static void i_QueueWww(void *url, void *body) {
  char *u = url ? M.to_utf8(url) : NULL, *b = body ? M.to_utf8(body) : NULL;
  if (u && b)
    dcr_www_queue(u, (const uint8_t *)b, strlen(b));
  if (u)
    M.mfree(u);
  if (b)
    M.mfree(b);
}

static void *i_GameRoot(void) { return M.string_new(M.root_domain(), dcr_game_root()); }

static int64_t i_TicksMs(void) { return (int64_t)(armTicksToNs(armGetSystemTick()) / 1000000ull); }

/* The Switch's controller screen: players min..max (single Joy-Cons held
 * sideways allowed). Blocks until it closes. Returns the players now
 * connected, or -1. */
int dcr_input_controller_applet(int min, int max);
static int i_ControllerApplet(int min, int max) { return dcr_input_controller_applet(min, max); }

/* HD rumble on a slot: amplitude 0..1 for ms milliseconds (low band). */
void dcr_input_rumble(int slot, float amp, int ms);
static void i_Rumble(int slot, float amp, int ms) { dcr_input_rumble(slot, amp, ms); }

/* The title menu is up: the start-up's CPU boost can end (dcr_boost.c). */
void dcr_launch_ready(void);
static void i_LaunchReady(void) { dcr_launch_ready(); }

/* The Switch's profiles (dcr_profile.c): the picker, a nickname, an icon file,
 * all profiles. uids are 32 hex digits; "" is no profile. */
int dcr_profile_pick(const char *excluded, char out[33]);
int dcr_profile_name(const char *hex, char *out, size_t cap);
int dcr_profile_icon(const char *hex, char *out, size_t cap);
int dcr_profile_list(char *out, size_t cap);
static void *i_ProfilePick(void *excluded) {
  char ex[400], out[33];
  mstr(excluded, ex, sizeof ex);
  dcr_profile_pick(ex, out);
  return M.string_new(M.root_domain(), out);
}
static void *i_ProfileName(void *uid) {
  char u[40], out[64];
  mstr(uid, u, sizeof u);
  dcr_profile_name(u, out, sizeof out);
  return M.string_new(M.root_domain(), out);
}
static void *i_ProfileIcon(void *uid) {
  char u[40], out[300];
  mstr(uid, u, sizeof u);
  dcr_profile_icon(u, out, sizeof out);
  return M.string_new(M.root_domain(), out);
}
static void *i_ProfileList(void) {
  char out[400];
  dcr_profile_list(out, sizeof out);
  return M.string_new(M.root_domain(), out);
}

/* ------------------------------------------------------------ loading */
#define SYM(field, name)                                                                   \
  do {                                                                                     \
    *(void **)&M.field = (void *)so_find_addr_rx(&mono_mod, name);                         \
  } while (0)

static int bind_mono(void) {
  SYM(root_domain, "mono_get_root_domain");
  SYM(open_named, "mono_image_open_from_data_with_name");
  SYM(load_from_full, "mono_assembly_load_from_full");
  SYM(get_image, "mono_assembly_get_image");
  SYM(class_from_name, "mono_class_from_name");
  SYM(method_from_name, "mono_class_get_method_from_name");
  SYM(invoke, "mono_runtime_invoke");
  SYM(add_icall, "mono_add_internal_call");
  SYM(compile, "mono_compile_method");
  SYM(to_utf8, "mono_string_to_utf8");
  SYM(mfree, "g_free");
  SYM(string_new, "mono_string_new");
  SYM(full_name, "mono_method_full_name");
  return 0;
}

static void register_icalls(void) {
  static const struct { const char *n; const void *f; } k[] = {
      {"DcrMod.Native::Log", (const void *)i_Log},
      {"DcrMod.Native::Detour", (const void *)i_Detour},
      {"DcrMod.Native::Pad", (const void *)i_Pad},
      {"DcrMod.Native::SetGameSlot", (const void *)i_SetGameSlot},
      {"DcrMod.Native::ApkEntry", (const void *)i_ApkEntry},
      {"DcrMod.Native::Config", (const void *)i_Config},
      {"DcrMod.Native::GameRoot", (const void *)i_GameRoot},
      {"DcrMod.Native::TicksMs", (const void *)i_TicksMs},
      {"DcrMod.Native::ControllerApplet", (const void *)i_ControllerApplet},
      {"DcrMod.Native::Rumble", (const void *)i_Rumble},
      {"DcrMod.Native::LaunchReady", (const void *)i_LaunchReady},
      {"DcrMod.Native::QueueWww", (const void *)i_QueueWww},
      {"DcrMod.Native::ProfilePick", (const void *)i_ProfilePick},
      {"DcrMod.Native::ProfileName", (const void *)i_ProfileName},
      {"DcrMod.Native::ProfileIcon", (const void *)i_ProfileIcon},
      {"DcrMod.Native::ProfileList", (const void *)i_ProfileList},
  };
  for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++)
    M.add_icall(k[i].n, k[i].f);
}

static void *g_tick;
static int g_tick_threw;

static int load(void) {
  bind_mono();
  register_icalls();
  int status = 0;
  /* need_copy: Mono keeps its own copy (the blob is read-only data) */
  void *img = M.open_named((char *)dcrmod_dll, dcrmod_dll_size, 1, &status, 0, "dcrmod.dll");
  if (!img) {
    debugPrintf("[mod] dcrmod.dll (%lu bytes) did not open: status %d\n", (unsigned long)dcrmod_dll_size,
                status);
    return -1;
  }
  void *asm_ = M.load_from_full(img, "dcrmod.dll", &status, 0);
  if (!asm_) {
    debugPrintf("[mod] dcrmod.dll did not load: status %d\n", status);
    return -1;
  }
  void *klass = M.class_from_name(M.get_image(asm_), "DcrMod", "Loader");
  void *init = klass ? M.method_from_name(klass, "Init", 0) : NULL;
  if (!init) {
    debugPrintf("[mod] DcrMod.Loader.Init not found\n");
    return -1;
  }
  g_tick = M.method_from_name(klass, "Tick", 0);
  debugPrintf("[mod] dcrmod.dll loaded (%lu bytes); DcrMod.Loader.Init\n", (unsigned long)dcrmod_dll_size);
  void *exc = NULL;
  M.invoke(init, NULL, NULL, &exc);
  if (exc) {
    debugPrintf("[mod] Init threw (see [mod] lines above)\n");
    return -1;
  }
  return 0;
}

/* dcr_boot.c, after each frame the engine renders */
void dcr_mod_frame(uint64_t frame) {
  /* Loader.Tick every 30 frames: the mod's objects checked from outside
   * Unity's own calls (its MonoBehaviours never logged a Start: run 30) */
  if (g_state == 1 && g_tick && frame % 30 == 0) {
    void *exc = NULL;
    M.invoke(g_tick, NULL, NULL, &exc);
    if (exc && !g_tick_threw++)
      debugPrintf("[mod] Loader.Tick threw\n");
    return;
  }
  if (g_state || !g_game_loaded)
    return;
  if (dcrmod_dll_size == 0) { /* no mod/dcrmod.dll built into this program */
    g_state = -1;
    return;
  }
  if (!dcr_config_value("debug.port_mod", 1)) {
    debugPrintf("[mod] off in config.ini: the port's own C# is not loaded\n");
    g_state = -1;
    return;
  }
  g_state = load() == 0 ? 1 : -1;
}

int dcr_mod_loaded(void) { return g_state == 1; }

/* ------------------------------------------------ JIT map (a debug aid)
 * With a file named "jitlog" in the game folder: every method Mono compiles,
 * every code buffer it makes (trampolines, wrappers) and every method it
 * frees is logged with its address range, through Mono's profiler hooks --
 * to tell what a crash address inside the JIT arena held. */
typedef struct { int dummy; } DcrProf;
static DcrProf g_prof;
static void *(*p_code_start)(void *ji);
static int (*p_code_size)(void *ji);

static void prof_jit_end(void *prof, void *method, void *ji, int result) {
  char n[160];
  if (!ji || result != 0)
    return;
  debugPrintf("[jitlog] +%p %d %s\n", p_code_start(ji), p_code_size(ji), mname(method, n, sizeof n));
}
static void prof_buffer(void *prof, void *buf, int size, int type, void *data) {
  debugPrintf("[jitlog] buf %p %d type %d\n", buf, size, type);
}
static void prof_free(void *prof, void *method) {
  char n[160];
  debugPrintf("[jitlog] free %s\n", mname(method, n, sizeof n));
}

static int g_jitlog;
int dcr_jitlog_on(void) { return g_jitlog; }

void dcr_mod_jitlog_install(void) {
  char path[300];
  snprintf(path, sizeof path, "%s/jitlog", dcr_game_root());
  FILE *f = fopen(path, "r");
  if (!f)
    return;
  fclose(f);
  g_jitlog = 1;
  bind_mono();
  void (*install)(void *, void *) = (void *)so_try_find_addr_rx(&mono_mod, "mono_profiler_install");
  void (*jit_end)(void *) = (void *)so_try_find_addr_rx(&mono_mod, "mono_profiler_install_jit_end");
  void (*buffer)(void *) = (void *)so_try_find_addr_rx(&mono_mod, "mono_profiler_install_code_buffer_new");
  void (*mfree)(void *) = (void *)so_try_find_addr_rx(&mono_mod, "mono_profiler_install_method_free");
  void (*events)(int) = (void *)so_try_find_addr_rx(&mono_mod, "mono_profiler_set_events");
  p_code_start = (void *)so_try_find_addr_rx(&mono_mod, "mono_jit_info_get_code_start");
  p_code_size = (void *)so_try_find_addr_rx(&mono_mod, "mono_jit_info_get_code_size");
  if (!install || !jit_end || !events || !p_code_start || !p_code_size) {
    debugPrintf("[jitlog] profiler API missing\n");
    return;
  }
  install(&g_prof, NULL);
  jit_end((void *)prof_jit_end);
  if (buffer)
    buffer((void *)prof_buffer);
  if (mfree)
    mfree((void *)prof_free);
  events((1 << 4) | (1 << 16)); /* JIT_COMPILATION | METHOD_EVENTS */
  debugPrintf("[jitlog] logging every JIT compile, code buffer and freed method\n");
}
