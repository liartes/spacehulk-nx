/* dcr_ilpatch.c -- the game's own C#, patched in memory as Mono loads it.
 *
 * Unity reads Assembly-CSharp.dll out of the APK and hands the bytes to Mono
 * (mono_image_open_from_data_with_name / _full, looked up with dlsym, which
 * bionic_dl.c routes through port_import_interpose). Before Mono sees them, a
 * copy is patched. Every patch finds its methods by name in the assembly's
 * own metadata tables and checks the code instruction by instruction; code
 * that does not look exactly as expected is left alone and logged, never
 * guessed at. Nothing of the game ships with the port, and there are no
 * offsets: the lookup runs on the user's copy. The save is never written.
 * tools/test_ilpatch.py checks every patch against the user's own game.apk.
 *
 * 1. UNLOCKS -- every character (and so every collection).
 *   This port is offline: the online events that give characters away never
 *   come. Character.IsUnlocked() is "does the
 *   player have this character" (carousel, its "n/587" count, collections,
 *   prizes, missions: 94 call sites). It is
 *     unlockedByDefault || (get_unlocked() && !IsHidden() && !universe.isHidden)
 *   with get_unlocked() the raw save flag. Only that one call becomes `true`
 *   (`ldarg.0; call get_unlocked` -> `ldc.i4.1` + five nops, same length), so
 *   hidden universes (unreleased or retired content) stay hidden.
 *   NOT get_unlocked() itself (hardware 2026-09-24): its other callers --
 *   IsUnlockedInHiddenUniverse, the cloud-save merge, toJsonObject -- then saw
 *   hidden-universe characters as owned and the next launch died in a Mono
 *   assertion (object.c:1718). The build's own UnlockAllCharacters and
 *   UnlockAllButton.Unlock are empty stubs.
 *
 * 2. STARS -- every character at its maximum level.
 *   Characters level up to 1-6 stars by rarity (CharacterManager.GetMaxLevel:
 *   classic 1 ... diamond 6); the stars switch on the boosts (coin and pixel
 *   drops, ticket bonuses) and are bought with pixels. Character.
 *   GetCurrentLevel() -- `ldarg.0; call get_currentLevel; ret`, what the
 *   gameplay and the UI read -- gets a new body:
 *     max(CharacterManager.Instance.GetMaxLevel(this), <saved level>)
 *   (the saved level alone if there is no CharacterManager yet). The new body
 *   is longer than the old one, so it goes into the zero padding at the end
 *   of the code section (file-aligned, so present in every PE; Mono maps RVAs
 *   by raw size) and the MethodDef row's RVA is pointed at it. The level-up
 *   button disappears (already at max), and the save's own level is only
 *   ever read by the cloud merge, which keeps the real value.
 *
 * 3. HOP ON PRESS -- the controller hops when the button goes down.
 *   The game copies its touch scheme to controllers: in TouchController.
 *   Action a press sends "face that way and crouch" (-1..-4) and only the
 *   release sends the hop (1..4). Every hop waited for the button to come
 *   back up, and a press during a hop overwrote the hop still queued from the
 *   tap before (PlayerController keeps one pending action) -- fast tapping
 *   lost hops. Now the press sends the hop and the release sends 5, which
 *   nothing acts on but which resets SendTouchAction's "same as last" filter
 *   so the next press of the same button hops again. Same-length constant
 *   changes in that one method: menus, the character carousel (hold to
 *   scroll) and touch are untouched.
 *
 * 4. TOP BAR -- the thin bar across the top of the screen, hidden.
 *   ModeBarController colours an Image along the top edge by game mode
 *   (crossyBlue normally, orange in events, another colour in daily
 *   missions). It sits over the world, so transparent shows the scene under
 *   it. Its UpdateToColor becomes a call of its own ClearColor().
 *
 * 5. SHORTER TRANSITIONS -- the screen wipe (theme change, missions) starts
 *   loading at 0.6 s instead of 0.8 s: its fade is complete at 0.55 s; and
 * 6.  the game's own Resources.UnloadUnusedAssets() right after the scene
 *   load (which already did one) is skipped: ~0.4 s per theme change.
 *
 * 7. FREE PURCHASES -- the store (ticket and pixel packs, bundles) is free.
 *   EconomyController.newPlatformBiller picks the store by platform: Google
 *   Play on Android, else the game's own PlatformBiller, the offline store it
 *   ships for the editor: it has the prices from the game's config and "buys"
 *   anything after 1.5 s, through the same success callbacks a real purchase
 *   takes (tickets, pixels, bundle contents). Google Play does not exist here,
 *   so the store's items were locked. The Android test (`ldc.i4.s 11`) becomes
 *   -1, which no platform is, so PlatformBiller is chosen. Its price labels
 *   (localizedPriceBillerID, "$0.99" from the config) become the game's own
 *   Language.Get("FREE") -- the call is copied from the one place the game
 *   already shows "FREE" -- and no receipts are kept: ReceiptManager.
 *   sendReceipt (the 7-argument one, that queues them in the save for a
 *   server that never answers) becomes an empty method, and figurineIdIsATicket
 *   (which made ticket-pack purchases wait for that receipt) returns false.
 *   Replaced bodies are nops up to their new last instructions.
 *
 * 8. ONE THEME-SWITCH SOUND -- in the character selector a theme change played
 *   its pop (SoundEmitter.playClipUI, pitch 0.75) and then, once
 *   CharacterSelectionRowController.ChangeRow had rebuilt the row, the
 *   carousel's "clack" (the same clip at pitch 1) for the character it centres
 *   on: two pops, heard as a double when the rebuild takes a few frames (every
 *   switch in the hardware log of 2026-09-24 had both). ChangeRow's only way
 *   there is CharacterSelectionSwipeNew.SetCenterCharacter(string), whose last
 *   two calls are SetCenterCharacter(DisplayCharacterNew) -- the clack, unless
 *   lastClack > 0 -- then ScrollToCharacter(id, true), which sets lastClack to
 *   0.05. Swapping the two calls (same bytes, reordered; nothing branches
 *   between them) makes that centring silent; scrolling the carousel still
 *   clacks per character.
 *
 * Each is a config.ini option (dcr_config.c): [game] unlock_all_characters
 * (1), max_character_stars (2), hide_top_bar (4), free_purchases (7),
 * [controls] hop_on_press (3), [performance] shorter_transitions (5 and 6),
 * [audio] one_theme_switch_sound (8).
 * MIT.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dcr_config.h"
#include "util.h"


/* ------------------------------------------------ PE / CLI metadata reader */
typedef struct {
  const uint8_t *d;
  size_t n;
  int nsec;
  struct {
    uint32_t va, vsize, raw, rawsize;
    size_t hdr; /* file offset of the section header */
  } sec[32];
} Pe;

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | rd16(p + 2) << 16; }
static void wr32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24);
}

/* Section holding an RVA (by raw size, as Mono maps them), or -1. */
static int rva_sec(const Pe *pe, uint32_t rva) {
  for (int i = 0; i < pe->nsec; i++) {
    uint32_t va = pe->sec[i].va, sz = pe->sec[i].vsize > pe->sec[i].rawsize ? pe->sec[i].vsize
                                                                           : pe->sec[i].rawsize;
    if (rva >= va && rva < va + sz)
      return i;
  }
  return -1;
}

/* File offset of an RVA, with at least `need` bytes available; 0 on failure. */
static size_t rva_off(const Pe *pe, uint32_t rva, size_t need) {
  int i = rva_sec(pe, rva);
  if (i < 0)
    return 0;
  size_t off = (size_t)pe->sec[i].raw + (rva - pe->sec[i].va);
  return off + need <= pe->n ? off : 0;
}

static int pe_open(Pe *pe, const uint8_t *d, size_t n) {
  memset(pe, 0, sizeof *pe);
  pe->d = d;
  pe->n = n;
  if (n < 0x40 || d[0] != 'M' || d[1] != 'Z')
    return -1;
  uint32_t lfanew = rd32(d + 0x3c);
  if ((size_t)lfanew + 24 > n || memcmp(d + lfanew, "PE\0\0", 4))
    return -1;
  const uint8_t *coff = d + lfanew + 4;
  int nsec = (int)rd16(coff + 2);
  uint32_t optsz = rd16(coff + 16);
  const uint8_t *opt = coff + 20;
  if ((size_t)(opt - d) + optsz > n || nsec > 32)
    return -1;
  const uint8_t *sh = opt + optsz;
  if ((size_t)(sh - d) + (size_t)nsec * 40 > n)
    return -1;
  pe->nsec = nsec;
  for (int i = 0; i < nsec; i++) {
    pe->sec[i].vsize = rd32(sh + i * 40 + 8);
    pe->sec[i].va = rd32(sh + i * 40 + 12);
    pe->sec[i].rawsize = rd32(sh + i * 40 + 16);
    pe->sec[i].raw = rd32(sh + i * 40 + 20);
    pe->sec[i].hdr = (size_t)(sh - d) + (size_t)i * 40;
  }
  return 0;
}

/* The pieces of the metadata this file needs: #Strings, and the TypeDef and
 * MethodDef tables. Offsets are into the image the Pe was opened on, so they
 * stay valid for a same-size copy. */
typedef struct {
  const uint8_t *strings;
  size_t strings_len;
  const uint8_t *us; /* #US, the string literals */
  size_t us_len;
  size_t typedef_off, methoddef_off; /* file offsets of the tables */
  uint32_t n_typedef, n_methoddef;
  uint32_t typedef_row, methoddef_row;
  int str_sz; /* #Strings index width */
  int method_idx_sz;
  size_t memberref_off; /* file offset of the MemberRef table */
  uint32_t n_memberref, memberref_row;
  int mrp_sz; /* MemberRefParent coded index width */
  size_t typeref_off; /* file offset of the TypeRef table */
  uint32_t n_typeref, typeref_row;
  int rs_sz; /* ResolutionScope coded index width */
  size_t field_off; /* file offset of the Field table */
  uint32_t n_field, field_row;
  int field_idx_sz;
  const uint8_t *base;
} Meta;

static int coded_size(const uint32_t *rows, const int *tables, int ntab, int tagbits) {
  uint32_t max = 0;
  for (int i = 0; i < ntab; i++)
    if (rows[tables[i]] > max)
      max = rows[tables[i]];
  return max < (1u << (16 - tagbits)) ? 2 : 4;
}

static int meta_open(const Pe *pe, Meta *m) {
  memset(m, 0, sizeof *m);
  const uint8_t *d = pe->d;
  m->base = d;
  /* optional header data directory 14 = CLI header */
  uint32_t lfanew = rd32(d + 0x3c);
  const uint8_t *opt = d + lfanew + 24;
  uint32_t magic = rd16(opt);
  const uint8_t *dirs = opt + (magic == 0x20b ? 112 : 96);
  uint32_t cli_rva = rd32(dirs + 14 * 8);
  size_t cli = rva_off(pe, cli_rva, 72);
  if (!cli)
    return -1;
  size_t md = rva_off(pe, rd32(d + cli + 8), 32);
  if (!md || rd32(d + md) != 0x424A5342)
    return -1;
  uint32_t vlen = rd32(d + md + 12);
  size_t p = md + 16 + vlen;
  if (p + 4 > pe->n)
    return -1;
  int nstreams = (int)rd16(d + p + 2);
  p += 4;
  const uint8_t *tables = NULL;
  for (int i = 0; i < nstreams; i++) {
    if (p + 8 > pe->n)
      return -1;
    uint32_t off = rd32(d + p), size = rd32(d + p + 4);
    const char *name = (const char *)d + p + 8;
    size_t nl = strnlen(name, 32);
    if (md + off + size > pe->n)
      return -1;
    if (!strcmp(name, "#~"))
      tables = d + md + off;
    else if (!strcmp(name, "#Strings")) {
      m->strings = d + md + off;
      m->strings_len = size;
    } else if (!strcmp(name, "#US")) {
      m->us = d + md + off;
      m->us_len = size;
    }
    p += 8 + ((nl + 4) & ~3u);
  }
  if (!tables || !m->strings)
    return -1;

  uint8_t heaps = tables[6];
  uint64_t valid = (uint64_t)rd32(tables + 8) | (uint64_t)rd32(tables + 12) << 32;
  uint32_t rows[64] = {0};
  const uint8_t *q = tables + 24;
  for (int t = 0; t < 64; t++)
    if (valid & (1ull << t)) {
      rows[t] = rd32(q);
      q += 4;
    }
  if (heaps & 0x40)
    q += 4; /* extra data */
  if (rows[0x03] || rows[0x05])
    return -1; /* FieldPtr / MethodPtr: an unoptimised (#-) layout; not expected */

  int str = (heaps & 1) ? 4 : 2, guid = (heaps & 2) ? 4 : 2, blob = (heaps & 4) ? 4 : 2;
#define IDX(t) (rows[t] < 65536 ? 2 : 4)
  static const int rs[] = {0x00, 0x1A, 0x23, 0x01}, tdor[] = {0x02, 0x01, 0x1B};
  int rs_sz = coded_size(rows, rs, 4, 2), tdor_sz = coded_size(rows, tdor, 3, 2);
  uint32_t row_module = 2 + str + 3 * guid;
  uint32_t row_typeref = rs_sz + 2 * str;
  uint32_t row_typedef = 4 + 2 * str + tdor_sz + IDX(0x04) + IDX(0x06);
  uint32_t row_field = 2 + str + blob;
  uint32_t row_methoddef = 4 + 2 + 2 + str + blob + IDX(0x08);
  m->method_idx_sz = IDX(0x06);
  m->field_idx_sz = IDX(0x04);
#undef IDX
  m->typeref_off = (size_t)(q + rows[0x00] * row_module - d);
  m->n_typeref = rows[0x01];
  m->typeref_row = row_typeref;
  m->rs_sz = rs_sz;
  const uint8_t *t = q + rows[0x00] * row_module + rows[0x01] * row_typeref;
  m->typedef_off = (size_t)(t - d);
  t += rows[0x02] * row_typedef;
  m->field_off = (size_t)(t - d);
  m->n_field = rows[0x04];
  m->field_row = row_field;
  t += rows[0x04] * row_field;
  m->methoddef_off = (size_t)(t - d);
  if (t + (size_t)rows[0x06] * row_methoddef > d + pe->n)
    return -1;
  /* on to MemberRef (0x0A), past Param (0x08) and InterfaceImpl (0x09) */
  static const int mrp[] = {0x02, 0x01, 0x1A, 0x06, 0x1B};
  m->mrp_sz = coded_size(rows, mrp, 5, 3);
  uint32_t row_param = 2 + 2 + str;
  uint32_t row_iface = (rows[0x02] < 65536 ? 2 : 4) + tdor_sz;
  const uint8_t *mr = t + (size_t)rows[0x06] * row_methoddef + (size_t)rows[0x08] * row_param +
                      (size_t)rows[0x09] * row_iface;
  m->memberref_row = m->mrp_sz + str + blob;
  m->n_memberref = rows[0x0A];
  m->memberref_off = (size_t)(mr - d);
  if (mr + (size_t)rows[0x0A] * m->memberref_row > d + pe->n)
    m->n_memberref = 0;
  m->n_typedef = rows[0x02];
  m->n_methoddef = rows[0x06];
  m->typedef_row = row_typedef;
  m->methoddef_row = row_methoddef;
  m->str_sz = str;
  return 0;
}

static uint32_t rd_idx(const uint8_t *p, int sz) { return sz == 2 ? rd16(p) : rd32(p); }

static const char *meta_str(const Meta *m, uint32_t off) {
  return off < m->strings_len ? (const char *)m->strings + off : "";
}

/* ns.type::method: the MethodDef row (1-based) of the one method of that name,
 * or 0 if there is none or more than one (overloads are not wanted here). */
static uint32_t find_method(const Meta *m, const char *ns, const char *type, const char *method) {
  uint32_t found = 0;
  int count = 0;
  for (uint32_t i = 0; i < m->n_typedef; i++) {
    const uint8_t *row = m->base + m->typedef_off + (size_t)i * m->typedef_row;
    const char *tn = meta_str(m, rd_idx(row + 4, m->str_sz));
    const char *tns = meta_str(m, rd_idx(row + 4 + m->str_sz, m->str_sz));
    if (strcmp(tn, type) || strcmp(tns, ns))
      continue;
    uint32_t mlist_off = m->typedef_row - m->method_idx_sz;
    uint32_t first = rd_idx(row + mlist_off, m->method_idx_sz);
    uint32_t last = i + 1 < m->n_typedef
                        ? rd_idx(row + m->typedef_row + mlist_off, m->method_idx_sz)
                        : m->n_methoddef + 1;
    for (uint32_t k = first; k < last && k >= 1 && k <= m->n_methoddef; k++) {
      const uint8_t *mr = m->base + m->methoddef_off + (size_t)(k - 1) * m->methoddef_row;
      if (!strcmp(meta_str(m, rd_idx(mr + 8, m->str_sz)), method)) {
        found = k;
        count++;
      }
    }
  }
  return count == 1 ? found : 0;
}

static uint32_t tok(uint32_t row) { return 0x06000000u | row; }

/* ns.type::field: the Field row (1-based), or 0. Fields are listed per type
 * (TypeDef.FieldList, the column before MethodList). */
__attribute__((unused)) static uint32_t find_field(const Meta *m, const char *ns, const char *type, const char *field) {
  for (uint32_t i = 0; i < m->n_typedef; i++) {
    const uint8_t *row = m->base + m->typedef_off + (size_t)i * m->typedef_row;
    const char *tn = meta_str(m, rd_idx(row + 4, m->str_sz));
    const char *tns = meta_str(m, rd_idx(row + 4 + m->str_sz, m->str_sz));
    if (strcmp(tn, type) || strcmp(tns, ns))
      continue;
    uint32_t flist_off = m->typedef_row - m->method_idx_sz - m->field_idx_sz;
    uint32_t first = rd_idx(row + flist_off, m->field_idx_sz);
    uint32_t last = i + 1 < m->n_typedef ? rd_idx(row + m->typedef_row + flist_off, m->field_idx_sz)
                                         : m->n_field + 1;
    for (uint32_t k = first; k < last && k >= 1 && k <= m->n_field; k++) {
      const uint8_t *fr = m->base + m->field_off + (size_t)(k - 1) * m->field_row;
      if (!strcmp(meta_str(m, rd_idx(fr + 2, m->str_sz)), field))
        return k;
    }
  }
  return 0;
}
__attribute__((unused)) static uint32_t ftok(uint32_t row) { return 0x04000000u | row; }

/* The name of MemberRef token `t` (0x0Axxxxxx), or "". */
static const char *memberref_name(const Meta *m, uint32_t t) {
  uint32_t row = t & 0xFFFFFF;
  if ((t >> 24) != 0x0A || !row || row > m->n_memberref)
    return "";
  const uint8_t *r = m->base + m->memberref_off + (size_t)(row - 1) * m->memberref_row;
  return meta_str(m, rd_idx(r + m->mrp_sz, m->str_sz));
}

/* The MemberRef token of ns.type::name whose parent is a TypeRef (a type
 * of another assembly: System.Action::Invoke), or 0 if none or several. */
static uint32_t find_memberref(const Meta *m, const char *ns, const char *type, const char *name) {
  uint32_t found = 0;
  int count = 0;
  for (uint32_t r = 1; r <= m->n_memberref; r++) {
    const uint8_t *row = m->base + m->memberref_off + (size_t)(r - 1) * m->memberref_row;
    const uint32_t cls = rd_idx(row, m->mrp_sz);
    if ((cls & 7) != 1 || strcmp(meta_str(m, rd_idx(row + m->mrp_sz, m->str_sz)), name))
      continue; /* MemberRefParent tag 1: TypeRef */
    const uint32_t tr = cls >> 3;
    if (!tr || tr > m->n_typeref)
      continue;
    const uint8_t *t = m->base + m->typeref_off + (size_t)(tr - 1) * m->typeref_row;
    if (!strcmp(meta_str(m, rd_idx(t + m->rs_sz, m->str_sz)), type) &&
        !strcmp(meta_str(m, rd_idx(t + m->rs_sz + m->str_sz, m->str_sz)), ns)) {
      found = 0x0A000000u | r;
      count++;
    }
  }
  return count == 1 ? found : 0;
}

/* The ldstr token (0x70xxxxxx) of the string literal `s` (ASCII), or 0. */
static uint32_t find_us(const Meta *m, const char *s) {
  size_t n = strlen(s);
  for (size_t i = 1; i < m->us_len;) {
    uint32_t b = m->us[i], len, h;
    if (!(b & 0x80))
      len = b, h = 1;
    else if ((b & 0xC0) == 0x80 && i + 1 < m->us_len)
      len = (b & 0x3F) << 8 | m->us[i + 1], h = 2;
    else if ((b & 0xE0) == 0xC0 && i + 3 < m->us_len)
      len = (b & 0x1F) << 24 | (uint32_t)m->us[i + 1] << 16 | (uint32_t)m->us[i + 2] << 8 | m->us[i + 3], h = 4;
    else
      return 0;
    if (i + h + len > m->us_len)
      return 0;
    const uint8_t *u = m->us + i + h; /* UTF-16LE, then one flag byte */
    if (len == 2 * n + 1) {
      size_t k = 0;
      while (k < n && u[2 * k] == (uint8_t)s[k] && !u[2 * k + 1])
        k++;
      if (k == n)
        return 0x70000000u | (uint32_t)i;
    }
    i += h + len;
  }
  return 0;
}

/* Every method type::name (global namespace; overloads): rows in out[], the count. */
static int find_overloads(const Meta *m, const char *type, const char *name, uint32_t *out, int max) {
  int count = 0;
  for (uint32_t i = 0; i < m->n_typedef; i++) {
    const uint8_t *row = m->base + m->typedef_off + (size_t)i * m->typedef_row;
    if (strcmp(meta_str(m, rd_idx(row + 4, m->str_sz)), type) ||
        *meta_str(m, rd_idx(row + 4 + m->str_sz, m->str_sz)))
      continue;
    uint32_t mlist_off = m->typedef_row - m->method_idx_sz;
    uint32_t first = rd_idx(row + mlist_off, m->method_idx_sz);
    uint32_t last = i + 1 < m->n_typedef ? rd_idx(row + m->typedef_row + mlist_off, m->method_idx_sz)
                                         : m->n_methoddef + 1;
    for (uint32_t k = first; k < last && k >= 1 && k <= m->n_methoddef; k++) {
      const uint8_t *mr = m->base + m->methoddef_off + (size_t)(k - 1) * m->methoddef_row;
      if (!strcmp(meta_str(m, rd_idx(mr + 8, m->str_sz)), name) && count < max)
        out[count++] = k;
    }
  }
  return count;
}

/* Offset in `d` of MethodDef row k's RVA field. */
static size_t method_rva_field(const Meta *m, uint32_t k) {
  return m->methoddef_off + (size_t)(k - 1) * m->methoddef_row;
}

/* The IL of a method body: code offset and length in the image; -1 if the
 * body cannot be read. */
static int method_code(const Pe *pe, const uint8_t *d, uint32_t rva, size_t *code, uint32_t *len,
                       int *tiny) {
  size_t off = rva ? rva_off(pe, rva, 1) : 0;
  if (!off)
    return -1;
  uint8_t h = d[off];
  if ((h & 3) == 2) {
    *code = off + 1;
    *len = h >> 2;
    *tiny = 1;
  } else if ((h & 3) == 3) {
    if (off + 12 > pe->n)
      return -1;
    *code = off + (rd16(d + off) >> 12) * 4;
    *len = rd32(d + off + 4);
    *tiny = 0;
  } else {
    return -1;
  }
  return *code + *len <= pe->n ? 0 : -1;
}

/* Length of the IL instruction at c[i] (ECMA-335 III), 0 if not decodable. */
static uint32_t il_len(const uint8_t *c, uint32_t n, uint32_t i) {
  uint8_t op = c[i];
  uint32_t sz;
  if (op == 0xFE) {
    if (i + 1 >= n)
      return 0;
    uint8_t o2 = c[i + 1];
    if (o2 <= 0x05 || o2 == 0x0F || o2 == 0x11 || o2 == 0x13 || o2 == 0x14 || o2 == 0x17 ||
        o2 == 0x18 || o2 == 0x1A || o2 == 0x1D || o2 == 0x1E)
      sz = 2;
    else if (o2 == 0x06 || o2 == 0x07 || o2 == 0x15 || o2 == 0x16 || o2 == 0x1C)
      sz = 6;
    else if (o2 >= 0x09 && o2 <= 0x0E)
      sz = 4;
    else if (o2 == 0x12 || o2 == 0x19)
      sz = 3;
    else
      return 0;
  } else if (op == 0x45) { /* switch */
    if (i + 5 > n)
      return 0;
    sz = 5 + 4 * rd32(c + i + 1);
  } else if ((op >= 0x0E && op <= 0x13) || op == 0x1F || (op >= 0x2B && op <= 0x37) || op == 0xDE) {
    sz = 2;
  } else if (op == 0x21 || op == 0x23) {
    sz = 9;
  } else if (op == 0x20 || op == 0x22 || (op >= 0x27 && op <= 0x29) || (op >= 0x38 && op <= 0x44) ||
             (op >= 0x6F && op <= 0x75) || (op >= 0x79 && op <= 0x81) || op == 0x8C || op == 0x8D ||
             op == 0x8F || (op >= 0xA3 && op <= 0xA5) || op == 0xC2 || op == 0xC6 || op == 0xD0 ||
             op == 0xDD) {
    sz = 5;
  } else {
    sz = 1;
  }
  return i + sz <= n ? sz : 0;
}

/* Walks a body; -1 if it does not decode cleanly to its end. */
#define IL_FOR(c, n, i, sz) for (uint32_t i = 0, sz; i < (n) && (sz = il_len((c), (n), i)); i += sz)
static int il_ok(const uint8_t *c, uint32_t n) {
  uint32_t end = 0;
  IL_FOR(c, n, i, sz) end = i + sz;
  return end == n ? 0 : -1;
}

/* ------------------------------------------------------------ the patches */
typedef struct {
  Pe pe;
  Meta m;
  uint8_t *d;       /* the copy being patched */
  size_t cave, cave_end; /* free zero bytes at the end of the code section */
  uint32_t cave_rva;
  int cave_sec;
  const char *name;
} Img;

/* Zero padding after the code section's data, for new method bodies. */
static void find_cave(Img *im, uint32_t rva_in_text) {
  int s = rva_sec(&im->pe, rva_in_text);
  im->cave = im->cave_end = 0;
  if (s < 0 || im->pe.sec[s].vsize >= im->pe.sec[s].rawsize)
    return;
  uint32_t start = (im->pe.sec[s].vsize + 3) & ~3u;
  if (start >= im->pe.sec[s].rawsize)
    return;
  size_t a = (size_t)im->pe.sec[s].raw + start, b = (size_t)im->pe.sec[s].raw + im->pe.sec[s].rawsize;
  if (b > im->pe.n)
    return;
  for (size_t k = a; k < b; k++)
    if (im->d[k])
      return; /* not padding */
  im->cave = a;
  im->cave_end = b;
  im->cave_rva = im->pe.sec[s].va + start;
  im->cave_sec = s;
}

/* The section's VirtualSize grows over what was put in the cave: Mono maps
 * RVAs by raw size, but other readers (System.Reflection.Metadata) by
 * VirtualSize, and the body must be inside the section for both. */
static void cave_commit(Img *im) {
  int s = im->cave_sec;
  uint32_t used = im->cave_rva - im->pe.sec[s].va;
  if (used > im->pe.sec[s].vsize) {
    wr32(im->d + im->pe.sec[s].hdr + 8, used);
    im->pe.sec[s].vsize = used;
  }
}

/* 1. Character.IsUnlocked: `ldarg.0; call get_unlocked` -> `ldc.i4.1; nop x5`. */
static int patch_unlocks(Img *im) {
  uint32_t get = find_method(&im->m, "", "Character", "get_unlocked");
  uint32_t is = find_method(&im->m, "", "Character", "IsUnlocked");
  size_t code;
  uint32_t len;
  int tiny;
  if (!get || !is ||
      method_code(&im->pe, im->d, rd32(im->d + method_rva_field(&im->m, is)), &code, &len, &tiny) ||
      il_ok(im->d + code, len))
    return -1;
  uint8_t *c = im->d + code;
  uint32_t at = 0, hits = 0, prev = ~0u;
  IL_FOR(c, len, i, sz) {
    if (c[i] == 0x28 && rd32(c + i + 1) == tok(get) && prev != ~0u && c[prev] == 0x02 && prev + 1 == i) {
      at = prev;
      hits++;
    }
    prev = i;
  }
  if (hits != 1)
    return -1;
  c[at] = 0x17; /* ldc.i4.1 */
  memset(c + at + 1, 0x00, 5); /* nop */
  debugPrintf("[ilpatch] unlocks: Character.IsUnlocked treats every character as owned (IL +%lu); "
              "hidden universes stay hidden\n", (unsigned long)at);
  return 0;
}

/* 2. Character.GetCurrentLevel -> max(CharacterManager.Instance.GetMaxLevel(this), saved). */
static int patch_stars(Img *im) {
  uint32_t get_lvl = find_method(&im->m, "", "Character", "get_currentLevel");
  uint32_t cur = find_method(&im->m, "", "Character", "GetCurrentLevel");
  uint32_t inst = find_method(&im->m, "", "CharacterManager", "get_Instance");
  uint32_t maxl = find_method(&im->m, "", "CharacterManager", "GetMaxLevel");
  size_t code;
  uint32_t len;
  int tiny;
  if (!get_lvl || !cur || !inst || !maxl ||
      method_code(&im->pe, im->d, rd32(im->d + method_rva_field(&im->m, cur)), &code, &len, &tiny))
    return -1;
  /* the whole old body: ldarg.0; call get_currentLevel; ret */
  const uint8_t *c = im->d + code;
  if (!tiny || len != 7 || c[0] != 0x02 || c[1] != 0x28 || rd32(c + 2) != tok(get_lvl) || c[6] != 0x2A)
    return -1;

  uint8_t b[41], *p = b;
  *p++ = 40 << 2 | 2; /* tiny header: 40 bytes of IL, max stack 8, no locals */
  *p++ = 0x28, wr32(p, tok(inst)), p += 4; /*  0 call CharacterManager::get_Instance */
  *p++ = 0x25;                             /*  5 dup */
  *p++ = 0x2C, *p++ = 24;                  /*  6 brfalse.s NONE (32) */
  *p++ = 0x02;                             /*  8 ldarg.0 */
  *p++ = 0x6F, wr32(p, tok(maxl)), p += 4; /*  9 callvirt GetMaxLevel(Character) -> max */
  *p++ = 0x25;                             /* 14 dup */
  *p++ = 0x02;                             /* 15 ldarg.0 */
  *p++ = 0x28, wr32(p, tok(get_lvl)), p += 4; /* 16 call get_currentLevel -> saved */
  *p++ = 0x2F, *p++ = 8;                   /* 21 bge.s MAX (31): max >= saved */
  *p++ = 0x26;                             /* 23 pop */
  *p++ = 0x02;                             /* 24 ldarg.0 */
  *p++ = 0x28, wr32(p, tok(get_lvl)), p += 4; /* 25 call get_currentLevel */
  *p++ = 0x2A;                             /* 30 ret saved */
  *p++ = 0x2A;                             /* 31 MAX: ret max */
  *p++ = 0x26;                             /* 32 NONE: pop (the null manager) */
  *p++ = 0x02;                             /* 33 ldarg.0 */
  *p++ = 0x28, wr32(p, tok(get_lvl)), p += 4; /* 34 call get_currentLevel */
  *p++ = 0x2A;                             /* 39 ret saved */
  if (p - b != (long)sizeof b || il_ok(b + 1, 40))
    return -1;
  if (im->cave + sizeof b > im->cave_end)
    return -1;
  /* the section's VirtualSize must still end before the next section */
  for (int k = 0; k < im->pe.nsec; k++)
    if (im->pe.sec[k].va > im->pe.sec[im->cave_sec].va && im->cave_rva + sizeof b > im->pe.sec[k].va)
      return -1;
  memcpy(im->d + im->cave, b, sizeof b);
  wr32(im->d + method_rva_field(&im->m, cur), im->cave_rva);
  debugPrintf("[ilpatch] stars: Character.GetCurrentLevel reports each character's maximum level "
              "(new body at RVA 0x%lx); the saved levels are untouched\n",
              (unsigned long)im->cave_rva);
  im->cave += sizeof b;
  im->cave_rva += sizeof b;
  cave_commit(im);
  return 0;
}

/* ldc.i4 at c[i]: its value in *v; 0 if c[i] is not one. */
static int il_ldc(const uint8_t *c, uint32_t i, uint32_t sz, int32_t *v) {
  uint8_t op = c[i];
  if (op >= 0x15 && op <= 0x1E && sz == 1)
    *v = (int32_t)op - 0x16;
  else if (op == 0x1F && sz == 2)
    *v = (int8_t)c[i + 1];
  else if (op == 0x20 && sz == 5)
    *v = (int32_t)rd32(c + i + 1);
  else
    return 0;
  return 1;
}

/* The same-length encoding of `v` over the ldc.i4 at c[i]. */
static void il_set_ldc(uint8_t *c, uint32_t i, uint32_t sz, int32_t v) {
  if (sz == 1)
    c[i] = (uint8_t)(0x16 + v); /* -1..8 */
  else if (sz == 2)
    c[i + 1] = (uint8_t)(int8_t)v;
  else
    wr32(c + i + 1, (uint32_t)v);
}

/* 3. TouchController.Action: press sends the hop, release sends 5. */
static int patch_hop(Img *im) {
  uint32_t act = find_method(&im->m, "", "TouchController", "Action");
  uint32_t send = find_method(&im->m, "", "TouchController", "SendTouchAction");
  size_t code;
  uint32_t len;
  int tiny;
  if (!act || !send ||
      method_code(&im->pe, im->d, rd32(im->d + method_rva_field(&im->m, act)), &code, &len, &tiny) ||
      il_ok(im->d + code, len))
    return -1;
  uint8_t *c = im->d + code;
  /* every `ldarg.0; ldc.i4 k; call SendTouchAction`, and every call at all */
  struct {
    uint32_t i, sz;
    int32_t v;
  } site[16];
  int nsite = 0, calls = 0, press = 0, release = 0;
  uint32_t i2 = ~0u, s2 = 0, i1 = ~0u, s1 = 0; /* the two instructions before */
  IL_FOR(c, len, i, sz) {
    if (c[i] == 0x28 && rd32(c + i + 1) == tok(send)) {
      int32_t v;
      calls++;
      if (i2 != ~0u && c[i2] == 0x02 && s2 == 1 && il_ldc(c, i1, s1, &v) && nsite < 16 && v != 0 &&
          v >= -4 && v <= 4) {
        site[nsite].i = i1, site[nsite].sz = s1, site[nsite].v = v;
        nsite++;
        if (v < 0)
          press++;
        else
          release++;
      }
    }
    i2 = i1, s2 = s1, i1 = i, s1 = sz;
  }
  if (calls != nsite || press != 4 || release < 4)
    return -1;
  for (int k = 0; k < nsite; k++)
    il_set_ldc(c, site[k].i, site[k].sz, site[k].v < 0 ? -site[k].v : 5);
  debugPrintf("[ilpatch] hop on press: TouchController.Action -- a controller press hops, the release "
              "does nothing (%d sites)\n", nsite);
  return 0;
}

/* 4. ModeBarController.UpdateToColor: `modeBar.color = color` becomes a call
 * of the class's own ClearColor() (modeBar.color = Color.clear), same length. */
static int patch_mode_bar(Img *im) {
  uint32_t upd = find_method(&im->m, "", "ModeBarController", "UpdateToColor");
  uint32_t clr = find_method(&im->m, "", "ModeBarController", "ClearColor");
  size_t code;
  uint32_t len;
  int tiny;
  if (!upd || !clr ||
      method_code(&im->pe, im->d, rd32(im->d + method_rva_field(&im->m, upd)), &code, &len, &tiny))
    return -1;
  /* ldarg.0; ldfld modeBar; ldarg.1; callvirt Graphic::set_color; ret */
  uint8_t *c = im->d + code;
  if (len != 13 || c[0] != 0x02 || c[1] != 0x7B || c[6] != 0x03 || c[7] != 0x6F || c[12] != 0x2A)
    return -1;
  /* ClearColor must set the same field through the same setter */
  size_t ccode;
  uint32_t clen;
  if (method_code(&im->pe, im->d, rd32(im->d + method_rva_field(&im->m, clr)), &ccode, &clen, &tiny))
    return -1;
  const uint8_t *cc = im->d + ccode;
  if (clen != 17 || cc[1] != 0x7B || memcmp(cc + 2, c + 2, 4) || cc[11] != 0x6F || memcmp(cc + 12, c + 8, 4))
    return -1;
  uint8_t b[13] = {0x02, 0x28}; /* ldarg.0; call ClearColor; nop x6; ret */
  wr32(b + 2, tok(clr));
  b[12] = 0x2A;
  memcpy(c, b, sizeof b);
  debugPrintf("[ilpatch] top bar: ModeBarController keeps the mode bar transparent\n");
  return 0;
}

/* 5. WipeController.Restart: the screen wipe waits WaitForSeconds(0.8) before
 * loading, but its fade is complete at 0.55 s (PasrTimer 0.3 + 0.25). The
 * one `ldc.r4 0.8; newobj WaitForSeconds::.ctor` in its iterator becomes 0.6. */
static int patch_wipe(Img *im) {
  uint32_t mn = find_method(&im->m, "", "<Restart>c__Iterator0", "MoveNext");
  size_t code;
  uint32_t len;
  int tiny;
  if (!mn || method_code(&im->pe, im->d, rd32(im->d + method_rva_field(&im->m, mn)), &code, &len, &tiny) ||
      il_ok(im->d + code, len))
    return -1;
  uint8_t *c = im->d + code;
  uint32_t at = 0;
  int hits = 0, prev = -1;
  IL_FOR(c, len, i, sz) {
    /* ldc.r4 0.8f (22 cd cc 4c 3f) followed by newobj (73) */
    if (c[i] == 0x73 && prev >= 0 && c[prev] == 0x22 && rd32(c + prev + 1) == 0x3F4CCCCDu) {
      at = (uint32_t)prev;
      hits++;
    }
    prev = (int)i;
  }
  if (hits != 1)
    return -1;
  wr32(c + at + 1, 0x3F19999Au); /* 0.6f */
  debugPrintf("[ilpatch] transitions: the screen wipe starts loading at 0.6 s, not 0.8 s\n");
  return 0;
}

/* 6. WipeController.MidWipeSetup: its `Resources.UnloadUnusedAssets(); `
 * (call; pop) becomes six nops. It runs right after SceneManager.LoadScene,
 * which in Single mode has just unloaded unused assets itself: the second
 * pass is a whole-world mark plus a full collection -- the ~0.4 s frame after
 * every theme change (hardware 2026-09-24). What it would have freed goes at
 * the next scene load. */
static int patch_unload(Img *im) {
  uint32_t mid = find_method(&im->m, "", "WipeController", "MidWipeSetup");
  size_t code;
  uint32_t len;
  int tiny;
  if (!mid || method_code(&im->pe, im->d, rd32(im->d + method_rva_field(&im->m, mid)), &code, &len, &tiny) ||
      il_ok(im->d + code, len))
    return -1;
  uint8_t *c = im->d + code;
  uint32_t at = 0;
  int hits = 0, prev = -1;
  IL_FOR(c, len, i, sz) {
    if (c[i] == 0x26 && prev >= 0 && c[prev] == 0x28 &&
        !strcmp(memberref_name(&im->m, rd32(c + prev + 1)), "UnloadUnusedAssets")) {
      at = (uint32_t)prev;
      hits++;
    }
    prev = (int)i;
  }
  if (hits == 0 && !memchr(c, 0x26, len))
    return 1; /* SEA 1.5.4: MidWipeSetup has no UnloadUnusedAssets: nothing to do */
  if (hits != 1)
    return -1;
  memset(c + at, 0x00, 6); /* nop x6 */
  debugPrintf("[ilpatch] transitions: no second Resources.UnloadUnusedAssets after the scene load\n");
  return 0;
}

/* Method row k's body replaced by nops ending in tail[n] (its last
 * instructions), in place -- or, with !apply, only whether it can be: the
 * body must decode cleanly, have no exception clauses (so no handler points
 * into it) and room for the tail and its `stack` values. 0 if so. */
static int body_replace(Img *im, uint32_t k, const uint8_t *tail, uint32_t n, uint32_t stack, int apply) {
  uint32_t rva = k ? rd32(im->d + method_rva_field(&im->m, k)) : 0;
  size_t hdr = rva ? rva_off(&im->pe, rva, 12) : 0, code;
  uint32_t len;
  int tiny;
  if (!hdr || method_code(&im->pe, im->d, rva, &code, &len, &tiny) || il_ok(im->d + code, len) || len < n)
    return -1;
  if (!tiny && ((rd16(im->d + hdr) & 0x08) || rd16(im->d + hdr + 2) < stack)) /* MoreSects; max stack */
    return -1;
  if (apply) {
    memset(im->d + code, 0x00, len - n);
    memcpy(im->d + code + len - n, tail, n);
  }
  return 0;
}

/* 7. Free purchases (the notes at the top). */
static int patch_store(Img *im) {
  uint32_t npb = find_method(&im->m, "", "EconomyController", "newPlatformBiller");
  uint32_t ctor = find_method(&im->m, "", "PlatformBiller", ".ctor");
  uint32_t ticket = find_method(&im->m, "", "PlatformBiller", "figurineIdIsATicket");
  uint32_t price = find_method(&im->m, "", "PlatformBiller", "localizedPriceBillerID");
  uint32_t lget = find_method(&im->m, "", "Language", "Get");
  uint32_t free_str = find_us(&im->m, "FREE");
  uint32_t send[2];
  size_t code;
  uint32_t len;
  int tiny;
  if (!npb || !ctor || !ticket || !price || !lget || !free_str ||
      find_overloads(&im->m, "ReceiptManager", "sendReceipt", send, 2) != 2)
    return -1;

  /* the store: `call Application::get_platform; ldc.i4.s 11; bne.un` (the
   * one Android test), in the method that also builds the plain PlatformBiller */
  if (method_code(&im->pe, im->d, rd32(im->d + method_rva_field(&im->m, npb)), &code, &len, &tiny) ||
      il_ok(im->d + code, len))
    return -1;
  uint8_t *c = im->d + code;
  uint32_t at = 0;
  int hits = 0, base = 0;
  IL_FOR(c, len, i, sz) {
    if (c[i] == 0x28 && i + 8 < len && !strcmp(memberref_name(&im->m, rd32(c + i + 1)), "get_platform") &&
        c[i + 5] == 0x1F && c[i + 6] == 11 && (c[i + 7] == 0x40 || c[i + 7] == 0x33)) {
      at = i + 6;
      hits++;
    }
    if (c[i] == 0x73 && rd32(c + i + 1) == tok(ctor))
      base++;
  }
  if (hits != 1 || base != 1)
    return -1;

  /* ReceiptManager.sendReceipt: the 5-argument one is a call of the other */
  const uint8_t *s5 = NULL;
  uint32_t s7 = 0;
  for (int k = 0; k < 2; k++)
    if (!method_code(&im->pe, im->d, rd32(im->d + method_rva_field(&im->m, send[k])), &code, &len, &tiny) &&
        tiny && len >= 6 && im->d[code + len - 6] == 0x28 && im->d[code + len - 1] == 0x2A) {
      s5 = im->d + code + len - 5;
      s7 = send[!k];
    }
  if (!s5 || rd32(s5) != tok(s7))
    return -1;

  /* figurineIdIsATicket ends `ldc.i4.s -3; ceq; ret` (universe.Id == -3) */
  if (method_code(&im->pe, im->d, rd32(im->d + method_rva_field(&im->m, ticket)), &code, &len, &tiny) ||
      len < 5 || memcmp(im->d + code + len - 5, "\x1F\xFD\xFE\x01\x2A", 5))
    return -1;

  /* the game's own `ldstr "FREE"; ldc.i4.1; call Language::Get` */
  uint8_t label[12] = {0x72};
  wr32(label + 1, free_str);
  label[5] = 0x17;
  label[6] = 0x28;
  wr32(label + 7, tok(lget));
  label[11] = 0x2A; /* ret */
  int sec = rva_sec(&im->pe, rd32(im->d + method_rva_field(&im->m, price)));
  if (sec < 0)
    return -1;
  if ((size_t)im->pe.sec[sec].raw + im->pe.sec[sec].rawsize > im->pe.n || im->pe.sec[sec].rawsize < 11)
    return -1;
  const uint8_t *s = im->d + im->pe.sec[sec].raw, *e = s + im->pe.sec[sec].rawsize - 11;
  int uses = 0;
  for (; s < e; s++)
    uses += !memcmp(s, label, 11);
  if (uses < 1)
    return -1;

  static const uint8_t ret_false[] = {0x16, 0x2A}, ret[] = {0x2A};
  for (int apply = 0; apply < 2; apply++) /* all of it or none */
    if (body_replace(im, price, label, sizeof label, 2, apply) ||
        body_replace(im, ticket, ret_false, sizeof ret_false, 1, apply) ||
        body_replace(im, s7, ret, sizeof ret, 0, apply))
      return -1;
  c[at] = 0xFF; /* ldc.i4.s -1 */
  debugPrintf("[ilpatch] free purchases: the game's offline store (PlatformBiller) completes every "
              "purchase; prices read FREE; no receipts kept\n");
  return 0;
}

/* 7b. Free purchases, SEA 1.5.4's shape. EconomyController.newPlatformBiller
 * always builds `new OpenIABWrapper(receiptManager)` (the gogame build has no
 * store switch): the newobj becomes the base PlatformBiller(ReceiptManager)
 * -- the game's own offline store, same argument. Its startPurchase reports
 * success only through the 7-argument ReceiptManager.sendReceipt's
 * cloudSuccess callback (offline, the cloud request fails: an error
 * overlay), so that sendReceipt becomes `if (cloudSuccess != null)
 * cloudSuccess(); return;` -- no receipt kept, the purchase completes the
 * game's own way. Prices read FREE and tickets are not special, as 7. */
static int patch_store_sea(Img *im) {
  uint32_t npb = find_method(&im->m, "", "EconomyController", "newPlatformBiller");
  uint32_t ctor = find_method(&im->m, "", "PlatformBiller", ".ctor");
  uint32_t oiab = find_method(&im->m, "", "OpenIABWrapper", ".ctor");
  uint32_t ticket = find_method(&im->m, "", "PlatformBiller", "figurineIdIsATicket");
  uint32_t price = find_method(&im->m, "", "PlatformBiller", "localizedPriceBillerID");
  uint32_t lget = find_method(&im->m, "", "Language", "Get");
  uint32_t free_str = find_us(&im->m, "FREE");
  uint32_t invoke = find_memberref(&im->m, "System", "Action", "Invoke");
  uint32_t send[4];
  size_t code;
  uint32_t len;
  int tiny;
  int nsend = find_overloads(&im->m, "ReceiptManager", "sendReceipt", send, 4);
  if (!npb || !ctor || !oiab || !ticket || !price || !lget || !free_str || !invoke || nsend < 2)
    return -1;

  /* newPlatformBiller: the one `newobj OpenIABWrapper::.ctor` */
  if (method_code(&im->pe, im->d, rd32(im->d + method_rva_field(&im->m, npb)), &code, &len, &tiny) ||
      il_ok(im->d + code, len))
    return -1;
  uint8_t *c = im->d + code;
  uint32_t at = 0;
  int hits = 0;
  IL_FOR(c, len, i, sz) {
    if (c[i] == 0x73 && rd32(c + i + 1) == tok(oiab)) {
      at = i + 1;
      hits++;
    }
  }
  if (hits != 1)
    return -1;

  /* the 7-argument sendReceipt: what the 4-argument one forwards to --
   * ldarg.0..3; ldarg.s 4; ldnull x3; call <it>; ret */
  static const uint8_t fwd4[] = {0x02, 0x03, 0x04, 0x05, 0x0E, 0x04, 0x14, 0x14, 0x14, 0x28};
  uint32_t s7 = 0;
  for (int k = 0; k < nsend; k++)
    if (!method_code(&im->pe, im->d, rd32(im->d + method_rva_field(&im->m, send[k])), &code, &len, &tiny) &&
        len == sizeof fwd4 + 5 && !memcmp(im->d + code, fwd4, sizeof fwd4) && im->d[code + len - 1] == 0x2A)
      s7 = rd32(im->d + code + sizeof fwd4) & 0xFFFFFF;
  if (!s7)
    return -1;
  /* ldarg.s 6 (cloudSuccess); brfalse.s ret; ldarg.s 6; callvirt Action::Invoke; ret */
  uint8_t cb[12] = {0x0E, 0x06, 0x2C, 0x07, 0x0E, 0x06, 0x6F};
  wr32(cb + 7, invoke);
  cb[11] = 0x2A;

  /* figurineIdIsATicket ends `ldc.i4.s -3; ceq; ret` (universe.Id == -3) */
  if (method_code(&im->pe, im->d, rd32(im->d + method_rva_field(&im->m, ticket)), &code, &len, &tiny) ||
      len < 5 || memcmp(im->d + code + len - 5, "\x1F\xFD\xFE\x01\x2A", 5))
    return -1;

  /* the game's own `ldstr "FREE"; ldc.i4.1; call Language::Get` */
  uint8_t label[12] = {0x72};
  wr32(label + 1, free_str);
  label[5] = 0x17;
  label[6] = 0x28;
  wr32(label + 7, tok(lget));
  label[11] = 0x2A; /* ret */
  int sec = rva_sec(&im->pe, rd32(im->d + method_rva_field(&im->m, price)));
  if (sec < 0 || (size_t)im->pe.sec[sec].raw + im->pe.sec[sec].rawsize > im->pe.n || im->pe.sec[sec].rawsize < 11)
    return -1;
  const uint8_t *s = im->d + im->pe.sec[sec].raw, *e = s + im->pe.sec[sec].rawsize - 11;
  int uses = 0;
  for (; s < e; s++)
    uses += !memcmp(s, label, 11);
  if (uses < 1)
    return -1;

  static const uint8_t ret_false[] = {0x16, 0x2A};
  for (int apply = 0; apply < 2; apply++) /* all of it or none */
    if (body_replace(im, price, label, sizeof label, 2, apply) ||
        body_replace(im, ticket, ret_false, sizeof ret_false, 1, apply) ||
        body_replace(im, s7, cb, sizeof cb, 1, apply))
      return -1;
  wr32(c + at, tok(ctor)); /* newobj PlatformBiller::.ctor */
  debugPrintf("[ilpatch] free purchases: the game's offline store (PlatformBiller) in place of OpenIAB; "
              "every purchase completes; prices read FREE; no receipts kept\n");
  return 0;
}

/* A branch target of the IL instruction at c[i] (sz bytes) in *t: the number
 * of targets written (switch: up to max). */
static int il_targets(const uint8_t *c, uint32_t i, uint32_t sz, uint32_t *t, int max) {
  uint8_t op = c[i];
  if (op >= 0x2B && op <= 0x37 && sz == 2 && max > 0) { /* short branches, leave.s = 0xDE */
    t[0] = i + 2 + (uint32_t)(int32_t)(int8_t)c[i + 1];
    return 1;
  }
  if (op == 0xDE && sz == 2 && max > 0) {
    t[0] = i + 2 + (uint32_t)(int32_t)(int8_t)c[i + 1];
    return 1;
  }
  if (((op >= 0x38 && op <= 0x44) || op == 0xDD) && sz == 5 && max > 0) {
    t[0] = i + 5 + rd32(c + i + 1);
    return 1;
  }
  if (op == 0x45) {
    uint32_t n = rd32(c + i + 1), end = i + 5 + 4 * n;
    int k = 0;
    for (uint32_t j = 0; j < n && k < max; j++)
      t[k++] = end + rd32(c + i + 5 + 4 * j);
    return k;
  }
  return 0;
}

/* 8. One theme-switch sound (the notes at the top). */
static int patch_center_quiet(Img *im) {
  uint32_t rows[2];
  uint32_t scroll = find_method(&im->m, "", "CharacterSelectionSwipeNew", "ScrollToCharacter");
  if (!scroll || find_overloads(&im->m, "CharacterSelectionSwipeNew", "SetCenterCharacter", rows, 2) != 2)
    return -1;
  for (int k = 0; k < 2; k++) {
    size_t code;
    uint32_t len;
    int tiny;
    if (method_code(&im->pe, im->d, rd32(im->d + method_rva_field(&im->m, rows[k])), &code, &len, &tiny) ||
        il_ok(im->d + code, len) || len < 26)
      continue;
    uint8_t *c = im->d + code;
    /* ...; ldarg.0; ldloc.0; call SetCenterCharacter(other)            (A, 7 bytes)
     *      ldarg.0; ldloc.0; ldfld; ldfld; ldc.i4.1; call ScrollToCharacter  (B, 18)
     *      ret */
    uint32_t s = len - 26;
    uint8_t *a = c + s, *b = c + s + 7;
    if (a[0] != 0x02 || a[1] != 0x06 || a[2] != 0x28 || rd32(a + 3) != tok(rows[!k]) || b[0] != 0x02 ||
        b[1] != 0x06 || b[2] != 0x7B || b[7] != 0x7B || b[12] != 0x17 || b[13] != 0x28 ||
        rd32(b + 14) != tok(scroll) || c[len - 1] != 0x2A)
      continue;
    /* the swap must not move a branch target: none may point inside the pair */
    int bad = 0;
    IL_FOR(c, len, i, sz) {
      uint32_t t[64];
      int n = il_targets(c, i, sz, t, 64);
      for (int j = 0; j < n; j++)
        bad |= t[j] > s && t[j] < s + 25;
    }
    if (bad)
      return -1;
    uint8_t pair[25];
    memcpy(pair, b, 18);
    memcpy(pair + 18, a, 7);
    memcpy(a, pair, 25);
    debugPrintf("[ilpatch] theme switch: one sound -- CharacterSelectionSwipeNew.SetCenterCharacter(string) "
                "scrolls before centring, so the carousel does not clack over the theme pop\n");
    return 0;
  }
  return -1;
}

/* A method whose whole body is one constant: `<old>; ret` becomes
 * `ldc.i4.1; ret` (tiny body, same length or padded with nops before). */
static int patch_return_true(Img *im, const char *ns, const char *type, const char *method, const char *what) {
  uint32_t k = find_method(&im->m, ns, type, method);
  size_t code;
  uint32_t len;
  int tiny;
  if (!k || method_code(&im->pe, im->d, rd32(im->d + method_rva_field(&im->m, k)), &code, &len, &tiny))
    return -1;
  uint8_t *c = im->d + code;
  if (len < 2 || c[len - 1] != 0x2A || il_ok(c, len))
    return -1;
  static const uint8_t tail[] = {0x17, 0x2A}; /* ldc.i4.1; ret */
  if (body_replace(im, k, tail, 2, 1, 1))
    return -1;
  debugPrintf("[ilpatch] %s\n", what);
  return 0;
}

/* 10. MENU CONTROLS (SEA). The game has a full TV-style controller UI --
 * focus on every button, A = press, B = the screen's back button, focus
 * rows on the character carousel -- all behind
 * KlicktockInput.InputManager.IsControllerInUse(), which this build compiled
 * to `return false`. It becomes `return true` (a Switch always has a pad;
 * touch keeps working: the port's C# keeps the touch input module on). And
 * the pause control InputManager.Update asks the pad for is
 * InputControlType.Pause (29), which the NVIDIA Shield profile the port's pad
 * matches does not have: its Start is InputControlType.Start (25) -- so +
 * never paused. The constant becomes 25. */
static int patch_menu_controls(Img *im) {
  int done = patch_return_true(im, "KlicktockInput", "InputManager", "IsControllerInUse",
                               "menu controls: InputManager.IsControllerInUse is true (the game's TV controller UI)") == 0;
  uint32_t upd = find_method(&im->m, "KlicktockInput", "InputManager", "Update");
  uint32_t getc = find_memberref(&im->m, "InControl", "InputDevice", "GetControl");
  size_t code;
  uint32_t len;
  int tiny, n = 0;
  if (upd && getc && !method_code(&im->pe, im->d, rd32(im->d + method_rva_field(&im->m, upd)), &code, &len, &tiny)) {
    uint8_t *c = im->d + code;
    IL_FOR(c, len, i, sz) {
      /* ldc.i4.s 29; callvirt InputDevice::GetControl */
      if (sz == 2 && c[i] == 0x1F && c[i + 1] == 29 && i + 7 <= len && (c[i + 2] == 0x6F || c[i + 2] == 0x28) &&
          rd32(c + i + 3) == getc) {
        c[i + 1] = 25;
        n++;
      }
    }
  }
  if (n == 1)
    debugPrintf("[ilpatch] menu controls: + pauses (InputManager.Update asks for InputControlType.Start, the "
                "button the pad has, not Pause)\n");
  return done || n == 1 ? 0 : -1;
}

/* 11. ONLINE (SEA): SNManager.IsLoggedIn() -- Facebook -- is true, so the
 * leaderboards and the daily login reward open and no Facebook login (which
 * would wait for ever for an SDK that is not here) is ever started. The
 * port's C# answers the rest (mod/src/Online.cs). Inlined by the JIT (6
 * bytes), hence IL rather than a hook. */
static int patch_fb_logged_in(Img *im) {
  return patch_return_true(im, "", "SNManager", "IsLoggedIn",
                           "online: SNManager.IsLoggedIn is true (a local profile; no Facebook SDK here)");
}

/* ------------------------------------------------------------ the image */

/* A patched copy of the image if it is Assembly-CSharp, else NULL. */
/* 9. (SEA, always) FirebaseMessaging.Init: GameController.NewGame calls it
 * early, and while CheckOnline.IsOnline is still its initial true it
 * subscribes to Firebase.Messaging -- P/Invoke into libApp-4.5.0.so, the
 * Firebase C++ SDK, which this port does not load: DllNotFoundException
 * aborts NewGame half-way (no save loaded, no stopwatch: a
 * NullReferenceException in GameController.Update every frame after). Its
 * body becomes nops and its closing `ret` (Subscribe/Unsubscribe are gated
 * on the init it never does). Firebase is push notifications: nothing a Switch has. */
static int patch_firebase(Img *im) {
  uint32_t k = find_method(&im->m, "", "FirebaseMessaging", "Init");
  size_t code;
  uint32_t len;
  int tiny;
  if (!k || method_code(&im->pe, im->d, rd32(im->d + method_rva_field(&im->m, k)), &code, &len, &tiny))
    return -1;
  uint8_t *c = im->d + code;
  if (len < 6 || c[0] != 0x28 || c[len - 1] != 0x2A) /* call CheckOnline::get_Instance ... ret */
    return -1;
  memset(c, 0x00, len - 1); /* nop ... ret (Mono verifies dead IL too: no stray bytes) */
  debugPrintf("[ilpatch] firebase: FirebaseMessaging.Init does nothing (no Firebase SDK here)\n");
  return 0;
}

static uint8_t *patch_image(const char *data, uint32_t len, const char *name) {
  const DcrConfig *cfg = dcr_config();
  if (!data || len < 0x200)
    return NULL; /* the Firebase patch applies whatever config.ini says */
  Img im;
  memset(&im, 0, sizeof im);
  im.name = name ? name : "Assembly-CSharp";
  if (pe_open(&im.pe, (const uint8_t *)data, len) || meta_open(&im.pe, &im.m))
    return NULL;
  uint32_t probe = find_method(&im.m, "", "Character", "IsUnlocked");
  if (!probe)
    return NULL; /* not Assembly-CSharp */
  im.d = malloc(len);
  if (!im.d)
    return NULL;
  memcpy(im.d, data, len);
  im.pe.d = im.d;
  im.m.base = im.d;
  im.m.strings = im.d + (im.m.strings - (const uint8_t *)data);
  if (im.m.us)
    im.m.us = im.d + (im.m.us - (const uint8_t *)data);
  find_cave(&im, rd32(im.d + method_rva_field(&im.m, probe)));

  int done = 0;
  if (cfg->menu_controls) {
    if (patch_menu_controls(&im) == 0)
      done++;
    else
      debugPrintf("[ilpatch] %s: KlicktockInput.InputManager is not as expected -- menus stay touch-only\n", im.name);
  }
  if (cfg->pretend_online) {
    if (patch_fb_logged_in(&im) == 0)
      done++;
    else
      debugPrintf("[ilpatch] %s: SNManager.IsLoggedIn is not as expected -- leaderboards stay closed\n", im.name);
  }
  if (patch_firebase(&im) == 0)
    done++;
  else if (find_method(&im.m, "", "FirebaseMessaging", "Init"))
    debugPrintf("[ilpatch] %s: FirebaseMessaging.Init is not as expected -- a new game may stop half-way\n",
                im.name);
  if (cfg->unlock_all) {
    if (patch_unlocks(&im) == 0)
      done++;
    else
      debugPrintf("[ilpatch] %s: Character.IsUnlocked is not as expected -- unlocks left alone\n", im.name);
  }
  if (cfg->max_stars) {
    if (patch_stars(&im) == 0)
      done++;
    else
      debugPrintf("[ilpatch] %s: Character.GetCurrentLevel is not as expected (or no room, %lu bytes) "
                  "-- levels left alone\n", im.name, (unsigned long)(im.cave_end - im.cave));
  }
  if (cfg->hop_on_press) {
    if (patch_hop(&im) == 0)
      done++;
    else
      debugPrintf("[ilpatch] %s: TouchController.Action is not as expected -- hops stay on release\n",
                  im.name);
  }
  if (cfg->short_wipe) {
    if (patch_wipe(&im) == 0)
      done++;
    else
      debugPrintf("[ilpatch] %s: WipeController.Restart is not as expected -- the wipe keeps 0.8 s\n",
                  im.name);
    const int unl = patch_unload(&im);
    if (unl == 0)
      done++;
    else if (unl < 0)
      debugPrintf("[ilpatch] %s: WipeController.MidWipeSetup is not as expected -- it keeps its "
                  "UnloadUnusedAssets\n", im.name);
  }
  if (cfg->hide_top_bar) {
    if (patch_mode_bar(&im) == 0)
      done++;
    else
      debugPrintf("[ilpatch] %s: ModeBarController.UpdateToColor is not as expected -- the top bar "
                  "stays\n", im.name);
  }
  if (cfg->free_store) {
    if (patch_store(&im) == 0 || patch_store_sea(&im) == 0)
      done++;
    else
      debugPrintf("[ilpatch] %s: the store (EconomyController.newPlatformBiller, PlatformBiller, "
                  "ReceiptManager) is not as expected -- purchases left alone\n", im.name);
  }
  uint32_t centers[2];
  if (cfg->quiet_center && find_overloads(&im.m, "CharacterSelectionSwipeNew", "SetCenterCharacter", centers, 2) < 2) {
    /* SEA 1.5.4: no SetCenterCharacter(string) re-centring after a row change: one sound already */
  } else if (cfg->quiet_center) {
    if (patch_center_quiet(&im) == 0)
      done++;
    else
      debugPrintf("[ilpatch] %s: CharacterSelectionSwipeNew.SetCenterCharacter is not as expected -- the "
                  "theme switch keeps its second sound\n", im.name);
  }
  if (!done) {
    free(im.d);
    return NULL;
  }
  return im.d;
}

/* ---------------------------------------------------- Mono image loading */
typedef void *(*open_named_fn)(char *data, uint32_t len, int need_copy, int *status, int refonly,
                               const char *name);
typedef void *(*open_full_fn)(char *data, uint32_t len, int need_copy, int *status, int refonly);
static open_named_fn o_open_named;
static open_full_fn o_open_full;

/* Mono is given the patched copy with need_copy set, so it keeps its own. */
void dcr_mod_note_image(const char *name) __attribute__((weak)); /* dcr_mod.c (not in the host test) */
static void *w_open_named(char *data, uint32_t len, int need_copy, int *status, int refonly,
                          const char *name) {
  if (dcr_mod_note_image)
    dcr_mod_note_image(name);
  uint8_t *p = patch_image(data, len, name);
  if (!p)
    return o_open_named(data, len, need_copy, status, refonly, name);
  void *img = o_open_named((char *)p, len, 1, status, refonly, name);
  free(p);
  return img;
}

static void *w_open_full(char *data, uint32_t len, int need_copy, int *status, int refonly) {
  uint8_t *p = patch_image(data, len, NULL);
  if (!p)
    return o_open_full(data, len, need_copy, status, refonly);
  void *img = o_open_full((char *)p, len, 1, status, refonly);
  free(p);
  return img;
}

void *dcr_ilpatch_interpose(const char *sym, void *real) {
  if (!real || !sym)
    return real;
  if (!strcmp(sym, "mono_image_open_from_data_with_name")) {
    o_open_named = (open_named_fn)real;
    return (void *)w_open_named;
  }
  if (!strcmp(sym, "mono_image_open_from_data_full")) {
    o_open_full = (open_full_fn)real;
    return (void *)w_open_full;
  }
  return real;
}
