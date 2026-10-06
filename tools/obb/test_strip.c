/* test_strip.c -- source/sh_obbstrip.c on the PC (tools/obb/test_strip.sh). MIT. */
#include <stdio.h>
#include "sh_obbstrip.h"

static void progress(uint64_t done, uint64_t total, void *ctx) {
  static int last = -1;
  int pc = total ? (int)(done * 100 / total) : 0;
  if (pc / 10 != last) {
    last = pc / 10;
    fprintf(stderr, "  %d%%\n", pc);
  }
  (void)ctx;
}

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: %s <obb> <out>\n", argv[0]);
    return 2;
  }
  printf("already stripped: %d\n", sh_obbstrip_is_done(argv[1]));
  ShObbStripStats st;
  int r = sh_obbstrip_run(argv[1], argv[2], progress, NULL, &st);
  printf("result %d: %u textures stripped, %u GUI kept, %u files rewritten, %llu MB saved\n", r, st.textures,
         st.gui_kept, st.files, (unsigned long long)(st.bytes_saved >> 20));
  return r == 1 ? 0 : 1;
}
