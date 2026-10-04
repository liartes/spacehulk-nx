#!/usr/bin/env python3
"""Host test for source/dcr_ilpatch.c against the user's own game.apk.

Builds the patcher natively (cc), feeds it assets/bin/Data/Managed/
Assembly-CSharp.dll out of the APK, and checks every byte that changed:

  1. unlocks  in Character.IsUnlocked, `ldarg.0; call get_unlocked`
              (02 28 <MethodDef>) became `ldc.i4.1` and five nops;
  2. stars    Character.GetCurrentLevel's MethodDef RVA now points into the
              zero padding after the code section, where the new 41-byte
              tiny-header body sits, and the section's VirtualSize covers it;
  3. hop      in TouchController.Action the four press constants (-1..-4)
              are now 1..4 and the release constants are 5, same encodings;
  4. top bar  ModeBarController.UpdateToColor calls ClearColor instead;
  5. wipe     WipeController.<Restart>'s WaitForSeconds(0.8f) is 0.6f;
  6. unload   WipeController.MidWipeSetup's Resources.UnloadUnusedAssets(); is nops;
  7. store    EconomyController.newPlatformBiller's `ldc.i4.s 11` (Android) is -1,
              and three bodies are nops up to new last instructions:
              PlatformBiller.localizedPriceBillerID `ldstr "FREE"; ldc.i4.1;
              call Language::Get; ret` (the game's own call, found in the
              original), figurineIdIsATicket `ldc.i4.0; ret`, the 7-argument
              ReceiptManager.sendReceipt `ret`;
  8. theme    CharacterSelectionSwipeNew.SetCenterCharacter(string)'s last two
              calls swapped: ScrollToCharacter (sets lastClack) first, then
              SetCenterCharacter(DisplayCharacterNew) -- 25 bytes reordered.

With dnfile installed (pip install dnfile) the patched methods are also looked
up by name and their IL decoded.

    python3 tools/test_ilpatch.py <path to game.apk>
"""
import os, struct, subprocess, sys, tempfile, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, '..', 'source', 'dcr_ilpatch.c')

HARNESS = r'''
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "dcr_config.h"
void debugPrintf(const char *f, ...) { va_list a; va_start(a, f); vprintf(f, a); va_end(a); }
static DcrConfig g_all = {.unlock_all = 1, .max_stars = 1, .hide_top_bar = 1, .hop_on_press = 1, .short_wipe = 1,
                           .free_store = 1, .quiet_center = 1};
const DcrConfig *dcr_config(void) { return &g_all; }
void *dcr_ilpatch_interpose(const char *sym, void *real);
static char *g_out; static uint32_t g_len;
static void *sink(char *d, uint32_t n, int nc, int *st, int ro, const char *name) {
  g_out = malloc(n); for (uint32_t i = 0; i < n; i++) g_out[i] = d[i]; g_len = n; return (void *)1;
}
int main(int argc, char **argv) {
  FILE *f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
  char *d = malloc(n); if (fread(d, 1, n, f) != (size_t)n) return 2; fclose(f);
  typedef void *(*fn)(char *, uint32_t, int, int *, int, const char *);
  fn w = (fn)dcr_ilpatch_interpose("mono_image_open_from_data_with_name", (void *)sink);
  int st = 0; w(d, (uint32_t)n, 0, &st, 0, "Assembly-CSharp.dll");
  FILE *o = fopen(argv[2], "wb"); fwrite(g_out, 1, g_len, o); fclose(o);
  return 0;
}
'''


def runs(a, b):
    """Maximal runs of differing bytes: [(start, end)]."""
    out, i = [], 0
    while i < len(a):
        if a[i] != b[i]:
            j = i
            while j < len(a) and a[j] != b[j]:
                j += 1
            out.append((i, j))
            i = j
        else:
            i += 1
    return out


def sections(d):
    lfanew = struct.unpack_from('<I', d, 0x3c)[0]
    nsec = struct.unpack_from('<H', d, lfanew + 6)[0]
    optsz = struct.unpack_from('<H', d, lfanew + 20)[0]
    sh = lfanew + 24 + optsz
    return [struct.unpack_from('<IIII', d, sh + i * 40 + 8) for i in range(nsec)]  # vsize, va, rawsize, raw


def swapped_pair(dll, patched, lo):
    """End of the 25-byte region if `lo` is in the SetCenterCharacter swap:
    A = 02 06 28 <tok> (7), B = 02 06 7b .. 7b .. 17 28 <tok> (18), then ret;
    patched = B + A."""
    for s in range(lo - 25, lo + 1):
        a, b = dll[s:s + 7], dll[s + 7:s + 25]
        if (a[:3] == b'\x02\x06\x28' and b[:3] == b'\x02\x06\x7b' and b[7] == 0x7b and b[12] == 0x17 and
                b[13] == 0x28 and dll[s + 25] == 0x2a and patched[s:s + 25] == b + a):
            return s + 25
    return None


def replaced_body(dll, patched, lo):
    """(header offset, end, tail) if `lo` is in a fat method body that the
    patch made all nops up to a tail; else None."""
    for h in range(lo - 12, max(0, lo - 12 - 400), -1):
        fs, ms, cs = struct.unpack_from('<HHI', dll, h)
        if fs & 0xF00B != 0x3003 or not (h + 12 <= lo < h + 12 + cs) or cs > 400:
            continue
        code = patched[h + 12:h + 12 + cs]
        k = next((i for i, b in enumerate(code) if b), None)
        if k is None or k == 0 or dll[h + 12:h + 12 + cs] == code:
            continue
        return h, h + 12 + cs, code[k:]
    return None


def check_names(orig, patched, cave_rva):
    try:
        import dnfile
    except ImportError:
        print('(dnfile not installed: method names not checked)')
        return
    with tempfile.NamedTemporaryFile(suffix='.dll', delete=False) as t:
        t.write(patched)
    pe = dnfile.dnPE(t.name)
    md = pe.net.mdtables
    owner = {}
    for td in md.TypeDef.rows:
        for m in td.MethodList:
            owner[m.row_index] = td.TypeName

    def row(tn, mn):
        hits = [(i, m) for i, m in enumerate(md.MethodDef.rows, 1) if owner.get(i) == tn and m.Name == mn]
        assert len(hits) == 1, (tn, mn, len(hits))
        return hits[0]

    def body(rva):
        off = pe.get_offset_from_rva(rva)
        h = patched[off]
        assert h & 3 == 2, 'expected a tiny header'
        return patched[off + 1:off + 1 + (h >> 2)]

    k_cur, cur = row('Character', 'GetCurrentLevel')
    assert cur.Rva == cave_rva, 'GetCurrentLevel RVA %#x, cave %#x' % (cur.Rva, cave_rva)
    k_get, _ = row('Character', 'get_currentLevel')
    k_inst, _ = row('CharacterManager', 'get_Instance')
    k_max, _ = row('CharacterManager', 'GetMaxLevel')
    b = body(cur.Rva)
    tk = lambda k: struct.pack('<I', 0x06000000 | k)
    want = (b'\x28' + tk(k_inst) + b'\x25\x2c\x18\x02\x6f' + tk(k_max) + b'\x25\x02\x28' + tk(k_get) +
            b'\x2f\x08\x26\x02\x28' + tk(k_get) + b'\x2a\x2a\x26\x02\x28' + tk(k_get) + b'\x2a')
    assert b == want, 'GetCurrentLevel body %s' % b.hex(' ')
    print('OK: Character.GetCurrentLevel = max(CharacterManager.Instance.GetMaxLevel(this), saved)')

    us = pe.net.user_strings

    def code_of(k):
        off = pe.get_offset_from_rva(md.MethodDef.rows[k - 1].Rva)
        if patched[off] & 3 == 2:
            return patched[off + 1:off + 1 + (patched[off] >> 2)]
        fs, ms, cs = struct.unpack_from('<HHI', patched, off)
        return patched[off + (fs >> 12) * 4:off + (fs >> 12) * 4 + cs]

    k_npb, _ = row('EconomyController', 'newPlatformBiller')
    assert b'\x1f\xff' in code_of(k_npb) and b'\x1f\x0b' not in code_of(k_npb), 'newPlatformBiller'
    k_lget, _ = row('Language', 'Get')
    k_price, _ = row('PlatformBiller', 'localizedPriceBillerID')
    c = code_of(k_price)
    free_tok = struct.unpack_from('<I', c, len(c) - 11)[0]
    assert us.get(free_tok & 0xFFFFFF).value == 'FREE', 'price label string'
    assert c.endswith(b'\x17\x28' + tk(k_lget) + b'\x2a') and not c[:-12].strip(b'\0'), 'price body'
    k_tk, _ = row('PlatformBiller', 'figurineIdIsATicket')
    c = code_of(k_tk)
    assert c.endswith(b'\x16\x2a') and not c[:-2].strip(b'\0'), 'figurineIdIsATicket body'
    sends = [(i, m) for i, m in enumerate(md.MethodDef.rows, 1)
             if owner.get(i) == 'ReceiptManager' and m.Name == 'sendReceipt']
    assert len(sends) == 2
    long_send = [i for i, m in sends if len(code_of(i)) > 16]
    assert len(long_send) == 1 and code_of(long_send[0]).endswith(b'\x2a') and \
        not code_of(long_send[0])[:-1].strip(b'\0'), 'sendReceipt body'
    print('OK: store -- PlatformBiller chosen, prices = Language.Get("FREE"), '
          'figurineIdIsATicket = false, sendReceipt(7) empty')
    os.unlink(t.name)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    with zipfile.ZipFile(sys.argv[1]) as z:
        dll = z.read('assets/bin/Data/Managed/Assembly-CSharp.dll')
    with tempfile.TemporaryDirectory() as t:
        src = open(SRC).read().replace('#include "util.h"', 'void debugPrintf(const char *f, ...);')
        open(os.path.join(t, 'u.c'), 'w').write(src)
        open(os.path.join(t, 'h.c'), 'w').write(HARNESS)
        exe = os.path.join(t, 'test')
        inc = os.path.join(HERE, '..', 'source')
        subprocess.check_call(['cc', '-O1', '-Wall', '-I', inc, '-o', exe, os.path.join(t, 'h.c'),
                               os.path.join(t, 'u.c')])
        a, b = os.path.join(t, 'in.dll'), os.path.join(t, 'out.dll')
        open(a, 'wb').write(dll)
        out = subprocess.check_output([exe, a, b]).decode()
        print(out.strip())
        patched = open(b, 'rb').read()
    assert len(dll) == len(patched), 'size changed'
    for s in ('treats every character as owned', 'maximum level', 'a controller press hops',
              'mode bar transparent', 'starts loading at 0.6 s', 'no second Resources.UnloadUnusedAssets',
              'free purchases', 'theme switch: one sound'):
        assert s in out, 'patch missing: ' + s

    vsize, va, rawsize, raw = sections(dll)[0]
    cave_off = raw + ((vsize + 3) & ~3)
    cave_rva = va + ((vsize + 3) & ~3)
    unlock = stars_body = rva_field = vsize_grown = mode_bar = wipe = unload = 0
    lfanew = struct.unpack_from('<I', dll, 0x3c)[0]
    vsize_field = lfanew + 24 + struct.unpack_from('<H', dll, lfanew + 20)[0] + 8  # .text VirtualSize
    hop = []
    store_platform = 0
    store_bodies = {}
    theme = 0
    done = 0  # changes up to here are accounted for
    for lo, hi in runs(dll, patched):
        if lo < done:
            continue
        before, after = dll[lo:hi], patched[lo:hi]
        sw = swapped_pair(dll, patched, lo)
        if sw:
            theme += 1
            done = sw
            continue
        body = replaced_body(dll, patched, lo)
        if body:
            h, end, tail = body
            kind = {b'\x16\x2a': 'ticket', b'\x2a': 'receipt'}.get(tail)
            if not kind and len(tail) == 12 and tail[0] == 0x72 and tail[5:7] == b'\x17\x28' and tail[11] == 0x2a:
                assert dll.find(tail[:11]) >= 0, 'the FREE label call is not the game\'s own'
                kind = 'price'
            assert kind and kind not in store_bodies, 'replaced body at %#x: %s' % (h, tail.hex(' '))
            store_bodies[kind] = h
            done = end
            continue
        if (hi - lo == 1 and dll[lo] == 0x0b and patched[lo] == 0xff and dll[lo - 1] == 0x1f and
                dll[lo + 1] in (0x40, 0x33) and dll[lo - 6] == 0x28):
            store_platform += 1  # newPlatformBiller: ldc.i4.s 11 -> -1
            done = hi
            continue
        if cave_off <= lo < cave_off + 41:
            assert lo == cave_off and set(dll[lo:lo + 41]) == {0} and patched[lo] == (40 << 2 | 2), 'cave'
            assert patched[lo + 41:lo + 64] == dll[lo + 41:lo + 64], 'written past the new body'
            stars_body += 1
            hi = lo + 41
        elif dll[lo - 1:lo + 1] == b'\x02\x7b' and patched[lo - 1:lo + 1] == b'\x02\x28':
            # ModeBarController.UpdateToColor: ldarg.0; call ClearColor; nop x6; ret
            assert dll[lo + 5] == 0x03 and dll[lo + 6] == 0x6f and dll[lo + 11] == 0x2a, 'mode bar body'
            assert patched[lo + 4] == 0x06 and patched[lo + 5:lo + 11] == b'\0' * 6 and patched[lo + 11] == 0x2a
            mode_bar += 1
            hi = lo + 12
        elif dll[lo:lo + 2] == b'\x02\x28' and dll[lo + 5] == 0x06:
            assert patched[lo:lo + 6] == b'\x17\x00\x00\x00\x00\x00', 'unlock bytes'
            unlock += 1
            hi = lo + 6
        elif lo >= vsize_field and hi <= vsize_field + 4:
            assert struct.unpack_from('<I', patched, vsize_field)[0] == cave_rva + 41 - va, 'VirtualSize'
            vsize_grown += 1
        elif hi - lo <= 4 and any(struct.unpack_from('<I', patched, s)[0] == cave_rva
                                  for s in range(max(0, hi - 4), lo + 1)):
            rva_field += 1  # the MethodDef row's RVA, now the cave
        elif dll[lo:lo + 1] == b'\x28' and dll[lo + 5] == 0x26 and patched[lo:lo + 6] == b'\0' * 6:
            unload += 1  # WipeController.MidWipeSetup: call Resources::UnloadUnusedAssets; pop -> nops
            hi = lo + 6
        elif dll[lo - 1:lo + 4] == b'\x22\xcd\xcc\x4c\x3f' and patched[lo - 1:lo + 4] == b'\x22\x9a\x99\x19\x3f':
            wipe += 1  # WaitForSeconds(0.8f) -> 0.6f in WipeController.<Restart>
            hi = lo + 4
        elif hi - lo == 1:
            hop.append((lo, before[0], after[0]))
        else:
            raise AssertionError('unexpected change at %#x: %s -> %s' % (lo, before.hex(' '), after.hex(' ')))
        done = hi
    assert unlock == 1, 'unlock sites %d' % unlock
    assert mode_bar == 1, 'mode bar sites %d' % mode_bar
    assert wipe == 1, 'wipe sites %d' % wipe
    assert unload == 1, 'unload sites %d' % unload
    assert theme == 1, 'theme-switch swaps %d' % theme
    assert store_platform == 1 and sorted(store_bodies) == ['price', 'receipt', 'ticket'], \
        'store: platform %d, bodies %s' % (store_platform, sorted(store_bodies))
    assert stars_body == 1 and rva_field == 1 and vsize_grown == 1, \
        'stars: body %d, rva %d, VirtualSize %d' % (stars_body, rva_field, vsize_grown)
    # hop: single-byte constant changes -- ldc.i4.m1 -> ldc.i4.1, the ldc.i4.s operands -4..-2 -> 4..2,
    # and ldc.i4.1..4 (or their ldc.i4.s form) -> 5
    press = [h for h in hop if (h[1], h[2]) in ((0x15, 0x17), (0xfc, 0x04), (0xfd, 0x03), (0xfe, 0x02))]
    release = [h for h in hop if h[2] in (0x1b, 0x05) and h[1] in (0x17, 0x18, 0x19, 0x1a, 0x01, 0x02, 0x03, 0x04)]
    assert len(press) == 4 and len(release) >= 4 and len(press) + len(release) == len(hop), \
        'hop bytes %s' % [(hex(a), hex(b_), hex(c)) for a, b_, c in hop]
    print('OK: unlock site, star-level body at RVA %#x (cave), %d press / %d release hop constants, '
          'mode bar, wipe 0.6 s, no second unload, store (platform test + 3 bodies), theme-switch swap'
          % (cave_rva, len(press), len(release)))
    check_names(dll, patched, cave_rva)


if __name__ == '__main__':
    main()
