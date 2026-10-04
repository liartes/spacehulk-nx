/* dcr_fmod.c -- what FMOD (inside libunity) is actually doing with a sound.
 *
 * Unity 2017.4 mixes through FMOD Ex 4.4, linked into libunity: 32 real
 * voices (the project's "Real Voices"), 512 virtual. When more than 32 sounds
 * play, FMOD silences ("virtualises") the least important: lowest priority
 * first, then least audible. A sound the game started, whose clip is loaded,
 * that nobody hears -- the hop, hardware 2026-09-24 -- is either virtual, or
 * never got a channel. This file looks, for the [debug] log_sounds log:
 *
 *   dcr_fmod_one_shots(src)  the one-shots now playing on an AudioSource:
 *                            real / VIRTUAL / stopped, and their audibility
 *   dcr_fmod_census()        channels playing, how many are virtual, the
 *                            loudest audibilities
 *
 * And, with [audio] important_sounds_first, dcr_fmod_patch_voices() gives FMOD
 * 64 real voices instead of the project's 32. The census of hardware
 * 2026-09-24 had 25-45 channels playing in a run (52 at most) -- car engines,
 * crowds, rivers, most of them nearly silent at a distance -- so up to 20
 * sounds were virtual at once, and 74 of 166 hops started virtual: FMOD Ex
 * puts a new sound on a real voice only if one is free, whatever its priority,
 * and a 74 ms hop is over before the next re-sort. The count is the one
 * argument AudioManager::InitNormal passes to System::setSoftwareChannels
 * (`ldr r1, [r4, #188]`, the project setting), which becomes `mov r1, #64`.
 *
 * And, with [audio] mix_at_48khz, dcr_fmod_patch_rate() has FMOD mix at 48 kHz
 * -- what a phone does. Unity's Android audio picks FMOD's OpenSL output on
 * phones with low-latency audio (AndroidAudio::GetAndroidAudioOutputType), and
 * OpenSL's init sets FMOD's mix rate to the device's native rate (48 kHz;
 * OutputOpenSL::getDeviceNativeParams). This port has no OpenSL, so Unity
 * falls back to the Java AudioTrack output, which keeps FMOD Ex's built-in
 * default, 24 kHz (SystemI::SystemI). Every 44.1 kHz clip was then resampled
 * down by FMOD's linear resampler, folding everything from 12 to 22 kHz back
 * into the audible band almost unattenuated (bells, hits, trains, crowds: harsh
 * and loud), music lost everything above 12 kHz (muffled), and dcr_audio.c's
 * 24->48 kHz linear upsampling added images of its own. The project's sample
 * rate setting is 0 ("default"): AudioManager::InitNormal loads it
 * (`ldr r5, [r4, #308]`) and passes it to System::setSoftwareFormat when it is
 * not 0; that load becomes `movw r5, #48000`. (If FMOD refused 48000, Unity
 * retries with the default by itself.) At 48 kHz dcr_audio.c passes FMOD's
 * blocks straight through.
 *
 * The functions called are libunity's own, located in the game's build by
 * matching the symbolised Unity 2017.4.17f1 reference (FMOD sits at a fixed
 * offset, -0x10e0, from the reference; Unity's audio code at -0x2260), each
 * checked against its first four instructions before use. The structure
 * offsets are read off the same code (AudioSource::Stop, SoundChannelInstance::
 * isVirtual, FMOD::SystemI::updateChannels, AudioManager::InitNormal), whose
 * instruction words are checked too. Main thread only (Unity's audio API
 * asserts it). MIT.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "so_util.h"
#include "util.h"

extern so_module unity_mod;

size_t dcr_readable(uint32_t p, size_t want); /* exc_handler.c */

typedef struct {
  uint32_t rva;
  uint32_t guard[4];
} Site;

/* functions */
static const Site k_sci_is_virtual = {0x1c6018, {0xe92d4830, 0xe24dd030, 0xe1a05000, 0xe59f0110}};
static const Site k_sci_is_playing = {0x1c4e44, {0xe92d4830, 0xe24dd030, 0xe1a04000, 0xe59f010c}};
static const Site k_ci_validate = {0xb28edc, {0xe92d4df0, 0xe28db018, 0xe24dd008, 0xe1a05000}};
static const Site k_ci_is_playing = {0xb29dd0, {0xe92d48f0, 0xe28db010, 0xe24dd008, 0xe1a05001}};
static const Site k_ci_is_virtual = {0xb29fa8, {0xe1a02000, 0xe3a00025, 0xe3510000, 0x012fff1e}};
static const Site k_ci_audibility = {0xb2eb34, {0xe3a02001, 0xeaffebd4, 0xe1a02000, 0xe3a00024}};
static const Site k_get_audio_manager = {0x192f9c, {0xe92d4800, 0xe3a00003, 0xeb095ec1, 0xe8bd8800}};
/* the instructions the structure offsets come from */
static const Site k_off_oneshots = {0x195960, {0xe59872a0, 0xe2884fa7, 0, 0}};           /* +672, +668 */
static const Site k_off_walk = {0x195974, {0xe5975008, 0xe5977004, 0xe1a06005, 0xe5b6000c}};
static const Site k_off_channel = {0x1c6044, {0xe595009c, 0, 0, 0}};                     /* +156 */
static const Site k_off_used = {0xaaa9d4, {0xe28a4f5e, 0xe59a617c, 0, 0}};               /* +376 +380 */
static const Site k_off_system = {0x19a4a4, {0xe594008c, 0, 0, 0}};                     /* +140 */
/* ldr r0,[r4,#140]; ldr r1,[r4,#188]; bl System::setSoftwareChannels; mov r1,r0 */
static const Site k_voices = {0x19a4a4, {0xe594008c, 0xe59410bc, 0xeb25b961, 0xe1a01000}};
/* Unity 5.6.4f1 (Disney Crossy Road: SEA): the same code, the fields moved --
 * ldr r0,[r4,#136]; ldr r1,[r4,#184]; bl setSoftwareChannels; mov r1,r0 */
static const Site k_voices56 = {0x81ee74, {0xe5940088, 0xe59410b8, 0xeb0ce797, 0xe1a01000}};
/* Unity 5.3.4f1 (Space Hulk): ldr r0,[r4,#132]; ldr r1,[r4,#180]; bl setSoftwareChannels; mov r1,r0 */
static const Site k_voices53 = {0x61f65c, {0xe5940084, 0xe59410b4, 0xeb0ac771, 0xe1a01000}};
#define REAL_VOICES 64
static int g_voices = 32;
/* ldr r5,[r4,#308]; mov r6,#8; ldr r0,[r4,#140]; ldr r2,[sp,#24] */
static const Site k_rate = {0x19a428, {0xe5945134, 0xe3a06008, 0xe594008c, 0xe59d2018}};
/* 5.6.4f1: ldr r7,[r4,#220]; mov r6,#8; ldr r0,[r4,#136]; mov r5,#0 -- r7 the rate */
static const Site k_rate56 = {0x81edf4, {0xe59470dc, 0xe3a06008, 0xe5940088, 0xe3a05000}};
/* 5.3.4f1 (Space Hulk): ldr r7,[r4,#192]; mov r6,#8; ldr r0,[r4,#132]; mov r5,#0 -- r7 the rate */
static const Site k_rate53 = {0x61f5dc, {0xe59470c0, 0xe3a06008, 0xe5940084, 0xe3a05000}};
/* the main channel (AudioSource+696), a channel's 3D position (ChannelI+360),
 * FMOD's listener 0 position (SystemI+21116; SystemI::set3DListenerAttributes:
 * `movw r7, #21116; add r5, r0, r6, lsl #4; movw r6, #21128`) */
static const Site k_off_main = {0x1958c0, {0xe59862b8, 0xe3560000, 0, 0}};
static const Site k_off_chan3d = {0xb2bd10, {0xed910a00, 0xe2843f5a, 0xed941a5a, 0}};
static const Site k_off_listener = {0xab4378, {0xe305727c, 0xe0805206, 0xe3056288, 0}};

typedef int (*sci_bool_fn)(void *inst, uint8_t *out);
typedef int (*ci_validate_fn)(void *handle, void **ci);
typedef int (*ci_bool_fn)(void *ci, uint8_t *out);
typedef int (*ci_float_fn)(void *ci, float *out);
typedef void *(*get_am_fn)(void);

static sci_bool_fn sci_is_virtual, sci_is_playing;
static ci_validate_fn ci_validate;
static ci_bool_fn ci_is_playing, ci_is_virtual;
static ci_float_fn ci_audibility;
static get_am_fn get_audio_manager;
static int g_state; /* 0 untried, 1 ready, -1 unavailable */

static int site_ok(const Site *s, int words) {
  const uint32_t *p = (const uint32_t *)((uintptr_t)unity_mod.load_virtbase + s->rva);
  for (int i = 0; i < words; i++)
    if (p[i] != s->guard[i])
      return 0;
  return 1;
}

static void *site_fn(const Site *s) { return (void *)((uintptr_t)unity_mod.load_virtbase + s->rva); }

static int ready(void) {
  if (g_state)
    return g_state > 0;
  g_state = -1;
  if (!unity_mod.load_virtbase)
    return 0;
  const struct {
    const Site *s;
    int words;
    const char *what;
  } checks[] = {
      {&k_sci_is_virtual, 4, "SoundChannelInstance::isVirtual"},
      {&k_sci_is_playing, 4, "SoundChannelInstance::isPlaying"},
      {&k_ci_validate, 4, "FMOD::ChannelI::validate"},
      {&k_ci_is_playing, 4, "FMOD::ChannelI::isPlaying"},
      {&k_ci_is_virtual, 4, "FMOD::ChannelI::isVirtual"},
      {&k_ci_audibility, 4, "FMOD::ChannelI::getAudibility"},
      {&k_get_audio_manager, 4, "GetAudioManager"},
      {&k_off_oneshots, 2, "AudioSource one-shot list"},
      {&k_off_walk, 4, "AudioSource one-shot walk"},
      {&k_off_channel, 1, "SoundChannelInstance channel"},
      {&k_off_used, 2, "FMOD used-channel list"},
      {&k_off_system, 1, "AudioManager FMOD system"}, /* word 2 is the patched voice count */
      {&k_off_main, 2, "AudioSource main channel"},
      {&k_off_chan3d, 3, "FMOD channel 3D position"},
      {&k_off_listener, 3, "FMOD listener position"},
  };
  for (unsigned i = 0; i < sizeof checks / sizeof checks[0]; i++)
    if (!site_ok(checks[i].s, checks[i].words)) {
      debugPrintf("[fmod] %s is not where Unity 2017.4.17f1 has it (libunity+0x%lx): no FMOD details\n",
                  checks[i].what, (unsigned long)checks[i].s->rva);
      return 0;
    }
  sci_is_virtual = (sci_bool_fn)site_fn(&k_sci_is_virtual);
  sci_is_playing = (sci_bool_fn)site_fn(&k_sci_is_playing);
  ci_validate = (ci_validate_fn)site_fn(&k_ci_validate);
  ci_is_playing = (ci_bool_fn)site_fn(&k_ci_is_playing);
  ci_is_virtual = (ci_bool_fn)site_fn(&k_ci_is_virtual);
  ci_audibility = (ci_float_fn)site_fn(&k_ci_audibility);
  get_audio_manager = (get_am_fn)site_fn(&k_get_audio_manager);
  g_state = 1;
  return 1;
}

void dcr_fmod_patch_rate(void) {
  const Site *site = &k_rate;
  uint32_t insn = 0xE30B5B80u; /* movw r5, #48000 */
  if (unity_mod.load_virtbase && !site_ok(&k_rate, 4) && site_ok(&k_rate56, 4)) {
    site = &k_rate56;
    insn = 0xE30B7B80u; /* movw r7, #48000 */
  } else if (unity_mod.load_virtbase && !site_ok(&k_rate, 4) && site_ok(&k_rate53, 4)) {
    site = &k_rate53;
    insn = 0xE30B7B80u; /* movw r7, #48000 */
  }
  if (!unity_mod.load_virtbase || !site_ok(site, 4)) {
    debugPrintf("[fmod] AudioManager::InitNormal is not as expected (libunity+0x%lx): FMOD keeps its "
                "24 kHz mix\n", (unsigned long)k_rate.rva);
    return;
  }
  if (so_patch_code((void *)((uintptr_t)unity_mod.load_virtbase + site->rva), &insn, 4) != 0) {
    debugPrintf("[fmod] mix rate: write failed\n");
    return;
  }
  debugPrintf("[fmod] FMOD mixes at 48 kHz, as on a phone (the AudioTrack fallback's 24 kHz dulled the "
              "music and made bright effects harsh)\n");
}

void dcr_fmod_patch_voices(void) {
  const Site *site = &k_voices;
  if (unity_mod.load_virtbase && !site_ok(&k_voices, 4) && site_ok(&k_voices56, 4))
    site = &k_voices56;
  else if (unity_mod.load_virtbase && !site_ok(&k_voices, 4) && site_ok(&k_voices53, 4))
    site = &k_voices53;
  if (!unity_mod.load_virtbase || !site_ok(site, 4)) {
    debugPrintf("[fmod] AudioManager::InitNormal is not as expected (libunity+0x%lx): FMOD keeps the "
                "project's real voices\n", (unsigned long)k_voices.rva);
    return;
  }
  const uint32_t insn = 0xE3A01000u | REAL_VOICES; /* mov r1, #64 */
  if (so_patch_code((void *)((uintptr_t)unity_mod.load_virtbase + site->rva + 4), &insn, 4) != 0) {
    debugPrintf("[fmod] real voices: write failed\n");
    return;
  }
  g_voices = REAL_VOICES;
  debugPrintf("[fmod] FMOD gets %d real voices (the project's 32 left ambient loops and hops silent)\n",
              REAL_VOICES);
}

static uintptr_t rd(uintptr_t p) { return p && dcr_readable((uint32_t)p, 4) >= 4 ? *(volatile uintptr_t *)p : 0; }

static float rdf(uintptr_t p) {
  uintptr_t v = rd(p);
  float f;
  memcpy(&f, &v, sizeof f);
  return f;
}

/* FMOD's listener 0 position, from AudioManager's FMOD system. */
static int listener_pos(float *xyz) {
  uintptr_t am = (uintptr_t)get_audio_manager();
  uintptr_t sys = am ? rd(am + 140) : 0;
  if (!sys)
    return 0;
  for (int i = 0; i < 3; i++)
    xyz[i] = rdf(sys + 21116 + 4 * (uintptr_t)i);
  return 1;
}

/* One Unity SoundChannelInstance: "real 0.30", "VIRTUAL 0.30", "stopped". */
static int describe_instance(uintptr_t inst, char *out, size_t cap) {
  uint8_t virt = 0, playing = 0;
  if (!inst || sci_is_playing((void *)inst, &playing) != 0)
    return snprintf(out, cap, "no channel");
  sci_is_virtual((void *)inst, &virt);
  float aud = -1.0f;
  uintptr_t handle = rd(inst + 156);
  void *ci = NULL;
  if (handle && ci_validate((void *)handle, &ci) == 0 && ci)
    ci_audibility(ci, &aud);
  if (!playing)
    return snprintf(out, cap, "stopped");
  float l[3], d = -1.0f;
  if (ci && listener_pos(l)) {
    float dx = rdf((uintptr_t)ci + 360) - l[0], dy = rdf((uintptr_t)ci + 364) - l[1],
          dz = rdf((uintptr_t)ci + 368) - l[2];
    d = sqrtf(dx * dx + dy * dy + dz * dz);
  }
  return snprintf(out, cap, "%s, audibility %.2f, %.1f from the listener", virt ? "VIRTUAL (silent)" : "real",
                  (double)aud, (double)d);
}

/* The one-shots playing on the AudioSource behind managed wrapper `self`. */
void dcr_fmod_one_shots(void *self, char *out, size_t cap) {
  out[0] = 0;
  if (!ready() || !self)
    return;
  uintptr_t src = rd((uintptr_t)self + 8); /* m_CachedPtr: the native AudioSource */
  if (!src)
    return;
  uintptr_t sentinel = src + 668, node = rd(src + 672);
  int n = 0;
  size_t o = 0;
  while (node && node != sentinel && n < 8) {
    uintptr_t data = rd(node + 8), holder = data ? rd(data + 12) : 0;
    uintptr_t inst = holder ? rd(holder + 12) : 0;
    if (n < 3 && o < cap) {
      int k = snprintf(out + o, cap - o, "%s", n ? "; " : "");
      if (k > 0 && (size_t)k < cap - o)
        o += (size_t)k;
      k = describe_instance(inst, out + o, cap - o);
      if (k > 0 && (size_t)k < cap - o)
        o += (size_t)k;
    }
    n++;
    node = rd(node + 4);
  }
  if (!n)
    snprintf(out, cap, "no one-shot playing (it got no channel)");
  else if (n > 3 && o < cap)
    snprintf(out + o, cap - o, " (+%d more)", n - 3);
}

/* The main (Play) channel of the AudioSource behind managed wrapper `self`. */
void dcr_fmod_main_channel(void *self, char *out, size_t cap) {
  out[0] = 0;
  if (!ready() || !self)
    return;
  uintptr_t src = rd((uintptr_t)self + 8);
  uintptr_t holder = src ? rd(src + 696) : 0;
  describe_instance(holder ? rd(holder + 12) : 0, out, cap);
}

/* Voices: real in use, channels playing, virtual, loudest audibilities. */
void dcr_fmod_census(char *out, size_t cap) {
  out[0] = 0;
  if (!ready())
    return;
  uintptr_t am = (uintptr_t)get_audio_manager();
  uintptr_t sys = am ? rd(am + 140) : 0;
  if (!sys)
    return;
  int chans = 0, playing = 0, virt = 0, loud = 0;
  float top[5] = {0};
  uintptr_t sentinel = sys + 376, ci = rd(sys + 380);
  if (ci)
    ci -= 4;
  while (ci && ci != sentinel && chans < 1024) {
    uintptr_t next = rd(ci + 4);
    uint8_t p = 0, v = 0;
    float a = 0.0f;
    chans++;
    if (ci_is_playing((void *)ci, &p) == 0 && p) {
      playing++;
      if (ci_is_virtual((void *)ci, &v) == 0 && v)
        virt++;
      if (ci_audibility((void *)ci, &a) == 0) {
        if (a >= 0.3f)
          loud++;
        for (int k = 0; k < 5; k++)
          if (a > top[k]) {
            memmove(top + k + 1, top + k, (size_t)(4 - k) * sizeof top[0]);
            top[k] = a;
            break;
          }
      }
    }
    ci = next ? next - 4 : 0;
  }
  float l[3] = {0, 0, 0};
  listener_pos(l);
  snprintf(out, cap, "%d channels playing on %d real voices: %d virtual (silent), %d at audibility >= 0.30; "
           "loudest %.2f %.2f %.2f %.2f %.2f; listener at (%.1f, %.1f, %.1f)",
           playing, g_voices, virt, loud, (double)top[0], (double)top[1], (double)top[2], (double)top[3],
           (double)top[4], (double)l[0], (double)l[1], (double)l[2]);
}
