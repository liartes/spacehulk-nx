/* sh_bigpages.c -- the game's code and Mono's JIT arena in 2 MB blocks
 * ([performance] large_pages).
 *
 * WHY
 * ---
 * A TLB miss costs about as much as a RAM access on the Switch: +77 to +165 ns
 * per miss, measured on the console by the torchlight-nx port (2026-11-04),
 * whose game window and code went from 4 KB pages to 2 MB blocks (main thread
 * -15% and -2..-4%). The kernel maps a range with a 2 MB block, one TLB entry
 * for 512 pages, only where the virtual and the physical address have the same
 * remainder modulo 2 MB, and only over a 2 MB stretch with one state and one
 * permission. The heap is mapped with VA = PA modulo 2 MB (measured there).
 *
 * The runtime's loader stages a module page-aligned (memalign(0x1000)) and
 * maps it at a page-aligned random address (virtmemFindCodeMemory(size,
 * 0x1000): the second argument is a guard, not an alignment). Each one is
 * 2 MB-aligned with a chance of 1 in 512, so libunity's 14.5 MB of code and
 * libmono's 3.5 MB ran in 4 KB pages (logs of 2026-10-20: staged 0xaaa89000
 * -> code 0x1a031000). So did Mono's JIT arena (libnx jitCreate, the same two
 * calls).
 *
 * HOW
 * ---
 * Inside sh_bigpages_begin() .. sh_bigpages_end(), on the thread that called
 * begin (dcr_main.c load_module, jit_arena.c jit_arena_init):
 *   - the staging buffer is 2 MB-aligned: load_module gets it here
 *     (sh_bigpages_staging, sized from the ELF's program headers) and gives it
 *     to so_load as its base; jitCreate's source comes through
 *     __wrap___libnx_aligned_alloc;
 *   - each virtmemFindCodeMemory of 2 MB or more (the module's address,
 *     jitCreate's two views) looks for 2 MB more and rounds the address up
 *     (__wrap_virtmemFindCodeMemory); the caller reserves or maps only its
 *     own size, inside what was found free.
 * Both are linker --wrap's (Makefile PORT_LDFLAGS): the runtime, a submodule
 * shared with other ports, is not changed. Outside the brackets, and on any
 * other thread, both pass through. If an aligned search fails, the normal one
 * runs: the module then loads in 4 KB pages, as before.
 *
 * libunity gets 7 blocks (0-14 MB of its 14.5 MB of code; the stretch with
 * the code/data boundary splits into pages at the permission change), libmono
 * 1, the JIT arena's views 24 each. "[pages]" lines give the layout.
 *
 * MEASURED (hardware 2026-11-06, [debug] tlb_probe = sh_tlbprobe.c, one run
 * with large_pages on and one off, 3 cycles each; ns per access, scattered
 * minus grouped):
 *   witness (same pages, 2 MB-aligned vs 4 KB off)  +44..+51 in both runs:
 *     blocks do happen in a 32-bit process, and its heap is VA = PA mod 2 MB
 *   libunity read      off +34..+38   on +0.0..+0.3
 *   libunity execute   off +30..+32   on +1..+4
 *   libmono execute    off  +6..+8    on -0.7..+0.2
 *   JIT arena read     off +57..+79   on +3..+8
 *   heap, this program's code and data: the same in both runs, already in
 *     blocks (the system's loader puts this program at a 2 MB-aligned address)
 * MIT.
 */
#include <elf.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "dcr_config.h"
#include "sh_bigpages.h"
#include "util.h"

static Handle g_owner; /* the thread inside begin/end, 0 when none */

int sh_bigpages_on(void) { return dcr_config()->large_pages && !dcr_is_emulator(); }

void sh_bigpages_begin(void) {
  if (sh_bigpages_on())
    g_owner = threadGetCurHandle();
}

void sh_bigpages_end(void) { g_owner = 0; }

static int active(size_t size) { return g_owner && size >= SH_BLOCK && threadGetCurHandle() == g_owner; }

void *__real_virtmemFindCodeMemory(size_t size, size_t guard_size);
void *__wrap_virtmemFindCodeMemory(size_t size, size_t guard_size) {
  if (active(size)) {
    uintptr_t z = (uintptr_t)__real_virtmemFindCodeMemory(size + SH_BLOCK, guard_size);
    if (z)
      return (void *)((z + SH_BLOCK - 1) & ~(uintptr_t)(SH_BLOCK - 1));
    debugPrintf("[pages] no free 2 MB-aligned range for 0x%x bytes: 4 KB pages\n", (unsigned)size);
  }
  return __real_virtmemFindCodeMemory(size, guard_size);
}

void *__real___libnx_aligned_alloc(size_t alignment, size_t size);
void *__wrap___libnx_aligned_alloc(size_t alignment, size_t size) {
  if (active(size) && alignment <= SH_BLOCK) {
    void *p = __real___libnx_aligned_alloc(SH_BLOCK, size);
    if (p)
      return p;
  }
  return __real___libnx_aligned_alloc(alignment, size);
}

void *sh_bigpages_staging(const char *path, size_t *size) {
  if (!sh_bigpages_on())
    return NULL;
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  Elf32_Ehdr eh;
  Elf32_Phdr ph[16];
  size_t end = 0;
  int ok = fread(&eh, sizeof eh, 1, f) == 1 && !memcmp(eh.e_ident, ELFMAG, SELFMAG) &&
           eh.e_ident[EI_CLASS] == ELFCLASS32 && eh.e_phentsize == sizeof(Elf32_Phdr) &&
           eh.e_phnum > 0 && eh.e_phnum <= 16 && fseek(f, (long)eh.e_phoff, SEEK_SET) == 0 &&
           fread(ph, sizeof(Elf32_Phdr), eh.e_phnum, f) == eh.e_phnum;
  fclose(f);
  if (!ok)
    return NULL;
  for (int i = 0; i < eh.e_phnum; i++)
    if (ph[i].p_type == PT_LOAD && ph[i].p_vaddr + ph[i].p_memsz > end)
      end = ph[i].p_vaddr + ph[i].p_memsz;
  end = (end + 0xFFF) & ~(size_t)0xFFF;
  if (end < SH_BLOCK)
    return NULL;
  void *p = memalign(SH_BLOCK, end);
  if (!p) {
    debugPrintf("[pages] %s: no 2 MB-aligned %u KB of heap: 4 KB pages\n", path, (unsigned)(end >> 10));
    return NULL;
  }
  *size = end;
  return p;
}

void sh_bigpages_report(const char *name, uintptr_t va, uintptr_t src, size_t size) {
  const uintptr_t first = (va + SH_BLOCK - 1) & ~(uintptr_t)(SH_BLOCK - 1);
  const uintptr_t end = va + size;
  int whole = 0, uniform = 0;
  for (uintptr_t s = first; s + SH_BLOCK <= end; s += SH_BLOCK) {
    MemoryInfo mi;
    u32 pi;
    whole++;
    if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, s)) && mi.addr <= s && mi.addr + mi.size >= (u64)s + SH_BLOCK)
      uniform++;
  }
  const int same = src && !((va ^ src) & (SH_BLOCK - 1));
  debugPrintf("[pages] %s: 0x%08x +%u KB (rem 0x%x), source 0x%08x (rem 0x%x): %d of %d whole 2 MB "
              "stretches with one permission%s -> %d blocks possible\n",
              name, (unsigned)va, (unsigned)(size >> 10), (unsigned)(va & (SH_BLOCK - 1)), (unsigned)src,
              (unsigned)(src & (SH_BLOCK - 1)), uniform, whole,
              !src ? ", source unknown" : same ? ", same remainder" : ", remainders differ",
              same ? uniform : 0);
}
