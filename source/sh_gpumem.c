/* sh_gpumem.c -- the GPU buffers' memory, kept apart from the heap's small
 * allocations.
 *
 * libdrm_nouveau takes each buffer object's memory (textures, meshes, render
 * targets) from the heap, page-aligned, and gives it back when the object
 * goes. Mixed in with Unity's and Mono's small allocations, those blocks of
 * every size left the heap in pieces: at the second mission's load a 1 MB
 * texture found no room with 285 MB free (hardware, 2026-10-09: "memalign of
 * that size FAILS").
 *
 * libdrm calls nouveau_switch_bo_alloc / nouveau_switch_bo_free for that
 * memory (a hook added to its build: mesa32's build_mesa32.sh); here they
 * hand out pages of 32 MB chunks taken from the heap as they are needed and
 * kept: first fit, page by page, a bitmap and each allocation's length per
 * chunk. Bigger than a chunk, or no chunk to be had: the heap as before
 * (counted). [performance] gpu_buffer_pool. MIT.
 */
#include <malloc.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "dcr_config.h"
#include "util.h"

#define PAGE 0x1000u
#define CHUNK (32u << 20)
#define PAGES (CHUNK / PAGE) /* 8192 */
#define MAX_CHUNKS 28

typedef struct {
  uint8_t *base;
  uint32_t used[PAGES / 32]; /* a bit per page */
  uint16_t len[PAGES];       /* pages of the allocation starting at that page */
  uint32_t free_pages;
} Chunk;

static Chunk *g_chunk[MAX_CHUNKS];
static int g_nchunks;
static Mutex g_lock;
static uint64_t g_live, g_peak, g_heap_live, g_heap_n, g_no_chunk;

static int used(const Chunk *c, uint32_t p) { return (c->used[p >> 5] >> (p & 31)) & 1; }

static void mark(Chunk *c, uint32_t p, uint32_t n, int on) {
  for (uint32_t i = p; i < p + n; i++) {
    if (on)
      c->used[i >> 5] |= 1u << (i & 31);
    else
      c->used[i >> 5] &= ~(1u << (i & 31));
  }
}

/* n free pages in a row, first fit; -1 if none */
static int32_t find(const Chunk *c, uint32_t n) {
  uint32_t p = 0;
  while (p + n <= PAGES) {
    if (used(c, p)) { /* an allocation starts here: over it */
      p += c->len[p];
      continue;
    }
    if (!(p & 31) && c->used[p >> 5] == 0 && n >= 32) { /* a whole free word */
      uint32_t q = p;
      while (q < PAGES && q - p < n && c->used[q >> 5] == 0)
        q += 32;
      if (q - p >= n)
        return (int32_t)p;
      p = q;
      continue;
    }
    uint32_t q = p;
    while (q < PAGES && q - p < n && !used(c, q))
      q++;
    if (q - p >= n)
      return (int32_t)p;
    p = q;
  }
  return -1;
}

static void *from_heap(size_t size) {
  void *m = memalign(PAGE, size);
  if (m) {
    g_heap_live += size;
    g_heap_n++;
  }
  return m;
}

void *nouveau_switch_bo_alloc(size_t size) {
  if (!dcr_config()->gpu_pool)
    return memalign(PAGE, size);
  size = (size + PAGE - 1) & ~(size_t)(PAGE - 1);
  const uint32_t n = (uint32_t)(size / PAGE);
  mutexLock(&g_lock);
  void *m = NULL;
  if (n && size <= CHUNK) {
    for (int k = 0; k < g_nchunks && !m; k++) {
      Chunk *c = g_chunk[k];
      if (c->free_pages < n)
        continue;
      int32_t p = find(c, n);
      if (p >= 0) {
        mark(c, (uint32_t)p, n, 1);
        c->len[p] = (uint16_t)n;
        c->free_pages -= n;
        m = c->base + (uint32_t)p * PAGE;
      }
    }
    if (!m && g_nchunks < MAX_CHUNKS) { /* another chunk */
      Chunk *c = calloc(1, sizeof *c);
      uint8_t *base = c ? memalign(PAGE, CHUNK) : NULL;
      if (base) {
        c->base = base;
        c->free_pages = PAGES - n;
        mark(c, 0, n, 1);
        c->len[0] = (uint16_t)n;
        g_chunk[g_nchunks++] = c;
        m = base;
      } else {
        free(c);
        g_no_chunk++;
      }
    }
  }
  if (m) {
    g_live += size;
    if (g_live > g_peak)
      g_peak = g_live;
  } else {
    m = from_heap(size);
  }
  mutexUnlock(&g_lock);
  return m;
}

void nouveau_switch_bo_free(void *ptr) {
  if (!ptr)
    return;
  mutexLock(&g_lock);
  for (int k = 0; k < g_nchunks; k++) {
    Chunk *c = g_chunk[k];
    if ((uint8_t *)ptr >= c->base && (uint8_t *)ptr < c->base + CHUNK) {
      const uint32_t p = (uint32_t)(((uint8_t *)ptr - c->base) / PAGE), n = c->len[p];
      if (n && used(c, p)) {
        mark(c, p, n, 0);
        c->len[p] = 0;
        c->free_pages += n;
        g_live -= (uint64_t)n * PAGE;
      } else {
        debugPrintf("[gpu] pool: free of %p, not an allocation's start\n", ptr);
      }
      mutexUnlock(&g_lock);
      return;
    }
  }
  /* the heap's: its size is malloc's to know */
  size_t sz = malloc_usable_size(ptr);
  g_heap_live = g_heap_live > sz ? g_heap_live - sz : 0;
  mutexUnlock(&g_lock);
  free(ptr);
}

/* dcr_boot.c, every 600 frames */
void sh_gpumem_report(void) {
  if (!dcr_config()->gpu_pool)
    return;
  mutexLock(&g_lock);
  uint32_t largest = 0; /* the biggest free run, pages: what the pool could still give at once */
  for (int k = 0; k < g_nchunks; k++) {
    const Chunk *c = g_chunk[k];
    uint32_t run = 0;
    for (uint32_t p = 0; p < PAGES; p++) {
      run = used(c, p) ? 0 : run + 1;
      if (run > largest)
        largest = run;
    }
  }
  struct mallinfo mi = mallinfo();
  debugPrintf("[gpu] buffer pool: %d x 32 MB, %llu MB in use (peak %llu), largest free run %u KB; "
              "outside it %llu MB (%llu buffers so far, %llu chunks refused); heap %u MB in use, %u MB free\n",
              g_nchunks, (unsigned long long)(g_live >> 20), (unsigned long long)(g_peak >> 20),
              (unsigned)(largest * (PAGE >> 10)), (unsigned long long)(g_heap_live >> 20),
              (unsigned long long)g_heap_n, (unsigned long long)g_no_chunk, (unsigned)(mi.uordblks >> 20),
              (unsigned)(mi.fordblks >> 20));
  mutexUnlock(&g_lock);
}
