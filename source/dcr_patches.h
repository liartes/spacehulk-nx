/* dcr_patches.h -- guarded code patches against dcr_offsets.h. */
#ifndef DCR_PATCHES_H
#define DCR_PATCHES_H
#include <stdint.h>
#include "so_util.h"

/* 1 if the four words at mod+rva are `guard`; logs and returns 0 otherwise. */
int dcr_guard_ok(so_module *mod, uint32_t rva, const uint32_t guard[4], const char *what);

/* Hook the ARM-mode function at mod+rva to `dst` (guard checked first). */
int dcr_hook(so_module *mod, uint32_t rva, const uint32_t guard[4], void *dst, const char *what);

/* The engine patches applied before initJni (Choreographer VSYNC -> ret). */
void dcr_patches_apply(void);

#endif
