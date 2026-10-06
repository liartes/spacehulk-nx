/* dcr_audio.c -- FMOD's Android output, played through audout.
 *
 * Unity 2017.4's FMOD has two Android outputs: OpenSL ES (dlopen of
 * libOpenSLES.so, which this port reports absent) and a Java one,
 * org.fmod.FMODAudioDevice, which it then uses. The APK's run() loop is:
 *
 *     rate  = fmodGetInfo(0); block = fmodGetInfo(1)          (frames)
 *     buf   = ByteBuffer.allocateDirect(block * 2 * 2)        (stereo s16)
 *     while (running)
 *         if (fmodGetInfo(3) == 1) { fmodProcess(buf); audioTrack.write(buf); }
 *
 * This file is that loop: FMODAudioDevice.start() (jni_android.c) starts a
 * thread that calls the two natives FMOD registered and writes the PCM to
 * audout, which paces it the way a blocking AudioTrack.write would. The output
 * device is 48 kHz stereo s16; any other FMOD rate is resampled linearly
 * (FMOD mixes at 24 kHz: see the end of these notes).
 *
 * LATENCY: three 1024-frame buffers (21.3 ms each) are queued at most, so a
 * sound is heard at most ~64 ms after FMOD mixed it (four, 85 ms, before);
 * the pump thread runs above the engine's threads and outside the GC's reach.
 * Every 3000 blocks the log carries the pump's own measurements: the rate FMOD
 * is being pulled at (should be FMOD's rate), underruns (audout ran dry) and
 * failed submits (retried, then dropped) -- a hardware log of 2026-09-24 had
 * the pull rate ~5% high, which these tell apart -- and the level of what
 * FMOD mixed since the last line: its peak, and how many samples hit full
 * scale (clipping: the mix is too loud).
 *
 * 24 kHz is FMOD Ex's own default mix rate on Android (the project's sample
 * rate setting is 0, "the platform's"), so phones mix at that rate too.
 *
 * audout's buffer descriptor is an IPC structure with 64-bit fields for every
 * client (Ryujinx and the audio server both read it that way), while libnx32's
 * AudioOutBuffer has 32-bit pointers. So append / get-released are issued here
 * with the right layout rather than through libnx32's wrappers. MIT.
 */
#include <malloc.h>
#include <math.h>
#include <string.h>
#include <switch.h>

#include "dcr_jni_unity.h"
#include "util.h"

typedef struct {
  u64 next, buffer, buffer_size, data_size, data_offset;
} AoBuf;
_Static_assert(sizeof(AoBuf) == 0x28, "audout buffer descriptor");

#define NBUF 3
#define FRAMES_PER_BUF 1024              /* 1024 * 4 bytes = one 0x1000 page */
#define BUF_BYTES (FRAMES_PER_BUF * 4)

static AoBuf g_bufs[NBUF] __attribute__((aligned(16)));
static int16_t *g_pcm[NBUF];
static int g_queued[NBUF];
static int g_ao_ready;
static u32 g_out_rate = 48000;

static Result ao_append(AoBuf *b) {
  u64 tag = (u64)(uintptr_t)b;
  const bool auto_ = hosversionAtLeast(3, 0, 0);
  return serviceDispatchIn(audoutGetServiceSession_AudioOut(), auto_ ? 7 : 3, tag,
                           .buffer_attrs = {auto_ ? (SfBufferAttr_HipcAutoSelect | SfBufferAttr_In)
                                                  : (SfBufferAttr_HipcMapAlias | SfBufferAttr_In)},
                           .buffers = {{b, sizeof(*b)}});
}

static Result ao_released(u64 *tags, u32 max, u32 *count) {
  const bool auto_ = hosversionAtLeast(3, 0, 0);
  return serviceDispatchOut(audoutGetServiceSession_AudioOut(), auto_ ? 8 : 5, *count,
                            .buffer_attrs = {auto_ ? (SfBufferAttr_HipcAutoSelect | SfBufferAttr_Out)
                                                   : (SfBufferAttr_HipcMapAlias | SfBufferAttr_Out)},
                            .buffers = {{tags, max * sizeof(u64)}});
}

/* Mark the buffers the audio server has finished with as free again. */
static void reap(void) {
  u64 tags[NBUF] = {0};
  u32 n = 0;
  if (R_SUCCEEDED(ao_released(tags, NBUF, &n)))
    for (u32 k = 0; k < n && k < NBUF; k++)
      for (int i = 0; i < NBUF; i++)
        if (tags[k] == (u64)(uintptr_t)&g_bufs[i])
          g_queued[i] = 0;
}

/* Index of a free buffer (reaping only when none is), or -1. */
static int free_buffer(void) {
  for (int pass = 0; pass < 2; pass++) {
    for (int i = 0; i < NBUF; i++)
      if (!g_queued[i])
        return i;
    reap();
  }
  return -1;
}

int dcr_audio_open(void) {
  if (g_ao_ready)
    return 0;
  Result rc = audoutInitialize();
  if (R_FAILED(rc)) {
    debugPrintf("[audio] audoutInitialize failed 0x%x\n", rc);
    return -1;
  }
  rc = audoutStartAudioOut();
  if (R_FAILED(rc)) {
    debugPrintf("[audio] audoutStartAudioOut failed 0x%x\n", rc);
    audoutExit();
    return -1;
  }
  g_out_rate = audoutGetSampleRate() ? audoutGetSampleRate() : 48000;
  for (int i = 0; i < NBUF; i++) {
    g_pcm[i] = memalign(0x1000, BUF_BYTES);
    if (!g_pcm[i])
      return -1;
    memset(g_pcm[i], 0, BUF_BYTES);
    g_bufs[i].buffer = (u64)(uintptr_t)g_pcm[i];
    g_bufs[i].buffer_size = BUF_BYTES;
    g_bufs[i].data_size = BUF_BYTES;
  }
  g_ao_ready = 1;
  debugPrintf("[audio] audout open: %u Hz, %u ch\n", (unsigned)g_out_rate,
              (unsigned)audoutGetChannelCount());
  return 0;
}

static unsigned long g_underruns, g_append_fails, g_dropped, g_submits;

/* Queue one full buffer of 48 kHz stereo s16; blocks while all are in use. */
static void submit(const int16_t *frames) {
  int i;
  reap();
  int queued = 0;
  for (int k = 0; k < NBUF; k++)
    queued += g_queued[k];
  if (!queued && g_submits > NBUF)
    g_underruns++; /* audout had nothing left to play */
  while ((i = free_buffer()) < 0)
    svcSleepThread(2000000ll);
  memcpy(g_pcm[i], frames, BUF_BYTES);
  armDCacheFlush(g_pcm[i], BUF_BYTES);
  g_bufs[i].data_size = BUF_BYTES;
  g_bufs[i].data_offset = 0;
  for (int attempt = 0; attempt < 5; attempt++) {
    Result rc = ao_append(&g_bufs[i]);
    if (R_SUCCEEDED(rc)) {
      g_queued[i] = 1;
      g_submits++;
      return;
    }
    if (g_append_fails++ < 3)
      debugPrintf("[audio] audout append failed 0x%x (retrying)\n", (unsigned)rc);
    svcSleepThread(2000000ll);
    reap();
  }
  g_dropped++;
}

/* Self-check of the descriptor layout, before the game runs: two buffers of
 * silence must come back from the audio server. */
void dcr_audio_selftest(void) {
  if (dcr_audio_open() != 0)
    return;
  static int16_t silence[FRAMES_PER_BUF * 2];
  submit(silence);
  submit(silence);
  u64 t0 = armGetSystemTick();
  int back = 0;
  while (armTicksToNs(armGetSystemTick() - t0) < 500000000ull) {
    reap();
    back = 0;
    for (int i = 0; i < NBUF; i++)
      back += !g_queued[i];
    if (back == NBUF)
      break;
    svcSleepThread(5000000ll);
  }
  debugPrintf("[audio] self-test: %s (%d/%d buffers returned in %llu ms)\n",
              back == NBUF ? "OK" : "FAILED -- buffer descriptor not accepted", back, NBUF,
              (unsigned long long)(armTicksToNs(armGetSystemTick() - t0) / 1000000ull));
}

/* ------------------------------------------------------------- FMOD pump */
typedef jint (*fn_getinfo)(void *env, void *thiz, jint which);
typedef jint (*fn_process)(void *env, void *thiz, void *bytebuffer);

static Thread g_thread;
static volatile int g_run;
static JObj *g_device;

/* A source that replaces FMOD's mix while it is set (sh_video.c: an intro
 * movie's sound, the player being paused): frames of stereo s16 at rate. */
int (*dcr_audio_source)(int16_t *out, int frames, int rate);

static void pump(void *arg) {
  fn_getinfo getinfo = (fn_getinfo)jni_native("org/fmod/FMODAudioDevice", "fmodGetInfo");
  fn_process process = (fn_process)jni_native("org/fmod/FMODAudioDevice", "fmodProcess");
  if (!getinfo || !process || dcr_audio_open() != 0) {
    debugPrintf("[audio] FMOD pump not started (natives %p %p)\n", (void *)getinfo, (void *)process);
    return;
  }
  JObj *bb = NULL;
  int16_t *blk = NULL;
  int rate = 0, frames = 0;
  static int16_t out[FRAMES_PER_BUF * 2];
  int out_n = 0;
  double pos = 0.0; /* resampler read position within the current block */
  int16_t prev[2] = {0, 0};
  unsigned long blocks = 0;
  u64 t_first = 0;
  int peak = 0;
  unsigned long full = 0;
  u64 mix_ticks = 0, mix_max = 0; /* FMOD's own mixing time, per report */

  while (g_run) {
    int (*src)(int16_t *, int, int) = dcr_audio_source;
    if (src) { /* a movie's sound: audout paces it as it does FMOD's */
      static int16_t vbuf[FRAMES_PER_BUF * 2];
      src(vbuf, FRAMES_PER_BUF, (int)g_out_rate);
      submit(vbuf);
      continue;
    }
    if (getinfo(g_jni_env, g_device, 3) != 1) {
      svcSleepThread(10000000ll);
      continue;
    }
    if (!bb) {
      rate = getinfo(g_jni_env, g_device, 0);
      frames = getinfo(g_jni_env, g_device, 1);
      if (rate < 8000 || rate > 192000 || frames <= 0 || frames > 65536) {
        debugPrintf("[audio] FMOD reports rate %d, block %d: not usable\n", rate, frames);
        svcSleepThread(100000000ll);
        continue;
      }
      blk = memalign(64, (size_t)frames * 4);
      bb = jni_new("java/nio/DirectByteBuffer");
      bb->p = blk;
      bb->v[0] = frames * 4;
      debugPrintf("[audio] FMOD output: %d Hz, %d-frame blocks -> %u Hz\n", rate, frames,
                  (unsigned)g_out_rate);
    }
    u64 m0 = armGetSystemTick();
    process(g_jni_env, g_device, bb);
    u64 m1 = armGetSystemTick() - m0;
    mix_ticks += m1;
    if (m1 > mix_max)
      mix_max = m1;
    blocks++;
    for (int k = 0; k < frames * 2; k++) {
      int v = blk[k] < 0 ? -(int)blk[k] : blk[k];
      if (v > peak)
        peak = v;
      full += v >= 32767;
    }
    if (blocks == 1) {
      t_first = armGetSystemTick();
      debugPrintf("[audio] first FMOD block mixed\n");
    } else if (blocks % 3000 == 0) {
      /* blocks after the first, over the time since it: FMOD's pull rate */
      double secs = (double)armTicksToNs(armGetSystemTick() - t_first) / 1e9;
      debugPrintf("[audio] %lu FMOD blocks mixed; pulled at %.0f Hz over %.0f s (FMOD rate %d); "
                  "%lu underruns, %lu failed submits (%lu dropped); output peak %.1f dBFS, %lu samples "
                  "at full scale; mixing took %.2f ms a block on average, %.2f at most (of %.1f)\n",
                  blocks, secs > 0 ? (double)(blocks - 1) * frames / secs : 0.0, secs, rate,
                  g_underruns, g_append_fails, g_dropped, peak ? 20.0 * log10((double)peak / 32768.0) : -99.0,
                  full, (double)armTicksToNs(mix_ticks) / 3000.0 / 1e6, (double)armTicksToNs(mix_max) / 1e6,
                  1000.0 * frames / rate);
      peak = 0;
      full = 0;
      mix_ticks = mix_max = 0;
    }

    /* Linear resampling across block boundaries: index -1 is the previous
     * block's last frame, so pos runs over [-1, frames - 1). */
    const double step = (double)rate / (double)g_out_rate;
    while (pos < (double)(frames - 1)) {
      int i0 = (int)floor(pos);
      double t = pos - (double)i0;
      const int16_t *a = i0 < 0 ? prev : &blk[i0 * 2];
      const int16_t *b2 = &blk[(i0 + 1) * 2];
      out[out_n * 2] = (int16_t)((double)a[0] + (double)(b2[0] - a[0]) * t);
      out[out_n * 2 + 1] = (int16_t)((double)a[1] + (double)(b2[1] - a[1]) * t);
      if (++out_n == FRAMES_PER_BUF) {
        submit(out);
        out_n = 0;
      }
      pos += step;
    }
    pos -= (double)frames;
    prev[0] = blk[(frames - 1) * 2];
    prev[1] = blk[(frames - 1) * 2 + 1];
  }
  if (bb)
    jni_release(bb);
  free(blk);
}

void dcr_audio_fmod_start(JObj *device) {
  if (g_run)
    return;
  g_device = jni_retain(device);
  g_run = 1;
  /* Above the engine's workers: an audio underrun is heard, a late frame is not. */
  Result rc = threadCreate(&g_thread, pump, NULL, NULL, 0x10000, 0x2A, -2);
  if (R_SUCCEEDED(rc))
    rc = threadStart(&g_thread);
  if (R_FAILED(rc)) {
    g_run = 0;
    debugPrintf("[audio] pump thread failed 0x%x\n", rc);
    return;
  }
  debugPrintf("[audio] FMODAudioDevice.start(): pump running\n");
}

void dcr_audio_fmod_stop(void) {
  if (!g_run)
    return;
  g_run = 0;
  threadWaitForExit(&g_thread);
  threadClose(&g_thread);
  jni_release(g_device);
  g_device = NULL;
  debugPrintf("[audio] FMODAudioDevice.stop()\n");
}

int dcr_audio_fmod_running(void) { return g_run; }

/* A source of its own while FMOD is stopped: Unity stops FMOD's device when
 * the player pauses -- exactly when a movie plays (sh_video.c) -- and the
 * pump above goes with it. This thread plays dcr_audio_source until
 * dcr_audio_source_end(); if FMOD's pump is running, that one plays it. */
static Thread g_src_thread;
static volatile int g_src_run;

static void src_pump(void *arg) {
  (void)arg;
  static int16_t b[FRAMES_PER_BUF * 2];
  while (g_src_run) {
    int (*s)(int16_t *, int, int) = dcr_audio_source;
    if (!s) {
      svcSleepThread(5000000ll);
      continue;
    }
    s(b, FRAMES_PER_BUF, (int)g_out_rate);
    submit(b);
  }
}

int dcr_audio_source_begin(int (*fn)(int16_t *out, int frames, int rate)) {
  dcr_audio_source = fn;
  if (g_run || g_src_run)
    return 0;
  if (dcr_audio_open() != 0)
    return -1;
  g_src_run = 1;
  if (R_FAILED(threadCreate(&g_src_thread, src_pump, NULL, NULL, 0x8000, 0x2A, -2)) ||
      R_FAILED(threadStart(&g_src_thread))) {
    g_src_run = 0;
    debugPrintf("[audio] no thread for the movie's sound\n");
    return -1;
  }
  return 0;
}

void dcr_audio_source_end(void) {
  dcr_audio_source = NULL;
  if (g_src_run) {
    g_src_run = 0;
    threadWaitForExit(&g_src_thread);
    threadClose(&g_src_thread);
  }
}
