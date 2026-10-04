/* dcr_jit_mem.c -- mmap and friends, and the JIT-aware memory primitives:
 * Disney Crossy Road's replacements for the runtime's weak b_mmap, b_munmap,
 * b_mremap, b_mprotect, b_mmap_bytes and b_memcpy / b_memmove / b_memset
 * (runtime/source/bionic_mem.c keeps the __aeabi_ and _chk forms, which call
 * these). Mono's code manager maps its code with PROT_EXEC, and that memory
 * is the JIT arena, with an executable view and a writable one.
 *
 * There is no virtual memory API for applications beyond the heap, so:
 *   - anonymous mappings are page-aligned heap blocks (zeroed);
 *   - file mappings are heap blocks filled from the file (MAP_PRIVATE semantics;
 *     Mono maps its assemblies read-only this way, Unity maps some data files);
 *   - PROT_EXEC mappings come from the JIT arena (jit_arena.c) -- Mono's code
 *     manager is the only client;
 *   - PROT_NONE reservations are backed too (logged when large, since 32-bit
 *     address space and the 1 GiB heap region are the budget).
 * mprotect() is accepted and ignored (nothing enforces page permissions on
 * heap memory), except that granting EXEC to non-arena memory is logged: that
 * code would not be executable on hardware.
 *
 * memcpy / memmove / memset and their __aeabi_ forms check whether the
 * DESTINATION lies in the JIT arena's executable view and, if so, write through
 * its writable alias instead -- that covers Mono copying each compiled method
 * into place. MIT.
 */
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "bionic.h"
#include "bionic_io.h"
#include "jit_arena.h"
#include "util.h"

/* =============================== mapping table ============================= */
typedef struct {
  uintptr_t addr;
  size_t len;
  int exec;
} Mapping;

#define MAX_MAPPINGS 4096
static Mapping g_maps[MAX_MAPPINGS];
static int g_nmaps;
static Mutex g_map_lock;
static u64 g_mapped_bytes;

static int map_find(uintptr_t a) {
  for (int i = 0; i < g_nmaps; i++)
    if (a >= g_maps[i].addr && a < g_maps[i].addr + g_maps[i].len)
      return i;
  return -1;
}

static void map_add(uintptr_t a, size_t len, int exec) {
  mutexLock(&g_map_lock);
  if (g_nmaps < MAX_MAPPINGS) {
    g_maps[g_nmaps++] = (Mapping){a, len, exec};
    g_mapped_bytes += len;
  } else {
    static int warned;
    if (!warned++)
      debugPrintf("[mmap] mapping table full; munmap of later mappings will leak\n");
  }
  mutexUnlock(&g_map_lock);
}

void *b_mmap(void *addr, size_t len, int prot, int flags, int fd, b_off_t off) {
  if (!len) {
    b_set_errno(L_EINVAL);
    return L_MAP_FAILED;
  }
  size_t alen = (len + 0xFFF) & ~0xFFFu;

  /* MAP_FIXED inside something we already handed out: the caller is committing
   * or re-initialising part of its own reservation. */
  if ((flags & L_MAP_FIXED) && addr) {
    mutexLock(&g_map_lock);
    int i = map_find((uintptr_t)addr);
    mutexUnlock(&g_map_lock);
    if (i >= 0 || jit_contains(addr)) {
      if (jit_contains(addr)) {
        static int n;
        if (n++ < 64)
          debugPrintf("[mmap] MAP_FIXED over JIT memory %p+0x%x prot %d flags 0x%x (from %p)\n", addr,
                      (unsigned)len, prot, flags, __builtin_return_address(0));
      }
      if (flags & L_MAP_ANONYMOUS)
        memset(jit_rw(addr), 0, len);
      else if (fd >= 0)
        b_pread_all(fd, jit_rw(addr), len, off);
      return addr;
    }
    debugPrintf("[mmap] MAP_FIXED at unknown %p (%u KB) refused\n", addr, (unsigned)(len >> 10));
    b_set_errno(L_EINVAL);
    return L_MAP_FAILED;
  }

  void *p;
  int exec = (prot & L_PROT_EXEC) != 0;
  if (exec) {
    p = jit_alloc(alen);
    static int n;
    if (n++ < 64)
      debugPrintf("[mmap] exec %p+0x%x (asked %p+0x%x prot %d flags 0x%x, from %p)\n", p, (unsigned)alen, addr,
                  (unsigned)len, prot, flags, __builtin_return_address(0));
  } else {
    p = memalign(0x1000, alen);
    if (p && (flags & L_MAP_ANONYMOUS))
      memset(p, 0, alen);
  }
  if (!p) {
    debugPrintf("[mmap] out of memory for %u KB (prot %d)\n", (unsigned)(alen >> 10), prot);
    b_set_errno(L_ENOMEM);
    return L_MAP_FAILED;
  }
  if (!(flags & L_MAP_ANONYMOUS) && fd >= 0) {
    static int n;
    if (n++ < 24)
      debugPrintf("[mmap] file fd %d at 0x%llx, %u KB (prot %d flags 0x%x, from %p)\n", fd, (unsigned long long)off,
                  (unsigned)(len >> 10), prot, flags, __builtin_return_address(0));
    size_t got = b_pread_all(fd, jit_rw(p), len, off);
    if (got < alen)
      memset((char *)jit_rw(p) + got, 0, alen - got);
  }
  if (alen >= (16u << 20))
    debugPrintf("[mmap] large mapping %u MB prot=%d flags=0x%x -> %p\n", (unsigned)(alen >> 20),
                prot, flags, p);
  map_add((uintptr_t)p, alen, exec);
  return p;
}

int b_munmap(void *addr, size_t len) {
  mutexLock(&g_map_lock);
  int i = map_find((uintptr_t)addr);
  if (i < 0) {
    mutexUnlock(&g_map_lock);
    return 0; /* not ours / already gone: bionic returns 0 for unmapped ranges */
  }
  Mapping m = g_maps[i];
  if ((uintptr_t)addr != m.addr || ((len + 0xFFF) & ~0xFFFu) < m.len) {
    /* Partial unmap: keep the block (the memory stays valid for the rest). */
    mutexUnlock(&g_map_lock);
    if (m.exec) {
      static int n;
      if (n++ < 32)
        debugPrintf("[mmap] partial munmap of exec %p+0x%x ignored (block %p+0x%x)\n", addr, (unsigned)len,
                    (void *)m.addr, (unsigned)m.len);
    }
    return 0;
  }
  g_maps[i] = g_maps[--g_nmaps];
  g_mapped_bytes -= m.len;
  mutexUnlock(&g_map_lock);
  if (m.exec) {
    static int n;
    if (n++ < 64)
      debugPrintf("[mmap] munmap exec %p+0x%x (from %p)\n", addr, (unsigned)m.len, __builtin_return_address(0));
    jit_free(addr, m.len);
  }
  else
    free(addr);
  return 0;
}

void *b_mremap(void *old, size_t old_len, size_t new_len, int flags, ...) {
  if (new_len <= old_len)
    return old;
  if (!(flags & L_MREMAP_MAYMOVE)) {
    b_set_errno(L_ENOMEM);
    return L_MAP_FAILED;
  }
  mutexLock(&g_map_lock);
  int i = map_find((uintptr_t)old);
  int exec = i >= 0 ? g_maps[i].exec : 0;
  mutexUnlock(&g_map_lock);
  void *n = b_mmap(NULL, new_len, L_PROT_READ | L_PROT_WRITE | (exec ? L_PROT_EXEC : 0),
                   L_MAP_PRIVATE | L_MAP_ANONYMOUS, -1, 0);
  if (n == L_MAP_FAILED)
    return n;
  memcpy(jit_rw(n), old, old_len);
  b_munmap(old, old_len);
  return n;
}

int b_mprotect(void *addr, size_t len, int prot) {
  if ((prot & L_PROT_EXEC) && !jit_contains(addr)) {
    static int warned;
    if (warned++ < 8)
      debugPrintf("[mmap] mprotect(%p, %u, EXEC) outside the JIT arena from %p -- code there "
                  "will not execute on hardware\n",
                  addr, (unsigned)len, __builtin_return_address(0));
  }
  return 0;
}


u64 b_mmap_bytes(void) { return g_mapped_bytes; }

/* ========================= JIT-aware memory primitives ===================== */
void *b_memcpy(void *d, const void *s, size_t n) {
  if (__builtin_expect(jit_range_contains(d, n), 0))
    return memcpy(jit_rw(d), s, n), d;
  return memcpy(d, s, n);
}

void *b_memmove(void *d, const void *s, size_t n) {
  if (__builtin_expect(jit_range_contains(d, n), 0))
    return memmove(jit_rw(d), s, n), d;
  return memmove(d, s, n);
}

void *b_memset(void *d, int c, size_t n) {
  if (__builtin_expect(jit_range_contains(d, n), 0))
    return memset(jit_rw(d), c, n), d;
  return memset(d, c, n);
}
