/* resample_test.c -- source/sh_resample.h on the PC: raw s16le stereo in
 * (in_rate) -> raw s16le stereo out (out_rate), to compare with a reference
 * resampler. gcc -O2 -Isource tools/resample_test.c -lm -o rt &&
 * ./rt in.raw 44100 out.raw 48000. MIT. */
#include <stdio.h>
#include <stdlib.h>
#include "sh_resample.h"

int main(int argc, char **argv) {
  if (argc != 5)
    return 2;
  FILE *f = fopen(argv[1], "rb");
  if (!f)
    return 1;
  fseek(f, 0, SEEK_END);
  long n = ftell(f) / 4;
  fseek(f, 0, SEEK_SET);
  int16_t *in = malloc((size_t)n * 4);
  if (fread(in, 4, (size_t)n, f) != (size_t)n)
    return 1;
  fclose(f);
  static ShResampler r;
  const int ir = atoi(argv[2]), or_ = atoi(argv[4]);
  sh_resample_init(&r, ir, or_);
  FILE *o = fopen(argv[3], "wb");
  const double step = (double)ir / or_;
  for (double pos = 0; pos + 1 < n; pos += step) {
    int16_t s[2];
    sh_resample_at(&r, in, n, pos, s);
    fwrite(s, 2, 2, o);
  }
  fclose(o);
  return 0;
}
