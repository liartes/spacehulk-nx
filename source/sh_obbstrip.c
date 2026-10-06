/* sh_obbstrip.c -- the OBB made Switch-sized, once, on the console.
 *
 * The port loads textures from their second mip (sh_quality.c: the 1 GB of a
 * 32-bit Switch program cannot hold the 2048x2048 ETC2 ones), yet Unity reads
 * and inflates every texture's top mip from the OBB first, then drops it.
 * This rewrites the player's OBB with every mipmapped texture starting at its
 * second mip already -- the same picture, the OBB 1.48 GB -> 0.9 GB and 1.3
 * GB less to read and inflate per full read -- and marks it with the entry
 * "assets/switch-mips-stripped", which sh_obbindex.c sees (sh_quality.c then
 * skips no further mip). It is tools/obb/strip_mips.py (the PC prototype,
 * checked on hardware 2026-10-06) done on the console at the first launch,
 * so that players only copy their own OBB.
 *
 * Unity 5.3's serialized files (assets/bin/Data/levelN, sharedassetsN.assets,
 * resources.assets and the 32-hex-digit ones, whole or in 1 MB .splitN parts)
 * are format 15, little-endian, without type trees:
 *   header (big-endian): metadata size, file size, version 15, data offset;
 *     endianness byte (0) + 3
 *   metadata: Unity version (C string), platform, type-tree flag (0),
 *     types {class id; 16-byte script id if class id < 0; 16-byte hash},
 *     objects {align 4; path id (8); start (4, from the data offset);
 *              size (4); type id (4); class id (2); script type (2);
 *              stripped (1)}, then scripts, externals... (left as they are)
 *   data: each object at an 8-byte boundary, the file ending with the last.
 * A Texture2D (class 28) is: name (int length, bytes, align 4), width,
 * height, complete image size, format, mip count, readable, read allowed
 * (align 4), image count, dimension, settings (4 x 4), lightmap format,
 * colour space, image data (int size, bytes, align 4), stream data (offset,
 * size, path: empty -- all of this game's are inline).
 *
 * Left whole: textures without mipmaps, smaller than 8, of a format with no
 * known size, streamed, or named *GUI* -- NGUI places its sprites in pixels
 * of texture.width/height, which a stripped atlas halves (the "Menu GUI"
 * atlas made the menus' selection frames solid blocks, 2026-10-06).
 *
 * Entries that do not change are copied compressed as they are (miniz's
 * add_from_zip_reader); rewritten files are deflated at speed 1. Plain C and
 * miniz: tools/obb/test_strip.sh runs it on the PC. MIT.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef SH_OBBSTRIP_HOST
#include "miniz.h"
#define LOG(...) printf(__VA_ARGS__)
#else
#include <miniz/miniz.h>
#include "util.h"
#define LOG(...) debugPrintf(__VA_ARGS__)
#endif

#include "sh_obbstrip.h"

#define SPLIT_BYTES (1024u * 1024u) /* Unity's .splitN parts */
#define DATA_PREFIX "assets/bin/Data/"
#define CLASS_TEXTURE2D 28

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static void put_be32(uint8_t *p, uint32_t v) { p[0] = v >> 24, p[1] = v >> 16, p[2] = v >> 8, p[3] = v; }
static void put_le32(uint8_t *p, uint32_t v) { p[0] = v, p[1] = v >> 8, p[2] = v >> 16, p[3] = v >> 24; }

/* ------------------------------------------------------------ a texture */
/* bytes of a w x h level, by Unity TextureFormat; 0 unknown */
static uint32_t level_bytes(int fmt, uint32_t w, uint32_t h) {
  switch (fmt) {
  case 10: case 34: case 45: case 46: /* DXT1, ETC_RGB4, ETC2_RGB, ETC2_RGBA1 */
    return ((w + 3) / 4) * ((h + 3) / 4) * 8;
  case 12: case 47: /* DXT5, ETC2_RGBA8 */
    return ((w + 3) / 4) * ((h + 3) / 4) * 16;
  case 1: return w * h;              /* Alpha8 */
  case 3: return w * h * 3;          /* RGB24 */
  case 4: case 5: case 14: return w * h * 4; /* RGBA32, ARGB32, BGRA32 */
  case 7: case 13: return w * h * 2; /* RGB565, RGBA4444 */
  default: return 0;
  }
}

/* src (size bytes) -> dst without its top mip; the new size, or 0 to keep it */
static uint32_t strip_texture(const uint8_t *src, uint32_t size, uint8_t *dst, ShObbStripStats *st) {
  uint32_t p = 0;
#define NEED(n) do { if (p + (n) > size) return 0; } while (0)
  NEED(4);
  uint32_t nlen = le32(src);
  p = 4;
  NEED(nlen);
  const uint8_t *name = src + 4;
  p = (4 + nlen + 3) & ~3u;
  const uint32_t at_dims = p;
  NEED(20);
  uint32_t w = le32(src + p), h = le32(src + p + 4);
  int fmt = (int)le32(src + p + 12), mips = (int)le32(src + p + 16);
  p += 20 + 2;
  p = (p + 3) & ~3u;
  NEED(8);
  uint32_t images = le32(src + p);
  p += 8 + 16 + 8; /* image count, dimension; settings; lightmap format, colour space */
  NEED(4);
  const uint32_t at_size = p;
  uint32_t dsz = le32(src + p);
  p += 4;
  NEED(dsz);
  const uint32_t at_data = p;
  p = (p + dsz + 3) & ~3u;
  if (p + 12 != size || le32(src + p + 4) != 0) /* the stream data: inline only */
    return 0;
  for (uint32_t i = 0; i + 3 <= nlen; i++)
    if (!memcmp(name + i, "GUI", 3)) {
      st->gui_kept++;
      return 0;
    }
  uint32_t top = level_bytes(fmt, w, h);
  if (mips <= 1 || w < 8 || h < 8 || images != 1 || !top || dsz <= top)
    return 0;
  const uint32_t nd = dsz - top;
  /* name and everything before the dimensions, as it was */
  memcpy(dst, src, at_dims);
  put_le32(dst + at_dims, w / 2);
  put_le32(dst + at_dims + 4, h / 2);
  put_le32(dst + at_dims + 8, nd); /* complete image size: the one image */
  put_le32(dst + at_dims + 12, (uint32_t)fmt);
  put_le32(dst + at_dims + 16, (uint32_t)(mips - 1));
  memcpy(dst + at_dims + 20, src + at_dims + 20, at_size - (at_dims + 20));
  put_le32(dst + at_size, nd);
  memcpy(dst + at_data, src + at_data + top, nd);
  uint32_t q = at_data + nd;
  while (q & 3)
    dst[q++] = 0;
  memcpy(dst + q, src + p, 12);
  st->textures++;
  st->bytes_saved += top;
  return q + 12;
#undef NEED
}

/* ------------------------------------------------------------ a file */
typedef struct {
  uint32_t entry, start, size;
  uint16_t cls;
} Obj;

static int by_start(const void *a, const void *b) {
  uint32_t x = ((const Obj *)a)->start, y = ((const Obj *)b)->start;
  return x < y ? -1 : x > y;
}

/* A serialized file -> *out (malloc'd) with its textures stripped; the
 * textures changed (0: leave the file as it is). */
static int strip_file(const uint8_t *in, uint32_t len, uint8_t **out, uint32_t *out_len, ShObbStripStats *st) {
  *out = NULL;
  if (len < 20 || be32(in + 8) != 15 || in[16] != 0 || be32(in + 4) != len)
    return 0;
  const uint32_t doff = be32(in + 12);
  uint32_t p = 20;
  while (p < len && in[p])
    p++;
  p += 1 + 4; /* the version's NUL; platform */
  if (p + 5 > len || in[p] != 0) /* type trees: not this game's */
    return 0;
  p++;
  uint32_t types = le32(in + p);
  p += 4;
  for (uint32_t i = 0; i < types; i++) {
    if (p + 4 > len)
      return 0;
    int32_t cid = (int32_t)le32(in + p);
    p += 4 + (cid < 0 ? 16 : 0) + 16;
  }
  if (p + 4 > len)
    return 0;
  uint32_t n = le32(in + p);
  p += 4;
  Obj *o = malloc(sizeof(Obj) * (n ? n : 1));
  if (!o)
    return 0;
  int textures = 0;
  for (uint32_t i = 0; i < n; i++) {
    p = (p + 3) & ~3u;
    if (p + 25 > doff) {
      free(o);
      return 0;
    }
    o[i].entry = p;
    o[i].start = le32(in + p + 8);
    o[i].size = le32(in + p + 12);
    o[i].cls = le16(in + p + 20);
    p += 25;
    if ((uint64_t)doff + o[i].start + o[i].size > len) {
      free(o);
      return 0;
    }
    textures += o[i].cls == CLASS_TEXTURE2D;
  }
  if (!textures) {
    free(o);
    return 0;
  }
  qsort(o, n, sizeof(Obj), by_start);
  uint8_t *w = malloc((size_t)len + 8u * n + 16);
  if (!w) {
    free(o);
    return 0;
  }
  memcpy(w, in, doff);
  uint32_t at = doff;
  int changed = 0;
  for (uint32_t i = 0; i < n; i++) {
    while ((at - doff) & 7)
      w[at++] = 0;
    const uint8_t *src = in + doff + o[i].start;
    uint32_t ns = 0;
    if (o[i].cls == CLASS_TEXTURE2D)
      ns = strip_texture(src, o[i].size, w + at, st);
    if (ns)
      changed++;
    else {
      memcpy(w + at, src, o[i].size);
      ns = o[i].size;
    }
    put_le32(w + o[i].entry + 8, at - doff);
    put_le32(w + o[i].entry + 12, ns);
    at += ns;
  }
  free(o);
  if (!changed) {
    free(w);
    return 0;
  }
  put_be32(w + 4, at);
  *out = w;
  *out_len = at;
  return changed;
}

/* ------------------------------------------------------------ the OBB */
/* name -> its serialized file's name (without .splitN) and part (-1 whole);
 * 0 when it is not one */
static int file_part(const char *name, char *base, size_t cap, int *part) {
  size_t pl = sizeof DATA_PREFIX - 1;
  if (strncmp(name, DATA_PREFIX, pl))
    return 0;
  const char *r = name + pl;
  if (strchr(r, '/'))
    return 0;
  size_t n = strlen(r);
  *part = -1;
  const char *sp = strstr(r, ".split");
  if (sp) {
    char *end;
    long k = strtol(sp + 6, &end, 10);
    if (*end || end == sp + 6)
      return 0;
    *part = (int)k;
    n = (size_t)(sp - r);
  }
  if (n + pl + 1 > cap)
    return 0;
  int ok = 0;
  if (n > 5 && !strncmp(r, "level", 5)) {
    ok = 1;
    for (size_t i = 5; i < n; i++)
      ok &= r[i] >= '0' && r[i] <= '9';
  } else if (n > 19 && !strncmp(r, "sharedassets", 12) && !strncmp(r + n - 7, ".assets", 7)) {
    ok = 1;
    for (size_t i = 12; i < n - 7; i++)
      ok &= r[i] >= '0' && r[i] <= '9';
  } else if (n == 16 && !strncmp(r, "resources.assets", 16)) {
    ok = 1;
  } else if (n == 32) {
    ok = 1;
    for (size_t i = 0; i < 32; i++)
      ok &= (r[i] >= '0' && r[i] <= '9') || (r[i] >= 'a' && r[i] <= 'f');
  }
  if (!ok)
    return 0;
  memcpy(base, name, pl + n);
  base[pl + n] = 0;
  return 1;
}

int sh_obbstrip_is_done(const char *obb) {
  mz_zip_archive z;
  memset(&z, 0, sizeof z);
  if (!mz_zip_reader_init_file(&z, obb, 0))
    return -1;
  int done = mz_zip_reader_locate_file(&z, SH_OBBSTRIP_MARK, NULL, 0) >= 0;
  mz_zip_reader_end(&z);
  return done;
}

int sh_obbstrip_run(const char *obb, const char *out, ShObbStripProgress progress, void *ctx,
                    ShObbStripStats *st) {
  memset(st, 0, sizeof *st);
  mz_zip_archive zr, zw;
  memset(&zr, 0, sizeof zr);
  memset(&zw, 0, sizeof zw);
  if (!mz_zip_reader_init_file(&zr, obb, 0)) {
    LOG("[obb] %s: not a zip miniz reads\n", obb);
    return -1;
  }
  const mz_uint n = mz_zip_reader_get_num_files(&zr);
  uint8_t *done = calloc(n, 1);
  int *order = malloc(sizeof(int) * n);
  if (!done || !order || !mz_zip_writer_init_file(&zw, out, 0)) {
    LOG("[obb] cannot write %s\n", out);
    free(done);
    free(order);
    mz_zip_reader_end(&zr);
    return -1;
  }
  uint64_t total = 0, sofar = 0;
  for (mz_uint i = 0; i < n; i++) {
    mz_zip_archive_file_stat fs;
    if (mz_zip_reader_file_stat(&zr, i, &fs))
      total += fs.m_comp_size;
  }
  int ok = 1;
  for (mz_uint i = 0; i < n && ok; i++) {
    if (done[i])
      continue;
    mz_zip_archive_file_stat fs;
    char base[256];
    int part;
    if (!mz_zip_reader_file_stat(&zr, i, &fs)) {
      ok = 0;
      break;
    }
    if (!file_part(fs.m_filename, base, sizeof base, &part)) { /* as it is */
      ok = mz_zip_writer_add_from_zip_reader(&zw, &zr, i);
      done[i] = 1;
      sofar += fs.m_comp_size;
      if (progress)
        progress(sofar, total, ctx);
      continue;
    }
    /* the whole serialized file: its parts, in order */
    int parts = 0, max_part = -1;
    uint64_t raw = 0, comp = 0;
    for (mz_uint j = i; j < n; j++) {
      mz_zip_archive_file_stat f2;
      char b2[256];
      int p2;
      if (!mz_zip_reader_file_stat(&zr, j, &f2) || !file_part(f2.m_filename, b2, sizeof b2, &p2) || strcmp(b2, base))
        continue;
      order[parts++] = (int)j;
      raw += f2.m_uncomp_size;
      comp += f2.m_comp_size;
      if (p2 > max_part)
        max_part = p2;
    }
    const int split = max_part >= 0;
    if (split && parts != max_part + 1)
      LOG("[obb] %s: %d of %d parts: left as it is\n", base, parts, max_part + 1);
    uint8_t *buf = raw <= 0x7fffffff ? malloc((size_t)raw + 1) : NULL;
    int whole = buf && (!split || parts == max_part + 1);
    if (whole && split) { /* part k at its place */
      uint64_t off = 0;
      for (int k = 0; k <= max_part && whole; k++) {
        int idx = -1;
        for (int q = 0; q < parts; q++) {
          mz_zip_archive_file_stat f3;
          char b3[256];
          int p3;
          if (mz_zip_reader_file_stat(&zr, (mz_uint)order[q], &f3) && file_part(f3.m_filename, b3, sizeof b3, &p3) &&
              p3 == k) {
            idx = order[q];
            if (!mz_zip_reader_extract_to_mem(&zr, (mz_uint)idx, buf + off, (size_t)f3.m_uncomp_size, 0))
              whole = 0;
            off += f3.m_uncomp_size;
            break;
          }
        }
        if (idx < 0)
          whole = 0;
      }
    } else if (whole) {
      whole = mz_zip_reader_extract_to_mem(&zr, i, buf, (size_t)raw, 0);
    }
    uint8_t *nb = NULL;
    uint32_t nlen = 0;
    int changed = whole ? strip_file(buf, (uint32_t)raw, &nb, &nlen, st) : 0;
    free(buf);
    if (changed) {
      if (split)
        for (uint32_t off = 0, k = 0; off < nlen && ok; off += SPLIT_BYTES, k++) {
          char nm[300];
          snprintf(nm, sizeof nm, "%s.split%u", base, (unsigned)k);
          uint32_t sz = nlen - off < SPLIT_BYTES ? nlen - off : SPLIT_BYTES;
          ok = mz_zip_writer_add_mem(&zw, nm, nb + off, sz, MZ_BEST_SPEED);
        }
      else
        ok = mz_zip_writer_add_mem(&zw, base, nb, nlen, MZ_BEST_SPEED);
      st->files++;
      free(nb);
    } else {
      for (int q = 0; q < parts && ok; q++)
        ok = mz_zip_writer_add_from_zip_reader(&zw, &zr, (mz_uint)order[q]);
    }
    for (int q = 0; q < parts; q++)
      done[order[q]] = 1;
    sofar += comp;
    if (progress)
      progress(sofar, total, ctx);
  }
  if (ok)
    ok = mz_zip_writer_add_mem(&zw, SH_OBBSTRIP_MARK, "1\n", 2, MZ_NO_COMPRESSION);
  if (ok)
    ok = mz_zip_writer_finalize_archive(&zw);
  mz_zip_writer_end(&zw);
  mz_zip_reader_end(&zr);
  free(done);
  free(order);
  if (!ok)
    LOG("[obb] writing %s failed\n", out);
  return ok ? 1 : -1;
}
