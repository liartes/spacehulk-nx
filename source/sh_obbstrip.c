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

/* miniz's own file I/O goes through stdio with the C library's buffer -- 1
 * KB with libnx's fsdev, each refill and each flush a request to the SD
 * card: the first console run took 682 s, ~2 MB/s (2026-10-06). The reads
 * and writes come through these instead, on FILEs with big buffers: the
 * entries are taken in file order and the output is written in order (but
 * for each local header, patched once its data is in). */
#define READ_BUF (256u * 1024u)
#define WRITE_BUF (1024u * 1024u)

static size_t io_read(void *opaque, mz_uint64 ofs, void *buf, size_t n) {
  FILE *f = opaque;
  if ((mz_uint64)ftello(f) != ofs && fseeko(f, (off_t)ofs, SEEK_SET) != 0)
    return 0;
  return fread(buf, 1, n, f);
}

static size_t io_write(void *opaque, mz_uint64 ofs, const void *buf, size_t n) {
  FILE *f = opaque;
  if ((mz_uint64)ftello(f) != ofs && fseeko(f, (off_t)ofs, SEEK_SET) != 0)
    return 0;
  return fwrite(buf, 1, n, f);
}

/* ------------------------------------------------------------ threads
 * Reading and writing stay on the calling thread, in file order; the CPU
 * work of each serialized file (inflate, strip, deflate, CRC) goes to
 * workers on the three application cores. On the console the work was
 * ~85% of the 146 s (PC run of the same code: inflate 26 s, deflate 14 s of
 * 68), the SD card idle meanwhile. */
#ifdef SH_OBBSTRIP_HOST
#include <pthread.h>
#include <time.h>
typedef pthread_mutex_t Lock;
typedef pthread_cond_t Cond;
static void lock_init(Lock *l) { pthread_mutex_init(l, NULL); }
static void lock(Lock *l) { pthread_mutex_lock(l); }
static void unlock(Lock *l) { pthread_mutex_unlock(l); }
static void cond_init(Cond *c) { pthread_cond_init(c, NULL); }
static void cond_wait(Cond *c, Lock *l) { pthread_cond_wait(c, l); }
static void cond_wake(Cond *c) { pthread_cond_broadcast(c); }
static uint64_t now_ns(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
#else
#include <switch.h>
typedef Mutex Lock;
typedef CondVar Cond;
static void lock_init(Lock *l) { mutexInit(l); }
static void lock(Lock *l) { mutexLock(l); }
static void unlock(Lock *l) { mutexUnlock(l); }
static void cond_init(Cond *c) { condvarInit(c); }
static void cond_wait(Cond *c, Lock *l) { condvarWait(c, l); }
static void cond_wake(Cond *c) { condvarWakeAll(c); }
static uint64_t now_ns(void) { return armTicksToNs(armGetSystemTick()); }
#endif

#define WORKERS 3
#define INFLIGHT_BYTES (160u << 20) /* read ahead of the writer, compressed + raw */

typedef struct {
  char *base;     /* the serialized file's name; NULL: an entry copied as it is */
  int part;       /* -1 whole */
  mz_uint idx;
  mz_uint16 method;
  mz_uint32 crc;
  uint64_t comp, raw;
} Ent;

typedef struct {
  int first, count;        /* ents[first..first+count), parts in order */
  uint8_t **comp;          /* each part's bytes as stored in the OBB */
  uint8_t *out;            /* the stripped file, or NULL: unchanged */
  uint32_t out_len;
  uint8_t **dpart;         /* the stripped file's parts, deflated */
  size_t *dlen;
  mz_uint32 *dcrc;
  int nout;                /* its parts (1 when not split) */
  ShObbStripStats st;
  uint64_t bytes;          /* held in memory, for the in-flight bound */
  uint64_t t_inflate, t_strip, t_deflate;
  volatile int state;      /* 0 read, 1 taken, 2 done */
} Job;

typedef struct {
  Lock lock;
  Cond cond;
  Job *jobs;
  int njobs, nread, quit;
  const Ent *ents;
} Pool;

static int by_base(const void *a, const void *b) {
  const Ent *x = a, *y = b;
  int c = strcmp(x->base, y->base);
  if (c)
    return c;
  if (x->part != y->part)
    return x->part < y->part ? -1 : 1;
  return x->idx < y->idx ? -1 : x->idx > y->idx;
}

static void work(Job *j, const Ent *e) {
  uint64_t t0 = now_ns(), raw = 0;
  for (int k = 0; k < j->count; k++)
    raw += e[k].raw;
  uint8_t *buf = raw <= 0x7fffffff ? malloc((size_t)raw + 1) : NULL;
  int whole = buf != NULL;
  uint64_t off = 0;
  for (int k = 0; k < j->count && whole; k++) {
    if (e[k].method == 0)
      memcpy(buf + off, j->comp[k], (size_t)e[k].raw);
    else if (tinfl_decompress_mem_to_mem(buf + off, (size_t)e[k].raw, j->comp[k], (size_t)e[k].comp, 0) !=
             (size_t)e[k].raw)
      whole = 0;
    off += e[k].raw;
  }
  uint64_t t1 = now_ns();
  j->t_inflate = t1 - t0;
  int changed = whole ? strip_file(buf, (uint32_t)raw, &j->out, &j->out_len, &j->st) : 0;
  free(buf);
  uint64_t t2 = now_ns();
  j->t_strip = t2 - t1;
  if (!changed) {
    j->out = NULL;
    return;
  }
  const int split = e[0].part >= 0;
  j->nout = split ? (int)((j->out_len + SPLIT_BYTES - 1) / SPLIT_BYTES) : 1;
  if (j->nout < 1)
    j->nout = 1;
  j->dpart = calloc((size_t)j->nout, sizeof *j->dpart);
  j->dlen = calloc((size_t)j->nout, sizeof *j->dlen);
  j->dcrc = calloc((size_t)j->nout, sizeof *j->dcrc);
  const int flags = (int)tdefl_create_comp_flags_from_zip_params(MZ_BEST_SPEED, -MZ_DEFAULT_WINDOW_BITS, MZ_DEFAULT_STRATEGY);
  for (int k = 0; k < j->nout && j->dpart; k++) {
    uint32_t o = split ? (uint32_t)k * SPLIT_BYTES : 0;
    uint32_t sz = split ? (j->out_len - o < SPLIT_BYTES ? j->out_len - o : SPLIT_BYTES) : j->out_len;
    j->dcrc[k] = (mz_uint32)mz_crc32(MZ_CRC32_INIT, j->out + o, sz);
    j->dpart[k] = tdefl_compress_mem_to_heap(j->out + o, sz, &j->dlen[k], flags);
  }
  j->t_deflate = now_ns() - t2;
}

#ifdef SH_OBBSTRIP_HOST
static void *worker(void *arg) {
#else
static void worker(void *arg) {
#endif
  Pool *p = arg;
  lock(&p->lock);
  for (;;) {
    int i = 0;
    while (i < p->nread && p->jobs[i].state != 0)
      i++;
    if (i < p->nread) {
      Job *j = &p->jobs[i];
      j->state = 1;
      unlock(&p->lock);
      work(j, p->ents + j->first);
      lock(&p->lock);
      j->state = 2;
      cond_wake(&p->cond);
      continue;
    }
    if (p->quit)
      break;
    cond_wait(&p->cond, &p->lock);
  }
  unlock(&p->lock);
#ifdef SH_OBBSTRIP_HOST
  return NULL;
#endif
}

static void job_free(Job *j) {
  for (int k = 0; k < j->count && j->comp; k++)
    free(j->comp[k]);
  free(j->comp);
  for (int k = 0; k < j->nout && j->dpart; k++)
    free(j->dpart[k]);
  free(j->dpart);
  free(j->dlen);
  free(j->dcrc);
  free(j->out);
  memset(j, 0, sizeof *j);
  j->state = 3; /* written */
}

int sh_obbstrip_run(const char *obb, const char *out, ShObbStripProgress progress, void *ctx,
                    ShObbStripStats *st) {
  memset(st, 0, sizeof *st);
  mz_zip_archive zr, zw;
  memset(&zr, 0, sizeof zr);
  memset(&zw, 0, sizeof zw);
  FILE *fr = fopen(obb, "rb");
  if (!fr) {
    LOG("[obb] %s: cannot open it\n", obb);
    return -1;
  }
  setvbuf(fr, NULL, _IOFBF, READ_BUF);
  fseeko(fr, 0, SEEK_END);
  const mz_uint64 in_size = (mz_uint64)ftello(fr);
  zr.m_pRead = io_read;
  zr.m_pIO_opaque = fr;
  if (!mz_zip_reader_init(&zr, in_size, 0)) {
    LOG("[obb] %s: not a zip miniz reads\n", obb);
    fclose(fr);
    return -1;
  }
  FILE *fw = fopen(out, "wb");
  if (fw)
    setvbuf(fw, NULL, _IOFBF, WRITE_BUF);
  zw.m_pWrite = io_write;
  zw.m_pIO_opaque = fw;
  const mz_uint n = mz_zip_reader_get_num_files(&zr);
  Ent *ents = calloc(n ? n : 1, sizeof *ents), *sorted = calloc(n ? n : 1, sizeof *sorted);
  int *group = malloc(sizeof(int) * (n ? n : 1)); /* entry -> its file's first in sorted, or -1 */
  Job *jobs = calloc(n ? n : 1, sizeof *jobs);
  if (!ents || !sorted || !group || !jobs || !fw || !mz_zip_writer_init(&zw, 0)) {
    LOG("[obb] cannot write %s\n", out);
    free(ents), free(sorted), free(group), free(jobs);
    mz_zip_reader_end(&zr);
    fclose(fr);
    if (fw)
      fclose(fw);
    return -1;
  }
  /* every entry looked at once; a serialized file's parts grouped by sorting
   * on (name, part) -- looking for them afresh for each file was a third of
   * the time (24 s of 68 on the PC, 2026-10-08) */
  uint64_t total = 0, sofar = 0, t_read = 0, t_write = 0, t_wait = 0, t_inflate_all = 0, t_strip_all = 0, t_deflate_all = 0;
  int ok = 1, ns = 0;
  for (mz_uint i = 0; i < n && ok; i++) {
    mz_zip_archive_file_stat fs;
    char base[256];
    if (!mz_zip_reader_file_stat(&zr, i, &fs)) {
      ok = 0;
      break;
    }
    ents[i].idx = i;
    ents[i].method = fs.m_method;
    ents[i].crc = fs.m_crc32;
    ents[i].comp = fs.m_comp_size;
    ents[i].raw = fs.m_uncomp_size;
    total += fs.m_comp_size;
    group[i] = -1;
    if ((fs.m_method == 0 || fs.m_method == MZ_DEFLATED) && file_part(fs.m_filename, base, sizeof base, &ents[i].part)) {
      ents[i].base = strdup(base);
      sorted[ns++] = ents[i];
    }
  }
  qsort(sorted, (size_t)ns, sizeof *sorted, by_base);
  for (int a = 0, b; a < ns; a = b) {
    for (b = a + 1; b < ns && !strcmp(sorted[b].base, sorted[a].base);)
      b++;
    for (int k = a; k < b; k++)
      group[sorted[k].idx] = a;
  }

  Pool pool;
  memset(&pool, 0, sizeof pool);
  lock_init(&pool.lock);
  cond_init(&pool.cond);
  pool.jobs = jobs;
  pool.ents = sorted;
#ifdef SH_OBBSTRIP_HOST
  pthread_t th[WORKERS];
#else
  Thread th[WORKERS];
#endif
  int nth = 0;
  for (int w = 0; w < WORKERS; w++) {
#ifdef SH_OBBSTRIP_HOST
    if (pthread_create(&th[nth], NULL, worker, &pool) == 0)
      nth++;
#else
    /* below the caller's priority, which reads and writes for them */
    if (R_SUCCEEDED(threadCreate(&th[nth], worker, &pool, NULL, 0x10000, 0x2D, w)) && R_SUCCEEDED(threadStart(&th[nth])))
      nth++;
#endif
  }

  /* jobs in file order: each serialized file once (at its first entry), the
   * other entries as jobs with count 0 (copied as they are) */
  uint8_t *queued = calloc(n ? n : 1, 1);
  int nj = 0;
  for (mz_uint i = 0; i < n && ok && queued; i++) {
    if (queued[i])
      continue;
    Job *j = &jobs[nj++];
    if (group[i] < 0) {
      j->first = (int)i;
      j->count = 0;
      queued[i] = 1;
      continue;
    }
    int a = group[i], b = a;
    while (b < ns && !strcmp(sorted[b].base, sorted[a].base))
      b++;
    j->first = a;
    j->count = b - a;
    for (int k = a; k < b; k++)
      queued[sorted[k].idx] = 1;
  }
  free(queued);
  pool.njobs = nj;

  uint64_t held = 0;
  int w = 0; /* the next job to write */
  for (int r = 0; ok && (r < nj || w < nj);) {
    /* read ahead while the workers have room */
    if (r < nj && (r == w || held < INFLIGHT_BYTES)) {
      Job *j = &jobs[r];
      if (j->count == 0) {
        lock(&pool.lock);
        j->state = 2;
        pool.nread = ++r;
        unlock(&pool.lock);
        continue;
      }
      const Ent *e = sorted + j->first;
      const int split = e[0].part >= 0;
      int max_part = e[j->count - 1].part;
      int whole = !split || (max_part + 1 == j->count && e[0].part == 0);
      for (int k = 1; k < j->count && whole; k++)
        whole = e[k].part == e[k - 1].part + 1;
      if (!whole)
        LOG("[obb] %s: %d parts, not 0..%d: left as it is\n", e[0].base, j->count, max_part);
      uint64_t t = now_ns();
      j->comp = whole ? calloc((size_t)j->count, sizeof *j->comp) : NULL;
      for (int k = 0; k < j->count && j->comp; k++) {
        j->comp[k] = malloc((size_t)e[k].comp + 1);
        if (!j->comp[k] || !mz_zip_reader_extract_to_mem(&zr, e[k].idx, j->comp[k], (size_t)e[k].comp, MZ_ZIP_FLAG_COMPRESSED_DATA)) {
          LOG("[obb] %s: part %d unreadable\n", e[0].base, e[k].part);
          ok = 0;
        }
        j->bytes += e[k].comp + e[k].raw * 2;
      }
      t_read += now_ns() - t;
      held += j->bytes;
      lock(&pool.lock);
      j->state = j->comp ? 0 : 2; /* not whole: copied as it is */
      pool.nread = ++r;
      cond_wake(&pool.cond);
      unlock(&pool.lock);
      continue;
    }
    /* the oldest, once done */
    Job *j = &jobs[w];
    uint64_t t = now_ns();
    lock(&pool.lock);
    while (j->state != 2)
      cond_wait(&pool.cond, &pool.lock);
    unlock(&pool.lock);
    t_wait += now_ns() - t;
    t = now_ns();
    if (j->count == 0) {
      ok = mz_zip_writer_add_from_zip_reader(&zw, &zr, (mz_uint)j->first);
      sofar += ents[j->first].comp;
    } else {
      const Ent *e = sorted + j->first;
      if (j->out && j->dpart) {
        for (int k = 0; k < j->nout && ok; k++) {
          char nm[300];
          if (e[0].part >= 0)
            snprintf(nm, sizeof nm, "%s.split%d", e[0].base, k);
          else
            snprintf(nm, sizeof nm, "%s", e[0].base);
          uint32_t sz = e[0].part >= 0 ? (j->out_len - (uint32_t)k * SPLIT_BYTES < SPLIT_BYTES ? j->out_len - (uint32_t)k * SPLIT_BYTES : SPLIT_BYTES) : j->out_len;
          ok = j->dpart[k] && mz_zip_writer_add_mem_ex(&zw, nm, j->dpart[k], j->dlen[k], NULL, 0,
                                                      MZ_BEST_SPEED | MZ_ZIP_FLAG_COMPRESSED_DATA, sz, j->dcrc[k]);
        }
        st->files++;
      } else if (j->comp) { /* unchanged: its bytes as they were read */
        for (int k = 0; k < j->count && ok; k++) {
          char nm[300];
          if (e[k].part >= 0)
            snprintf(nm, sizeof nm, "%s.split%d", e[k].base, e[k].part);
          else
            snprintf(nm, sizeof nm, "%s", e[k].base);
          ok = e[k].method == 0 ? mz_zip_writer_add_mem(&zw, nm, j->comp[k], (size_t)e[k].raw, MZ_NO_COMPRESSION)
                                : mz_zip_writer_add_mem_ex(&zw, nm, j->comp[k], (size_t)e[k].comp, NULL, 0,
                                                           MZ_BEST_SPEED | MZ_ZIP_FLAG_COMPRESSED_DATA, e[k].raw, e[k].crc);
        }
      } else {
        for (int k = 0; k < j->count && ok; k++)
          ok = mz_zip_writer_add_from_zip_reader(&zw, &zr, e[k].idx);
      }
      for (int k = 0; k < j->count; k++)
        sofar += e[k].comp;
      st->textures += j->st.textures;
      st->gui_kept += j->st.gui_kept;
      st->bytes_saved += j->st.bytes_saved;
      t_inflate_all += j->t_inflate;
      t_strip_all += j->t_strip;
      t_deflate_all += j->t_deflate;
    }
    t_write += now_ns() - t;
    held -= j->bytes;
    job_free(j);
    w++;
    if (progress)
      progress(sofar, total, ctx);
  }

  lock(&pool.lock);
  pool.quit = 1;
  cond_wake(&pool.cond);
  unlock(&pool.lock);
  for (int k = 0; k < nth; k++) {
#ifdef SH_OBBSTRIP_HOST
    pthread_join(th[k], NULL);
#else
    threadWaitForExit(&th[k]);
    threadClose(&th[k]);
#endif
  }
  for (mz_uint i = w; i < (mz_uint)nj; i++) /* after a failure */
    if (jobs[i].state != 3)
      job_free(&jobs[i]);
  LOG("[obb] %d workers; caller: read %llu ms, waited %llu ms, wrote %llu ms; workers: inflate %llu ms, strip %llu ms, deflate %llu ms\n",
      nth, (unsigned long long)(t_read / 1000000), (unsigned long long)(t_wait / 1000000),
      (unsigned long long)(t_write / 1000000), (unsigned long long)(t_inflate_all / 1000000),
      (unsigned long long)(t_strip_all / 1000000), (unsigned long long)(t_deflate_all / 1000000));

  if (ok)
    ok = mz_zip_writer_add_mem(&zw, SH_OBBSTRIP_MARK, "1\n", 2, MZ_NO_COMPRESSION);
  if (ok)
    ok = mz_zip_writer_finalize_archive(&zw);
  mz_zip_writer_end(&zw);
  mz_zip_reader_end(&zr);
  fclose(fr);
  if (fclose(fw) != 0)
    ok = 0;
  for (int k = 0; k < ns; k++)
    free(sorted[k].base);
  free(ents), free(sorted), free(group), free(jobs);
  if (!ok)
    LOG("[obb] writing %s failed\n", out);
  return ok ? 1 : -1;
}
