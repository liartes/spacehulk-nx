/* sh_resample.h -- a movie's sound from its rate to the output's (sh_video.c).
 *
 * A windowed-sinc interpolator: 32 taps, 256 phases (linearly blended), a
 * Kaiser window (beta 8), cut off at 0.91 of the lower Nyquist. The linear
 * interpolation it replaces left, for the 44.1 kHz intro played at 48 kHz,
 * an image of the music shifted ~4 kHz up, 12 dB under it from 10 kHz on:
 * heard as hiss and a metallic echo (2026-10-09; tools/resample_test.c
 * measures it). Header-only so the PC test builds it as the console does.
 * MIT.
 */
#ifndef SH_RESAMPLE_H
#define SH_RESAMPLE_H
#include <math.h>
#include <stdint.h>

#define RS_TAPS 32
#define RS_PHASES 256

typedef struct {
  float h[RS_PHASES + 1][RS_TAPS]; /* phase p: taps at offsets -15..16 from i0, for t = p / RS_PHASES */
  int in_rate, out_rate;
} ShResampler;

static double rs_bessel_i0(double x) {
  double s = 1, t = 1;
  for (int k = 1; k < 30; k++) {
    t *= (x / (2 * k)) * (x / (2 * k));
    s += t;
  }
  return s;
}

static void sh_resample_init(ShResampler *r, int in_rate, int out_rate) {
  r->in_rate = in_rate;
  r->out_rate = out_rate;
  const double lower = in_rate < out_rate ? in_rate : out_rate;
  const double fc = 0.91 * 0.5 * lower / in_rate; /* cycles per input sample */
  const double beta = 8.0, half = RS_TAPS / 2.0;
  for (int p = 0; p <= RS_PHASES; p++) {
    const double t = (double)p / RS_PHASES;
    double sum = 0;
    for (int k = 0; k < RS_TAPS; k++) {
      const double x = (k - (RS_TAPS / 2 - 1)) - t; /* the tap's distance from the output point */
      const double sinc = x == 0 ? 2 * fc : sin(2 * M_PI * fc * x) / (M_PI * x);
      const double w = fabs(x) >= half ? 0 : rs_bessel_i0(beta * sqrt(1 - (x / half) * (x / half))) / rs_bessel_i0(beta);
      r->h[p][k] = (float)(sinc * w);
      sum += sinc * w;
    }
    for (int k = 0; k < RS_TAPS; k++) /* unity gain at DC */
      r->h[p][k] = (float)(r->h[p][k] / sum);
  }
}

/* One stereo output frame at input position pos (frames, interleaved
 * pcm[0..have)); samples outside are silence. */
static inline void sh_resample_at(const ShResampler *r, const int16_t *pcm, int64_t have, double pos,
                                  int16_t out[2]) {
  const int64_t i0 = (int64_t)pos;
  const double ft = (pos - (double)i0) * RS_PHASES;
  const int p = (int)ft;
  const float b = (float)(ft - p), a = 1.0f - b;
  const float *h0 = r->h[p], *h1 = r->h[p + 1];
  float l = 0, rr = 0;
  const int64_t first = i0 - (RS_TAPS / 2 - 1);
  for (int k = 0; k < RS_TAPS; k++) {
    const int64_t i = first + k;
    if (i < 0 || i >= have)
      continue;
    const float c = a * h0[k] + b * h1[k];
    l += c * pcm[i * 2];
    rr += c * pcm[i * 2 + 1];
  }
  const int vl = (int)lrintf(l), vr = (int)lrintf(rr);
  out[0] = (int16_t)(vl < -32768 ? -32768 : vl > 32767 ? 32767 : vl);
  out[1] = (int16_t)(vr < -32768 ? -32768 : vr > 32767 ? 32767 : vr);
}

#endif
