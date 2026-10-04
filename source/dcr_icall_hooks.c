/* dcr_icall_hooks.c -- wrap selected Unity internal calls (icalls) by name.
 *
 * Every icall reaches Mono through mono_add_internal_call(name, fn), which
 * libunity looks up with dlsym (it links no mono_* symbols). bionic_dl.c hands
 * it add_icall() instead, which substitutes a wrapper for the names below and
 * passes everything on -- whatever way the engine registers them (per-class
 * tables, or direct calls as the UnityWebRequest module does), and with no
 * version-specific offsets.
 *
 * Wrapped here:
 *   AssetBundle::LoadFromMemory(byte[], uint)
 *     Logs the size. Given fewer than 13 bytes -- not even an AssetBundle
 *     header -- Unity's decoder thread polls for more data for ever while the
 *     caller waits on it (hardware 2026-09-23: stuck on the Disney loading
 *     screen; watchdog showed the main thread in this icall and a decoder
 *     thread looping). Such a call now returns null at once: to the C#
 *     caller that is "not a valid asset bundle", which the game handles.
 *   Networking.UnityWebRequest::SetUrl(string)
 *     Logs every request URL (WWW is built on UnityWebRequest in 2017.4), so
 *     the log shows where the game's data requests go.
 *
 *   AudioSource::Play, AudioSource::PlayOneShotHelper
 *     With [audio] important_sounds_first: the game's pooled audio sources
 *     (AudioController's "Pooled Audio Source" objects, which play the hops,
 *     menu clicks and most short game sounds) are given priority 64 before
 *     they play, above the 128 of the car engines and other ambient loops.
 *     FMOD has 32 real voices here; past that it silences the least important
 *     sounds, and at equal priority the least audible -- a hop at 0.3 volume
 *     among engine loops at 0.81. (The census showed priority alone is not
 *     enough: a new sound goes on a real voice only if one is free, so the
 *     same option also gives FMOD 64 real voices -- dcr_fmod.c.)
 *
 * And with config.ini [debug] log_sounds on, the audio: every sound started
 * (AudioSource.Play and PlayOneShot: frame, clip, volume, pitch and the game
 * method behind it, and for one-shots what FMOD made of them -- real or
 * virtual, audibility, voices in use: dcr_fmod.c), a clip given to an
 * AudioSource before its audio data has loaded (the game's AudioController
 * then skips the sound silently), AudioMixer.SetFloat (the game's volume
 * faders, in dB) and AudioListener volume/pause.
 * MIT.
 */
#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "dcr_config.h"
#include "util.h"

/* Mono 2.x, 32-bit: MonoArray = {vtable, sync, bounds, max_length, data...};
 * MonoString = {vtable, sync, int length, uint16 chars[]}. */
static uint32_t mono_array_len(const void *a) { return a ? *(const uint32_t *)((const char *)a + 12) : 0; }

static void mono_string_ascii(const void *s, char *out, size_t cap) {
  size_t o = 0;
  if (s) {
    int32_t len = *(const int32_t *)((const char *)s + 8);
    const uint16_t *c = (const uint16_t *)((const char *)s + 12);
    for (int32_t i = 0; i < len && o + 1 < cap; i++)
      out[o++] = c[i] < 0x80 ? (char)c[i] : '?';
  }
  out[o] = 0;
}

typedef void *(*load_from_memory_fn)(void *bytes, uint32_t crc);
static load_from_memory_fn o_LoadFromMemory;
static void *w_LoadFromMemory(void *bytes, uint32_t crc) {
  uint32_t len = mono_array_len(bytes);
  debugPrintf("[bundle] AssetBundle.LoadFromMemory(%lu bytes, crc %lu)\n", (unsigned long)len,
              (unsigned long)crc);
  if (len < 13) {
    debugPrintf("[bundle]   too short for an AssetBundle header: returning null (Unity's "
                "decoder would wait for more data for ever)\n");
    return NULL;
  }
  return o_LoadFromMemory(bytes, crc);
}

typedef int (*set_url_fn)(void *self, void *url);
static set_url_fn o_SetUrl;
static int w_SetUrl(void *self, void *url) {
  char buf[400];
  mono_string_ascii(url, buf, sizeof buf);
  static int logged;
  if (logged++ < 64)
    debugPrintf("[web] UnityWebRequest url = %s\n", buf);
  return o_SetUrl(self, url);
}

/* ------------------------------------------------ [debug] log_sounds */
uint64_t dcr_boot_frames(void);                       /* dcr_boot.c */
int dcr_mono_callers(char *out, size_t cap, int max); /* mono_rt.c */

/* icalls called, not wrapped: instance methods on the managed wrapper */
typedef void *(*obj_fn)(void *self);
typedef int (*int_fn)(void *self);
typedef float (*float_fn)(void *self);
typedef void (*set_int_fn)(void *self, int v);
static obj_fn i_get_name, i_get_clip;
static int_fn i_load_state, i_get_priority;
static float_fn i_get_volume, i_get_pitch;
static set_int_fn i_set_priority;
void dcr_fmod_one_shots(void *self, char *out, size_t cap); /* dcr_fmod.c */
void dcr_fmod_census(char *out, size_t cap);
void dcr_fmod_main_channel(void *self, char *out, size_t cap);

#define SOUND_LINES 4000
static int g_sound_lines;

/* A UnityEngine.Object whose native object is gone (m_CachedPtr, the first
 * field after the 8-byte MonoObject header, is 0) must not reach an icall:
 * that raises a managed exception. */
static int alive(const void *o) { return o && *(void *const *)((const char *)o + 8); }

static void obj_name(void *o, char *out, size_t cap) {
  if (!o)
    snprintf(out, cap, "(none)");
  else if (!alive(o) || !i_get_name)
    snprintf(out, cap, "(destroyed)");
  else
    mono_string_ascii(i_get_name(o), out, cap);
}

static const char *load_state_name(int s) {
  static const char *n[] = {"unloaded", "loading", "loaded", "FAILED"};
  return s >= 0 && s < 4 ? n[s] : "?";
}

static void log_start(const char *what, void *src, void *clip, int one_shot, float scale) {
  if (!dcr_config()->log_sounds || g_sound_lines >= SOUND_LINES)
    return;
  g_sound_lines++;
  char name[96], who[320];
  obj_name(clip, name, sizeof name);
  if (!dcr_mono_callers(who, sizeof who, 3))
    snprintf(who, sizeof who, "?");
  float vol = i_get_volume ? i_get_volume(src) : -1.0f, pitch = i_get_pitch ? i_get_pitch(src) : -1.0f;
  int st = alive(clip) && i_load_state ? i_load_state(clip) : -1;
  char scale_s[24] = "";
  if (one_shot)
    snprintf(scale_s, sizeof scale_s, " x%.2f", (double)scale);
  int prio = i_get_priority ? i_get_priority(src) : -1;
  debugPrintf("[sound] f%llu %s %s%s (vol %.2f, pitch %.2f, priority %d%s%s) <- %s\n",
              (unsigned long long)dcr_boot_frames(), what, name, scale_s, (double)vol, (double)pitch, prio,
              st == 2 || st < 0 ? "" : ", clip ", st == 2 || st < 0 ? "" : load_state_name(st), who);
  static char fm[400], census[360];
  if (one_shot)
    dcr_fmod_one_shots(src, fm, sizeof fm);
  else
    dcr_fmod_main_channel(src, fm, sizeof fm);
  census[0] = 0;
  if (one_shot)
    dcr_fmod_census(census, sizeof census);
  if (fm[0] || census[0])
    debugPrintf("[fmod]   %s%s%s\n", fm[0] ? fm : "?", census[0] ? " | " : "", census);
}

/* [audio] important_sounds_first: AudioController's pooled sources at 64. */
#define POOL_PRIORITY 64
static void prioritize(void *self) {
  if (!dcr_config()->sound_priority || !alive(self) || !i_get_priority || !i_set_priority || !i_get_name)
    return;
  if (i_get_priority(self) == POOL_PRIORITY)
    return;
  char n[48];
  obj_name(self, n, sizeof n);
  if (strncmp(n, "Pooled Audio Source", 19))
    return;
  i_set_priority(self, POOL_PRIORITY);
  static int told;
  if (!told++)
    debugPrintf("[sound] the game's pooled audio sources (hops, clicks, short sounds) play at priority %d, "
                "above the ambient loops' 128\n", POOL_PRIORITY);
}

typedef void (*play_fn)(void *self, uint64_t delay);
static play_fn o_Play;
static void w_Play(void *self, uint64_t delay) {
  prioritize(self);
  o_Play(self, delay);
  log_start("play", self, i_get_clip ? i_get_clip(self) : NULL, 0, 0);
}

typedef void (*one_shot_fn)(void *self, void *clip, float scale);
static one_shot_fn o_PlayOneShotHelper;
static void w_PlayOneShotHelper(void *self, void *clip, float scale) {
  prioritize(self);
  o_PlayOneShotHelper(self, clip, scale);
  log_start("one-shot", self, clip, 1, scale);
}

/* The game plays a pooled source only once its clip has loaded; say when not. */
typedef void (*set_clip_fn)(void *self, void *clip);
static set_clip_fn o_set_clip;
static void w_set_clip(void *self, void *clip) {
  o_set_clip(self, clip);
  if (!alive(clip) || !i_load_state || g_sound_lines >= SOUND_LINES)
    return;
  int st = i_load_state(clip);
  if (st != 2) {
    char name[96];
    obj_name(clip, name, sizeof name);
    g_sound_lines++;
    debugPrintf("[sound] f%llu clip %s given to a source while %s: a PlaySound of it is silent\n",
                (unsigned long long)dcr_boot_frames(), name, load_state_name(st));
  }
}

/* The mixer's faders: logged at their ends (0 / -80 dB) and every 10 dB in
 * between, per parameter, so a 0.5 s fade is a few lines, not thirty. */
typedef int (*set_float_fn)(void *self, void *name, float value);
static set_float_fn o_SetFloat;
static int w_SetFloat(void *self, void *name, float value) {
  int ok = o_SetFloat(self, name, value);
  static struct {
    char key[40];
    float v;
  } seen[16];
  char key[40];
  mono_string_ascii(name, key, sizeof key);
  int i = 0;
  while (i < 16 && seen[i].key[0] && strcmp(seen[i].key, key))
    i++;
  int fresh = i < 16 && !seen[i].key[0];
  if (i < 16 && (fresh || (value != seen[i].v && (value <= -79.9f || value >= -0.1f ||
                                                  value - seen[i].v >= 10.0f || seen[i].v - value >= 10.0f)))) {
    snprintf(seen[i].key, sizeof seen[i].key, "%s", key);
    seen[i].v = value;
    if (g_sound_lines++ < SOUND_LINES)
      debugPrintf("[mixer] f%llu %s = %.1f dB%s\n", (unsigned long long)dcr_boot_frames(), key, (double)value,
                  (ok & 0xff) ? "" : " (no such parameter)");
  }
  return ok;
}

typedef void (*listener_volume_fn)(float v);
static listener_volume_fn o_listener_volume;
static void w_listener_volume(float v) {
  o_listener_volume(v);
  debugPrintf("[mixer] f%llu AudioListener.volume = %.2f\n", (unsigned long long)dcr_boot_frames(), (double)v);
}

typedef void (*listener_pause_fn)(int paused);
static listener_pause_fn o_listener_pause;
static void w_listener_pause(int paused) {
  o_listener_pause(paused);
  debugPrintf("[mixer] f%llu AudioListener.pause = %d\n", (unsigned long long)dcr_boot_frames(), paused & 0xff);
}

/* sh_quality.c: the graphics settings held against the game's own */
extern void *sh_i_GetKeyInt; /* sh_padprobe.c */
extern void *sh_o_SetQualityLevel, *sh_i_set_masterTextureLimit, *sh_i_set_antiAliasing, *sh_i_GetQualityLevel;
void sh_w_SetQualityLevel(int index, int apply_expensive);

static const struct {
  const char *name;
  void *wrapper; /* NULL: only remembered, for calling */
  void **orig;
  int sounds; /* 1: only with [debug] log_sounds; 2: with it or [audio] important_sounds_first */
} g_wrap[] = {
    {"UnityEngine.AssetBundle::LoadFromMemory", (void *)w_LoadFromMemory, (void **)&o_LoadFromMemory, 0},
    {"UnityEngine.Networking.UnityWebRequest::SetUrl", (void *)w_SetUrl, (void **)&o_SetUrl, 0},
    {"UnityEngine.AudioSource::Play", (void *)w_Play, (void **)&o_Play, 2},
    {"UnityEngine.AudioSource::PlayOneShotHelper", (void *)w_PlayOneShotHelper, (void **)&o_PlayOneShotHelper, 2},
    {"UnityEngine.AudioSource::set_clip", (void *)w_set_clip, (void **)&o_set_clip, 1},
    {"UnityEngine.Audio.AudioMixer::SetFloat", (void *)w_SetFloat, (void **)&o_SetFloat, 1},
    {"UnityEngine.AudioListener::set_volume", (void *)w_listener_volume, (void **)&o_listener_volume, 1},
    {"UnityEngine.AudioListener::set_pause", (void *)w_listener_pause, (void **)&o_listener_pause, 1},
    {"UnityEngine.Input::GetKeyInt", NULL, (void **)&sh_i_GetKeyInt, 0},
    {"UnityEngine.QualitySettings::SetQualityLevel", (void *)sh_w_SetQualityLevel, (void **)&sh_o_SetQualityLevel, 0},
    {"UnityEngine.QualitySettings::set_masterTextureLimit", NULL, (void **)&sh_i_set_masterTextureLimit, 0},
    {"UnityEngine.QualitySettings::set_antiAliasing", NULL, (void **)&sh_i_set_antiAliasing, 0},
    {"UnityEngine.QualitySettings::GetQualityLevel", NULL, (void **)&sh_i_GetQualityLevel, 0},
    {"UnityEngine.Object::get_name", NULL, (void **)&i_get_name, 0},
    {"UnityEngine.AudioSource::get_priority", NULL, (void **)&i_get_priority, 0},
    {"UnityEngine.AudioSource::set_priority", NULL, (void **)&i_set_priority, 0},
    {"UnityEngine.AudioSource::get_clip", NULL, (void **)&i_get_clip, 1},
    {"UnityEngine.AudioSource::get_volume", NULL, (void **)&i_get_volume, 1},
    {"UnityEngine.AudioSource::get_pitch", NULL, (void **)&i_get_pitch, 1},
    {"UnityEngine.AudioClip::get_loadState", NULL, (void **)&i_load_state, 1},
};

typedef void (*add_icall_fn)(const char *name, const void *fn);
static add_icall_fn g_real_add;

/* Names may carry a "(signature)" suffix: compare up to it. */
static int same_icall(const char *reg, const char *want) {
  size_t n = strlen(want);
  return !strncmp(reg, want, n) && (reg[n] == 0 || reg[n] == '(');
}

static void add_icall(const char *name, const void *fn) {
  for (unsigned i = 0; i < sizeof g_wrap / sizeof g_wrap[0]; i++)
    if (name && same_icall(name, g_wrap[i].name) && fn != g_wrap[i].wrapper &&
        (!g_wrap[i].sounds || dcr_config()->log_sounds ||
         (g_wrap[i].sounds == 2 && dcr_config()->sound_priority))) {
      *g_wrap[i].orig = (void *)fn;
      if (g_wrap[i].wrapper) {
        fn = g_wrap[i].wrapper;
        debugPrintf("[icall] wrapped %s\n", name);
      }
      break;
    }
  g_real_add(name, fn);
}

void *dcr_ilpatch_interpose(const char *sym, void *real); /* dcr_ilpatch.c */

/* bionic_dl.c: every dlsym into libmono passes through here. */
void *port_import_interpose(const char *sym, void *real) {
  real = dcr_ilpatch_interpose(sym, real);
  if (real && !strcmp(sym, "mono_add_internal_call")) {
    g_real_add = (add_icall_fn)real;
    return (void *)add_icall;
  }
  return real;
}
