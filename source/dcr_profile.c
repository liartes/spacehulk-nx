/* dcr_profile.c -- the Switch's user profiles, for the port's C#
 * (mod/src/Profiles.cs, through the icalls in dcr_mod.c).
 *
 * Nothing here runs unless a player asks for a profile: the leaderboard
 * offers the system's own profile picker (the playerSelect applet, as a game
 * shows it) the first time it opens, and multiplayer seats can pick one in the
 * waiting room. A profile gives the player's nickname and icon; the icon is
 * written as a JPEG to <game folder>/profiles/<uid>.jpg, where the game's
 * leaderboard loads it like the avatar URL its server used to send.
 *
 * A uid travels as 32 hex digits (its two words). MIT. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <switch.h>

#include "util.h"

const char *dcr_game_root(void); /* main.c */
void dcr_applet_busy(int on);    /* watchdog.c: a system screen is up */
void dcr_input_mute_held(void);  /* dcr_input.c: what closed it is not a press in the game */

static int g_acc; /* 0 not tried, 1 ready, -1 unavailable */

static int acc_ready(void) {
  if (!g_acc) {
    Result rc = accountInitialize(AccountServiceType_Application);
    g_acc = R_SUCCEEDED(rc) ? 1 : -1;
    debugPrintf("[profile] account service %s (0x%x)\n", g_acc > 0 ? "ready" : "unavailable", rc);
  }
  return g_acc > 0;
}

static void uid_hex(AccountUid u, char out[33]) {
  snprintf(out, 33, "%016llx%016llx", (unsigned long long)u.uid[0], (unsigned long long)u.uid[1]);
}

static int uid_parse(const char *s, AccountUid *u) {
  if (!s || strlen(s) != 32)
    return 0;
  char a[17], b[17];
  memcpy(a, s, 16);
  a[16] = 0;
  memcpy(b, s + 16, 16);
  b[16] = 0;
  u->uid[0] = strtoull(a, NULL, 16);
  u->uid[1] = strtoull(b, NULL, 16);
  return accountUidIsValid(u);
}

/* The system's profile picker. `excluded`: uids (hex, comma-separated) shown
 * greyed out (profiles other players took). out: the uid picked; "" when the
 * player backed out; "-" when there is no picker (no account service). */
int dcr_profile_pick(const char *excluded, char out[33]) {
  out[0] = 0;
  if (!acc_ready()) {
    strcpy(out, "-");
    return -1;
  }
  PselUserSelectionSettings set;
  memset(&set, 0, sizeof set);
  int n = 0;
  for (const char *p = excluded; p && *p && n < ACC_USER_LIST_SIZE;) {
    const char *c = strchr(p, ',');
    size_t len = c ? (size_t)(c - p) : strlen(p);
    char one[33];
    if (len == 32) {
      memcpy(one, p, 32);
      one[32] = 0;
      if (uid_parse(one, &set.invalid_uid_list[n]))
        n++;
    }
    p = c ? c + 1 : p + len;
  }
  AccountUid uid;
  memset(&uid, 0, sizeof uid);
  dcr_applet_busy(1);
  const u64 t0 = armGetSystemTick();
  Result rc = pselShowUserSelector(&uid, &set);
  dcr_applet_busy(0);
  dcr_input_mute_held();
  if (R_SUCCEEDED(rc) && accountUidIsValid(&uid))
    uid_hex(uid, out);
  debugPrintf("[profile] picker (%d taken): 0x%x, %s, after %llu ms\n", n, rc, out[0] ? "a profile" : "none",
              (unsigned long long)(armTicksToNs(armGetSystemTick() - t0) / 1000000ull));
  return out[0] ? 0 : 1;
}

/* A profile's nickname (UTF-8, up to 32 bytes). 0 on success. */
int dcr_profile_name(const char *hex, char *out, size_t cap) {
  out[0] = 0;
  AccountUid uid;
  if (!acc_ready() || !uid_parse(hex, &uid))
    return -1;
  AccountProfile pr;
  if (R_FAILED(accountGetProfile(&pr, uid)))
    return -1;
  AccountProfileBase base;
  AccountUserData ud;
  Result rc = accountProfileGet(&pr, &ud, &base);
  if (R_SUCCEEDED(rc)) {
    size_t n = strnlen(base.nickname, sizeof base.nickname);
    if (n >= cap)
      n = cap - 1;
    memcpy(out, base.nickname, n);
    out[n] = 0;
  }
  accountProfileClose(&pr);
  return R_SUCCEEDED(rc) && out[0] ? 0 : -1;
}

/* A profile's icon, written to <root>/profiles/<uid>.jpg (again each time:
 * the player may have changed it). out: that path. 0 on success. */
int dcr_profile_icon(const char *hex, char *out, size_t cap) {
  out[0] = 0;
  AccountUid uid;
  if (!acc_ready() || !uid_parse(hex, &uid))
    return -1;
  AccountProfile pr;
  if (R_FAILED(accountGetProfile(&pr, uid)))
    return -1;
  u32 size = 0;
  int ok = 0;
  if (R_SUCCEEDED(accountProfileGetImageSize(&pr, &size)) && size > 0 && size < (1u << 20)) {
    u8 *buf = malloc(size);
    u32 got = 0;
    if (buf && R_SUCCEEDED(accountProfileLoadImage(&pr, buf, size, &got)) && got > 0) {
      char dir[300];
      snprintf(dir, sizeof dir, "%s/profiles", dcr_game_root());
      mkdir(dir, 0777);
      snprintf(out, cap, "%s/%s.jpg", dir, hex);
      FILE *f = fopen(out, "wb");
      if (f) {
        ok = fwrite(buf, 1, got, f) == got;
        fclose(f);
      }
    }
    free(buf);
  }
  accountProfileClose(&pr);
  if (!ok)
    out[0] = 0;
  return ok ? 0 : -1;
}

/* Every profile on this Switch, as hex uids, comma-separated. */
int dcr_profile_list(char *out, size_t cap) {
  out[0] = 0;
  if (!acc_ready())
    return -1;
  AccountUid ids[ACC_USER_LIST_SIZE];
  s32 n = 0;
  if (R_FAILED(accountListAllUsers(ids, ACC_USER_LIST_SIZE, &n)))
    return -1;
  size_t k = 0;
  for (s32 i = 0; i < n && k + 34 < cap; i++) {
    if (k)
      out[k++] = ',';
    uid_hex(ids[i], out + k);
    k += 32;
  }
  out[k] = 0;
  return (int)n;
}
