/* sh_video.c -- Handheld.PlayFullScreenMovie: the intro movies.
 *
 * The game's intro (IntroVideoManager / AndroidVideoPlayer) plays
 * Movies/TWIMTBP_Spin_Up.mp4 (1920x1080 H.264, the NVIDIA logo, 8 s) then
 * Movies/SH_Intro_PS3_USA.mp4 (1280x720 H.264 + AAC, 74 s) with
 * Handheld.PlayFullScreenMovie(CancelOnInput). Unity 5.3 hands that to Java:
 * UnityPlayer.showVideoPlayer(path, colour, control, scaling, isURL, offset,
 * length) with the OBB's path and the stored entry's byte range. On a phone
 * that pauses the player, takes Unity's SurfaceView away (surfaceDestroyed ->
 * nativeRecreateGfxState(0, null)), plays the range with MediaPlayer over the
 * screen, and on completion (or a key) puts the view back and resumes;
 * the C# side then waits two frames and goes on.
 *
 * Here the same, from the frame loop (dcr_boot.c, sh_video_frame): the
 * player paused and its surface released (the window's buffers freed here:
 * Unity never destroys that surface), the movie played into a libnx framebuffer on that window, the
 * surface given back and the player resumed. Decoding is the Sonic port's
 * ssr_video.c (from the PvZ port): the byte range through a custom
 * AVIOContext, FFmpeg (ffmpeg32 with the H.264 decoder) on a thread of its
 * own a few pictures ahead, BT.601 -> RGBA in NEON. The sound replaces
 * FMOD's in the audio pump (dcr_audio.c: dcr_audio_source), resampled to
 * 48 kHz, and is the clock the pictures follow; a picture that is late is
 * passed over. Any button ends it, as CancelOnInput does.
 *
 * Compiled with -fno-short-enums, as FFmpeg is (Makefile). MIT.
 */
#include <arm_neon.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "dcr_path.h"
#include "util.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

void dcr_window_size(int *w, int *h);
void dcr_boost_hold(int on);                                  /* dcr_boost.c */
int dcr_audio_source_begin(int (*fn)(int16_t *out, int frames, int rate)); /* dcr_audio.c */
void dcr_audio_source_end(void);

/* FFmpeg 7.1's h2645_sei.c resets the AOM film grain sets unconditionally,
 * but aom_film_grain.o is only built with HEVC, which ffmpeg32 leaves out
 * (the Sonic and Angry Birds ports do the same). */
void ff_aom_uninit_film_grain_params(void *s);
void ff_aom_uninit_film_grain_params(void *s) { (void)s; }

#define NSLOT 4 /* pictures decoded ahead */

/* ------------------------------------------------------------ the request */
static struct {
  volatile int pending;
  char path[DCR_PATH_MAX];
  int64_t off, len;
} R;

/* jni_android.c: UnityPlayer.showVideoPlayer, on the engine's thread */
void sh_video_request(const char *android_path, int is_url, int64_t off, int64_t len) {
  if (is_url || !android_path) {
    debugPrintf("[video] %s: a URL, not played\n", android_path ? android_path : "(null)");
    return;
  }
  char buf[DCR_PATH_MAX];
  const char *real = dcr_translate_path(android_path, buf, sizeof buf);
  snprintf(R.path, sizeof R.path, "%s", real);
  R.off = off;
  R.len = len;
  R.pending = 1;
  debugPrintf("[video] showVideoPlayer(%s, %lld bytes at %lld)\n", android_path, (long long)len, (long long)off);
}

int sh_video_pending(void) { return R.pending; }

/* ------------------------------------------------------------ the movie */
static struct {
  Mutex lock;
  volatile int stop, eof;
  FILE *f;
  int64_t base, len, pos;
  AVFormatContext *fmt;
  AVIOContext *io;
  AVCodecContext *vdec, *adec;
  int vs, as;
  double vtb;
  Thread thread;
  int thread_on;
  int w, h;
  uint8_t *rgba[NSLOT];
  double pts[NSLOT];
  volatile int ready[NSLOT];
  int16_t *pcm; /* all of the sound, s16 stereo at arate */
  size_t pcm_cap, pcm_frames;
  int arate;
  double apos; /* next frame to play */
  volatile int audio_go;
  volatile uint64_t played; /* output frames given to the pump */
} V;

static int io_read(void *opaque, uint8_t *buf, int n) {
  (void)opaque;
  const int64_t left = V.len - V.pos;
  if (left <= 0)
    return AVERROR_EOF;
  if (n > left)
    n = (int)left;
  if (fseeko(V.f, (off_t)(V.base + V.pos), SEEK_SET) != 0)
    return AVERROR(EIO);
  size_t got = fread(buf, 1, (size_t)n, V.f);
  if (!got)
    return AVERROR_EOF;
  V.pos += (int64_t)got;
  return (int)got;
}

static int64_t io_seek(void *opaque, int64_t off, int whence) {
  (void)opaque;
  if (whence == AVSEEK_SIZE)
    return V.len;
  whence &= ~AVSEEK_FORCE;
  int64_t p = whence == SEEK_SET ? off : whence == SEEK_CUR ? V.pos + off : V.len + off;
  if (p < 0 || p > V.len)
    return -1;
  V.pos = p;
  return p;
}

/* BT.601 (video range) YUV 4:2:0 -> RGBA, 16 pixels a step (ssr_video.c) */
static void yuv_to_rgba(const AVFrame *f, uint8_t *out, int w, int h) {
  const int16x8_t k74 = vdupq_n_s16(74), k102 = vdupq_n_s16(102), k25 = vdupq_n_s16(25), k52 = vdupq_n_s16(52),
                  k129 = vdupq_n_s16(129), k16 = vdupq_n_s16(16), k128 = vdupq_n_s16(128);
  for (int y = 0; y < h; y++) {
    const uint8_t *py = f->data[0] + (size_t)y * f->linesize[0];
    const uint8_t *pu = f->data[1] + (size_t)(y / 2) * f->linesize[1];
    const uint8_t *pv = f->data[2] + (size_t)(y / 2) * f->linesize[2];
    uint8_t *d = out + (size_t)y * w * 4;
    int x = 0;
    for (; x + 16 <= w; x += 16) {
      const uint8x16_t yy = vld1q_u8(py + x);
      const uint8x8_t uu = vld1_u8(pu + x / 2), vv = vld1_u8(pv + x / 2);
      const uint8x8x2_t u2 = vzip_u8(uu, uu), v2 = vzip_u8(vv, vv);
      uint8x16x4_t px;
      for (int half = 0; half < 2; half++) {
        const uint8x8_t y8 = half ? vget_high_u8(yy) : vget_low_u8(yy);
        const int16x8_t c = vmulq_s16(vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(y8)), k16), k74);
        const int16x8_t d_ = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(u2.val[half])), k128);
        const int16x8_t e = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(v2.val[half])), k128);
        const int16x8_t r = vqaddq_s16(c, vmulq_s16(e, k102));
        const int16x8_t g = vqsubq_s16(vqsubq_s16(c, vmulq_s16(d_, k25)), vmulq_s16(e, k52));
        const int16x8_t b = vqaddq_s16(c, vmulq_s16(d_, k129));
        const uint8x8_t r8 = vqrshrun_n_s16(r, 6), g8 = vqrshrun_n_s16(g, 6), b8 = vqrshrun_n_s16(b, 6);
        if (half) {
          px.val[0] = vcombine_u8(vget_low_u8(px.val[0]), r8);
          px.val[1] = vcombine_u8(vget_low_u8(px.val[1]), g8);
          px.val[2] = vcombine_u8(vget_low_u8(px.val[2]), b8);
        } else {
          px.val[0] = vcombine_u8(r8, r8);
          px.val[1] = vcombine_u8(g8, g8);
          px.val[2] = vcombine_u8(b8, b8);
        }
      }
      px.val[3] = vdupq_n_u8(255);
      vst4q_u8(d + x * 4, px);
    }
    for (; x < w; x++) {
      const int c = (py[x] - 16) * 74, du = pu[x / 2] - 128, ev = pv[x / 2] - 128;
      int r = (c + 102 * ev + 32) >> 6, g = (c - 25 * du - 52 * ev + 32) >> 6, b = (c + 129 * du + 32) >> 6;
      d[x * 4 + 0] = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
      d[x * 4 + 1] = (uint8_t)(g < 0 ? 0 : g > 255 ? 255 : g);
      d[x * 4 + 2] = (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
      d[x * 4 + 3] = 255;
    }
  }
}

static void put_picture(const AVFrame *f) {
  if (f->format != AV_PIX_FMT_YUV420P || f->width != V.w || f->height != V.h)
    return;
  int slot = -1;
  while (!V.stop) {
    mutexLock(&V.lock);
    for (int i = 0; i < NSLOT && slot < 0; i++)
      if (!V.ready[i])
        slot = i;
    mutexUnlock(&V.lock);
    if (slot >= 0)
      break;
    svcSleepThread(2000000ll);
  }
  if (slot < 0)
    return;
  yuv_to_rgba(f, V.rgba[slot], V.w, V.h);
  const int64_t ts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
  mutexLock(&V.lock);
  V.pts[slot] = ts == AV_NOPTS_VALUE ? 0 : (double)ts * V.vtb;
  V.ready[slot] = 1;
  mutexUnlock(&V.lock);
}

static void put_sound(const AVFrame *f) {
  if (!V.pcm || f->nb_samples <= 0)
    return;
  const int ch = f->ch_layout.nb_channels;
  size_t n = (size_t)f->nb_samples;
  mutexLock(&V.lock);
  if (V.pcm_frames + n > V.pcm_cap)
    n = V.pcm_cap - V.pcm_frames;
  int16_t *d = V.pcm + V.pcm_frames * 2;
  mutexUnlock(&V.lock);
  for (size_t i = 0; i < n; i++)
    for (int c = 0; c < 2; c++) {
      const int k = c < ch ? c : 0;
      float s;
      switch (f->format) {
      case AV_SAMPLE_FMT_FLTP: s = ((const float *)f->extended_data[k])[i]; break;
      case AV_SAMPLE_FMT_FLT: s = ((const float *)f->extended_data[0])[i * ch + k]; break;
      case AV_SAMPLE_FMT_S16P: s = ((const int16_t *)f->extended_data[k])[i] / 32768.0f; break;
      case AV_SAMPLE_FMT_S16: s = ((const int16_t *)f->extended_data[0])[i * ch + k] / 32768.0f; break;
      default: s = 0; break;
      }
      int v = (int)(s * 32767.0f);
      d[i * 2 + c] = (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
    }
  mutexLock(&V.lock);
  V.pcm_frames += n;
  mutexUnlock(&V.lock);
}

/* The audio pump's source while a movie plays (dcr_audio.c): the movie's
 * sound at the output rate, silence until the first picture. */
static int audio_source(int16_t *out, int frames, int rate) {
  memset(out, 0, (size_t)frames * 4);
  if (!V.audio_go || !V.pcm || V.arate <= 0) {
    V.played += (uint64_t)frames;
    return 1;
  }
  mutexLock(&V.lock);
  /* the position moves on whether or not the sound there is decoded yet
   * (silence then): it is the clock the pictures follow, and must not stall
   * behind a decoder that is busy with pictures */
  const double step = (double)V.arate / (double)rate;
  const double have = (double)V.pcm_frames;
  for (int i = 0; i < frames; i++) {
    const int i0 = (int)V.apos;
    const double t = V.apos - i0;
    if (V.apos + 1 < have)
      for (int c = 0; c < 2; c++)
        out[i * 2 + c] = (int16_t)(V.pcm[i0 * 2 + c] * (1 - t) + V.pcm[(i0 + 1) * 2 + c] * t);
    V.apos += step;
  }
  mutexUnlock(&V.lock);
  V.played += (uint64_t)frames;
  return 1;
}

static void decode_thread(void *arg) {
  (void)arg;
  AVPacket *pkt = av_packet_alloc();
  AVFrame *fr = av_frame_alloc();
  int pictures = 0;
  while (pkt && fr && !V.stop) {
    const int r = av_read_frame(V.fmt, pkt);
    AVCodecContext *dec = NULL;
    if (r >= 0)
      dec = pkt->stream_index == V.vs ? V.vdec : pkt->stream_index == V.as ? V.adec : NULL;
    if (r < 0) {
      if (V.vdec)
        avcodec_send_packet(V.vdec, NULL);
      while (V.vdec && !V.stop && avcodec_receive_frame(V.vdec, fr) == 0)
        put_picture(fr), pictures++;
      if (V.adec)
        avcodec_send_packet(V.adec, NULL);
      while (V.adec && avcodec_receive_frame(V.adec, fr) == 0)
        put_sound(fr);
      break;
    }
    if (dec && avcodec_send_packet(dec, pkt) >= 0)
      while (!V.stop && avcodec_receive_frame(dec, fr) == 0) {
        if (dec == V.vdec)
          put_picture(fr), pictures++;
        else
          put_sound(fr);
      }
    av_packet_unref(pkt);
  }
  av_frame_free(&fr);
  av_packet_free(&pkt);
  debugPrintf("[video] decoded %d pictures, %u sound frames%s\n", pictures, (unsigned)V.pcm_frames,
              V.stop ? " (stopped)" : "");
  V.eof = 1;
}

static void close_all(void) {
  V.stop = 1;
  if (V.thread_on) {
    threadWaitForExit(&V.thread);
    threadClose(&V.thread);
    V.thread_on = 0;
  }
  dcr_audio_source_end();
  V.audio_go = 0;
  avcodec_free_context(&V.vdec);
  avcodec_free_context(&V.adec);
  if (V.fmt)
    avformat_close_input(&V.fmt);
  if (V.io) {
    av_freep(&V.io->buffer);
    avio_context_free(&V.io);
  }
  for (int i = 0; i < NSLOT; i++) {
    free(V.rgba[i]);
    V.rgba[i] = NULL;
    V.ready[i] = 0;
  }
  free(V.pcm);
  V.pcm = NULL;
  if (V.f)
    fclose(V.f);
  V.f = NULL;
}

static AVCodecContext *open_decoder(AVStream *st, int fast) {
  const AVCodec *c = avcodec_find_decoder(st->codecpar->codec_id);
  AVCodecContext *ctx = c ? avcodec_alloc_context3(c) : NULL;
  if (!ctx || avcodec_parameters_to_context(ctx, st->codecpar) < 0) {
    avcodec_free_context(&ctx);
    return NULL;
  }
  if (fast) { /* 1080p H.264 on one core: no deblocking, the fast paths */
    ctx->skip_loop_filter = AVDISCARD_ALL;
    ctx->flags2 |= AV_CODEC_FLAG2_FAST;
  }
  if (avcodec_open2(ctx, c, NULL) < 0)
    avcodec_free_context(&ctx);
  return ctx;
}

static int open_movie(void) {
  memset(&V, 0, sizeof V);
  V.vs = V.as = -1;
  mutexInit(&V.lock);
  V.f = fopen(R.path, "rb");
  if (!V.f) {
    debugPrintf("[video] %s: cannot open it\n", R.path);
    return 0;
  }
  setvbuf(V.f, NULL, _IOFBF, 256 * 1024);
  if (R.len <= 0) { /* a whole file */
    fseeko(V.f, 0, SEEK_END);
    R.len = (int64_t)ftello(V.f);
  }
  V.base = R.off;
  V.len = R.len;
  av_log_set_level(AV_LOG_ERROR);
  unsigned char *buf = av_malloc(65536);
  V.io = buf ? avio_alloc_context(buf, 65536, 0, NULL, io_read, NULL, io_seek) : NULL;
  V.fmt = avformat_alloc_context();
  if (!V.io || !V.fmt)
    return 0;
  V.fmt->pb = V.io;
  if (avformat_open_input(&V.fmt, NULL, NULL, NULL) < 0 || avformat_find_stream_info(V.fmt, NULL) < 0) {
    debugPrintf("[video] not a movie FFmpeg reads here\n");
    return 0;
  }
  V.vs = av_find_best_stream(V.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
  V.as = av_find_best_stream(V.fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
  if (V.vs >= 0) {
    const AVCodecParameters *cp = V.fmt->streams[V.vs]->codecpar;
    V.vdec = open_decoder(V.fmt->streams[V.vs], cp->width * cp->height > 1280 * 720);
  }
  if (V.as >= 0)
    V.adec = open_decoder(V.fmt->streams[V.as], 0);
  if (!V.vdec) {
    debugPrintf("[video] no picture decoder for it\n");
    return 0;
  }
  V.w = V.vdec->width;
  V.h = V.vdec->height;
  V.vtb = av_q2d(V.fmt->streams[V.vs]->time_base);
  for (int i = 0; i < NSLOT; i++)
    if (!(V.rgba[i] = memalign(64, (size_t)V.w * V.h * 4)))
      return 0;
  if (V.adec) {
    V.arate = V.adec->sample_rate;
    const double secs = V.fmt->duration > 0 ? (double)V.fmt->duration / AV_TIME_BASE : 120.0;
    V.pcm_cap = (size_t)((secs + 2.0) * V.arate);
    V.pcm = malloc(V.pcm_cap * 4);
  }
  debugPrintf("[video] %dx%d %s, %s %d Hz, %.1f s\n", V.w, V.h, avcodec_get_name(V.vdec->codec_id),
              V.adec ? avcodec_get_name(V.adec->codec_id) : "no sound", V.arate,
              V.fmt->duration > 0 ? (double)V.fmt->duration / AV_TIME_BASE : 0.0);
  return 1;
}

/* ------------------------------------------------------------ the screen */
/* src (sw x sh RGBA) into the framebuffer (fw x fh, stride in bytes),
 * fitted and centred, nearest pixel; black around it. */
static void blit(uint32_t *fb, u32 stride, int fw, int fh, const uint32_t *src, int sw, int sh) {
  int dw = fw, dh = (int)((int64_t)fw * sh / sw);
  if (dh > fh) {
    dh = fh;
    dw = (int)((int64_t)fh * sw / sh);
  }
  const int ox = (fw - dw) / 2, oy = (fh - dh) / 2;
  static int xmap[4096];
  for (int x = 0; x < dw && x < 4096; x++)
    xmap[x] = (int)((int64_t)x * sw / dw);
  const u32 sp = stride / 4;
  for (int y = 0; y < fh; y++) {
    uint32_t *d = fb + (size_t)y * sp;
    if (y < oy || y >= oy + dh) {
      memset(d, 0, (size_t)fw * 4);
      continue;
    }
    const uint32_t *s = src + (size_t)((int64_t)(y - oy) * sh / dh) * sw;
    if (dw == sw) {
      memset(d, 0, (size_t)ox * 4);
      memcpy(d + ox, s, (size_t)dw * 4);
      memset(d + ox + dw, 0, (size_t)(fw - ox - dw) * 4);
    } else {
      for (int x = 0; x < ox; x++)
        d[x] = 0;
      for (int x = 0; x < dw; x++)
        d[ox + x] = s[xmap[x]];
      for (int x = ox + dw; x < fw; x++)
        d[x] = 0;
    }
  }
}

/* any button: CancelOnInput */
static int skip_pressed(PadState *pad) {
  padUpdate(pad);
  return (padGetButtonsDown(pad) & (HidNpadButton_A | HidNpadButton_B | HidNpadButton_X | HidNpadButton_Y |
                                    HidNpadButton_Plus | HidNpadButton_Minus)) != 0;
}

/* The movie, start to end, on this (the engine's) thread, with Unity's
 * surface released. 0 when nothing could be played. */
static int play(void) {
  if (!open_movie()) {
    close_all();
    return 0;
  }
  int fw = 1280, fh = 720;
  dcr_window_size(&fw, &fh);
  /* Unity lets go of its EGL surface (eglMakeCurrent(none)) but never
   * destroys it -- it makes a new one when the surface comes back -- so Mesa
   * keeps the window's buffers and libnx refuses a framebuffer on it
   * (AlreadyInitialized, hardware 2026-10-06). They are released here, as
   * Mesa's surface destruction would. */
  Result rr = nwindowReleaseBuffers(nwindowGetDefault());
  if (R_FAILED(rr))
    debugPrintf("[video] releasing the window's buffers: 0x%x\n", (unsigned)rr);
  Framebuffer fb;
  Result rc = framebufferCreate(&fb, nwindowGetDefault(), (u32)fw, (u32)fh, PIXEL_FORMAT_RGBA_8888, 2);
  if (R_FAILED(rc)) {
    debugPrintf("[video] no framebuffer on the window (0x%x): not played\n", (unsigned)rc);
    close_all();
    return 0;
  }
  framebufferMakeLinear(&fb);
  /* framebufferBegin aborts the process -- a fatal error that restarts the
   * console -- when it cannot dequeue a buffer (2345-0021, with threaded
   * rendering off, hardware 2026-10-08). So one buffer is dequeued and given
   * back here first, a few tries apart; without one, no movie. */
  {
    Result dq = 0;
    int slot = -1;
    for (int t = 0; t < 10; t++) {
      dq = nwindowDequeueBuffer(nwindowGetDefault(), &slot, NULL);
      if (R_SUCCEEDED(dq))
        break;
      svcSleepThread(20000000ll);
    }
    if (R_FAILED(dq)) {
      debugPrintf("[video] the window gives no buffer (0x%x): not played\n", (unsigned)dq);
      framebufferClose(&fb);
      close_all();
      return 0;
    }
    nwindowCancelBuffer(nwindowGetDefault(), slot, NULL);
  }
  if (R_FAILED(threadCreate(&V.thread, decode_thread, NULL, NULL, 0x40000, 0x2C, 2)) ||
      R_FAILED(threadStart(&V.thread))) {
    debugPrintf("[video] no decoder thread\n");
    framebufferClose(&fb);
    close_all();
    return 0;
  }
  V.thread_on = 1;
  dcr_boost_hold(1);
  if (dcr_audio_source_begin(audio_source) != 0)
    debugPrintf("[video] no sound output: the pictures follow the wall clock\n");
  PadState pad;
  padInitializeAny(&pad);

  const u64 t_start = armGetSystemTick();
  double clock0 = -1; /* pts of the first picture */
  u64 wall0 = 0;
  int shown = 0, passed = 0, skipped = 0;
  const char *why = "the end";
  while (appletMainLoop()) {
    if (skip_pressed(&pad)) {
      why = "skipped";
      skipped = 1;
      break;
    }
    /* the clock: the sound played (the pump's frames), else the wall */
    double now;
    if (clock0 < 0)
      now = -1;
    else if (V.adec && V.played)
      now = clock0 + (double)V.played / 48000.0;
    else
      now = clock0 + (double)armTicksToNs(armGetSystemTick() - wall0) / 1e9;

    mutexLock(&V.lock);
    int due = -1, left = 0;
    for (int i = 0; i < NSLOT; i++)
      if (V.ready[i]) {
        left++;
        if (clock0 < 0 ? (due < 0 || V.pts[i] < V.pts[due]) : (V.pts[i] <= now && (due < 0 || V.pts[i] > V.pts[due])))
          due = i;
      }
    if (due >= 0 && clock0 < 0) { /* the first picture starts the clock and the sound */
      clock0 = V.pts[due];
      wall0 = armGetSystemTick();
      V.played = 0;
      V.audio_go = 1;
    }
    if (due >= 0)
      for (int i = 0; i < NSLOT; i++) /* pictures it passed: too late */
        if (V.ready[i] && i != due && V.pts[i] < V.pts[due])
          V.ready[i] = 0, passed++;
    mutexUnlock(&V.lock);

    if (due >= 0) {
      u32 stride;
      uint32_t *px = framebufferBegin(&fb, &stride);
      blit(px, stride, fw, fh, (const uint32_t *)V.rgba[due], V.w, V.h);
      framebufferEnd(&fb); /* presents, at the display's pace */
      mutexLock(&V.lock);
      V.ready[due] = 0;
      mutexUnlock(&V.lock);
      shown++;
    } else {
      if (V.eof && !left && (!V.adec || V.apos + 1 >= (double)V.pcm_frames))
        break;
      /* the clock moved on and nothing new is decoded: keep it moving */
      svcSleepThread(2000000ll);
    }
    if (clock0 < 0 && armTicksToNs(armGetSystemTick() - t_start) > 10000000000ull) {
      why = "no picture in 10 s";
      break;
    }
  }
  debugPrintf("[video] %s: %d pictures shown, %d passed over (late), %.1f s\n", why, shown, passed,
              (double)armTicksToNs(armGetSystemTick() - t_start) / 1e9);
  (void)skipped;
  dcr_audio_source_end();
  dcr_boost_hold(0);
  close_all();
  framebufferClose(&fb);
  return 1;
}

/* dcr_boot.c, between frames, with the player paused and its surface
 * released (as the Android VideoView does): the movie asked for, if any. */
void sh_video_play_pending(void) {
  if (!R.pending)
    return;
  R.pending = 0;
  play();
}
