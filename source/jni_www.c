/* jni_www.c -- com.unity3d.player.WWW, the Java side of Unity 5.6's WWW.
 *
 * Unity 5.6 on Android hands EVERY WWW request to Java (AndroidWWW::initJava
 * registers the natives; one WWW object, a java.lang.Thread, per request):
 *
 *   new WWW(int id, String url, byte[] post, Map headers); start();
 *   ... isAlive() / join() from the engine
 *
 * and WWW.runSafe() answers through five natives, in this order:
 *
 *   headerCallback(id, "content-length: N\n\r")   (true = abort)
 *   headerCallback(id, "content-type: T\n\r")     (true = abort)
 *   readCallback(id, buf, 0), then readCallback(id, buf, n) per read of at
 *     most min(N, 32 KB) bytes                    (true = abort)
 *   progressCallback(id, 1.0f, got/N, secondsLeft, N) after each of those,
 *     and once more at the end with got = N
 *   doneCallback(id)       -- or errorCallback(id, message) instead
 *
 * That includes jar:file:// URLs: this game's AssetBundleManager loads its
 * bundles from Application.streamingAssetsPath = "jar:file:///data/app/
 * <pkg>-1/base.apk!/assets", which Java reads with a JarURLConnection. Here:
 *
 *   jar:file://<apk>!/<entry>  the entry, out of game.apk (stored entries are
 *                              streamed from their offset, which is what
 *                              dcr_setup.c's uncompressed APK makes them;
 *                              a compressed one is inflated whole)
 *   file://<path>              the file (dcr_path.c maps the Android path)
 *   http:// https:// ...       offline: UnknownHostException, as a phone
 *                              without a network reports it
 *
 * Unity 2017.4 (the world-wide release) does jar:/file: natively and has no
 * Java WWW; this file is what 5.6 needs on top. MIT.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <switch.h>

#include <miniz/miniz.h>

#include "bionic.h"
#include "bionic_pthread.h"
#include "dcr_path.h"
#include "dcr_jni_unity.h"
#include "util.h"

int b_pthread_create(b_pthread_t *out, const b_pthread_attr_t *attr, void *(*start)(void *), void *arg);
int b_pthread_attr_init(b_pthread_attr_t *a);
int b_pthread_attr_setdetachstate(b_pthread_attr_t *a, int st);
int b_pthread_attr_setstacksize(b_pthread_attr_t *a, size_t s);

#define C_WWW "com/unity3d/player/WWW"
#define CHUNK 0x8000

typedef jboolean (*fn_header)(void *env, void *cls, jint id, void *str);
typedef jboolean (*fn_read)(void *env, void *cls, jint id, void *bytes, jint n);
typedef void (*fn_progress)(void *env, void *cls, jint id, jfloat up, jfloat down, jdouble eta, jint total);
typedef void (*fn_error)(void *env, void *cls, jint id, void *msg);
typedef void (*fn_done)(void *env, void *cls, jint id);

typedef struct {
  jint id;
  char *url;
  volatile int started, alive;
  Mutex lock;
  CondVar done;
} Www;

static struct {
  fn_header header;
  fn_read read;
  fn_progress progress;
  fn_error error;
  fn_done done;
  void *cls;
} N;

/* ------------------------------------------------------------- game.apk */
/* One reader of the APK's central directory, shared (miniz is not thread-safe:
 * every use holds g_zip_lock). Entry data itself is read with a FILE of the
 * request's own. */
static Mutex g_zip_lock;
static mz_zip_archive g_zip;
static char g_zip_path[DCR_PATH_MAX];
static int g_zip_open;

static int zip_open(const char *sd_path) {
  if (g_zip_open && !strcmp(g_zip_path, sd_path))
    return 1;
  if (g_zip_open)
    mz_zip_reader_end(&g_zip);
  memset(&g_zip, 0, sizeof g_zip);
  g_zip_open = mz_zip_reader_init_file(&g_zip, sd_path, 0);
  snprintf(g_zip_path, sizeof g_zip_path, "%s", g_zip_open ? sd_path : "");
  return g_zip_open;
}

/* Where a STORED entry's data lies in game.apk (for the port's C#, which
 * loads asset bundles straight from there: dcr_mod.c). 0, or -1 if the entry
 * is missing or compressed. */
int dcr_apk_stored_entry(const char *name, uint64_t *offset, uint64_t *size) {
  char sd[DCR_PATH_MAX];
  snprintf(sd, sizeof sd, "%s", DCR_ANDROID_APK);
  dcr_translate_path(DCR_ANDROID_APK, sd, sizeof sd);
  mutexLock(&g_zip_lock);
  int idx = zip_open(sd) ? mz_zip_reader_locate_file(&g_zip, name, NULL, 0) : -1;
  mz_zip_archive_file_stat st;
  int ok = idx >= 0 && mz_zip_reader_file_stat(&g_zip, (mz_uint)idx, &st) && st.m_method == 0;
  mutexUnlock(&g_zip_lock);
  if (!ok)
    return -1;
  FILE *f = fopen(sd, "rb");
  uint8_t h[30];
  ok = f && !fseek(f, (long)st.m_local_header_ofs, SEEK_SET) && fread(h, 1, 30, f) == 30 &&
       (h[0] | h[1] << 8 | h[2] << 16 | (uint32_t)h[3] << 24) == 0x04034b50u;
  if (f)
    fclose(f);
  if (!ok)
    return -1;
  *offset = st.m_local_header_ofs + 30 + (h[26] | h[27] << 8) + (h[28] | h[29] << 8);
  *size = st.m_uncomp_size;
  return 0;
}

/* %XX escapes decoded (URL -> file or entry name). */
static void url_decode(char *s) {
  char *o = s;
  for (; *s; s++) {
    int hi, lo;
    if (*s == '%' && s[1] && s[2] &&
        (hi = s[1] >= '0' && s[1] <= '9' ? s[1] - '0' : (s[1] | 32) >= 'a' && (s[1] | 32) <= 'f' ? (s[1] | 32) - 'a' + 10 : -1) >= 0 &&
        (lo = s[2] >= '0' && s[2] <= '9' ? s[2] - '0' : (s[2] | 32) >= 'a' && (s[2] | 32) <= 'f' ? (s[2] | 32) - 'a' + 10 : -1) >= 0) {
      *o++ = (char)(hi << 4 | lo);
      s += 2;
    } else {
      *o++ = *s;
    }
  }
  *o = 0;
}

/* The source a request reads: an open FILE positioned at the data (len
 * bytes), or a heap copy (an inflated entry). */
typedef struct {
  FILE *f;
  uint8_t *heap;
  size_t len, pos;
} Src;

static int src_read(Src *s, uint8_t *buf, size_t cap) {
  size_t left = s->len - s->pos;
  size_t n = left < cap ? left : cap;
  if (!n)
    return -1; /* EOF */
  if (s->heap)
    memcpy(buf, s->heap + s->pos, n);
  else if (fread(buf, 1, n, s->f) != n)
    return -2;
  s->pos += n;
  return (int)n;
}

static void src_close(Src *s) {
  if (s->f)
    fclose(s->f);
  free(s->heap);
  memset(s, 0, sizeof *s);
}

/* "jar:file://<apk>!/<entry>": 0, or an error message (java.io exception text). */
static const char *open_jar(const char *url, Src *s, char *err, size_t errcap) {
  const char *p = url + 4; /* past "jar:" */
  if (strncmp(p, "file://", 7)) {
    snprintf(err, errcap, "java.net.MalformedURLException: no file: in %s", url);
    return err;
  }
  p += 7;
  const char *bang = strstr(p, "!/");
  if (!bang) {
    snprintf(err, errcap, "java.net.MalformedURLException: no !/ in spec");
    return err;
  }
  char file[DCR_PATH_MAX], entry[512], sd[DCR_PATH_MAX];
  snprintf(file, sizeof file, "%.*s", (int)(bang - p), p);
  snprintf(entry, sizeof entry, "%s", bang + 2);
  url_decode(file);
  url_decode(entry);
  dcr_translate_path(file, sd, sizeof sd);

  mutexLock(&g_zip_lock);
  int idx = zip_open(sd) ? mz_zip_reader_locate_file(&g_zip, entry, NULL, 0) : -1;
  mz_zip_archive_file_stat st;
  if (idx < 0 || !mz_zip_reader_file_stat(&g_zip, (mz_uint)idx, &st)) {
    mutexUnlock(&g_zip_lock);
    snprintf(err, errcap, "java.io.FileNotFoundException: JAR entry %.200s not found in %.200s", entry, file);
    return err;
  }
  if (st.m_method == 0) {
    mutexUnlock(&g_zip_lock);
    /* stored: stream it from its offset (local header: 30 bytes + name + extra) */
    s->f = fopen(sd, "rb");
    uint8_t h[30];
    if (!s->f || fseek(s->f, (long)st.m_local_header_ofs, SEEK_SET) || fread(h, 1, 30, s->f) != 30 ||
        (h[0] | h[1] << 8 | h[2] << 16 | (uint32_t)h[3] << 24) != 0x04034b50u ||
        fseek(s->f, (long)(st.m_local_header_ofs + 30 + (h[26] | h[27] << 8) + (h[28] | h[29] << 8)),
              SEEK_SET)) {
      src_close(s);
      snprintf(err, errcap, "java.io.IOException: cannot read %.200s from %.200s", entry, file);
      return err;
    }
    setvbuf(s->f, NULL, _IOFBF, CHUNK);
    s->len = (size_t)st.m_uncomp_size;
    return NULL;
  }
  size_t n = 0;
  s->heap = mz_zip_reader_extract_to_heap(&g_zip, (mz_uint)idx, &n, 0);
  mutexUnlock(&g_zip_lock);
  if (!s->heap) {
    snprintf(err, errcap, "java.util.zip.ZipException: cannot inflate %s", entry);
    return err;
  }
  s->len = n;
  return NULL;
}

static const char *open_file(const char *url, Src *s, char *err, size_t errcap) {
  const char *p = url + 5; /* past "file:" */
  if (!strncmp(p, "//", 2)) {
    p += 2;
    if (*p != '/') { /* file://host/... */
      const char *slash = strchr(p, '/');
      snprintf(err, errcap, "%.*s%s is not an absolute path!", slash ? (int)(slash - p) : (int)strlen(p), p,
               slash ? slash : "");
      return err;
    }
  }
  char file[DCR_PATH_MAX], sd[DCR_PATH_MAX];
  snprintf(file, sizeof file, "%s", p);
  url_decode(file);
  s->f = fopen(dcr_translate_path(file, sd, sizeof sd), "rb");
  if (!s->f) {
    snprintf(err, errcap, "java.io.FileNotFoundException: %.400s (No such file or directory)", file);
    return err;
  }
  fseek(s->f, 0, SEEK_END);
  long n = ftell(s->f);
  fseek(s->f, 0, SEEK_SET);
  s->len = n > 0 ? (size_t)n : 0;
  setvbuf(s->f, NULL, _IOFBF, CHUNK);
  return NULL;
}

/* ------------------------------------------------------------- a request */
static int header(jint id, const char *k, const char *v) {
  if (!N.header)
    return 0;
  JObj *s = jni_str_fmt("%s: %s\n\r", k, v);
  int abort = N.header(g_jni_env, N.cls, id, s) != 0;
  jni_release(s);
  return abort;
}

static void error(jint id, const char *msg) {
  if (!N.error)
    return;
  JObj *s = jni_str(msg);
  N.error(g_jni_env, N.cls, id, s);
  jni_release(s);
}

/* Java's progressCallback(0, 0, got, total, now, start) -> the native one. */
static void progress(jint id, size_t got, size_t total, uint64_t start_ms) {
  if (!N.progress || !total)
    return;
  const double ms = (double)(armTicksToNs(armGetSystemTick()) / 1000000ull - start_ms);
  double rate = 1000.0 * (double)got / (ms > 0.1 ? ms : 0.1);
  double eta = (double)(total > got ? total - got : 0) / rate;
  if (eta != eta || eta > 1e300)
    eta = 0.0;
  N.progress(g_jni_env, N.cls, id, 1.0f, (float)got / (float)total, eta, (jint)total);
}

static int is_scheme(const char *url, const char *scheme) {
  size_t n = strlen(scheme);
  return !strncasecmp(url, scheme, n) && url[n] == ':';
}

/* Replies the port's C# prepared for the game's server requests (its fake
 * server: mod/src/Online.cs, through DcrMod.Native.QueueWww): the next WWW
 * for exactly that URL gets the body, as a server would have sent it. */
typedef struct Fake {
  struct Fake *next;
  char *url;
  uint8_t *body;
  size_t len;
  uint64_t t_ms;
} Fake;
static Mutex g_fake_lock;
static Fake *g_fake;

void dcr_www_queue(const char *url, const uint8_t *body, size_t len) {
  Fake *f = calloc(1, sizeof *f);
  if (!f || !(f->url = strdup(url)) || !(f->body = malloc(len ? len : 1))) {
    if (f)
      free(f->url);
    free(f);
    return;
  }
  memcpy(f->body, body, len);
  f->len = len;
  f->t_ms = armTicksToNs(armGetSystemTick()) / 1000000ull;
  mutexLock(&g_fake_lock);
  Fake **pp = &g_fake;
  while (*pp)
    pp = &(*pp)->next;
  *pp = f;
  mutexUnlock(&g_fake_lock);
}

/* the oldest reply queued for url (entries older than a minute are dropped) */
static Fake *fake_take(const char *url) {
  const uint64_t now = armTicksToNs(armGetSystemTick()) / 1000000ull;
  Fake *hit = NULL;
  mutexLock(&g_fake_lock);
  for (Fake **pp = &g_fake; *pp;) {
    Fake *f = *pp;
    if (now - f->t_ms > 60000) {
      *pp = f->next;
      free(f->url);
      free(f->body);
      free(f);
      continue;
    }
    if (!hit && !strcmp(f->url, url)) {
      *pp = f->next;
      hit = f;
      continue;
    }
    pp = &f->next;
  }
  mutexUnlock(&g_fake_lock);
  return hit;
}

static void run(Www *w) {
  const jint id = w->id;
  const char *url = w->url ? w->url : "";
  char err[600];
  const char *e;
  Src s = {0};
  Fake *fake = NULL;
  if (is_scheme(url, "jar")) {
    e = open_jar(url, &s, err, sizeof err);
  } else if (is_scheme(url, "file")) {
    e = open_file(url, &s, err, sizeof err);
  } else if ((fake = fake_take(url)) != NULL) {
    /* a server's latency: the game's cloud queue calls the reply's callback
     * from its coroutine, and a reply that is already there when the request
     * is being sent re-enters the queue with that request still in it (it
     * was sent twice and the second send threw: Ryujinx 2026-09-29) */
    svcSleepThread(50000000ll);
    s.heap = fake->body; /* src_close frees it */
    s.len = fake->len;
    free(fake->url);
    free(fake);
    e = NULL;
  } else {
    /* http(s) and anything else: the network is not there */
    const char *h = strstr(url, "://");
    h = h ? h + 3 : url;
    snprintf(err, sizeof err,
             "java.net.UnknownHostException: Unable to resolve host \"%.*s\": No address associated with hostname",
             (int)strcspn(h, "/:?#"), h);
    static int logged;
    if (logged++ < 8)
      debugPrintf("[www] offline: %s\n", url);
    e = err;
  }
  if (e) {
    debugPrintf("[www] #%d %s -> %s\n", (int)id, url, e);
    error(id, e);
    return;
  }

  char num[16];
  snprintf(num, sizeof num, "%u", (unsigned)s.len);
  if ((s.len && header(id, "content-length", num)) ||
      header(id, "content-type", s.heap && !is_scheme(url, "jar") ? "application/json" : "content/unknown")) {
    snprintf(err, sizeof err, "%s aborted", url);
    error(id, err);
    src_close(&s);
    return;
  }

  const size_t cap = s.len ? (s.len < CHUNK ? s.len : CHUNK) : CHUNK;
  JObj *buf = jni_array('B', (jsize)cap);
  const uint64_t t0 = armTicksToNs(armGetSystemTick()) / 1000000ull;
  size_t got = 0;
  int n = 0, aborted = 0;
  for (;;) {
    if (N.read && N.read(g_jni_env, N.cls, id, buf, n)) {
      aborted = 1;
      break;
    }
    got += (size_t)n;
    progress(id, got, s.len, t0);
    n = src_read(&s, (uint8_t *)buf->a.data, cap);
    if (n < 0)
      break;
  }
  jni_release(buf);
  src_close(&s);
  if (aborted) {
    snprintf(err, sizeof err, "%s aborted", url);
    error(id, err);
    return;
  }
  if (n == -2) {
    snprintf(err, sizeof err, "java.io.IOException: read error in %s", url);
    error(id, err);
    return;
  }
  progress(id, got, got, 0);
  if (N.done)
    N.done(g_jni_env, N.cls, id);
}

static void *thread_main(void *arg) {
  JObj *self = arg;
  Www *w = self->p;
  run(w);
  mutexLock(&w->lock);
  w->alive = 0;
  condvarWakeAll(&w->done);
  mutexUnlock(&w->lock);
  jni_release(self);
  return NULL;
}

/* ------------------------------------------------------------- handlers */
static void www_finalize(JObj *o) {
  Www *w = o->p;
  if (w) {
    free(w->url);
    free(w);
  }
  o->p = NULL;
}

static void bind_natives(void) {
  if (N.cls)
    return;
  N.header = (fn_header)jni_native(C_WWW, "headerCallback");
  N.read = (fn_read)jni_native(C_WWW, "readCallback");
  N.progress = (fn_progress)jni_native(C_WWW, "progressCallback");
  N.error = (fn_error)jni_native(C_WWW, "errorCallback");
  N.done = (fn_done)jni_native(C_WWW, "doneCallback");
  N.cls = jni_class(C_WWW)->obj;
  if (!N.read || !N.done || !N.error)
    debugPrintf("[www] WARNING: WWW natives missing (read %p done %p error %p)\n", (void *)N.read,
                (void *)N.done, (void *)N.error);
}

JNI_H_DECL(jni_h_www_init) {
  Www *w = calloc(1, sizeof *w);
  w->id = a[0].i;
  w->url = strdup(jni_utf(a[1].l));
  mutexInit(&w->lock);
  condvarInit(&w->done);
  self->p = w;
  self->finalize = www_finalize;
  return jv_none();
}

JNI_H_DECL(jni_h_www_start) {
  Www *w = self->p;
  if (!w || w->started)
    return jv_none();
  bind_natives();
  static int shown;
  if (shown++ < 40)
    debugPrintf("[www] #%d %s\n", (int)w->id, w->url);
  w->started = w->alive = 1;
  b_pthread_attr_t at;
  b_pthread_attr_init(&at);
  b_pthread_attr_setdetachstate(&at, B_PTHREAD_CREATE_DETACHED);
  b_pthread_attr_setstacksize(&at, 0x10000);
  b_pthread_t t;
  jni_retain(self); /* the thread's reference, dropped when it ends */
  if (b_pthread_create(&t, &at, thread_main, self) != 0) {
    debugPrintf("[www] could not start a thread for #%d: running it here\n", (int)w->id);
    thread_main(self);
  }
  return jv_none();
}

JNI_H_DECL(jni_h_www_isAlive) {
  Www *w = self->p;
  return jv_z(w && w->alive);
}

JNI_H_DECL(jni_h_www_join) {
  Www *w = self->p;
  if (!w)
    return jv_none();
  mutexLock(&w->lock);
  while (w->alive)
    condvarWait(&w->done, &w->lock);
  mutexUnlock(&w->lock);
  return jv_none();
}
