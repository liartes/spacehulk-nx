/* sh_io.c -- stdio buffers for the files Unity reads, and what the reads cost.
 *
 * Loading the first mission, Unity's "asyncProcessor" thread spent 48% of a
 * 10 s window blocked in fsFileRead, called from fread -> __srefill_r
 * (hardware 2026-10-04, profile_continuous): the OBB is read through stdio,
 * and a newlib FILE refills its buffer one BUFSIZ at a time -- 1 KB, since
 * libnx's fsdev gives no st_blksize -- each refill an IPC round trip to the
 * fs service. A read-only FILE here gets [performance] read_buffer_kb
 * instead (default 32): one round trip brings that much. The OBB itself is
 * not read through stdio's buffers at all (below): 128 KB made every
 * 30-byte header read cost a 128 KB read.
 *
 * A bigger buffer is not a win if the engine seeks before every small read
 * (each fseek outside the buffer throws it away and the next read refills it
 * whole), so fread and fseek are counted: every 2 s with reads in it, the log
 * gets "[io] stdio:" with the calls, bytes, average read size, seeks and the
 * time spent inside them.
 *
 * THE OBB, OPENED ONCE. The counts showed what the loads really do: for
 * every asset it reads, Unity opens the 1.4 GB OBB again, seeks to the
 * entry's zip header and reads 30 bytes -- 80+ opens a second, each an
 * fsFsOpenFile on the SD card ("175 opens, 175 freads 5 KB (30 B each) in
 * 1002 ms, 350 fseeks in 706 ms", hardware 2026-10-04, a 1-minute mission
 * load). The runtime fixed the same pattern for the APK (bionic_stdio.c:
 * apk_fopen); here a read-only fopen of the OBB is a FILE of our own over
 * ONE FsFile, opened at the first fopen and kept: each FILE has its own
 * position, reads are positional (fsFileRead at an offset: no shared seek,
 * so threads do not disturb each other), and the FILE is unbuffered -- a
 * 30-byte read reads 30 bytes, an 8 KB inflate read goes straight into the
 * engine's buffer. (fpos_t is 32 bits: fine for this 1.4 GB OBB, not past
 * 2 GB.)
 *
 * The runtime's b_fopen/b_fread (bionic_stdio.c) do the path work, the APK's
 * RAM cache and the bionic FILE kinds; these only wrap them, through the
 * port's import overlay. MIT.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "dcr_config.h"
#include "dcr_path.h"
#include "imports.h"
#include "util.h"

void *b_fopen(const char *path, const char *mode);
size_t b_fread(void *p, size_t sz, size_t n, void *fp);
int b_fseek(void *fp, long off, int whence);

static struct {
  uint32_t opens, obb_opens, reads, seeks, ra_fills, stats, stats_obb;
  uint64_t bytes, read_ticks, seek_ticks, open_ticks, ra_bytes, stat_ticks, hdr_hits0, last_report;
} g;

/* ------------------------------------------------------- the OBB's FILE */
static FsFile g_obb;
static s64 g_obb_size = -1;
static Mutex g_obb_lock;
static char g_obb_real[300];

/* Read-ahead, per FILE: Unity inflates an entry in 8 KB reads, one after
 * the other, each a round trip to the SD card (~11 MB/s, hardware
 * 2026-10-04: "2959 freads 22162 KB (7669 B each) in 1900 ms"). A read that
 * continues the last one doubles the window (RA_MIN up to RA_MAX) and fills
 * it in one request; a read elsewhere (a 30-byte zip header, then the seek
 * to its data) reads only what it asked for and starts the window over. A
 * window never runs past the entry's end (the next zip header, from the
 * index): it did, and 40% of what was read ahead was the next entry's,
 * thrown away ("read-ahead 221 fills 31200 KB" for 22384 KB read). */
#define RA_MIN (64u * 1024u)
#define RA_MAX (512u * 1024u)

typedef struct {
  s64 pos;
  u8 *buf;        /* RA_MAX, allocated at the first read-ahead */
  s64 buf_off;    /* file offset of buf[0] */
  u32 buf_len;    /* valid bytes in buf */
  u32 ra;         /* the next window; 0 until reads run on */
  s64 last_end;   /* where the previous read ended */
} ObbFile;

static int obb_pread(s64 off, void *dst, u64 n, u64 *out) {
  if (off >= g_obb_size) {
    *out = 0;
    return 0;
  }
  if ((s64)n > g_obb_size - off)
    n = (u64)(g_obb_size - off);
  return R_FAILED(fsFileRead(&g_obb, off, dst, n, FsReadOption_None, out)) ? -1 : 0;
}

int sh_obbindex_load(int64_t obb_size);                   /* sh_obbindex.c */
int sh_obbindex_read(int64_t pos, void *dst, uint32_t n);
uint64_t sh_obbindex_hits(void);
int64_t sh_obbindex_next(int64_t pos);

static int obbf_read(void *c, char *buf, _READ_WRITE_BUFSIZE_TYPE n) {
  ObbFile *k = c;
  if (n <= 0 || k->pos >= g_obb_size)
    return 0;
  if (sh_obbindex_read(k->pos, buf, (uint32_t)n)) { /* a zip header: from RAM */
    k->pos += n;
    k->last_end = k->pos;
    return (int)n;
  }
  u32 done = 0;
  /* what the window already holds */
  if (k->buf_len && k->pos >= k->buf_off && k->pos < k->buf_off + k->buf_len) {
    u32 have = (u32)(k->buf_off + k->buf_len - k->pos);
    u32 take = have < (u32)n ? have : (u32)n;
    memcpy(buf, k->buf + (k->pos - k->buf_off), take);
    k->pos += take;
    done = take;
    if (done == (u32)n) {
      k->last_end = k->pos;
      return (int)done;
    }
  }
  u32 want = (u32)n - done;
  int sequential = k->pos == k->last_end || (k->buf_len && k->pos == k->buf_off + k->buf_len);
  k->ra = !sequential ? 0 : k->ra ? (k->ra * 2 > RA_MAX ? RA_MAX : k->ra * 2) : RA_MIN;
  u64 out = 0;
  if (!k->ra || want >= k->ra) { /* straight into the caller's buffer */
    if (obb_pread(k->pos, buf + done, want, &out)) {
      errno = EIO;
      return done ? (int)done : -1;
    }
    k->pos += (s64)out;
    k->last_end = k->pos;
    return (int)(done + out);
  }
  if (!k->buf && !(k->buf = malloc(RA_MAX))) { /* no memory: read it plainly */
    k->ra = 0;
    if (obb_pread(k->pos, buf + done, want, &out)) {
      errno = EIO;
      return done ? (int)done : -1;
    }
    k->pos += (s64)out;
    k->last_end = k->pos;
    return (int)(done + out);
  }
  u32 fill = k->ra;
  int64_t end = sh_obbindex_next(k->pos);
  if (end > k->pos && end - k->pos < (int64_t)fill)
    fill = (u32)(end - k->pos) > want ? (u32)(end - k->pos) : want;
  if (obb_pread(k->pos, k->buf, fill, &out)) {
    k->buf_len = 0;
    errno = EIO;
    return done ? (int)done : -1;
  }
  k->buf_off = k->pos;
  k->buf_len = (u32)out;
  u32 take = (u32)out < want ? (u32)out : want;
  memcpy(buf + done, k->buf, take);
  k->pos += take;
  k->last_end = k->pos;
  g.ra_fills++;
  g.ra_bytes += out;
  return (int)(done + take);
}

static fpos_t obbf_seek(void *c, fpos_t off, int whence) {
  ObbFile *k = c;
  s64 base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? k->pos : g_obb_size;
  s64 np = base + (s64)off;
  if (np < 0) {
    errno = EINVAL;
    return -1;
  }
  k->pos = np;
  return (fpos_t)np;
}

static int obbf_close(void *c) {
  ObbFile *k = c;
  free(k->buf);
  free(k);
  return 0;
}

static int is_obb(const char *real) {
  size_t n = real ? strlen(real) : 0;
  return n > 4 && !strcmp(real + n - 4, ".obb");
}

/* The one FsFile, opened on the first fopen (for whichever .obb that is:
 * there is one). 0 when it cannot be: the caller falls back to stdio. */
static int obb_open_once(const char *real) {
  mutexLock(&g_obb_lock);
  if (g_obb_size < 0 && !g_obb_real[0]) {
    snprintf(g_obb_real, sizeof g_obb_real, "%s", real);
    FsFileSystem *fs = fsdevGetDeviceFileSystem("sdmc");
    const char *p = strchr(real, ':') ? strchr(real, ':') + 1 : real;
    char path[FS_MAX_PATH];
    snprintf(path, sizeof path, "%s", p);
    Result rc = fs ? fsFsOpenFile(fs, path, FsOpenMode_Read, &g_obb) : MAKERESULT(Module_Libnx, LibnxError_NotFound);
    s64 size = -1;
    if (R_SUCCEEDED(rc) && R_FAILED(fsFileGetSize(&g_obb, &size))) {
      fsFileClose(&g_obb);
      size = -1;
    }
    g_obb_size = size;
    if (size >= 0)
      sh_obbindex_load(size);
    debugPrintf("[io] OBB %s: %s (%lld MB); every fopen of it shares this handle\n", path,
                size >= 0 ? "opened once" : "could NOT be opened -- stdio as before",
                (long long)(size >> 20));
  }
  int ok = g_obb_size >= 0 && !strcmp(g_obb_real, real);
  mutexUnlock(&g_obb_lock);
  return ok;
}

static FILE *obb_fopen(const char *real) {
  if (!obb_open_once(real))
    return NULL;
  ObbFile *k = calloc(1, sizeof *k);
  if (!k)
    return NULL;
  FILE *f = funopen(k, obbf_read, NULL, obbf_seek, obbf_close);
  if (!f) {
    free(k);
    return NULL;
  }
  setvbuf(f, NULL, _IONBF, 0); /* newlib reads an unbuffered FILE straight into the caller's buffer */
  g.obb_opens++;
  return f;
}

static void *sh_fopen(const char *path, const char *mode) {
  uint64_t t0 = armGetSystemTick();
  int ro = path && mode && mode[0] == 'r' && !strchr(mode, '+');
  if (ro && dcr_config()->obb_shared) {
    char buf[DCR_PATH_MAX];
    const char *real = dcr_translate_path(path, buf, sizeof buf);
    if (is_obb(real)) {
      FILE *o = obb_fopen(real);
      if (o) {
        g.open_ticks += armGetSystemTick() - t0;
        return o;
      }
    }
  }
  FILE *f = b_fopen(path, mode);
  g.open_ticks += armGetSystemTick() - t0;
  unsigned kb = (unsigned)dcr_config()->read_buf_kb;
  if (f && kb && ro && !strstr(path, ".apk")) { /* the APK's FILE reads RAM (bionic_stdio.c) */
    g.opens++;
    /* newlib allocates the buffer itself (and frees it at fclose) */
    if (setvbuf(f, NULL, _IOFBF, kb * 1024u) != 0) {
      static int warned;
      if (!warned++)
        debugPrintf("[io] setvbuf(%u KB) refused for %s\n", kb, path);
    }
  }
  return f;
}

static void report(uint64_t now) {
  if (!g.reads && !g.seeks && !g.stats && !g.stats_obb)
    return;
  debugPrintf("[io] stdio: %lu opens + %lu of the OBB in %llu ms, %lu freads %llu KB (%llu B each) in %llu ms, "
              "%lu fseeks in %llu ms; OBB read-ahead %lu fills %llu KB, %llu headers from RAM; %lu stat() in "
              "%llu ms + %lu inside the OBB answered (buffer %d KB)\n",
              (unsigned long)g.opens, (unsigned long)g.obb_opens,
              (unsigned long long)(armTicksToNs(g.open_ticks) / 1000000ull), (unsigned long)g.reads,
              (unsigned long long)(g.bytes >> 10),
              (unsigned long long)(g.reads ? g.bytes / g.reads : 0),
              (unsigned long long)(armTicksToNs(g.read_ticks) / 1000000ull), (unsigned long)g.seeks,
              (unsigned long long)(armTicksToNs(g.seek_ticks) / 1000000ull), (unsigned long)g.ra_fills,
              (unsigned long long)(g.ra_bytes >> 10), (unsigned long long)(sh_obbindex_hits() - g.hdr_hits0),
              (unsigned long)g.stats, (unsigned long long)(armTicksToNs(g.stat_ticks) / 1000000ull),
              (unsigned long)g.stats_obb, dcr_config()->read_buf_kb);
  uint64_t keep = now, hits = sh_obbindex_hits();
  memset(&g, 0, sizeof g);
  g.last_report = keep;
  g.hdr_hits0 = hits;
}

static void maybe_report(uint64_t now) {
  if (!g.last_report)
    g.last_report = now;
  else if (armTicksToNs(now - g.last_report) >= 2000000000ull)
    report(now);
}

/* counted from any thread; a lost update now and then does not matter */
static size_t sh_fread(void *p, size_t sz, size_t n, void *fp) {
  uint64_t t0 = armGetSystemTick();
  size_t r = b_fread(p, sz, n, fp);
  uint64_t t1 = armGetSystemTick();
  g.reads++;
  g.bytes += (uint64_t)r * sz;
  g.read_ticks += t1 - t0;
  maybe_report(t1);
  return r;
}

static int sh_fseek(void *fp, long off, int whence) {
  uint64_t t0 = armGetSystemTick();
  int r = b_fseek(fp, off, whence);
  g.seeks++;
  g.seek_ticks += armGetSystemTick() - t0;
  return r;
}

/* stat(): the main thread spent 15% of a mission-load window in fsdev_stat
 * (hardware 2026-10-04). Counted, and the first paths named, to see what it
 * asks about. */
int b_stat(const char *path, void *out);
void b_set_errno(int e);
#define SH_L_ENOENT 2
static int sh_stat(const char *path, void *out) {
  uint64_t t0 = armGetSystemTick();
  int r;
  /* Unity looks for every serialized file (and its .resS/.res/.resG, twice)
   * as a real file under the OBB's path before it reads it from inside the
   * zip: 800-1500 stats per 2 s of a mission load, 0.2-0.9 ms each on the
   * card. Nothing can exist below a regular file: answered here. */
  if (path && strstr(path, ".obb/")) {
    b_set_errno(SH_L_ENOENT);
    r = -1;
    g.stats_obb++;
    return r;
  }
  r = b_stat(path, out);
  uint64_t dt = armGetSystemTick() - t0;
  g.stats++;
  g.stat_ticks += dt;
  static unsigned named;
  if (named < 20) {
    named++;
    debugPrintf("[io] stat(%s) -> %d, %llu us\n", path ? path : "(null)", r,
                (unsigned long long)(armTicksToNs(dt) / 1000ull));
  }
  return r;
}

const DynLibFunction port_imports[] = {
    {"stat", (uintptr_t)sh_stat},
    {"fopen", (uintptr_t)sh_fopen},
    {"fread", (uintptr_t)sh_fread},
    {"fseek", (uintptr_t)sh_fseek},
};
const int port_imports_count = sizeof port_imports / sizeof port_imports[0];
