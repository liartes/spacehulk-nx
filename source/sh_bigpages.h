/* sh_bigpages.h -- the game's code and Mono's JIT arena in 2 MB blocks
 * ([performance] large_pages). See sh_bigpages.c. MIT. */
#ifndef SH_BIGPAGES_H
#define SH_BIGPAGES_H

#include <stddef.h>
#include <stdint.h>

#define SH_BLOCK 0x200000u

/* Between begin and end, on the calling thread only: code-memory addresses
 * (virtmemFindCodeMemory) and libnx's aligned allocations of 2 MB or more come
 * 2 MB-aligned. Both do nothing when large_pages is off or under an emulator. */
void sh_bigpages_begin(void);
void sh_bigpages_end(void);
int sh_bigpages_on(void);

/* A 2 MB-aligned staging buffer for the module at path, sized as so_load
 * sizes it (highest PT_LOAD end, page-rounded); *size gets that size. NULL
 * (so_load then stages it itself) when large_pages is off, the module is under
 * 2 MB, or anything about the file is unexpected. */
void *sh_bigpages_staging(const char *path, size_t *size);

/* One "[pages]" line: where a mapping and its heap source sit modulo 2 MB,
 * and how many of its 2 MB stretches can be blocks (one memory block with one
 * permission over the whole stretch, and the same remainder as the source:
 * the heap is VA = PA modulo 2 MB). */
void sh_bigpages_report(const char *name, uintptr_t va, uintptr_t src, size_t size);

#endif
