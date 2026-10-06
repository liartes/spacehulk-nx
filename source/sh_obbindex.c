/* sh_obbindex.c -- the OBB's zip headers, read once and kept.
 *
 * Unity reads an asset out of the OBB by seeking to the entry's local header
 * and reading its 30 bytes, then seeking to the data. The headers are spread
 * over 1.4 GB, so no read-ahead helps: each is a round trip to the SD card,
 * ~1.3 ms -- ~6000 of them at start-up ("1470 freads 43 KB (30 B each) in
 * 1988 ms", for seconds on end) and hundreds per mission load (hardware
 * 2026-10-04).
 *
 * So the first launch (or a new OBB) reads them all once, in file order,
 * from the zip's central directory, into <root>/obb/.headers (~200 KB):
 *     "SHOI" | u32 version | u64 OBB size | u32 count | u32 flags | count x {u32 offset, u8[30]}
 * (flags: FLAG_MIPS_STRIPPED when the OBB has the entry
 * "assets/switch-mips-stripped": its textures start at their second mip
 * already, so sh_quality.c must not skip one more)
 * sorted by offset. sh_io.c loads it with the OBB and serves any read that
 * falls inside a header from it (sh_obbindex_read), without the card.
 * Stale (another OBB size) or broken: rebuilt; absent: reads go to the card
 * as before. MIT.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "dcr_setup.h"
#include "util.h"

#define HDR 30u
#define MAGIC 0x494F4853u /* "SHOI" */
#define VERSION 2u
#define FLAG_MIPS_STRIPPED 1u /* tools/obb/strip_mips.py made this OBB */

typedef struct {
  uint32_t off;
  uint8_t h[HDR];
} __attribute__((packed)) Ent;

static Ent *g_ent;
static uint32_t g_n, g_flags;

int sh_obbindex_mips_stripped(void) { return (g_flags & FLAG_MIPS_STRIPPED) != 0; }
static uint64_t g_hits;

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static int cmp_off(const void *a, const void *b) {
  uint32_t x = ((const Ent *)a)->off, y = ((const Ent *)b)->off;
  return x < y ? -1 : x > y;
}

static void index_path(char *out, size_t cap) { rt_root_path(out, cap, "obb/.headers"); }

/* ------------------------------------------------------------------ load */
int sh_obbindex_load(int64_t obb_size) {
  char p[300];
  index_path(p, sizeof p);
  FILE *f = fopen(p, "rb");
  if (!f)
    return -1;
  uint8_t hd[24];
  int ok = fread(hd, 1, sizeof hd, f) == sizeof hd && le32(hd) == MAGIC && le32(hd + 4) == VERSION &&
           (int64_t)((uint64_t)le32(hd + 8) | (uint64_t)le32(hd + 12) << 32) == obb_size;
  uint32_t n = ok ? le32(hd + 16) : 0, flags = ok ? le32(hd + 20) : 0;
  Ent *e = ok && n ? malloc(sizeof(Ent) * n) : NULL;
  ok = e && fread(e, sizeof(Ent), n, f) == n;
  fclose(f);
  if (!ok) {
    free(e);
    return -1;
  }
  g_ent = e;
  g_n = n;
  g_flags = flags;
  debugPrintf("[io] OBB header index: %lu zip headers in RAM (%lu KB)%s\n", (unsigned long)n,
              (unsigned long)(sizeof(Ent) * n >> 10),
              flags & FLAG_MIPS_STRIPPED ? "; textures already at their second mip (strip_mips.py)" : "");
  return 0;
}

/* A read of [pos, pos+n) wholly inside one header: copied, 1. Else 0. */
int sh_obbindex_read(int64_t pos, void *dst, uint32_t n) {
  if (!g_n || pos < 0 || pos > 0xFFFFFFFFll)
    return 0;
  uint32_t lo = 0, hi = g_n;
  while (lo < hi) { /* the last header at or before pos */
    uint32_t mid = (lo + hi) / 2;
    if (g_ent[mid].off <= (uint32_t)pos)
      lo = mid + 1;
    else
      hi = mid;
  }
  if (!lo)
    return 0;
  const Ent *e = &g_ent[lo - 1];
  uint32_t in = (uint32_t)pos - e->off;
  if (in + n > HDR)
    return 0;
  memcpy(dst, e->h + in, n);
  g_hits++;
  return 1;
}

uint64_t sh_obbindex_hits(void) { return g_hits; }

/* The first header after pos (where the entry pos is in ends), or -1. */
int64_t sh_obbindex_next(int64_t pos) {
  if (!g_n || pos < 0 || pos > 0xFFFFFFFFll)
    return -1;
  uint32_t lo = 0, hi = g_n;
  while (lo < hi) {
    uint32_t mid = (lo + hi) / 2;
    if (g_ent[mid].off <= (uint32_t)pos)
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo < g_n ? (int64_t)g_ent[lo].off : -1;
}

/* ----------------------------------------------------------------- build */
static int read_at(FILE *f, int64_t off, void *dst, size_t n) {
  return fseeko(f, (off_t)off, SEEK_SET) == 0 && fread(dst, 1, n, f) == n ? 0 : -1;
}

/* The setup step (dcr_setup_plan.c): nothing when the index matches. */
void sh_obbindex_build(const char *obb, int64_t size, int p0, int p1) {
  if (size <= 22 || size > 0xFFFFFFFFll || sh_obbindex_load(size) == 0)
    return;
  FILE *f = fopen(obb, "rb");
  if (!f)
    return;
  setvbuf(f, NULL, _IOFBF, 64 * 1024);
  /* the end of central directory record: in the last 64 KB + 22 */
  uint32_t tail = size < 65557 ? (uint32_t)size : 65557u;
  uint8_t *t = malloc(tail);
  uint8_t *cd = NULL;
  Ent *e = NULL;
  uint32_t n = 0, cd_size = 0, cd_off = 0, count = 0, flags = 0;
  if (!t || read_at(f, size - tail, t, tail))
    goto out;
  for (int32_t i = (int32_t)tail - 22; i >= 0; i--)
    if (le32(t + i) == 0x06054b50u) {
      count = le16(t + i + 10);
      cd_size = le32(t + i + 12);
      cd_off = le32(t + i + 16);
      break;
    }
  if (!count || (int64_t)cd_off + cd_size > size || !(cd = malloc(cd_size)) || read_at(f, cd_off, cd, cd_size))
    goto out;
  if (!(e = malloc(sizeof(Ent) * count)))
    goto out;
  for (uint32_t p = 0; p + 46 <= cd_size && n < count;) {
    if (le32(cd + p) != 0x02014b50u)
      break;
    e[n++].off = le32(cd + p + 42);
    static const char mark[] = "assets/switch-mips-stripped";
    if (le16(cd + p + 28) == sizeof mark - 1 && !memcmp(cd + p + 46, mark, sizeof mark - 1))
      flags |= FLAG_MIPS_STRIPPED;
    p += 46u + le16(cd + p + 28) + le16(cd + p + 30) + le16(cd + p + 32);
  }
  qsort(e, n, sizeof(Ent), cmp_off);
  for (uint32_t i = 0; i < n; i++) {
    if (read_at(f, e[i].off, e[i].h, HDR) || le32(e[i].h) != 0x04034b50u) {
      debugPrintf("[setup] OBB header %lu (at %lu) unreadable: no index\n", (unsigned long)i,
                  (unsigned long)e[i].off);
      n = 0;
      goto out;
    }
    if (i % 64 == 0)
      rt_setup_progress_in("Indexing the OBB (once)", p0, p1, i, n);
  }
  {
    char p[300], tmp[310];
    index_path(p, sizeof p);
    snprintf(tmp, sizeof tmp, "%s.part", p);
    FILE *o = fopen(tmp, "wb");
    uint8_t hd[24];
    uint32_t v[6] = {MAGIC, VERSION, (uint32_t)size, (uint32_t)((uint64_t)size >> 32), n, flags};
    memcpy(hd, v, sizeof hd); /* little-endian, as le32 reads it */
    int ok = o && fwrite(hd, 1, sizeof hd, o) == sizeof hd && fwrite(e, sizeof(Ent), n, o) == n;
    if (o)
      ok = fclose(o) == 0 && ok;
    remove(p);
    if (ok && rename(tmp, p) == 0)
      debugPrintf("[setup] OBB indexed: %lu zip headers -> %s\n", (unsigned long)n, p);
    else
      debugPrintf("[setup] OBB index could not be written to %s\n", p);
  }
out:
  free(t);
  free(cd);
  free(e);
  fclose(f);
}
