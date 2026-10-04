#!/usr/bin/env python3
"""Host test for source/dcr_apkcache.c: game.apk read through the RAM cache.

Builds the cache natively against small stand-ins for the libnx calls it uses
(FsFile reads become pread on the host file) and checks thousands of random
reads -- block-crossing, end-of-file, huge -- byte for byte against the file:
  1. as built (128 KB blocks, up to 128 MB);
  2. with an 8-block cache, so blocks are evicted and refilled all the time;
  3. with the heap "full", so the cache may not grow and reads go straight
     to the card.

    python3 tools/test_apkcache.py <game.apk>
"""
import os, random, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, '..', 'source', 'dcr_apkcache.c')

SWITCH_H = r'''
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
typedef uint64_t u64; typedef int64_t s64; typedef uint32_t Result;
#define R_FAILED(r) ((r) != 0)
#define R_SUCCEEDED(r) ((r) == 0)
typedef struct { int fd; } FsFile;
typedef struct { int dummy; } FsFileSystem;
typedef int Mutex;
enum { FsOpenMode_Read = 1, FsReadOption_None = 0 };
static FsFileSystem g_fs;
static inline FsFileSystem *fsdevGetDeviceFileSystem(const char *n) { return &g_fs; }
static inline Result fsFsOpenFile(FsFileSystem *fs, const char *p, int mode, FsFile *f) {
  f->fd = open(p, O_RDONLY); return f->fd < 0; }
static inline Result fsFileGetSize(FsFile *f, s64 *out) { *out = lseek(f->fd, 0, SEEK_END); return 0; }
static inline Result fsFileRead(FsFile *f, s64 off, void *buf, uint64_t n, int opt, u64 *got) {
  ssize_t r = pread(f->fd, buf, n, off); if (r < 0) return 1; *got = (u64)r; return 0; }
static inline void fsFileClose(FsFile *f) { close(f->fd); }
static inline void mutexLock(Mutex *m) {}
static inline void mutexUnlock(Mutex *m) {}
'''
MALLOC_H = r'''
#include <stdlib.h>
struct mallinfo { size_t uordblks; };
extern size_t g_fake_used;
static inline struct mallinfo mallinfo(void) { struct mallinfo m = { g_fake_used }; return m; }
'''
NX_H = r'''
#include <stdint.h>
typedef struct { uint64_t heap; } NxInitInfo;
extern NxInitInfo g_nxinit;
'''
UTIL_H = r'''
void debugPrintf(const char *f, ...);
'''
HARNESS = r'''
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include "nx_init.h"
NxInitInfo g_nxinit = { 992ull << 20 };
size_t g_fake_used;
void debugPrintf(const char *f, ...) { va_list a; va_start(a, f); vprintf(f, a); va_end(a); }
void dcr_apkcache_set_path(const char *real);
ssize_t dcr_apkcache_read(uint64_t off, void *buf, size_t n);
int main(int argc, char **argv) {
  if (argc > 3) g_fake_used = (size_t)strtoull(argv[3], 0, 10);
  dcr_apkcache_set_path(argv[1]);
  FILE *f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); long size = ftell(f);
  FILE *ops = fopen(argv[2], "r");
  unsigned long long off, n; long bad = 0, count = 0;
  char *a = malloc(64 << 20), *b = malloc(64 << 20);
  while (fscanf(ops, "%llu %llu", &off, &n) == 2) {
    ssize_t r = dcr_apkcache_read(off, a, n);
    size_t want = off >= (unsigned long long)size ? 0 : (off + n > (unsigned long long)size ? size - off : n);
    fseek(f, (long)off, SEEK_SET);
    size_t got = want ? fread(b, 1, want, f) : 0;
    if (r != (ssize_t)want || got != want || memcmp(a, b, want)) {
      if (bad++ < 5) printf("MISMATCH at %llu+%llu: cache %zd, file %zu\n", off, n, r, want);
    }
    count++;
  }
  printf("%ld reads, %ld mismatches\n", count, bad);
  return bad != 0;
}
'''


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    apk = os.path.abspath(sys.argv[1])
    size = os.path.getsize(apk)
    rnd = random.Random(1234)
    ops = []
    for _ in range(3000):  # scattered small and medium reads
        ops.append((rnd.randrange(size), rnd.choice([1, 17, 4096, 65536, 131071, 131072, 131073, 300000])))
    pos = rnd.randrange(size // 2)
    for _ in range(2000):  # sequential, like a scene load, read twice
        n = rnd.choice([4096, 16384, 65536])
        ops.append((pos, n)); pos += n
    ops += ops[-2000:]
    ops += [(size - 10, 100), (size, 10), (size + 5, 10), (0, 40 << 20), ((1 << 17) - 1, 2)]  # ends, huge
    with tempfile.TemporaryDirectory() as t:
        for name, text in (('switch.h', SWITCH_H), ('malloc.h', MALLOC_H), ('nx_init.h', NX_H),
                           ('util.h', UTIL_H), ('h.c', HARNESS)):
            open(os.path.join(t, name), 'w').write(text)
        open(os.path.join(t, 'c.c'), 'w').write(open(SRC).read())
        open(os.path.join(t, 'ops'), 'w').write(''.join('%d %d\n' % o for o in ops))
        runs = [('as built', [], []), ('8-block cache (constant eviction)', ['-DMAX_BLOCKS=8'], []),
                ('heap full (no growth: straight reads)', [], [str(2000 << 20)])]
        for label, defs, extra in runs:
            exe = os.path.join(t, 'x')
            subprocess.check_call(['cc', '-O1', '-w', '-I', t] + defs +
                                  ['-o', exe, os.path.join(t, 'h.c'), os.path.join(t, 'c.c')])
            out = subprocess.run([exe, apk, os.path.join(t, 'ops')] + extra, capture_output=True, text=True)
            print('%-40s %s' % (label + ':', out.stdout.strip().splitlines()[-1]))
            if out.returncode:
                print(out.stdout)
                sys.exit('FAILED')
    print('OK: every read identical to the file')


if __name__ == '__main__':
    main()
