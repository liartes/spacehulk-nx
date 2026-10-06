/* jni_android.c -- the Android framework, as far as Unity 2017.4 asks for it.
 *
 * Every handler here answers one Java method or field the engine reaches
 * through JNI. The set was taken from the strings libunity itself carries
 * (class names and method name/signature pairs next to each other in
 * .rodata), cross-checked with the APK's own UnityPlayer smali for the
 * lifecycle; methods nobody has handled are logged once by jni_core.c at run
 * time, which is how this table grows.
 *
 * What the engine is told, and why:
 *   - package, versions and <meta-data> come from the user's APK
 *     (dcr_manifest.c), because unity.build-id must match the data;
 *   - sourceDir is a /data/app/.../base.apk path that the libc layer maps to
 *     <root>/game.apk: the engine reads its data out of the APK zip itself;
 *   - files/cache/external dirs are the real Android paths, mapped by
 *     dcr_path.c to <root>/data and <root>/external;
 *   - SharedPreferences (PlayerPrefs = the save) go to dcr_prefs.c;
 *   - the device is a landscape 1280x720 "phone" at xhdpi with no network,
 *     no sensors, no vibrator, API level 28.
 * MIT.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bionic.h"
#include "config.h"
#include "dcr_input.h"
#include "dcr_manifest.h"
#include "dcr_path.h"
#include "dcr_prefs.h"
#include "dcr_time.h"
#include "dcr_jni_unity.h"
#include "util.h"

void dcr_window_size(int *w, int *h);   /* android_ndk.c */
const char *dcr_game_root(void);        /* main.c */
int b_clock_gettime(int clk, struct b_timespec *ts); /* bionic_time.c */
volatile int g_dcr_quit_requested;

JNI_H_DECL(jni_h_www_init); JNI_H_DECL(jni_h_www_start); /* jni_www.c */
JNI_H_DECL(jni_h_www_isAlive); JNI_H_DECL(jni_h_www_join);

#define H(fn) static jvalue fn(JObj *self, const jvalue *a, const JMethod *m)
#define F(fn) static jvalue fn(JObj *self, const JField *f)
#define S(i) jni_utf(a[i].l)

#define C_ACTIVITY "com/unity3d/player/UnityPlayerActivity"
#define C_APPLICATION "io/fabric/unity/android/MultiDexFabricApplication"
#define C_PLAYER "com/unity3d/player/UnityPlayer"

#define APP_DIR "/data/app/" DCR_PKG_NAME "-1"
#define APK_PATH APP_DIR "/base.apk"
#define LIB_DIR APP_DIR "/lib/arm"
#define DATA_DIR "/data/data/" DCR_PKG_NAME
#define EXT_ROOT "/storage/emulated/0"
#define EXT_APP EXT_ROOT "/Android/data/" DCR_PKG_NAME
#define OBB_DIR EXT_ROOT "/Android/obb/" DCR_PKG_NAME

/* ============================ small helpers =============================== */
static int win_w(void) { int w, h; dcr_window_size(&w, &h); return w; }
static int win_h(void) { int w, h; dcr_window_size(&w, &h); return h; }

static void free_p(JObj *o) {
  free(o->p);
  o->p = NULL;
}

/* java.io.File: p = the Android path (malloc'd). */
static JObj *file_new(const char *path) {
  JObj *o = jni_new("java/io/File");
  char *p = strdup(path ? path : "");
  size_t n = strlen(p);
  while (n > 1 && p[n - 1] == '/')
    p[--n] = 0;
  o->p = p;
  o->finalize = free_p;
  return o;
}
static const char *file_path(const JObj *o) { return (jni_is(o, "java/io/File") && o->p) ? (const char *)o->p : ""; }
/* A path argument may be a File or a String. */
static const char *path_arg(const void *v) { return jni_is(v, "java/io/File") ? file_path(v) : jni_utf(v); }

static int sd_stat(const char *android_path, struct stat *st) {
  char buf[DCR_PATH_MAX];
  return stat(dcr_translate_path(android_path, buf, sizeof buf), st);
}
static void sd_mkdirs(const char *android_path) {
  char buf[DCR_PATH_MAX];
  char p[DCR_PATH_MAX];
  snprintf(p, sizeof p, "%s", dcr_translate_path(android_path, buf, sizeof buf));
  for (char *c = strchr(p + 1, '/'); c; c = strchr(c + 1, '/')) {
    *c = 0;
    if (strchr(p, '/') && p[strlen(p) - 1] != ':')
      mkdir(p, 0777);
    *c = '/';
  }
  mkdir(p, 0777);
}

/* Boxed values: v[0] int/bool, v[0..1] long, v[0] float bits. */
static JObj *box_int(const char *cls, jint i) {
  JObj *o = jni_new(cls);
  o->v[0] = i;
  return o;
}
static JObj *box_long(jlong j) {
  JObj *o = jni_new("java/lang/Long");
  memcpy(&o->v[0], &j, 8);
  return o;
}
static JObj *box_float(jfloat f) {
  JObj *o = jni_new("java/lang/Float");
  memcpy(&o->v[0], &f, 4);
  return o;
}

static const char *build_str(const char *name) {
  static const struct { const char *k, *v; } t[] = {
      {"MODEL", "Nintendo Switch"}, {"MANUFACTURER", "Nintendo"}, {"BRAND", "Nintendo"},
      {"DEVICE", "nx"}, {"PRODUCT", "nx"}, {"BOARD", "nx"}, {"HARDWARE", "nx"},
      {"FINGERPRINT", "Nintendo/nx/nx:9/PKQ1/1:user/release-keys"}, {"ID", "PKQ1"},
      {"DISPLAY", "PKQ1"}, {"TAGS", "release-keys"}, {"TYPE", "user"}, {"USER", "nx"},
      {"HOST", "nx"}, {"BOOTLOADER", "unknown"}, {"RADIO", "unknown"}, {"SERIAL", "unknown"},
      {"CPU_ABI", "armeabi-v7a"}, {"CPU_ABI2", "armeabi"},
  };
  for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++)
    if (!strcmp(t[i].k, name))
      return t[i].v;
  return NULL;
}

/* A stable per-install ANDROID_ID, generated once and kept in <root>/data. */
static const char *android_id(void) {
  static char id[17];
  if (id[0])
    return id;
  char path[DCR_PATH_MAX];
  snprintf(path, sizeof path, "%s/data/android_id", dcr_game_root());
  FILE *f = fopen(path, "rb");
  if (f) {
    size_t n = fread(id, 1, 16, f);
    fclose(f);
    id[n == 16 ? 16 : 0] = 0;
  }
  if (!id[0]) {
    u64 r[1];
    randomGet(r, sizeof r);
    snprintf(id, sizeof id, "%016llx", (unsigned long long)r[0]);
    if ((f = fopen(path, "wb"))) {
      fwrite(id, 1, 16, f);
      fclose(f);
    }
  }
  return id;
}

/* ============================ the Context family ========================== */
static JObj *metadata_bundle(void);
static JObj *app_info(void);

H(ctx_getPackageName) { return jv_l(jni_str(dcr_manifest_package())); }
H(ctx_getPackageCodePath) { return jv_l(jni_str(APK_PATH)); }
H(ctx_getApplicationInfo) { return jv_l(app_info()); }
H(ctx_getApplicationContext) { return jv_l(jni_singleton(C_APPLICATION)); }
H(ctx_getFilesDir) { sd_mkdirs(DCR_ANDROID_FILES); return jv_l(file_new(DCR_ANDROID_FILES)); }
H(ctx_getCacheDir) { sd_mkdirs(DCR_ANDROID_CACHE); return jv_l(file_new(DCR_ANDROID_CACHE)); }
H(ctx_getExternalFilesDir) {
  char p[DCR_PATH_MAX];
  const char *sub = a[0].l ? S(0) : "";
  snprintf(p, sizeof p, "%s/files%s%s", EXT_APP, *sub ? "/" : "", sub);
  sd_mkdirs(p);
  return jv_l(file_new(p));
}
H(ctx_getExternalCacheDir) { sd_mkdirs(EXT_APP "/cache"); return jv_l(file_new(EXT_APP "/cache")); }
H(ctx_getObbDir) { return jv_l(file_new(OBB_DIR)); }
H(ctx_getObbDirs) {
  JObj *arr = jni_array('L', 1);
  ((JObj **)arr->a.data)[0] = file_new(OBB_DIR);
  return jv_l(arr);
}
H(ctx_getExternalFilesDirs) {
  JObj *arr = jni_array('L', 1);
  ((JObj **)arr->a.data)[0] = ctx_getExternalFilesDir(self, a, m).l;
  return jv_l(arr);
}
H(ctx_getDir) {
  char p[DCR_PATH_MAX];
  snprintf(p, sizeof p, "%s/app_%s", DATA_DIR, S(0));
  sd_mkdirs(p);
  return jv_l(file_new(p));
}
H(ctx_getAssets) { return jv_l(jni_singleton("android/content/res/AssetManager")); }
H(ctx_getResources) { return jv_l(jni_singleton("android/content/res/Resources")); }
H(ctx_getContentResolver) { return jv_l(jni_singleton("android/content/ContentResolver")); }
H(ctx_getPackageManager) { return jv_l(jni_singleton("android/content/pm/PackageManager")); }
H(ctx_checkPermission) { return jv_i(0); /* PERMISSION_GRANTED */ }
H(ctx_getSharedPreferences) {
  JObj *o = jni_new("android/content/SharedPreferences");
  o->p = dcr_prefs_open(S(0));
  return jv_l(o);
}
H(act_getPreferences) {
  JObj *o = jni_new("android/content/SharedPreferences");
  o->p = dcr_prefs_open("UnityPlayerActivity");
  return jv_l(o);
}
H(ctx_getSystemService) {
  static const struct { const char *name, *cls; } svc[] = {
      {"window", "android/view/WindowManager"}, {"audio", "android/media/AudioManager"},
      {"connectivity", "android/net/ConnectivityManager"}, {"input", "android/hardware/input/InputManager"},
      {"display", "android/hardware/display/DisplayManager"}, {"phone", "android/telephony/TelephonyManager"},
      {"activity", "android/app/ActivityManager"}, {"power", "android/os/PowerManager"},
      {"sensor", "android/hardware/SensorManager"}, {"input_method", "android/view/inputmethod/InputMethodManager"},
      {"uimode", "android/app/UiModeManager"}, {"batterymanager", "android/os/BatteryManager"},
  };
  const char *want = S(0);
  for (unsigned i = 0; i < sizeof svc / sizeof svc[0]; i++)
    if (!strcmp(svc[i].name, want))
      return jv_l(jni_singleton(svc[i].cls));
  debugPrintf("[jni] getSystemService(\"%s\") -> null\n", want);
  return jv_l(NULL); /* vibrator, location, ...: absent on this device */
}
H(ctx_registerReceiver) {
  /* The only receiver the engine registers with a null receiver is the sticky
   * ACTION_BATTERY_CHANGED query: answer with a battery Intent. */
  JObj *i = jni_new("android/content/Intent");
  i->v[0] = 1; /* battery intent */
  return jv_l(i);
}
H(ctx_noop_false) { return jv_z(0); }
H(ctx_noop) { return jv_none(); }

/* Activity */
H(act_getIntent) { return jv_l(jni_singleton("android/content/Intent")); }
H(act_getWindow) { return jv_l(jni_singleton("android/view/Window")); }
H(act_getWindowManager) { return jv_l(jni_singleton("android/view/WindowManager")); }
H(act_getRequestedOrientation) { return jv_i(6); /* SCREEN_ORIENTATION_SENSOR_LANDSCAPE */ }
H(act_isFinishing) { return jv_z(0); }
H(act_finish) {
  debugPrintf("[jni] Activity.finish() -- quitting\n");
  g_dcr_quit_requested = 1;
  return jv_none();
}
H(act_getComponentName) { return jv_l(jni_singleton("android/content/ComponentName")); }
/* ============================ UnityPlayer (Java side) ===================== */
H(up_getSplashMode) {
  const DcrMeta *e = dcr_manifest_meta("unity.splash-mode");
  return jv_i(e ? e->i : 0);
}
H(up_getSettings) { return jv_l(jni_retain(metadata_bundle())); }
H(up_loadLibrary) {
  debugPrintf("[jni] UnityPlayer.loadLibrary(\"%s\") -> false\n", S(0));
  return jv_z(0);
}
H(up_getView) { return jv_l(jni_singleton("android/view/SurfaceView")); }
H(up_reportError) {
  debugPrintf("[unity] reportError: %s: %s\n", S(0), S(1));
  return jv_none();
}
/* Handheld.PlayFullScreenMovie: (path, colour, control mode, scaling, isURL,
 * offset, length); played from the frame loop (sh_video.c, dcr_boot.c) */
void sh_video_request(const char *android_path, int is_url, int64_t off, int64_t len);
H(up_showVideoPlayer) {
  sh_video_request(a[0].l ? jni_utf(a[0].l) : NULL, a[4].z, (int64_t)a[5].i, (int64_t)a[6].i);
  return jni_h_void(self, a, m);
}

H(up_quit) {
  g_dcr_quit_requested = 1;
  return jv_none();
}
F(up_currentActivity) { return jv_l(jni_singleton(C_ACTIVITY)); }

/* ============================ PackageManager ============================== */
static JObj *metadata_bundle(void) {
  JObj *b = jni_singleton("android/os/Bundle");
  b->v[0] = 1; /* the manifest meta-data bundle */
  return b;
}
static JObj *app_info(void) { return jni_singleton("android/content/pm/ApplicationInfo"); }

H(pm_getPackageInfo) { return jv_l(jni_singleton("android/content/pm/PackageInfo")); }
H(pm_getApplicationInfo) { return jv_l(app_info()); }
H(pm_getActivityInfo) { return jv_l(jni_singleton("android/content/pm/ActivityInfo")); }
H(pm_getInstallerPackageName) { return jv_l(jni_str("com.android.vending")); }
H(pm_hasSystemFeature) {
  const char *f = S(0);
  int yes = !strncmp(f, "android.hardware.touchscreen", 28) ||
            !strcmp(f, "android.hardware.screen.landscape");
  return jv_z(yes);
}
H(pm_checkPermission) { return jv_i(0); }
H(pm_getLaunchIntentForPackage) { return jv_l(jni_new("android/content/Intent")); }
H(pm_resolveActivity) { return jv_l(NULL); }

F(ai_sourceDir) { return jv_l(jni_str(APK_PATH)); }
F(ai_nativeLibraryDir) { return jv_l(jni_str(LIB_DIR)); }
F(ai_dataDir) { return jv_l(jni_str(DATA_DIR)); }
F(ai_packageName) { return jv_l(jni_str(dcr_manifest_package())); }
F(ai_flags) { return jv_i(0); }
F(ai_targetSdk) { return jv_i(28); }
F(ai_minSdk) { return jv_i(19); }
F(ai_uid) { return jv_i(10123); }
F(ai_metaData) { return jv_l(jni_retain(metadata_bundle())); }
F(ai_name) { return jv_l(jni_str(C_ACTIVITY)); }
F(pi_versionName) { return jv_l(jni_str(dcr_manifest_version_name())); }
F(pi_versionCode) { return jv_i(dcr_manifest_version_code()); }
F(pi_installTime) { return jv_j(1546300800000ll); /* 2019-01-01 */ }
F(pi_applicationInfo) { return jv_l(app_info()); }
F(ai_screenOrientation) { return jv_i(6); }
F(ai_configChanges) { return jv_i(0x40003fff); }

/* ================================ Bundle ================================== */
static const DcrMeta *bundle_meta(const JObj *b, const char *key) {
  return (b && b->v[0] == 1) ? dcr_manifest_meta(key) : NULL;
}
H(bundle_getBoolean) {
  const DcrMeta *e = bundle_meta(self, S(0));
  int def = strstr(m->sig, ";Z)") ? a[1].z : 0;
  if (!e) return jv_z(def);
  if (e->type == DCR_META_BOOL || e->type == DCR_META_INT) return jv_z(e->i != 0);
  if (e->type == DCR_META_STRING) return jv_z(!strcmp(e->s, "true"));
  return jv_z(def);
}
H(bundle_getInt) {
  const DcrMeta *e = bundle_meta(self, S(0));
  int has_def = strstr(m->sig, "(Ljava/lang/String;I)") != NULL;
  if (e && (e->type == DCR_META_INT || e->type == DCR_META_BOOL)) return jv_i(e->i);
  return jv_i(has_def ? a[1].i : 0);
}
H(bundle_getFloat) {
  const DcrMeta *e = bundle_meta(self, S(0));
  if (e && e->type == DCR_META_FLOAT) return jv_f(e->f);
  return jv_f(strstr(m->sig, "(Ljava/lang/String;F)") ? a[1].f : 0.0f);
}
H(bundle_getString) {
  const DcrMeta *e = bundle_meta(self, S(0));
  if (e && e->type == DCR_META_STRING) return jv_l(jni_str(e->s));
  return jv_l(strstr(m->sig, "(Ljava/lang/String;Ljava/lang/String;)") && a[1].l ? jni_str(S(1)) : NULL);
}
H(bundle_containsKey) { return jv_z(bundle_meta(self, S(0)) != NULL); }
H(bundle_get) {
  const DcrMeta *e = bundle_meta(self, S(0));
  if (!e) return jv_l(NULL);
  switch (e->type) {
  case DCR_META_STRING: return jv_l(jni_str(e->s));
  case DCR_META_BOOL: return jv_l(box_int("java/lang/Boolean", e->i != 0));
  case DCR_META_FLOAT: return jv_l(box_float(e->f));
  default: return jv_l(box_int("java/lang/Integer", e->i));
  }
}
H(bundle_isEmpty) { return jv_z(!(self && self->v[0] == 1 && dcr_manifest_meta_count())); }
H(bundle_size) { return jv_i((self && self->v[0] == 1) ? dcr_manifest_meta_count() : 0); }

/* ================================ Build =================================== */
F(build_str_field) {
  const char *v = build_str(f->name);
  if (!v && !strcmp(f->name, "SUPPORTED_ABIS")) {
    JObj *arr = jni_array('L', 2);
    ((JObj **)arr->a.data)[0] = jni_str("armeabi-v7a");
    ((JObj **)arr->a.data)[1] = jni_str("armeabi");
    return jv_l(arr);
  }
  return jv_l(v ? jni_str(v) : NULL);
}
F(bv_SDK_INT) { return jv_i(28); }
F(bv_RELEASE) { return jv_l(jni_str("9")); }
F(bv_INCREMENTAL) { return jv_l(jni_str("1")); }
F(bv_CODENAME) { return jv_l(jni_str("REL")); }
F(bv_SDK) { return jv_l(jni_str("28")); }

/* ====================== display / window / resources ====================== */
/* xhdpi at 720p; the same physical size at other resolutions (1080p: 3.0,
 * 480 dpi), so dp sizes and the game's dpi-based swipe distance hold. */
#define DENSITY (2.0f * (float)win_h() / 720.0f)
#define DPI (320 * win_h() / 720)
F(dm_widthPixels) { return jv_i(win_w()); }
F(dm_heightPixels) { return jv_i(win_h()); }
F(dm_density) { return jv_f(DENSITY); }
F(dm_densityDpi) { return jv_i(DPI); }
F(dm_xdpi) { return jv_f(237.0f * (float)win_h() / 720.0f); /* 6.2" panel */ }

F(cfg_orientation) { return jv_i(2); /* ORIENTATION_LANDSCAPE */ }
F(cfg_screenLayout) { return jv_i(0x23); /* SIZE_LARGE | LONG_YES */ }
F(cfg_uiMode) { return jv_i(0x11); /* TYPE_NORMAL | NIGHT_NO */ }
F(cfg_keyboard) { return jv_i(1); /* NOKEYS */ }
F(cfg_hidden_yes) { return jv_i(2); }
F(cfg_navigation) { return jv_i(1); /* NONAV */ }
F(cfg_touchscreen) { return jv_i(3); /* FINGER */ }
F(cfg_densityDpi) { return jv_i(DPI); }
F(cfg_screenWidthDp) { return jv_i((int)(win_w() / DENSITY)); }
F(cfg_screenHeightDp) { return jv_i((int)(win_h() / DENSITY)); }
F(cfg_fontScale) { return jv_f(1.0f); }
F(cfg_locale) { return jv_l(jni_singleton("java/util/Locale")); }
F(zero_int) { return jv_i(0); }

H(res_getConfiguration) { return jv_l(jni_singleton("android/content/res/Configuration")); }
H(res_getDisplayMetrics) { return jv_l(jni_singleton("android/util/DisplayMetrics")); }
H(res_getIdentifier) { return jv_i(0); }

H(wm_getDefaultDisplay) { return jv_l(jni_singleton("android/view/Display")); }
H(disp_getWidth) { return jv_i(win_w()); }
H(disp_getHeight) { return jv_i(win_h()); }
H(disp_zero) { return jv_i(0); }
H(disp_getRefreshRate) { return jv_f(60.0f); }
H(disp_getName) { return jv_l(jni_str("Built-in Screen")); }
H(dm_getDisplay) { return jv_l(a[0].i == 0 ? jni_singleton("android/view/Display") : NULL); }
H(dm_getDisplays) {
  JObj *arr = jni_array('L', 1);
  ((JObj **)arr->a.data)[0] = jni_singleton("android/view/Display");
  return jv_l(arr);
}

H(win_getDecorView) { return jv_l(jni_singleton("android/view/View")); }
H(win_getAttributes) { return jv_l(jni_singleton("android/view/WindowManager$LayoutParams")); }
H(view_getWidth) { return jv_i(win_w()); }
H(view_getHeight) { return jv_i(win_h()); }
H(view_self) { return jv_l(self); }

/* ================================ Locale ================================== */
H(loc_getDefault) { return jv_l(jni_singleton("java/util/Locale")); }
H(loc_getLanguage) {
  /* The console's language, as the two-letter code Unity maps to SystemLanguage. */
  static char lang[8];
  if (!lang[0]) {
    snprintf(lang, sizeof lang, "en");
    u64 code = 0;
    if (R_SUCCEEDED(setInitialize())) {
      if (R_SUCCEEDED(setGetSystemLanguage(&code))) {
        char c[9] = {0};
        memcpy(c, &code, 8);
        c[2] = 0; /* "en-US" -> "en", "zh-Hans" -> "zh" */
        if (c[0])
          memcpy(lang, c, 3);
      }
      setExit();
    }
  }
  return jv_l(jni_str(lang));
}
H(loc_getCountry) { return jv_l(jni_str("US")); }
H(loc_toString) {
  JObj *l = loc_getLanguage(self, a, m).l;
  JObj *r = jni_str_fmt("%s_US", jni_utf(l));
  jni_release(l);
  return jv_l(r);
}
H(loc_getDisplayLanguage) { return jv_l(jni_str("English")); }

/* ================================= File =================================== */
H(file_init_s) { return jv_l(file_new(S(0))); }
H(file_init_fs) {
  char p[DCR_PATH_MAX];
  snprintf(p, sizeof p, "%s/%s", path_arg(a[0].l), S(1));
  return jv_l(file_new(p));
}
H(file_getPath) { return jv_l(jni_str(file_path(self))); }
H(file_getName) {
  const char *p = file_path(self), *s = strrchr(p, '/');
  return jv_l(jni_str(s ? s + 1 : p));
}
H(file_getParent) {
  char p[DCR_PATH_MAX];
  snprintf(p, sizeof p, "%s", file_path(self));
  char *s = strrchr(p, '/');
  if (!s || s == p)
    return jv_l(NULL);
  *s = 0;
  return jv_l(!strcmp(m->name, "getParentFile") ? file_new(p) : jni_str(p));
}
H(file_exists) { struct stat st; return jv_z(sd_stat(file_path(self), &st) == 0); }
H(file_isDirectory) { struct stat st; return jv_z(sd_stat(file_path(self), &st) == 0 && S_ISDIR(st.st_mode)); }
H(file_isFile) { struct stat st; return jv_z(sd_stat(file_path(self), &st) == 0 && S_ISREG(st.st_mode)); }
H(file_length) { struct stat st; return jv_j(sd_stat(file_path(self), &st) == 0 ? (jlong)st.st_size : 0); }
H(file_mkdirs) { sd_mkdirs(file_path(self)); return jv_z(1); }
H(file_true) { return jv_z(1); }
H(file_delete) {
  char buf[DCR_PATH_MAX];
  return jv_z(remove(dcr_translate_path(file_path(self), buf, sizeof buf)) == 0);
}
H(file_getFreeSpace) { return jv_j(2ll << 30); }

/* ============================= Environment ================================ */
H(env_getExternalStorageState) { return jv_l(jni_str("mounted")); }
H(env_getExternalStorageDirectory) { return jv_l(file_new(EXT_ROOT)); }
H(env_getDataDirectory) { return jv_l(file_new("/data")); }
H(env_true) { return jv_z(1); }
H(env_false) { return jv_z(0); }

/* ========================== SharedPreferences ============================= */
static DcrPrefs *prefs_of(const JObj *o) { return (DcrPrefs *)(o ? o->p : NULL); }

static char *pref_get(JObj *self, const char *key, char *type) {
  DcrPrefs *p = prefs_of(self);
  return p ? dcr_prefs_get(p, key, type) : NULL;
}
H(sp_getString) {
  char t;
  char *v = pref_get(self, S(0), &t);
  JObj *r = v ? jni_str(v) : (a[1].l ? jni_str(S(1)) : NULL);
  free(v);
  return jv_l(r);
}
H(sp_getInt) {
  char t;
  char *v = pref_get(self, S(0), &t);
  jint r = v ? (jint)strtol(v, NULL, 10) : a[1].i;
  free(v);
  return jv_i(r);
}
H(sp_getLong) {
  char t;
  char *v = pref_get(self, S(0), &t);
  jlong r = v ? (jlong)strtoll(v, NULL, 10) : a[1].j;
  free(v);
  return jv_j(r);
}
H(sp_getFloat) {
  char t;
  char *v = pref_get(self, S(0), &t);
  jfloat r = v ? strtof(v, NULL) : a[1].f;
  free(v);
  return jv_f(r);
}
H(sp_getBoolean) {
  char t;
  char *v = pref_get(self, S(0), &t);
  int r = v ? !strcmp(v, "true") : a[1].z;
  free(v);
  return jv_z(r);
}
H(sp_contains) {
  char t;
  char *v = pref_get(self, S(0), &t);
  free(v);
  return jv_z(v != NULL);
}
static void editor_finalize(JObj *o) {
  dcr_prefs_edit_free(o->p);
  o->p = NULL;
}
H(sp_edit) {
  JObj *e = jni_new("android/content/SharedPreferences$Editor");
  e->p = dcr_prefs_edit(prefs_of(self));
  e->finalize = editor_finalize;
  return jv_l(e);
}
static jvalue editor_put(JObj *self, char type, const char *key, const char *val) {
  if (self && self->p)
    dcr_prefs_put(self->p, type, key, val);
  return jv_l(jni_retain(self));
}
H(ed_putString) { return editor_put(self, 's', S(0), S(1)); }
H(ed_putInt) {
  char v[16];
  snprintf(v, sizeof v, "%ld", (long)a[1].i);
  return editor_put(self, 'i', S(0), v);
}
H(ed_putLong) {
  char v[24];
  snprintf(v, sizeof v, "%lld", (long long)a[1].j);
  return editor_put(self, 'l', S(0), v);
}
H(ed_putFloat) {
  char v[32];
  snprintf(v, sizeof v, "%.9g", (double)a[1].f);
  return editor_put(self, 'f', S(0), v);
}
H(ed_putBoolean) { return editor_put(self, 'b', S(0), a[1].z ? "true" : "false"); }
H(ed_remove) {
  if (self && self->p)
    dcr_prefs_remove(self->p, S(0));
  return jv_l(jni_retain(self));
}
H(ed_clear) {
  if (self && self->p)
    dcr_prefs_clear(self->p);
  return jv_l(jni_retain(self));
}
H(ed_commit) { return jv_z(self && self->p && dcr_prefs_commit(self->p) == 0); }

/* ============================ System & friends ============================ */
H(sys_currentTimeMillis) {
  struct b_timespec ts;
  b_clock_gettime(L_CLOCK_REALTIME, &ts);
  return jv_j((jlong)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
H(sys_nanoTime) { return jv_j((jlong)dcr_monotonic_ns()); }
H(clock_uptimeMillis) { return jv_j((jlong)(dcr_monotonic_ns() / 1000000ull)); }
H(sys_getProperty) {
  static const struct { const char *k, *v; } props[] = {
      {"http.agent", "Dalvik/2.1.0 (Linux; U; Android 9; Nintendo Switch Build/PKQ1)"},
      {"java.vm.version", "2.1.0"}, {"os.arch", "armv8l"}, {"os.name", "Linux"},
      {"line.separator", "\n"}, {"file.separator", "/"}, {"java.io.tmpdir", DCR_ANDROID_CACHE},
      {"user.home", ""}, {"os.version", "4.9.0"},
  };
  for (unsigned i = 0; i < sizeof props / sizeof props[0]; i++)
    if (!strcmp(props[i].k, S(0)))
      return jv_l(jni_str(props[i].v));
  return jv_l(a[1].l && strstr(m->sig, "Ljava/lang/String;Ljava/lang/String;") ? jni_str(S(1)) : NULL);
}
H(sys_identityHashCode) { return jv_i((jint)((uintptr_t)a[0].l >> 2)); }
H(noop) { return jv_none(); }
H(ret_null) { return jv_l(NULL); }
H(ret_true) { return jv_z(1); }
H(ret_false) { return jv_z(0); }
H(ret_zero) { return jv_i(0); }

H(thread_current) { return jv_l(jni_singleton("java/lang/Thread")); }
H(thread_getName) { return jv_l(jni_str("UnityMain")); }
H(thread_getId) { return jv_j(1); }

H(secure_getString) {
  if (!strcmp(S(1), "android_id"))
    return jv_l(jni_str(android_id()));
  return jv_l(NULL);
}
H(system_getInt) { return jv_i(a[2].i); }

/* ============================ Object / Class ============================== */
H(obj_getClass) { return jv_l(self ? self->cls->obj : NULL); }
H(obj_hashCode) { return jv_i((jint)((uintptr_t)self >> 2)); }
H(obj_equals) { return jv_z(self == a[0].l); }
H(obj_toString) {
  if (self && self->kind == JK_STRING)
    return jv_l(jni_retain(self));
  if (jni_is(self, "java/lang/Throwable"))
    return jv_l(jni_str_fmt("%s: %s", self->cls->name, self->p ? (char *)self->p : ""));
  if (jni_is(self, "java/io/File"))
    return jv_l(jni_str(file_path(self)));
  return jv_l(jni_str_fmt("%s@%lx", self ? self->cls->name : "null", (unsigned long)(uintptr_t)self));
}
H(class_getName) {
  if (!self || self->kind != JK_CLASS)
    return jv_l(NULL);
  char n[112];
  snprintf(n, sizeof n, "%s", self->c.of->name);
  for (char *c = n; *c; c++)
    if (*c == '/')
      *c = '.';
  if (!strcmp(m->name, "getSimpleName")) {
    char *s = strrchr(n, '.');
    return jv_l(jni_str(s ? s + 1 : n));
  }
  return jv_l(jni_str(n));
}
H(class_getClassLoader) { return jv_l(jni_singleton("java/lang/ClassLoader")); }
H(class_forName) {
  /* ClassLoader.loadClass / Class.forName: the same answer FindClass gives. */
  char n[112];
  snprintf(n, sizeof n, "%s", S(0));
  for (char *c = n; *c; c++)
    if (*c == '.')
      *c = '/';
  if (!jni_class_exists(n)) {
    jni_throw("java/lang/ClassNotFoundException", n);
    return jv_l(NULL);
  }
  return jv_l(jni_class(n)->obj);
}

/* ================================ String ================================== */
H(str_getBytes) {
  const char *s = jni_utf(self);
  size_t n = strlen(s);
  JObj *arr = jni_array('B', (jsize)n);
  memcpy(arr->a.data, s, n);
  return jv_l(arr);
}
H(str_init_bytes) {
  const JObj *arr = a[0].l;
  if (!arr || arr->kind != JK_ARRAY)
    return jv_l(jni_str(""));
  char *s = malloc((size_t)arr->a.len + 1);
  memcpy(s, arr->a.data, (size_t)arr->a.len);
  s[arr->a.len] = 0;
  JObj *r = jni_str(s);
  free(s);
  return jv_l(r);
}
H(str_init_empty) { return jv_l(jni_str("")); }
H(str_length) {
  /* UTF-16 length, via the JNI string routines (jni_core caches the form). */
  const char *s = jni_utf(self);
  jint n = 0;
  for (const unsigned char *p = (const unsigned char *)s; *p; p++)
    n += (*p & 0xc0) != 0x80 ? ((*p & 0xf8) == 0xf0 ? 2 : 1) : 0;
  return jv_i(n);
}
/* ======================= com.unity3d.player.UnityWebRequest ================
 * The port is offline. A phone without a network fails every request in
 * runSafe() with UnknownHostException -> unknownHostCallback -> native
 * errorCallback(ptr, 7, e.toString()); do exactly that, so requests finish
 * (with an error) instead of staying pending for ever. v[0..1]: the native
 * request pointer (jlong), p: the URL. */
static void uwr_finalize(JObj *o) {
  jni_release(o->p);
  o->p = NULL;
}
H(uwr_init) {
  memcpy(&self->v[0], &a[0].j, 8);
  self->p = jni_retain(a[1].l);
  self->finalize = uwr_finalize;
  return jv_none();
}
H(uwr_run) {
  typedef void (*err_fn)(void *env, void *cls, jlong ptr, jint code, void *msg);
  err_fn fn = (err_fn)jni_native("com/unity3d/player/UnityWebRequest", "errorCallback");
  jlong ptr;
  memcpy(&ptr, &self->v[0], 8);
  const char *url = jni_utf(self->p);
  char host[128] = "";
  const char *h = strstr(url, "://");
  h = h ? h + 3 : url;
  size_t n = strcspn(h, "/:?#");
  snprintf(host, sizeof host, "%.*s", (int)(n < sizeof host ? n : sizeof host - 1), h);
  static int logged;
  if (logged++ < 8)
    debugPrintf("[net] offline: request to %s fails with UnknownHostException\n", url);
  if (fn && ptr) {
    JObj *msg = jni_str_fmt("java.net.UnknownHostException: Unable to resolve host \"%s\": "
                            "No address associated with hostname", host);
    fn(g_jni_env, jni_class("com/unity3d/player/UnityWebRequest")->obj, ptr, 7, msg);
    jni_release(msg);
  }
  return jv_none();
}
H(uimode_getCurrentModeType) { return jv_i(1); /* UI_MODE_TYPE_NORMAL */ }

/* ================================ android.net.Uri ========================= */
static int hexval(char c) {
  return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
       : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* Uri.decode: '%XX' octets decoded (UTF-8 passes through byte-wise); '+' is
 * left alone, as Android does. Uri.encode(s[, allow]): everything except
 * letters, digits, "_-!.~'()*" and `allow` becomes %XX (upper-case hex). */
H(uri_decode) {
  const char *s = jni_utf(a[0].l);
  if (!a[0].l)
    return jv_l(NULL);
  size_t n = strlen(s);
  char *out = malloc(n + 1), *o = out;
  for (size_t i = 0; i < n; i++) {
    int hi, lo;
    if (s[i] == '%' && i + 2 < n && (hi = hexval(s[i + 1])) >= 0 && (lo = hexval(s[i + 2])) >= 0) {
      *o++ = (char)(hi << 4 | lo);
      i += 2;
    } else {
      *o++ = s[i];
    }
  }
  *o = 0;
  JObj *r = jni_str(out);
  free(out);
  return jv_l(r);
}
H(uri_encode) {
  const char *s = jni_utf(a[0].l);
  if (!a[0].l)
    return jv_l(NULL);
  /* encode(String) or encode(String, String allow) */
  const char *allow = !strncmp(m->sig, "(Ljava/lang/String;Ljava/lang/String;)", 38) ? jni_utf(a[1].l) : "";
  static const char hex[] = "0123456789ABCDEF";
  size_t n = strlen(s);
  char *out = malloc(n * 3 + 1), *o = out;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
        (c && strchr("_-!.~'()*", c)) || (c && allow && strchr(allow, c))) {
      *o++ = (char)c;
    } else {
      *o++ = '%';
      *o++ = hex[c >> 4];
      *o++ = hex[c & 15];
    }
  }
  *o = 0;
  JObj *r = jni_str(out);
  free(out);
  return jv_l(r);
}

H(str_equals) { return jv_z(jni_is(a[0].l, "java/lang/String") && !strcmp(jni_utf(self), jni_utf(a[0].l))); }
H(str_hashCode) {
  int32_t h = 0;
  for (const unsigned char *p = (const unsigned char *)jni_utf(self); *p; p++)
    h = 31 * h + *p;
  return jv_i(h);
}

/* =============================== boxing =================================== */
H(box_intValue) { return jv_i((jint)self->v[0]); }
H(box_longValue) {
  jlong j;
  if (jni_is(self, "java/lang/Long")) memcpy(&j, &self->v[0], 8);
  else j = (jlong)self->v[0];
  return jv_j(j);
}
H(box_floatValue) {
  jfloat f;
  if (jni_is(self, "java/lang/Float")) memcpy(&f, &self->v[0], 4);
  else f = (jfloat)self->v[0];
  return jv_f(f);
}
H(box_booleanValue) { return jv_z(self->v[0] != 0); }
H(int_valueOf) { return jv_l(box_int("java/lang/Integer", a[0].i)); }
H(bool_valueOf) { return jv_l(box_int("java/lang/Boolean", a[0].z)); }
H(long_valueOf) { return jv_l(box_long(a[0].j)); }
H(float_valueOf) { return jv_l(box_float(a[0].f)); }

/* ============================== Throwable ================================= */
H(thr_getMessage) { return jv_l(jni_str(self && self->p ? (char *)self->p : "")); }
H(thr_getStackTrace) { return jv_l(jni_array('L', 0)); }

/* ================================ Intent ================================== */
H(intent_getIntExtra) {
  if (self && self->v[0] == 1) { /* battery */
    const char *k = S(0);
    if (!strcmp(k, "level")) {
      u32 pct = 100;
      if (R_SUCCEEDED(psmInitialize())) {
        psmGetBatteryChargePercentage(&pct);
        psmExit();
      }
      return jv_i((jint)pct);
    }
    if (!strcmp(k, "scale")) return jv_i(100);
    if (!strcmp(k, "status")) return jv_i(2); /* CHARGING: unknown is treated as fine */
  }
  return jv_i(a[1].i);
}
H(intent_getAction) { return jv_l(jni_str("android.intent.action.MAIN")); }

/* ============================ audio / network ============================= */
H(am_getProperty) {
  const char *k = S(0);
  if (strstr(k, "OUTPUT_SAMPLE_RATE")) return jv_l(jni_str("48000"));
  if (strstr(k, "OUTPUT_FRAMES_PER_BUFFER")) return jv_l(jni_str("256"));
  return jv_l(NULL);
}
H(am_getStreamVolume) { return jv_i(10); }
H(am_getStreamMaxVolume) { return jv_i(15); }
H(am_requestAudioFocus) { return jv_i(1); /* AUDIOFOCUS_REQUEST_GRANTED */ }
H(am_getRingerMode) { return jv_i(2); /* RINGER_MODE_NORMAL */ }

/* ============================ input devices =============================== */
/* ============================ the gamepad ================================
 * One InputDevice: the Switch controllers as an NVIDIA Shield controller
 * (dcr_input.h/.c). Unity registers it on its first event (AddJoystickInfo ->
 * JoystickInfo: getDevice(id), getName, getDescriptor, getMotionRange(axis),
 * getMotionRanges() walked with an Iterator, MotionRange.getAxis/getSource,
 * MotionEvent.axisToString) and lists it in Input.GetJoystickNames(), where
 * the game's InControl matches the name to its Shield profile. */
H(id_getDeviceIds) {
  JObj *arr = jni_array('I', 1);
  ((jint *)arr->a.data)[0] = DCR_PAD_DEVICE_ID;
  return jv_l(arr);
}
static JObj *pad_device(void) { return jni_singleton("android/view/InputDevice"); }
H(id_getDevice) {
  static int logged;
  if (a[0].i != DCR_PAD_DEVICE_ID)
    return jv_l(NULL);
  if (!logged++)
    debugPrintf("[input] the engine registered the gamepad (InputDevice.getDevice(%d))\n", a[0].i);
  return jv_l(pad_device());
}
H(pad_getName) { return jv_l(jni_str(DCR_PAD_NAME)); }
H(pad_getDescriptor) { return jv_l(jni_str("dcr-switch-controllers-0")); }
H(pad_getId) { return jv_i(DCR_PAD_DEVICE_ID); }
H(pad_getSources) { return jv_i(DCR_PAD_SOURCES); }
H(pad_getVendorId) { return jv_i(0x0955); /* NVIDIA */ }
H(pad_getProductId) { return jv_i(0x7214); /* Shield controller (2017) */ }
H(pad_getControllerNumber) { return jv_i(1); }
H(pad_getKeyboardType) { return jv_i(1); /* KEYBOARD_TYPE_NON_ALPHABETIC */ }
H(pad_false) { return jv_z(0); }
H(pad_true) { return jv_z(1); }
H(pad_hasKeys) { /* boolean[] hasKeys(int... keycodes): the Shield has them all */
  JObj *in = a[0].l;
  jsize n = in ? in->a.len : 0;
  JObj *out = jni_array('Z', n);
  for (jsize i = 0; i < n; i++)
    ((jboolean *)out->a.data)[i] = 1;
  return jv_l(out);
}

/* InputDevice.MotionRange: the payload is the axis number. */
static int pad_has_axis(int axis) {
  for (int i = 0; i < dcr_pad_naxes; i++)
    if (dcr_pad_axes[i] == axis)
      return 1;
  return 0;
}
static JObj *motion_range(int axis) {
  JObj *r = jni_new("android/view/InputDevice$MotionRange");
  r->p = (void *)(uintptr_t)axis;
  return r;
}
static int mr_axis(const JObj *o) { return o ? (int)(uintptr_t)o->p : 0; }
static int mr_is_trigger(int axis) { return axis == 22 || axis == 23; /* GAS, BRAKE */ }
H(pad_getMotionRange) {
  int axis = a[0].i;
  if (strstr(m->sig, "(II)") && a[1].i != DCR_SRC_JOYSTICK)
    return jv_l(NULL);
  return jv_l(pad_has_axis(axis) ? motion_range(axis) : NULL);
}
H(mr_getAxis) { return jv_i(mr_axis(self)); }
H(mr_getSource) { return jv_i(DCR_SRC_JOYSTICK); }
H(mr_getMin) { return jv_f(mr_is_trigger(mr_axis(self)) ? 0.0f : -1.0f); }
H(mr_getMax) { return jv_f(1.0f); }
H(mr_getRange) { return jv_f(mr_is_trigger(mr_axis(self)) ? 1.0f : 2.0f); }
H(mr_getZero) { return jv_f(0.0f); } /* flat, fuzz, resolution: InControl does its own dead zones */
H(mr_isFromSource) { return jv_z((a[0].i & DCR_SRC_JOYSTICK) == a[0].i); }
H(pad_supportsSource) { return jv_z((a[0].i & DCR_PAD_SOURCES) == a[0].i); }

/* java.util.List / Iterator, just enough for getMotionRanges(). The list
 * owns one reference to each item; get()/next() hand out a local ref. */
typedef struct {
  jsize n;
  JObj *item[DCR_AXES];
} ObjList;
static void list_free(JObj *o) {
  ObjList *l = o->p;
  for (jsize i = 0; l && i < l->n; i++)
    jni_release(l->item[i]);
  free(l);
  o->p = NULL;
}
typedef struct {
  JObj *list;
  jsize pos;
} ObjIter;
static void iter_free(JObj *o) {
  ObjIter *it = o->p;
  if (it)
    jni_release(it->list);
  free(it);
  o->p = NULL;
}
static ObjList *list_of(const JObj *o) { return (o && o->p && jni_is(o, "java/util/ArrayList")) ? o->p : NULL; }
H(pad_getMotionRanges) {
  ObjList *l = calloc(1, sizeof *l);
  JObj *o = jni_new("java/util/ArrayList");
  for (int i = 0; l && i < dcr_pad_naxes; i++)
    l->item[l->n++] = motion_range(dcr_pad_axes[i]);
  o->p = l;
  o->finalize = list_free;
  return jv_l(o);
}
H(list_size) { ObjList *l = list_of(self); return jv_i(l ? l->n : 0); }
H(list_isEmpty) { ObjList *l = list_of(self); return jv_z(!l || !l->n); }
H(list_get) {
  ObjList *l = list_of(self);
  jint i = a[0].i;
  return jv_l(l && i >= 0 && i < l->n ? jni_retain(l->item[i]) : NULL);
}
H(list_iterator) {
  ObjIter *it = calloc(1, sizeof *it);
  JObj *o = jni_new("java/util/ArrayList$Itr");
  if (it)
    it->list = jni_retain(self);
  o->p = it;
  o->finalize = iter_free;
  return jv_l(o);
}
H(iter_hasNext) {
  ObjIter *it = self ? self->p : NULL;
  ObjList *l = it ? list_of(it->list) : NULL;
  return jv_z(l && it->pos < l->n);
}
H(iter_next) {
  ObjIter *it = self ? self->p : NULL;
  ObjList *l = it ? list_of(it->list) : NULL;
  if (!l || it->pos >= l->n)
    return jv_l(NULL);
  return jv_l(jni_retain(l->item[it->pos++]));
}

H(me_axisToString) {
  static const char *const names[DCR_AXES] = {
      [0] = "AXIS_X",      [1] = "AXIS_Y",      [11] = "AXIS_Z",     [14] = "AXIS_RZ",
      [15] = "AXIS_HAT_X", [16] = "AXIS_HAT_Y", [17] = "AXIS_LTRIGGER", [18] = "AXIS_RTRIGGER", [22] = "AXIS_GAS",   [23] = "AXIS_BRAKE"};
  int axis = a[0].i;
  char buf[16];
  if (axis >= 0 && axis < DCR_AXES && names[axis])
    return jv_l(jni_str(names[axis]));
  snprintf(buf, sizeof buf, "%d", axis);
  return jv_l(jni_str(buf));
}

/* MotionEvent / KeyEvent getters over the port's event structs (dcr_input.c). */
static const DcrMotion *motion(const JObj *o) { return (o && o->p) ? o->p : NULL; }
static const DcrKey *keyev(const JObj *o) { return (o && o->p) ? o->p : NULL; }
static int ptr_index(const JMethod *m, const jvalue *a) { return strchr(m->sig + 1, 'I') == m->sig + 1 ? a[0].i : 0; }

H(me_getAction) { const DcrMotion *e = motion(self); return jv_i(e ? e->action : 0); }
H(me_getActionMasked) { const DcrMotion *e = motion(self); return jv_i(e ? (e->action & 0xff) : 0); }
H(me_getActionIndex) { const DcrMotion *e = motion(self); return jv_i(e ? ((e->action >> 8) & 0xff) : 0); }
H(me_getPointerCount) { const DcrMotion *e = motion(self); return jv_i(e ? e->count : 0); }
H(me_getPointerId) {
  const DcrMotion *e = motion(self);
  int i = a[0].i;
  return jv_i(e && i >= 0 && i < e->count ? e->id[i] : 0);
}
H(me_findPointerIndex) {
  const DcrMotion *e = motion(self);
  for (int i = 0; e && i < e->count; i++)
    if (e->id[i] == a[0].i)
      return jv_i(i);
  return jv_i(-1);
}
H(me_getX) {
  const DcrMotion *e = motion(self);
  int i = ptr_index(m, a);
  return jv_f(e && i >= 0 && i < e->count ? e->x[i] : 0.0f);
}
H(me_getY) {
  const DcrMotion *e = motion(self);
  int i = ptr_index(m, a);
  return jv_f(e && i >= 0 && i < e->count ? e->y[i] : 0.0f);
}
H(me_getAxisValue) {
  const DcrMotion *e = motion(self);
  int axis = a[0].i, i = strstr(m->sig, "(II)") ? a[1].i : 0;
  if (!e || i < 0 || i >= e->count) return jv_f(0.0f);
  if (e->source == DCR_SRC_JOYSTICK)
    return jv_f(axis >= 0 && axis < DCR_AXES ? e->axis[axis] : 0.0f);
  if (axis == 0) return jv_f(e->x[i]);
  if (axis == 1) return jv_f(e->y[i]);
  if (axis == 2) return jv_f(1.0f); /* pressure */
  return jv_f(0.0f);
}
H(me_getPressure) { return jv_f(1.0f); }
H(me_getSize) { return jv_f(0.1f); }
H(me_getToolType) { return jv_i(1); /* TOOL_TYPE_FINGER */ }
H(me_getSource) { const DcrMotion *e = motion(self); return jv_i(e ? e->source : 0x1002); }
H(me_getDeviceId) { const DcrMotion *e = motion(self); return jv_i(e ? e->device_id : 0); }
H(me_getEventTime) { const DcrMotion *e = motion(self); return jv_j(e ? e->event_time_ms : 0); }
H(me_getDownTime) { const DcrMotion *e = motion(self); return jv_j(e ? e->down_time_ms : 0); }

/* MotionEvent.obtain(MotionEvent): Unity's JavaInput::Register (libunity,
 * called by nativeInjectEvent) queues a COPY of every touch event and
 * JavaInput::Process recycle()s it after ProcessTouchEvent has read it. A null
 * copy is not queued -- so until this existed no touch, and none of the pad's
 * synthesized taps and swipes, ever reached the game (hardware 2026-09-24:
 * the IAP age screen ignored every press). KeyEvents are queued uncopied. */
static void me_free(JObj *o) {
  free(o->p);
  o->p = NULL;
}
/* The copy's local reference: a JVM frees it when nativeInjectEvent returns
 * (Unity keeps only the global ref it makes); this port has no local frames,
 * so dcr_input.c drops it through jni_release_obtained() after each
 * injection. Only the main thread injects. */
#define OBTAINED_MAX 16
static JObj *g_obtained[OBTAINED_MAX];
static int g_nobtained;

void jni_release_obtained(void) {
  for (int i = 0; i < g_nobtained; i++)
    jni_release(g_obtained[i]);
  g_nobtained = 0;
}

H(me_obtain) {
  const DcrMotion *src = motion(a[0].l);
  DcrMotion *c = malloc(sizeof *c);
  if (!c)
    return jv_l(NULL);
  if (src) {
    *c = *src;
  } else {
    memset(c, 0, sizeof *c);
    c->source = 0x1002;
  }
  JObj *ev = jni_new("android/view/MotionEvent");
  ev->p = c;
  ev->finalize = me_free;
  if (g_nobtained < OBTAINED_MAX)
    g_obtained[g_nobtained++] = ev;
  return jv_l(ev);
}

H(ke_getKeyCode) { const DcrKey *k = keyev(self); return jv_i(k ? k->key_code : 0); }
H(ke_getAction) { const DcrKey *k = keyev(self); return jv_i(k ? k->action : 0); }
H(ke_getMetaState) { const DcrKey *k = keyev(self); return jv_i(k ? k->meta_state : 0); }
H(ke_getRepeatCount) { const DcrKey *k = keyev(self); return jv_i(k ? k->repeat : 0); }
H(ke_getUnicodeChar) { const DcrKey *k = keyev(self); return jv_i(k ? k->unicode : 0); }
H(ke_getSource) { const DcrKey *k = keyev(self); return jv_i(k ? k->source : 0x501); }
H(ke_getDeviceId) { const DcrKey *k = keyev(self); return jv_i(k ? k->device_id : 0); }
H(ke_getEventTime) { const DcrKey *k = keyev(self); return jv_j(k ? k->event_time_ms : 0); }
H(ie_getSource) {
  if (jni_is(self, "android/view/MotionEvent")) return me_getSource(self, a, m);
  return ke_getSource(self, a, m);
}
H(ie_getDeviceId) {
  if (jni_is(self, "android/view/MotionEvent")) return me_getDeviceId(self, a, m);
  return ke_getDeviceId(self, a, m);
}

/* =============================== FMOD output ============================== */
void dcr_audio_fmod_start(JObj *device);
void dcr_audio_fmod_stop(void);
int dcr_audio_fmod_running(void);
H(fmod_start) { dcr_audio_fmod_start(self); return jv_none(); }
H(fmod_stop) { dcr_audio_fmod_stop(); return jv_none(); }
H(fmod_isRunning) { return jv_z(dcr_audio_fmod_running()); }
H(fmod_startAudioRecord) { return jv_i(-1); /* no microphone */ }

/* =============================== the tables =============================== */
const char *const jni_class_supers[][2] = {
    {C_ACTIVITY, "android/app/Activity"},
    {"android/app/NativeActivity", "android/app/Activity"},
    {"android/app/Activity", "android/view/ContextThemeWrapper"},
    {"android/view/ContextThemeWrapper", "android/content/ContextWrapper"},
    {C_APPLICATION, "android/app/Application"},
    {"android/app/Application", "android/content/ContextWrapper"},
    {"android/content/ContextWrapper", "android/content/Context"},
    {C_PLAYER, "android/widget/FrameLayout"},
    {"com/unity3d/player/WWW", "java/lang/Thread"},
    {"android/widget/FrameLayout", "android/view/ViewGroup"},
    {"android/view/ViewGroup", "android/view/View"},
    {"android/view/SurfaceView", "android/view/View"},
    {"android/content/pm/ApplicationInfo", "android/content/pm/PackageItemInfo"},
    {"android/content/pm/ActivityInfo", "android/content/pm/ComponentInfo"},
    {"android/content/pm/ComponentInfo", "android/content/pm/PackageItemInfo"},
    {"android/view/MotionEvent", "android/view/InputEvent"},
    {"java/util/ArrayList", "java/util/List"},
    {"java/util/List", "java/util/Collection"},
    {"java/util/Collection", "java/lang/Iterable"},
    {"java/util/ArrayList$Itr", "java/util/Iterator"},
    {"android/view/KeyEvent", "android/view/InputEvent"},
    {"java/nio/DirectByteBuffer", "java/nio/ByteBuffer"},
    {"java/nio/ByteBuffer", "java/nio/Buffer"},
    {"java/lang/Integer", "java/lang/Number"},
    {"java/lang/Long", "java/lang/Number"},
    {"java/lang/Float", "java/lang/Number"},
    {"java/lang/Double", "java/lang/Number"},
    {"java/lang/Error", "java/lang/Throwable"},
    {"java/lang/Exception", "java/lang/Throwable"},
    {"java/lang/LinkageError", "java/lang/Error"},
    {"java/lang/NoClassDefFoundError", "java/lang/LinkageError"},
    {"java/lang/IncompatibleClassChangeError", "java/lang/LinkageError"},
    {"java/lang/NoSuchMethodError", "java/lang/IncompatibleClassChangeError"},
    {"java/lang/RuntimeException", "java/lang/Exception"},
    {"java/lang/ReflectiveOperationException", "java/lang/Exception"},
    {"java/lang/ClassNotFoundException", "java/lang/ReflectiveOperationException"},
    {"java/lang/IndexOutOfBoundsException", "java/lang/RuntimeException"},
    {"java/lang/StringIndexOutOfBoundsException", "java/lang/IndexOutOfBoundsException"},
    {"java/lang/ArrayIndexOutOfBoundsException", "java/lang/IndexOutOfBoundsException"},
    {"java/io/IOException", "java/lang/Exception"},
    {"java/io/FileNotFoundException", "java/io/IOException"},
    {NULL, NULL},
};

/* FALLBACK ONLY, used when <root>/classes.txt is missing (see
 * jni_class_exists in jni_core.c): with no list of the APK's classes, every
 * class is assumed present except these, which a phone does not have either
 * -- the Google VR / ARCore / Tango SDKs that Unity's own wrapper classes
 * (com/unity3d/player/GoogleVrProxy, GoogleARCoreApi, ...: part of every APK,
 * and Unity's JNI_OnLoad registers natives on them, so FatalError if absent)
 * look for. A trailing '*' matches a prefix. */
const char *const jni_missing_classes[] = {
    "com/unity3d/unitygar/*",
    "com/google/atap/*",
    "com/google/vr/*",
    "com/google/ar/*",
    NULL,
};

#define CTX "android/content/Context"
const JMethodDef jni_method_defs[] = {
    /* Context */
    {CTX, "getPackageName", NULL, ctx_getPackageName},
    {CTX, "getPackageCodePath", NULL, ctx_getPackageCodePath},
    {CTX, "getPackageResourcePath", NULL, ctx_getPackageCodePath},
    {CTX, "getApplicationInfo", NULL, ctx_getApplicationInfo},
    {CTX, "getApplicationContext", NULL, ctx_getApplicationContext},
    {CTX, "getFilesDir", NULL, ctx_getFilesDir},
    {CTX, "getCacheDir", NULL, ctx_getCacheDir},
    {CTX, "getExternalFilesDir", NULL, ctx_getExternalFilesDir},
    {CTX, "getExternalFilesDirs", NULL, ctx_getExternalFilesDirs},
    {CTX, "getExternalCacheDir", NULL, ctx_getExternalCacheDir},
    {CTX, "getObbDir", NULL, ctx_getObbDir},
    {CTX, "getObbDirs", NULL, ctx_getObbDirs},
    {CTX, "getDir", NULL, ctx_getDir},
    {CTX, "getAssets", NULL, ctx_getAssets},
    {CTX, "getResources", NULL, ctx_getResources},
    {CTX, "getContentResolver", NULL, ctx_getContentResolver},
    {CTX, "getPackageManager", NULL, ctx_getPackageManager},
    {CTX, "getMainLooper", NULL, jni_h_getMainLooper},
    {CTX, "checkCallingOrSelfPermission", NULL, ctx_checkPermission},
    {CTX, "checkSelfPermission", NULL, ctx_checkPermission},
    {CTX, "checkPermission", NULL, ctx_checkPermission},
    {CTX, "getSharedPreferences", NULL, ctx_getSharedPreferences},
    {CTX, "getSystemService", NULL, ctx_getSystemService},
    {CTX, "registerReceiver", NULL, ctx_registerReceiver},
    {CTX, "unregisterReceiver", NULL, ctx_noop},
    {CTX, "bindService", NULL, ctx_noop_false},
    {CTX, "unbindService", NULL, ctx_noop},
    {CTX, "startActivity", NULL, ctx_noop},
    /* Activity */
    {"android/app/Activity", "getPreferences", NULL, act_getPreferences},
    {"android/app/Activity", "getIntent", NULL, act_getIntent},
    {"android/app/Activity", "getWindow", NULL, act_getWindow},
    {"android/app/Activity", "getWindowManager", NULL, act_getWindowManager},
    {"android/app/Activity", "getRequestedOrientation", NULL, act_getRequestedOrientation},
    {"android/app/Activity", "setRequestedOrientation", NULL, noop},
    {"android/app/Activity", "isFinishing", NULL, act_isFinishing},
    {"android/app/Activity", "finish", NULL, act_finish},
    {"android/app/Activity", "getComponentName", NULL, act_getComponentName},
    {"android/app/Activity", "runOnUiThread", NULL, jni_h_runOnUiThread},
    /* UnityPlayer (Java methods the engine calls back) */
    {C_PLAYER, "getSplashMode", NULL, up_getSplashMode},
    {C_PLAYER, "getSettings", NULL, up_getSettings},
    {C_PLAYER, "loadLibrary", NULL, up_loadLibrary},
    {C_PLAYER, "getView", NULL, up_getView},
    {C_PLAYER, "reportError", NULL, up_reportError},
    {C_PLAYER, "quit", NULL, up_quit},
    {C_PLAYER, "showVideoPlayer", NULL, up_showVideoPlayer},
    {C_PLAYER, "kill", NULL, up_quit},
    {C_PLAYER, "isFinishing", NULL, ret_false},
    {C_PLAYER, "executeGLThreadJobs", NULL, noop},
    {C_PLAYER, "disableLogger", NULL, noop},
    {C_PLAYER, "addPhoneCallListener", NULL, noop},
    {C_PLAYER, "initializeGoogleVr", NULL, ret_false},
    {C_PLAYER, "initializeGoogleAr", NULL, ret_false},
    {C_PLAYER, "showVideoPlayer", NULL, ret_false},
    {C_PLAYER, "hideSoftInput", NULL, noop},
    /* PackageManager and the info objects */
    {"android/content/pm/PackageManager", "getPackageInfo", NULL, pm_getPackageInfo},
    {"android/content/pm/PackageManager", "getApplicationInfo", NULL, pm_getApplicationInfo},
    {"android/content/pm/PackageManager", "getActivityInfo", NULL, pm_getActivityInfo},
    {"android/content/pm/PackageManager", "getInstallerPackageName", NULL, pm_getInstallerPackageName},
    {"android/content/pm/PackageManager", "hasSystemFeature", NULL, pm_hasSystemFeature},
    {"android/content/pm/PackageManager", "checkPermission", NULL, pm_checkPermission},
    {"android/content/pm/PackageManager", "getLaunchIntentForPackage", NULL, pm_getLaunchIntentForPackage},
    {"android/content/pm/PackageManager", "resolveActivity", NULL, pm_resolveActivity},
    /* Bundle */
    {"android/os/Bundle", "getBoolean", NULL, bundle_getBoolean},
    {"android/os/Bundle", "getInt", NULL, bundle_getInt},
    {"android/os/Bundle", "getFloat", NULL, bundle_getFloat},
    {"android/os/Bundle", "getString", NULL, bundle_getString},
    {"android/os/Bundle", "containsKey", NULL, bundle_containsKey},
    {"android/os/Bundle", "get", NULL, bundle_get},
    {"android/os/Bundle", "isEmpty", NULL, bundle_isEmpty},
    {"android/os/Bundle", "size", NULL, bundle_size},
    /* display, window, resources */
    {"android/content/res/Resources", "getConfiguration", NULL, res_getConfiguration},
    {"android/content/res/Resources", "getDisplayMetrics", NULL, res_getDisplayMetrics},
    {"android/content/res/Resources", "getIdentifier", NULL, res_getIdentifier},
    {"android/view/WindowManager", "getDefaultDisplay", NULL, wm_getDefaultDisplay},
    {"android/view/Display", "getWidth", NULL, disp_getWidth},
    {"android/view/Display", "getHeight", NULL, disp_getHeight},
    {"android/view/Display", "getRotation", NULL, disp_zero},
    {"android/view/Display", "getOrientation", NULL, disp_zero},
    {"android/view/Display", "getDisplayId", NULL, disp_zero},
    {"android/view/Display", "getRefreshRate", NULL, disp_getRefreshRate},
    {"android/view/Display", "getName", NULL, disp_getName},
    {"android/view/Display", "getMetrics", NULL, noop},
    {"android/view/Display", "getRealMetrics", NULL, noop},
    {"android/hardware/display/DisplayManager", "getDisplay", NULL, dm_getDisplay},
    {"android/hardware/display/DisplayManager", "getDisplays", NULL, dm_getDisplays},
    {"android/hardware/display/DisplayManager", "registerDisplayListener", NULL, noop},
    {"android/hardware/display/DisplayManager", "unregisterDisplayListener", NULL, noop},
    {"android/view/Window", "getDecorView", NULL, win_getDecorView},
    {"android/view/Window", "getAttributes", NULL, win_getAttributes},
    {"android/view/Window", "addFlags", NULL, noop},
    {"android/view/Window", "clearFlags", NULL, noop},
    {"android/view/Window", "setFlags", NULL, noop},
    {"android/view/Window", "setAttributes", NULL, noop},
    {"android/view/Window", "requestFeature", NULL, ret_true},
    {"android/view/View", "getWidth", NULL, view_getWidth},
    {"android/view/View", "getHeight", NULL, view_getHeight},
    {"android/view/View", "getRootView", NULL, view_self},
    {"android/view/View", "getSystemUiVisibility", NULL, ret_zero},
    {"android/view/View", "setSystemUiVisibility", NULL, noop},
    {"android/view/View", "setOnSystemUiVisibilityChangeListener", NULL, noop},
    {"android/view/View", "getDisplay", NULL, wm_getDefaultDisplay},
    {"android/view/Surface", "isValid", NULL, ret_true},
    /* Locale */
    {"java/util/Locale", "getDefault", NULL, loc_getDefault},
    {"java/util/Locale", "getLanguage", NULL, loc_getLanguage},
    {"java/util/Locale", "getCountry", NULL, loc_getCountry},
    {"java/util/Locale", "toString", NULL, loc_toString},
    {"java/util/Locale", "getDisplayLanguage", NULL, loc_getDisplayLanguage},
    /* File */
    {"java/io/File", "<init>", "(Ljava/lang/String;)V", file_init_s},
    {"java/io/File", "<init>", "(Ljava/io/File;Ljava/lang/String;)V", file_init_fs},
    {"java/io/File", "<init>", "(Ljava/lang/String;Ljava/lang/String;)V", file_init_fs},
    {"java/io/File", "getPath", NULL, file_getPath},
    {"java/io/File", "getAbsolutePath", NULL, file_getPath},
    {"java/io/File", "getCanonicalPath", NULL, file_getPath},
    {"java/io/File", "toString", NULL, file_getPath},
    {"java/io/File", "getName", NULL, file_getName},
    {"java/io/File", "getParent", NULL, file_getParent},
    {"java/io/File", "getParentFile", NULL, file_getParent},
    {"java/io/File", "exists", NULL, file_exists},
    {"java/io/File", "isDirectory", NULL, file_isDirectory},
    {"java/io/File", "isFile", NULL, file_isFile},
    {"java/io/File", "length", NULL, file_length},
    {"java/io/File", "mkdir", NULL, file_mkdirs},
    {"java/io/File", "mkdirs", NULL, file_mkdirs},
    {"java/io/File", "canRead", NULL, file_true},
    {"java/io/File", "canWrite", NULL, file_true},
    {"java/io/File", "delete", NULL, file_delete},
    {"java/io/File", "getFreeSpace", NULL, file_getFreeSpace},
    {"java/io/File", "getUsableSpace", NULL, file_getFreeSpace},
    /* Environment */
    {"android/os/Environment", "getExternalStorageState", NULL, env_getExternalStorageState},
    {"android/os/Environment", "getExternalStorageDirectory", NULL, env_getExternalStorageDirectory},
    {"android/os/Environment", "getDataDirectory", NULL, env_getDataDirectory},
    {"android/os/Environment", "isExternalStorageEmulated", NULL, env_true},
    {"android/os/Environment", "isExternalStorageRemovable", NULL, env_false},
    /* SharedPreferences (PlayerPrefs) */
    {"android/content/SharedPreferences", "getString", NULL, sp_getString},
    {"android/content/SharedPreferences", "getInt", NULL, sp_getInt},
    {"android/content/SharedPreferences", "getLong", NULL, sp_getLong},
    {"android/content/SharedPreferences", "getFloat", NULL, sp_getFloat},
    {"android/content/SharedPreferences", "getBoolean", NULL, sp_getBoolean},
    {"android/content/SharedPreferences", "contains", NULL, sp_contains},
    {"android/content/SharedPreferences", "edit", NULL, sp_edit},
    {"android/content/SharedPreferences$Editor", "putString", NULL, ed_putString},
    {"android/content/SharedPreferences$Editor", "putInt", NULL, ed_putInt},
    {"android/content/SharedPreferences$Editor", "putLong", NULL, ed_putLong},
    {"android/content/SharedPreferences$Editor", "putFloat", NULL, ed_putFloat},
    {"android/content/SharedPreferences$Editor", "putBoolean", NULL, ed_putBoolean},
    {"android/content/SharedPreferences$Editor", "remove", NULL, ed_remove},
    {"android/content/SharedPreferences$Editor", "clear", NULL, ed_clear},
    {"android/content/SharedPreferences$Editor", "commit", NULL, ed_commit},
    {"android/content/SharedPreferences$Editor", "apply", NULL, ed_commit},
    /* System, clocks, threads, settings */
    {"java/lang/System", "currentTimeMillis", NULL, sys_currentTimeMillis},
    {"java/lang/System", "nanoTime", NULL, sys_nanoTime},
    {"java/lang/System", "getProperty", NULL, sys_getProperty},
    {"java/lang/System", "identityHashCode", NULL, sys_identityHashCode},
    {"java/lang/System", "loadLibrary", NULL, noop},
    {"java/lang/System", "gc", NULL, noop},
    {"android/os/SystemClock", "uptimeMillis", NULL, clock_uptimeMillis},
    {"android/os/SystemClock", "elapsedRealtime", NULL, clock_uptimeMillis},
    {"java/lang/Thread", "currentThread", NULL, thread_current},
    {"java/lang/Thread", "getName", NULL, thread_getName},
    {"java/lang/Thread", "getId", NULL, thread_getId},
    {"java/lang/Thread", "setName", NULL, noop},
    {"java/lang/Thread", "setPriority", NULL, noop},
    {"android/provider/Settings$Secure", "getString", NULL, secure_getString},
    {"android/provider/Settings$System", "getInt", NULL, system_getInt},
    /* Object / Class / String */
    {"java/lang/Object", "getClass", NULL, obj_getClass},
    {"java/lang/Object", "hashCode", NULL, obj_hashCode},
    {"java/lang/Object", "equals", NULL, obj_equals},
    {"java/lang/Object", "toString", NULL, obj_toString},
    {"java/lang/Class", "getName", NULL, class_getName},
    {"java/lang/Class", "getSimpleName", NULL, class_getName},
    {"java/lang/Class", "getClassLoader", NULL, class_getClassLoader},
    {"java/lang/Class", "forName", NULL, class_forName},
    {"java/lang/ClassLoader", "loadClass", NULL, class_forName},
    {"java/lang/String", "getBytes", NULL, str_getBytes},
    {"java/lang/String", "<init>", "([BLjava/lang/String;)V", str_init_bytes},
    {"java/lang/String", "<init>", "([B)V", str_init_bytes},
    {"java/lang/String", "<init>", "()V", str_init_empty}, /* Unity's JoystickInfo: new String() */
    {"java/lang/String", "length", NULL, str_length},
    {"java/lang/String", "equals", NULL, str_equals},
    {"java/lang/String", "hashCode", NULL, str_hashCode},
    {"java/lang/String", "toString", NULL, obj_toString},
    {"java/lang/Number", "intValue", NULL, box_intValue},
    {"java/lang/Number", "longValue", NULL, box_longValue},
    {"java/lang/Number", "floatValue", NULL, box_floatValue},
    {"java/lang/Boolean", "booleanValue", NULL, box_booleanValue},
    {"java/lang/Integer", "valueOf", "(I)Ljava/lang/Integer;", int_valueOf},
    {"java/lang/Boolean", "valueOf", "(Z)Ljava/lang/Boolean;", bool_valueOf},
    {"java/lang/Long", "valueOf", "(J)Ljava/lang/Long;", long_valueOf},
    {"java/lang/Float", "valueOf", "(F)Ljava/lang/Float;", float_valueOf},
    {"java/lang/Throwable", "getMessage", NULL, thr_getMessage},
    {"java/lang/Throwable", "getLocalizedMessage", NULL, thr_getMessage},
    {"java/lang/Throwable", "toString", NULL, obj_toString},
    {"java/lang/Throwable", "getStackTrace", NULL, thr_getStackTrace},
    {"java/lang/Throwable", "printStackTrace", NULL, noop},
    /* Intent */
    {"android/content/Intent", "getIntExtra", NULL, intent_getIntExtra},
    {"android/content/Intent", "getAction", NULL, intent_getAction},
    {"android/content/Intent", "getStringExtra", NULL, ret_null},
    {"android/content/Intent", "getExtras", NULL, ret_null},
    {"android/content/Intent", "getData", NULL, ret_null},
    /* audio / network / vibration */
    {"android/media/AudioManager", "getProperty", NULL, am_getProperty},
    {"android/media/AudioManager", "getStreamVolume", NULL, am_getStreamVolume},
    {"android/media/AudioManager", "getStreamMaxVolume", NULL, am_getStreamMaxVolume},
    {"android/media/AudioManager", "requestAudioFocus", NULL, am_requestAudioFocus},
    {"android/media/AudioManager", "abandonAudioFocus", NULL, am_requestAudioFocus},
    {"android/media/AudioManager", "getRingerMode", NULL, am_getRingerMode},
    {"android/media/AudioManager", "isMusicActive", NULL, ret_false},
    {"android/media/AudioManager", "isBluetoothA2dpOn", NULL, ret_false},
    {"android/media/AudioManager", "isWiredHeadsetOn", NULL, ret_false},
    {"android/net/ConnectivityManager", "getActiveNetworkInfo", NULL, ret_null},
    {"android/os/Vibrator", "hasVibrator", NULL, ret_false},
    /* input devices and events */
    {"android/view/InputDevice", "getDeviceIds", NULL, id_getDeviceIds},
    {"android/view/InputDevice", "getDevice", NULL, id_getDevice},
    {"android/view/InputDevice", "getName", NULL, pad_getName},
    {"android/view/InputDevice", "getDescriptor", NULL, pad_getDescriptor},
    {"android/view/InputDevice", "getId", NULL, pad_getId},
    {"android/view/InputDevice", "getSources", NULL, pad_getSources},
    {"android/view/InputDevice", "getVendorId", NULL, pad_getVendorId},
    {"android/view/InputDevice", "getProductId", NULL, pad_getProductId},
    {"android/view/InputDevice", "getControllerNumber", NULL, pad_getControllerNumber},
    {"android/view/InputDevice", "getKeyboardType", NULL, pad_getKeyboardType},
    {"android/view/InputDevice", "isVirtual", NULL, pad_false},
    {"android/view/InputDevice", "isEnabled", NULL, pad_true},
    {"android/view/InputDevice", "isExternal", NULL, pad_true},
    {"android/view/InputDevice", "supportsSource", NULL, pad_supportsSource},
    {"android/view/InputDevice", "hasKeys", NULL, pad_hasKeys},
    {"android/view/InputDevice", "getMotionRange", NULL, pad_getMotionRange},
    {"android/view/InputDevice", "getMotionRanges", NULL, pad_getMotionRanges},
    {"android/view/InputDevice$MotionRange", "getAxis", NULL, mr_getAxis},
    {"android/view/InputDevice$MotionRange", "getSource", NULL, mr_getSource},
    {"android/view/InputDevice$MotionRange", "getMin", NULL, mr_getMin},
    {"android/view/InputDevice$MotionRange", "getMax", NULL, mr_getMax},
    {"android/view/InputDevice$MotionRange", "getRange", NULL, mr_getRange},
    {"android/view/InputDevice$MotionRange", "getFlat", NULL, mr_getZero},
    {"android/view/InputDevice$MotionRange", "getFuzz", NULL, mr_getZero},
    {"android/view/InputDevice$MotionRange", "getResolution", NULL, mr_getZero},
    {"android/view/InputDevice$MotionRange", "isFromSource", NULL, mr_isFromSource},
    {"java/util/List", "size", NULL, list_size},
    {"java/util/List", "isEmpty", NULL, list_isEmpty},
    {"java/util/List", "get", NULL, list_get},
    {"java/util/List", "iterator", NULL, list_iterator},
    {"java/util/Collection", "size", NULL, list_size},
    {"java/util/Collection", "iterator", NULL, list_iterator},
    {"java/lang/Iterable", "iterator", NULL, list_iterator},
    {"java/util/Iterator", "hasNext", NULL, iter_hasNext},
    {"java/util/Iterator", "next", NULL, iter_next},
    {"android/view/MotionEvent", "axisToString", NULL, me_axisToString},
    {"android/hardware/input/InputManager", "getInputDeviceIds", NULL, id_getDeviceIds},
    {"android/hardware/input/InputManager", "getInputDevice", NULL, id_getDevice},
    {"android/hardware/input/InputManager", "registerInputDeviceListener", NULL, noop},
    {"android/hardware/input/InputManager", "unregisterInputDeviceListener", NULL, noop},
    {"android/view/InputEvent", "getSource", NULL, ie_getSource},
    {"android/view/InputEvent", "getDeviceId", NULL, ie_getDeviceId},
    {"android/view/MotionEvent", "getAction", NULL, me_getAction},
    {"android/view/MotionEvent", "getActionMasked", NULL, me_getActionMasked},
    {"android/view/MotionEvent", "getActionIndex", NULL, me_getActionIndex},
    {"android/view/MotionEvent", "getPointerCount", NULL, me_getPointerCount},
    {"android/view/MotionEvent", "getPointerId", NULL, me_getPointerId},
    {"android/view/MotionEvent", "findPointerIndex", NULL, me_findPointerIndex},
    {"android/view/MotionEvent", "getX", NULL, me_getX},
    {"android/view/MotionEvent", "getY", NULL, me_getY},
    {"android/view/MotionEvent", "getRawX", NULL, me_getX},
    {"android/view/MotionEvent", "getRawY", NULL, me_getY},
    {"android/view/MotionEvent", "getAxisValue", NULL, me_getAxisValue},
    {"android/view/MotionEvent", "getPressure", NULL, me_getPressure},
    {"android/view/MotionEvent", "getSize", NULL, me_getSize},
    {"android/view/MotionEvent", "getToolType", NULL, me_getToolType},
    {"android/view/MotionEvent", "getEventTime", NULL, me_getEventTime},
    {"android/view/MotionEvent", "getDownTime", NULL, me_getDownTime},
    {"android/view/MotionEvent", "getHistorySize", NULL, ret_zero},
    {"android/view/MotionEvent", "getMetaState", NULL, ret_zero},
    {"android/view/MotionEvent", "getButtonState", NULL, ret_zero},
    {"android/view/MotionEvent", "getFlags", NULL, ret_zero},
    {"android/view/MotionEvent", "obtain", "(Landroid/view/MotionEvent;)Landroid/view/MotionEvent;", me_obtain},
    {"android/view/MotionEvent", "recycle", NULL, ret_zero}, /* freed with its last reference */
    {"android/view/KeyEvent", "getKeyCode", NULL, ke_getKeyCode},
    {"android/view/KeyEvent", "getAction", NULL, ke_getAction},
    {"android/view/KeyEvent", "getMetaState", NULL, ke_getMetaState},
    {"android/view/KeyEvent", "getRepeatCount", NULL, ke_getRepeatCount},
    {"android/view/KeyEvent", "getUnicodeChar", NULL, ke_getUnicodeChar},
    {"android/view/KeyEvent", "getEventTime", NULL, ke_getEventTime},
    {"android/view/KeyEvent", "getScanCode", NULL, ret_zero},
    {"android/view/KeyEvent", "getFlags", NULL, ret_zero},
    {"android/view/KeyEvent", "getCharacters", NULL, ret_null},
    /* loopers and handlers: nothing Java-side to run */
    {"android/os/Looper", "getMainLooper", NULL, jni_h_getMainLooper},
    {"android/os/Looper", "myLooper", NULL, jni_h_myLooper},
    {"android/os/Looper", "prepare", NULL, noop},
    {"android/os/Handler", "<init>", NULL, jni_h_handler_init},
    {"android/os/Handler", "post", "(Ljava/lang/Runnable;)Z", jni_h_handler_post},
    {"android/os/Handler", "postAtFrontOfQueue", "(Ljava/lang/Runnable;)Z", jni_h_handler_post},
    {"android/os/Handler", "postDelayed", NULL, jni_h_handler_postDelayed},
    {"android/os/Handler", "removeCallbacks", NULL, jni_h_handler_removeCallbacks},
    {"android/os/Handler", "getLooper", NULL, jni_h_handler_getLooper},
    /* Unity's JNIBridge (in the APK) and java.lang.reflect: see jni_proxy.c */
    {"bitter/jnibridge/JNIBridge", "newInterfaceProxy", NULL, jni_h_newInterfaceProxy},
    {"bitter/jnibridge/JNIBridge", "disableInterfaceProxy", NULL, jni_h_disableInterfaceProxy},
    {"java/lang/reflect/Method", "getName", NULL, jni_h_method_getName},
    {"java/lang/reflect/Method", "getDeclaringClass", NULL, jni_h_method_getDeclaringClass},
    {"java/lang/reflect/Method", "getReturnType", NULL, jni_h_method_getReturnType},
    {"java/lang/reflect/Method", "getParameterTypes", NULL, jni_h_method_getParameterTypes},
    {"java/lang/reflect/Method", "getModifiers", NULL, jni_h_method_getModifiers},
    {"java/lang/reflect/Method", "equals", NULL, jni_h_method_equals},
    {"java/lang/reflect/Method", "hashCode", NULL, jni_h_method_hashCode},
    {"java/lang/reflect/Method", "toString", NULL, jni_h_method_toString},
    {"java/lang/ClassLoader", "findLibrary", NULL, jni_h_findLibrary},
    {"com/unity3d/player/ReflectionHelper", "getMethodID", NULL, jni_h_rh_getMethodID},
    {"com/unity3d/player/ReflectionHelper", "getFieldID", NULL, jni_h_rh_getFieldID},
    {"com/unity3d/player/ReflectionHelper", "getConstructorID", NULL, jni_h_rh_getConstructorID},
    {"com/unity3d/player/ReflectionHelper", "newProxyInstance", NULL, jni_h_rh_newProxyInstance},
    {"android/net/Uri", "decode", NULL, uri_decode},
    /* Unity 5.6's Java WWW (jni_www.c): every WWW request, jar:file:// too */
    {"com/unity3d/player/WWW", "<init>", NULL, jni_h_www_init},
    {"com/unity3d/player/WWW", "start", "()V", jni_h_www_start},
    {"com/unity3d/player/WWW", "isAlive", "()Z", jni_h_www_isAlive},
    {"com/unity3d/player/WWW", "join", NULL, jni_h_www_join},
    {"com/unity3d/player/UnityWebRequest", "<init>", NULL, uwr_init},
    {"com/unity3d/player/UnityWebRequest", "run", NULL, uwr_run},
    {"android/app/UiModeManager", "getCurrentModeType", NULL, uimode_getCurrentModeType},
    {"android/net/Uri", "encode", NULL, uri_encode},
    /* FMOD's Java output device (dcr_audio.c runs its loop natively) */
    {"org/fmod/FMODAudioDevice", "start", NULL, fmod_start},
    {"org/fmod/FMODAudioDevice", "stop", NULL, fmod_stop},
    {"org/fmod/FMODAudioDevice", "close", NULL, fmod_stop},
    {"org/fmod/FMODAudioDevice", "isRunning", NULL, fmod_isRunning},
    {"org/fmod/FMODAudioDevice", "startAudioRecord", NULL, fmod_startAudioRecord},
    {"org/fmod/FMODAudioDevice", "stopAudioRecord", NULL, noop},
    {NULL, NULL, NULL, NULL},
};

const JFieldDef jni_field_defs[] = {
    {C_PLAYER, "currentActivity", up_currentActivity},
    {"android/os/Build", "MODEL", build_str_field},
    {"android/os/Build", "MANUFACTURER", build_str_field},
    {"android/os/Build", "BRAND", build_str_field},
    {"android/os/Build", "DEVICE", build_str_field},
    {"android/os/Build", "PRODUCT", build_str_field},
    {"android/os/Build", "BOARD", build_str_field},
    {"android/os/Build", "HARDWARE", build_str_field},
    {"android/os/Build", "FINGERPRINT", build_str_field},
    {"android/os/Build", "ID", build_str_field},
    {"android/os/Build", "DISPLAY", build_str_field},
    {"android/os/Build", "TAGS", build_str_field},
    {"android/os/Build", "TYPE", build_str_field},
    {"android/os/Build", "USER", build_str_field},
    {"android/os/Build", "HOST", build_str_field},
    {"android/os/Build", "BOOTLOADER", build_str_field},
    {"android/os/Build", "RADIO", build_str_field},
    {"android/os/Build", "SERIAL", build_str_field},
    {"android/os/Build", "CPU_ABI", build_str_field},
    {"android/os/Build", "CPU_ABI2", build_str_field},
    {"android/os/Build", "SUPPORTED_ABIS", build_str_field},
    {"android/os/Build$VERSION", "SDK_INT", bv_SDK_INT},
    {"android/os/Build$VERSION", "RELEASE", bv_RELEASE},
    {"android/os/Build$VERSION", "INCREMENTAL", bv_INCREMENTAL},
    {"android/os/Build$VERSION", "CODENAME", bv_CODENAME},
    {"android/os/Build$VERSION", "SDK", bv_SDK},
    {"android/util/DisplayMetrics", "widthPixels", dm_widthPixels},
    {"android/util/DisplayMetrics", "heightPixels", dm_heightPixels},
    {"android/util/DisplayMetrics", "density", dm_density},
    {"android/util/DisplayMetrics", "scaledDensity", dm_density},
    {"android/util/DisplayMetrics", "densityDpi", dm_densityDpi},
    {"android/util/DisplayMetrics", "xdpi", dm_xdpi},
    {"android/util/DisplayMetrics", "ydpi", dm_xdpi},
    {"android/content/res/Configuration", "orientation", cfg_orientation},
    {"android/content/res/Configuration", "screenLayout", cfg_screenLayout},
    {"android/content/res/Configuration", "uiMode", cfg_uiMode},
    {"android/content/res/Configuration", "keyboard", cfg_keyboard},
    {"android/content/res/Configuration", "keyboardHidden", cfg_hidden_yes},
    {"android/content/res/Configuration", "hardKeyboardHidden", cfg_hidden_yes},
    {"android/content/res/Configuration", "navigation", cfg_navigation},
    {"android/content/res/Configuration", "navigationHidden", cfg_hidden_yes},
    {"android/content/res/Configuration", "touchscreen", cfg_touchscreen},
    {"android/content/res/Configuration", "densityDpi", cfg_densityDpi},
    {"android/content/res/Configuration", "screenWidthDp", cfg_screenWidthDp},
    {"android/content/res/Configuration", "screenHeightDp", cfg_screenHeightDp},
    {"android/content/res/Configuration", "smallestScreenWidthDp", cfg_screenHeightDp},
    {"android/content/res/Configuration", "fontScale", cfg_fontScale},
    {"android/content/res/Configuration", "locale", cfg_locale},
    {"android/content/res/Configuration", "mcc", zero_int},
    {"android/content/res/Configuration", "mnc", zero_int},
    {"android/content/pm/ApplicationInfo", "sourceDir", ai_sourceDir},
    {"android/content/pm/ApplicationInfo", "publicSourceDir", ai_sourceDir},
    {"android/content/pm/ApplicationInfo", "nativeLibraryDir", ai_nativeLibraryDir},
    {"android/content/pm/ApplicationInfo", "dataDir", ai_dataDir},
    {"android/content/pm/ApplicationInfo", "flags", ai_flags},
    {"android/content/pm/ApplicationInfo", "targetSdkVersion", ai_targetSdk},
    {"android/content/pm/ApplicationInfo", "minSdkVersion", ai_minSdk},
    {"android/content/pm/ApplicationInfo", "uid", ai_uid},
    {"android/content/pm/PackageItemInfo", "packageName", ai_packageName},
    {"android/content/pm/PackageItemInfo", "metaData", ai_metaData},
    {"android/content/pm/PackageItemInfo", "name", ai_name},
    {"android/content/pm/ActivityInfo", "screenOrientation", ai_screenOrientation},
    {"android/content/pm/ActivityInfo", "configChanges", ai_configChanges},
    {"android/content/pm/PackageInfo", "versionName", pi_versionName},
    {"android/content/pm/PackageInfo", "versionCode", pi_versionCode},
    {"android/content/pm/PackageInfo", "packageName", ai_packageName},
    {"android/content/pm/PackageInfo", "firstInstallTime", pi_installTime},
    {"android/content/pm/PackageInfo", "lastUpdateTime", pi_installTime},
    {"android/content/pm/PackageInfo", "applicationInfo", pi_applicationInfo},
    /* ---- Android SDK constants (static final): the platform's real values.
     * Unhandled, they read as 0 / null -- which for SCREEN_ORIENTATION_* made
     * every orientation LANDSCAPE, and for *_SERVICE made getSystemService
     * miss services that exist. */
#define KI(c, n, v) {c, n, NULL, (v), NULL}
#define KS(c, n, v) {c, n, NULL, 0, (v)}
    /* OpenIAB (the SEA build's store plugin): C# maps every SKU per store name
     * read from these; a null name throws ArgumentNullException("key") and
     * the game's ConfigurationController boot step fails. Values from the APK. */
    /* Space Hulk: Assembly-CSharp starts this Activity (the Android 6
     * storage-permission prompt) and waits, every frame, for its static flag;
     * no Activity runs here, so the permission reads as granted at once. */
    KI("com/plunge/requestpermission/RequestUserPermission", "PermissionGranted", 1),
    KS("org/onepf/oms/OpenIabHelper", "NAME_GOOGLE", "com.google.play"),
    KS("org/onepf/oms/OpenIabHelper", "NAME_AMAZON", "com.amazon.apps"),
    KS("org/onepf/oms/OpenIabHelper", "NAME_SAMSUNG", "com.samsung.apps"),
    KS("org/onepf/oms/OpenIabHelper", "NAME_NOKIA", "com.nokia.nstore"),
    KS("org/onepf/oms/OpenIabHelper", "NAME_SKUBIT", "com.skubit.android"),
    KS("org/onepf/oms/OpenIabHelper", "NAME_SKUBIT_TEST", "net.skubit.android"),
    KS("org/onepf/oms/OpenIabHelper", "NAME_YANDEX", "com.yandex.store"),
    KS("org/onepf/oms/OpenIabHelper", "NAME_APPLAND", "Appland"),
    KS("org/onepf/oms/OpenIabHelper", "NAME_SLIDEME", "SlideME"),
    KS("org/onepf/oms/OpenIabHelper", "NAME_APTOIDE", "cm.aptoide.pt"),
    KS(CTX, "WINDOW_SERVICE", "window"), KS(CTX, "AUDIO_SERVICE", "audio"),
    KS(CTX, "DISPLAY_SERVICE", "display"), KS(CTX, "INPUT_SERVICE", "input"),
    KS(CTX, "LOCATION_SERVICE", "location"), KS(CTX, "MEDIA_ROUTER_SERVICE", "media_router"),
    KS(CTX, "CONNECTIVITY_SERVICE", "connectivity"), KS(CTX, "TELEPHONY_SERVICE", "phone"),
    KS(CTX, "ACTIVITY_SERVICE", "activity"), KS(CTX, "POWER_SERVICE", "power"),
    KS(CTX, "SENSOR_SERVICE", "sensor"), KS(CTX, "INPUT_METHOD_SERVICE", "input_method"),
    KS(CTX, "UI_MODE_SERVICE", "uimode"), KS(CTX, "BATTERY_SERVICE", "batterymanager"),
    KS(CTX, "VIBRATOR_SERVICE", "vibrator"), KS(CTX, "WIFI_SERVICE", "wifi"),
    KS(CTX, "CLIPBOARD_SERVICE", "clipboard"), KS(CTX, "NOTIFICATION_SERVICE", "notification"),
    KS(CTX, "ALARM_SERVICE", "alarm"), KS(CTX, "KEYGUARD_SERVICE", "keyguard"),
    KS(CTX, "STORAGE_SERVICE", "storage"), KS(CTX, "ACCOUNT_SERVICE", "account"),
    KI(CTX, "MODE_PRIVATE", 0), KI(CTX, "MODE_WORLD_READABLE", 1), KI(CTX, "MODE_WORLD_WRITEABLE", 2),
    KI(CTX, "MODE_MULTI_PROCESS", 4), KI(CTX, "MODE_APPEND", 0x8000),
    KI(CTX, "BIND_AUTO_CREATE", 1),
    KI("android/content/pm/PackageManager", "GET_ACTIVITIES", 0x1),
    KI("android/content/pm/PackageManager", "GET_RECEIVERS", 0x2),
    KI("android/content/pm/PackageManager", "GET_SERVICES", 0x4),
    KI("android/content/pm/PackageManager", "GET_PROVIDERS", 0x8),
    KI("android/content/pm/PackageManager", "GET_SIGNATURES", 0x40),
    KI("android/content/pm/PackageManager", "GET_META_DATA", 0x80),
    KI("android/content/pm/PackageManager", "GET_PERMISSIONS", 0x1000),
    KI("android/content/pm/PackageManager", "PERMISSION_GRANTED", 0),
    KI("android/content/pm/PackageManager", "PERMISSION_DENIED", -1),
    KS("android/content/pm/PackageManager", "FEATURE_AUDIO_LOW_LATENCY", "android.hardware.audio.low_latency"),
    KS("android/content/pm/PackageManager", "FEATURE_TOUCHSCREEN", "android.hardware.touchscreen"),
    KS("android/content/pm/PackageManager", "FEATURE_TELEVISION", "android.hardware.type.television"),
    KS("android/content/pm/PackageManager", "FEATURE_LEANBACK", "android.software.leanback"),
#define SO(n, v) KI("android/content/pm/ActivityInfo", "SCREEN_ORIENTATION_" n, v)
    SO("UNSPECIFIED", -1), SO("LANDSCAPE", 0), SO("PORTRAIT", 1), SO("USER", 2), SO("BEHIND", 3),
    SO("SENSOR", 4), SO("NOSENSOR", 5), SO("SENSOR_LANDSCAPE", 6), SO("SENSOR_PORTRAIT", 7),
    SO("REVERSE_LANDSCAPE", 8), SO("REVERSE_PORTRAIT", 9), SO("FULL_SENSOR", 10),
    SO("USER_LANDSCAPE", 11), SO("USER_PORTRAIT", 12), SO("FULL_USER", 13), SO("LOCKED", 14),
#undef SO
    KI("android/content/pm/ActivityInfo", "CONFIG_ORIENTATION", 0x80),
    KI("android/content/pm/ActivityInfo", "CONFIG_SCREEN_SIZE", 0x400),
    KI("android/content/res/Configuration", "ORIENTATION_UNDEFINED", 0),
    KI("android/content/res/Configuration", "ORIENTATION_PORTRAIT", 1),
    KI("android/content/res/Configuration", "ORIENTATION_LANDSCAPE", 2),
    KI("android/content/res/Configuration", "SCREENLAYOUT_SIZE_MASK", 0xf),
    KI("android/content/res/Configuration", "SCREENLAYOUT_SIZE_NORMAL", 2),
    KI("android/content/res/Configuration", "SCREENLAYOUT_SIZE_LARGE", 3),
    KI("android/content/res/Configuration", "UI_MODE_TYPE_MASK", 0xf),
    KI("android/content/res/Configuration", "UI_MODE_TYPE_NORMAL", 1),
    KI("android/content/res/Configuration", "UI_MODE_TYPE_TELEVISION", 4),
    KI("android/view/WindowManager$LayoutParams", "FLAG_NOT_FOCUSABLE", 0x8),
    KI("android/view/WindowManager$LayoutParams", "FLAG_KEEP_SCREEN_ON", 0x80),
    KI("android/view/WindowManager$LayoutParams", "FLAG_LAYOUT_NO_LIMITS", 0x200),
    KI("android/view/WindowManager$LayoutParams", "FLAG_FULLSCREEN", 0x400),
    KI("android/view/WindowManager$LayoutParams", "FLAG_FORCE_NOT_FULLSCREEN", 0x800),
    KI("android/view/WindowManager$LayoutParams", "FLAG_HARDWARE_ACCELERATED", 0x1000000),
    KI("android/view/WindowManager$LayoutParams", "SOFT_INPUT_ADJUST_RESIZE", 0x10),
    KI("android/view/WindowManager$LayoutParams", "SOFT_INPUT_ADJUST_PAN", 0x20),
#define SU(n, v) KI("android/view/View", "SYSTEM_UI_FLAG_" n, v)
    SU("VISIBLE", 0), SU("LOW_PROFILE", 0x1), SU("HIDE_NAVIGATION", 0x2), SU("FULLSCREEN", 0x4),
    SU("LAYOUT_STABLE", 0x100), SU("LAYOUT_HIDE_NAVIGATION", 0x200), SU("LAYOUT_FULLSCREEN", 0x400),
    SU("IMMERSIVE", 0x800), SU("IMMERSIVE_STICKY", 0x1000),
#undef SU
    KI("android/view/View", "VISIBLE", 0), KI("android/view/View", "INVISIBLE", 4),
    KI("android/view/View", "GONE", 8),
    KI("android/view/Surface", "ROTATION_0", 0), KI("android/view/Surface", "ROTATION_90", 1),
    KI("android/view/Surface", "ROTATION_180", 2), KI("android/view/Surface", "ROTATION_270", 3),
    KI("android/media/MediaRouter", "ROUTE_TYPE_LIVE_AUDIO", 1),
    KI("android/media/MediaRouter", "ROUTE_TYPE_LIVE_VIDEO", 2),
    KI("android/media/MediaRouter", "ROUTE_TYPE_USER", 0x800000),
    KI("android/media/AudioManager", "STREAM_MUSIC", 3),
    KI("android/media/AudioManager", "AUDIOFOCUS_GAIN", 1),
    KI("android/media/AudioManager", "AUDIOFOCUS_LOSS", -1),
    KI("android/media/AudioManager", "AUDIOFOCUS_LOSS_TRANSIENT", -2),
    KI("android/media/AudioManager", "AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK", -3),
    KI("android/media/AudioManager", "AUDIOFOCUS_REQUEST_FAILED", 0),
    KI("android/media/AudioManager", "AUDIOFOCUS_REQUEST_GRANTED", 1),
    KI("android/media/AudioManager", "RINGER_MODE_SILENT", 0),
    KI("android/media/AudioManager", "RINGER_MODE_VIBRATE", 1),
    KI("android/media/AudioManager", "RINGER_MODE_NORMAL", 2),
    KI("android/net/ConnectivityManager", "TYPE_MOBILE", 0),
    KI("android/net/ConnectivityManager", "TYPE_WIFI", 1),
    KI("android/net/ConnectivityManager", "TYPE_ETHERNET", 9),
    KI("android/util/DisplayMetrics", "DENSITY_DEFAULT", 160),
    KS("android/content/Intent", "ACTION_MAIN", "android.intent.action.MAIN"),
    KS("android/content/Intent", "ACTION_VIEW", "android.intent.action.VIEW"),
    KS("android/content/Intent", "ACTION_SEND", "android.intent.action.SEND"),
    KS("android/content/Intent", "CATEGORY_LAUNCHER", "android.intent.category.LAUNCHER"),
    KS("android/content/Intent", "EXTRA_TEXT", "android.intent.extra.TEXT"),
    KI("android/content/Intent", "FLAG_ACTIVITY_NEW_TASK", 0x10000000),
    KI("android/content/Intent", "FLAG_ACTIVITY_CLEAR_TOP", 0x4000000),
    /* read by Unity's JavaInput::Register / ProcessTouchEvent */
    KI("android/view/MotionEvent", "FLAG_WINDOW_IS_OBSCURED", 0x1),
    KI("android/view/KeyEvent", "KEYCODE_BACK", 4),
    KI("android/view/KeyEvent", "KEYCODE_VOLUME_UP", 24),
    KI("android/view/KeyEvent", "KEYCODE_VOLUME_DOWN", 25),
    KI("android/view/KeyEvent", "KEYCODE_ZOOM_IN", 168),
    KI("android/view/KeyEvent", "KEYCODE_ZOOM_OUT", 169),
    KS("android/os/Environment", "MEDIA_MOUNTED", "mounted"),
#undef KI
#undef KS
    {NULL, NULL, NULL},
};
