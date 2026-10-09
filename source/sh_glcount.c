/* sh_glcount.c -- the engine's draw calls, counted (the import overlay in
 * sh_io.c routes them here). Read by the port's C# (DcrMod.Native.DrawCount)
 * to measure what a setting costs the render thread, whose time goes per
 * draw: Mesa's state validation and command building were half of it
 * (hardware, 2026-10-09). A plain counter: only the render thread draws.
 * MIT. */
#include <stdint.h>
#include <GLES2/gl2.h>

static volatile uint64_t g_draws;

void sh_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices) {
  g_draws++;
  glDrawElements(mode, count, type, indices);
}

void sh_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
  g_draws++;
  glDrawArrays(mode, first, count);
}

uint64_t sh_draw_count(void) { return g_draws; }
