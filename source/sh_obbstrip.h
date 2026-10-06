/* sh_obbstrip.h -- the OBB made Switch-sized (sh_obbstrip.c). MIT. */
#ifndef SH_OBBSTRIP_H
#define SH_OBBSTRIP_H
#include <stdint.h>

/* the entry that marks a stripped OBB (sh_obbindex.c looks for it too) */
#define SH_OBBSTRIP_MARK "assets/switch-mips-stripped"

typedef struct {
  unsigned textures, gui_kept, files;
  uint64_t bytes_saved;
} ShObbStripStats;

typedef void (*ShObbStripProgress)(uint64_t done, uint64_t total, void *ctx);

/* 1 the OBB has the mark, 0 not, -1 unreadable */
int sh_obbstrip_is_done(const char *obb);

/* obb -> out (a new zip): 1 written, -1 failed (out may be left half done) */
int sh_obbstrip_run(const char *obb, const char *out, ShObbStripProgress progress, void *ctx,
                    ShObbStripStats *st);

#endif
