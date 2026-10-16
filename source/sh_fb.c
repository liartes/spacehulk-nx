/* sh_fb.c -- framebufferBegin, without its abort (sh_video.c's movies).
 *
 * libnx ends the process (2345-0021, the console restarted) when the window
 * gives no buffer, and it happened in the middle of the logo movie
 * (hardware, 2026-10-15). Here a few tries, then NULL: the movie stops and
 * the game goes on. The buffer is the linear one (framebufferMakeLinear);
 * framebufferEnd converts and queues it.
 *
 * In a file of its own, compiled as libnx is: sh_video.c is built with
 * -fno-short-enums for FFmpeg, which makes NvMap's NvKind 4 bytes instead of
 * 1 and moves every Framebuffer field after it -- read from there,
 * buf_linear was the stride, 0x1400, and the first picture was drawn at
 * that address (hardware, 2026-10-16). MIT. */
#include <switch.h>

#include "util.h"

void *sh_fb_begin(Framebuffer *fb, u32 *stride) {
  static int failures;
  s32 slot;
  Result rc = 0;
  if (!fb->buf_linear)
    return NULL;
  for (int t = 0; t < 10; t++) {
    rc = nwindowDequeueBuffer(fb->win, &slot, NULL);
    if (R_SUCCEEDED(rc)) {
      if (t)
        debugPrintf("[video] a buffer after %d tries\n", t + 1);
      *stride = fb->stride;
      return fb->buf_linear;
    }
    if (failures++ < 8)
      debugPrintf("[video] nwindowDequeueBuffer: 0x%x\n", (unsigned)rc);
    svcSleepThread(10000000ll);
  }
  return NULL;
}
