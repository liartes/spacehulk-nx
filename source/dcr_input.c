/* dcr_input.c -- Switch touchscreen and controllers -> Android input events.
 *
 * Unity 2017.4 takes input as UnityPlayer.nativeInjectEvent(InputEvent) and
 * reads the event back through JNI getters (jni_android.c serves those from the
 * DcrMotion / DcrKey payload). Events are injected on the main thread just
 * before nativeRender, which is where the engine drains its input queue.
 *
 * TOUCH: every panel finger is a pointer, with Android's action protocol:
 * ACTION_DOWN for the first finger, ACTION_POINTER_DOWN/UP (index in bits
 * 8-15) for the others, ACTION_MOVE carrying all pointers, ACTION_UP for the
 * last. The panel is 1280x720; touches are scaled to the view (the rendering
 * size, 1920x1080 by default -- config.ini), top-left origin, as on a phone.
 *
 * CONTROLLERS: the game shipped for Android TV and Fire TV and drives itself
 * from gamepads through InControl (InputManager.Update: Action1 = hop, the
 * direction of D-pad + left stick = steer, Action2 = back, Start = pause, and
 * menu focus navigation). So the controllers are presented as what InControl
 * knows -- one Android gamepad, an NVIDIA Shield controller (dcr_input.h) --
 * rather than as synthesized taps and swipes:
 *   A B X Y, L R, L3 R3, +   KeyEvent BUTTON_A B X Y, L1 R1, THUMBL THUMBR, START
 *   left / right stick       MotionEvent AXIS_X Y / AXIS_Z RZ
 *   D-pad                    AXIS_HAT_X HAT_Y
 *   ZL ZR                    AXIS_BRAKE GAS and AXIS_LTRIGGER RTRIGGER
 * Unity registers the pad (InputDevice.getDevice) on its first event; one
 * neutral event is sent on the first frame so InControl attaches it before
 * anyone presses anything. Switch A is the hop, B is back, + pauses -- the
 * game's own controller scheme, as on Android TV. In Space Hulk + is the
 * game's Start (the menu) and - its Select (the strategic view): see
 * k_pad_keys. With [debug] minus_screenshot, - saves the next frame to the
 * SD card instead (gl_mesa.c), for looking at exactly what the game drew.
 *
 * WIRELESS CONTROLLERS: which player slots may take a controller is the
 * application's call (hid SetSupportedNpadIdType); a controller with no slot to
 * go to -- Joy-Cons taken off the console, a Pro Controller -- keeps
 * searching. padConfigureInput() sends that list, but libnx32 is built for
 * arm-none-eabi, whose enums are as small as their values allow
 * (Tag_ABI_enum_size: small): HidNpadIdType is one byte, so the list {No1..No8,
 * Handheld} left as 9 bytes, which hid reads as two 32-bit IDs, 0x03020100 and
 * 0x07060504 -- no real player slot. The call still succeeded, so attached
 * Joy-Cons (the handheld slot) kept working and nothing else could connect
 * (hardware 2026-09-25). set_supported_npad_ids() sends the list again, as
 * 32-bit IDs.
 *
 * ONE PLAYER: the game has one character, so one controller drives it --
 * player 1. Any slot may connect (switching controllers needs no
 * disconnecting first), but only the first of these that is connected plays:
 *   player 1, then the attached Joy-Cons (handheld), then players 2-8.
 * So a controller connecting as player 1 takes over from the attached
 * Joy-Cons, as on a Switch; the attached Joy-Cons play when there is no
 * player 1; and if player 1 disconnects, the next controller in line plays
 * rather than none. Each slot is read on its own (libnx's padInitializeAny
 * would OR every controller together); presses and releases are worked out
 * against what the game last saw, so control changing hands mid-press
 * releases the old controller's buttons instead of leaving them stuck.
 *
 * LATENCY: this runs once per frame, right after the frame's vsync and just
 * before nativeRender, whose first step drains these events (dcr_vsync.c,
 * dcr_boot.c); the hop itself fires on the press, not the release
 * (dcr_ilpatch.c).
 * MIT.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "dcr_config.h"
#include "dcr_input.h"
#include "dcr_jni_unity.h"
#include "dcr_time.h"
#include "gl_layer.h"
#include "rt_window.h"
#include "util.h"

#define AM_DOWN 0
#define AM_UP 1
#define AM_MOVE 2
#define AM_POINTER_DOWN 5
#define AM_POINTER_UP 6
#define SRC_TOUCH 0x1002

/* MotionEvent.AXIS_* */
#define AXIS_X 0
#define AXIS_Y 1
#define AXIS_Z 11
#define AXIS_RZ 14
#define AXIS_HAT_X 15
#define AXIS_HAT_Y 16
#define AXIS_LTRIGGER 17
#define AXIS_RTRIGGER 18
#define AXIS_GAS 22
#define AXIS_BRAKE 23

const int dcr_pad_axes[] = {AXIS_X,     AXIS_Y,         AXIS_Z,         AXIS_RZ,  AXIS_HAT_X,
                            AXIS_HAT_Y, AXIS_LTRIGGER, AXIS_RTRIGGER, AXIS_GAS, AXIS_BRAKE};
const int dcr_pad_naxes = sizeof dcr_pad_axes / sizeof dcr_pad_axes[0];

typedef jboolean (*fn_inject)(void *env, void *thiz, void *event);
static fn_inject g_inject;
static void *g_thiz;
/* one PadState per slot: players 1-8 (index 0-7), handheld (8) */
#define NSLOTS 9
static PadState g_pads[NSLOTS];
static u64 g_held_sent;   /* buttons the game has seen held */
static int g_ready;
static float g_view_sx = 1.0f, g_view_sy = 1.0f; /* panel -> view */

/* current pointers as the engine last saw them */
static DcrMotion g_cur;
static int64_t g_down_ms;
static u64 g_ts_held; /* buttons the test script holds (see ts_frame) */

static int64_t now_ms(void) { return (int64_t)(dcr_monotonic_ns() / 1000000ull); }

/* hid SetSupportedNpadIdType (command 102), with 32-bit IDs: players 1-8 and
 * handheld (see the notes at the top). */
static Result set_supported_npad_ids(void) {
  static const u32 ids[] = {0, 1, 2, 3, 4, 5, 6, 7, 0x20};
  u64 aruid = appletGetAppletResourceUserId();
  return serviceDispatchIn(hidGetServiceSession(), 102, aruid,
                           .buffer_attrs = {SfBufferAttr_HipcPointer | SfBufferAttr_In},
                           .buffers = {{ids, sizeof ids}}, .in_send_pid = true);
}

void dcr_input_init(void) {
  padConfigureInput(8, HidNpadStyleSet_NpadStandard);
  Result rc = set_supported_npad_ids();
  debugPrintf("[input] controllers allowed: players 1-8 and handheld, any style (%s)\n",
              R_SUCCEEDED(rc) ? "ok" : "hid refused it");
  if (R_FAILED(rc))
    debugPrintf("[input]   SetSupportedNpadIdType failed 0x%x: only the attached Joy-Cons will work\n",
                (unsigned)rc);
  for (int i = 0; i < NSLOTS; i++)
    padInitializeWithMask(&g_pads[i], i < 8 ? BITL(i) : BITL(HidNpadIdType_Handheld));
  hidInitializeTouchScreen();
  g_inject = (fn_inject)jni_native("com/unity3d/player/UnityPlayer", "nativeInjectEvent");
  g_thiz = jni_singleton("com/unity3d/player/UnityPlayer");
  g_ready = g_inject != NULL;
  int w, h;
  dcr_window_size(&w, &h);
  g_view_sx = (float)w / 1280.0f;
  g_view_sy = (float)h / 720.0f;
  debugPrintf("[input] %s\n", g_ready ? "ready (nativeInjectEvent)" : "nativeInjectEvent not registered");
}

static void free_payload(JObj *o) {
  free(o->p);
  o->p = NULL;
}

/* Hand one event to the engine. The engine may DeleteLocalRef its argument,
 * so it is retained across the call; JavaInput::Register queues a copy of a
 * MotionEvent (MotionEvent.obtain), whose local ref a JVM would drop when the
 * native call returns -- jni_release_obtained() does. */
static void inject(const char *cls, void *payload) {
  JObj *ev = jni_new(cls);
  ev->p = payload;
  ev->finalize = free_payload;
  jni_retain(ev);
  g_inject(g_jni_env, g_thiz, ev);
  jni_release_obtained();
  jni_release(ev);
  jni_release(ev);
}

/* ---------------------------------------------------------------- touch */
static int g_logged_touches;

static void send_motion(int action) {
  DcrMotion *m = malloc(sizeof *m);
  if (!m)
    return;
  *m = g_cur;
  m->action = action;
  m->down_time_ms = g_down_ms;
  m->event_time_ms = now_ms();
  m->source = SRC_TOUCH;
  if (g_logged_touches < 4 && ((action & 0xff) == AM_DOWN || (action & 0xff) == AM_UP)) {
    int w, h;
    dcr_window_size(&w, &h);
    g_logged_touches++;
    debugPrintf("[input] touch %s at (%.0f, %.0f); view %dx%d%s\n",
                (action & 0xff) == AM_DOWN ? "down" : "up", m->x[0], m->y[0], w, h,
                g_logged_touches == 1 ? " (panel 1280x720 scaled to the view)" : "");
  }
  inject("android/view/MotionEvent", m);
}

static int find_ptr(int id) {
  for (int i = 0; i < g_cur.count; i++)
    if (g_cur.id[i] == id)
      return i;
  return -1;
}

static void ptr_down(int id, float x, float y) {
  if (g_cur.count >= DCR_MAX_POINTERS)
    return;
  int i = g_cur.count++;
  g_cur.id[i] = id;
  g_cur.x[i] = x;
  g_cur.y[i] = y;
  if (g_cur.count == 1) {
    g_down_ms = now_ms();
    send_motion(AM_DOWN);
  } else {
    send_motion(AM_POINTER_DOWN | (i << 8));
  }
}

static void ptr_up(int id) {
  int i = find_ptr(id);
  if (i < 0)
    return;
  send_motion(g_cur.count == 1 ? AM_UP : (AM_POINTER_UP | (i << 8)));
  for (int k = i; k < g_cur.count - 1; k++) {
    g_cur.id[k] = g_cur.id[k + 1];
    g_cur.x[k] = g_cur.x[k + 1];
    g_cur.y[k] = g_cur.y[k + 1];
  }
  g_cur.count--;
}

static void touch_frame(void) {
  HidTouchScreenState ts = {0};
  hidGetTouchScreenStates(&ts, 1);
  int seen[DCR_MAX_POINTERS] = {0};
  int moved = 0;
  for (int k = 0; k < ts.count && k < 8; k++) {
    int id = (int)ts.touches[k].finger_id;
    /* panel space (1280x720) -> view space (the rendering size) */
    float x = (float)ts.touches[k].x * g_view_sx, y = (float)ts.touches[k].y * g_view_sy;
    int i = find_ptr(id);
    if (i < 0) {
      ptr_down(id, x, y);
      i = find_ptr(id);
    } else if (g_cur.x[i] != x || g_cur.y[i] != y) {
      g_cur.x[i] = x;
      g_cur.y[i] = y;
      moved = 1;
    }
    if (i >= 0)
      seen[i] = 1;
  }
  if (moved)
    send_motion(AM_MOVE);
  for (int i = g_cur.count - 1; i >= 0; i--)
    if (!seen[i] && g_cur.id[i] != 99) /* 99: the test script's finger */
      ptr_up(g_cur.id[i]);
}

/* -------------------------------------------------------------- gamepad */
static const struct {
  u64 button;
  int keycode;
} k_pad_keys[] = {
    {HidNpadButton_A, 96},       /* BUTTON_A: InControl Action1 -- hop */
    {HidNpadButton_B, 97},       /* BUTTON_B: Action2 -- back */
    {HidNpadButton_X, 99},       /* BUTTON_X */
    {HidNpadButton_Y, 100},      /* BUTTON_Y */
    {HidNpadButton_L, 102},      /* BUTTON_L1 */
    {HidNpadButton_R, 103},      /* BUTTON_R1 */
    {HidNpadButton_StickL, 106}, /* BUTTON_THUMBL */
    {HidNpadButton_StickR, 107}, /* BUTTON_THUMBR */
    /* Space Hulk (GamepadState.Update, read in its IL): its "Start" -- the
     * in-mission menu, OptionsButton's hotkey -- is Input.GetKey(Escape),
     * Android's BACK key, as on the Shield controller; its "Select" -- the
     * strategic view -- is "joystick button 10", which Unity 5.3 makes of
     * BUTTON_START ([debug] log_buttons, hardware 2026-10-04: 96-103 -> 0-5
     * without 98/101, 104/105 -> 6/7, 106/107 -> 8/9, 108 -> 10, 109 -> 11) */
    {HidNpadButton_Plus, 4},     /* KEYCODE_BACK: Escape -- the game's Start, the menu */
    {HidNpadButton_Minus, 108},  /* BUTTON_START: joystick button 10 -- the game's Select */
};

static float g_axis_sent[DCR_AXES];
static int g_pad_announced;

static void send_key(int action, int code) {
  if (dcr_config()->log_buttons && action == 0)
    debugPrintf("[pad] sent Android key code %d\n", code);
  DcrKey *k = calloc(1, sizeof *k);
  if (!k)
    return;
  k->action = action;
  k->key_code = code;
  k->source = DCR_SRC_GAMEPAD_KEYS;
  k->device_id = DCR_PAD_DEVICE_ID;
  k->down_time_ms = k->event_time_ms = now_ms();
  inject("android/view/KeyEvent", k);
}

static void send_axes(const float *axis) {
  DcrMotion *m = calloc(1, sizeof *m);
  if (!m)
    return;
  m->action = AM_MOVE;
  m->count = 1; /* a joystick event carries one pointer */
  m->source = DCR_SRC_JOYSTICK;
  m->device_id = DCR_PAD_DEVICE_ID;
  m->down_time_ms = m->event_time_ms = now_ms();
  memcpy(m->axis, axis, sizeof m->axis);
  memcpy(g_axis_sent, axis, sizeof g_axis_sent);
  inject("android/view/MotionEvent", m);
}

static const char *slot_name(int i, char *buf, size_t cap) {
  if (i == 8)
    snprintf(buf, cap, "the attached Joy-Cons");
  else
    snprintf(buf, cap, "player %d", i + 1);
  return buf;
}

static const char *style_name(u32 st) {
  return (st & HidNpadStyleTag_NpadFullKey)    ? "Pro Controller"
         : (st & HidNpadStyleTag_NpadHandheld) ? "Joy-Cons, attached"
         : (st & HidNpadStyleTag_NpadJoyDual)  ? "Joy-Con pair"
         : (st & HidNpadStyleTag_NpadJoyLeft)  ? "left Joy-Con"
         : (st & HidNpadStyleTag_NpadJoyRight) ? "right Joy-Con"
                                               : "controller";
}

/* One Joy-Con alone (local multiplayer hands one to each player), held
 * sideways with its rail up, as the console's own games take it: the face
 * button at the right is A, SL / SR are L / R, its + or - is +, a click of
 * its stick is ZL, and its stick turned with it (the Labyrinth 2 and Sonic
 * Racing ports' mapping). */
static int is_single(u64 st) {
  return (st & (HidNpadStyleTag_NpadJoyLeft | HidNpadStyleTag_NpadJoyRight)) &&
         !(st & (HidNpadStyleTag_NpadHandheld | HidNpadStyleTag_NpadJoyDual | HidNpadStyleTag_NpadFullKey));
}

static u64 single_buttons(u64 st, u64 b) {
  u64 o = b & ~(HidNpadButton_A | HidNpadButton_B | HidNpadButton_X | HidNpadButton_Y | HidNpadButton_Up |
                HidNpadButton_Down | HidNpadButton_Left | HidNpadButton_Right | HidNpadButton_Minus |
                HidNpadButton_StickL | HidNpadButton_StickR);
  if (st & HidNpadStyleTag_NpadJoyLeft) { /* Down at the right, Left below, Up at the left, Right on top */
    if (b & HidNpadButton_Down) o |= HidNpadButton_A;
    if (b & HidNpadButton_Left) o |= HidNpadButton_B;
    if (b & HidNpadButton_Up) o |= HidNpadButton_Y;
    if (b & HidNpadButton_Right) o |= HidNpadButton_X;
  } else { /* X at the right, A below, B at the left, Y on top */
    if (b & HidNpadButton_X) o |= HidNpadButton_A;
    if (b & HidNpadButton_A) o |= HidNpadButton_B;
    if (b & HidNpadButton_B) o |= HidNpadButton_Y;
    if (b & HidNpadButton_Y) o |= HidNpadButton_X;
  }
  if (b & (HidNpadButton_LeftSL | HidNpadButton_RightSL)) o |= HidNpadButton_L;
  if (b & (HidNpadButton_LeftSR | HidNpadButton_RightSR)) o |= HidNpadButton_R;
  if (b & (HidNpadButton_Minus | HidNpadButton_Plus)) o |= HidNpadButton_Plus;
  if (b & (HidNpadButton_StickL | HidNpadButton_StickR)) o |= HidNpadButton_ZL;
  return o;
}

/* a controller's buttons and sticks as its player holds it (sticks -1..1, y up) */
static u64 g_ts_held_p[8]; /* the test script's presses for players 2-8 (index 1-7) */
static int g_ts_frames_p[8];
/* Buttons still held from a system screen (the A that picked a profile): the
 * game sees them only once they have been let go, not as a fresh press on the
 * button that happens to be focused when the screen closes. */
static u64 g_mute[NSLOTS];
static u8 g_mute_arm[NSLOTS];
void dcr_input_mute_held(void) {
  for (int i = 0; i < NSLOTS; i++)
    g_mute_arm[i] = 1;
}
static u64 slot_read(int i, float *st4) {
  PadState *pad = &g_pads[i];
  const u64 style = padGetStyleSet(pad);
  u64 b = padGetButtons(pad);
  const int single = is_single(style);
  if (single)
    b = single_buttons(style, b);
  if (g_mute_arm[i]) {
    g_mute[i] = b;
    g_mute_arm[i] = 0;
  }
  if (g_mute[i]) {
    g_mute[i] &= b;
    b &= ~g_mute[i];
  }
  const int right_alone = single && (style & HidNpadStyleTag_NpadJoyRight);
  HidAnalogStickState l = padGetStickPos(pad, right_alone ? 1 : 0), r = padGetStickPos(pad, 1);
  float lx = (float)l.x / (float)JOYSTICK_MAX, ly = (float)l.y / (float)JOYSTICK_MAX;
  float rx = single ? 0.0f : (float)r.x / (float)JOYSTICK_MAX, ry = single ? 0.0f : (float)r.y / (float)JOYSTICK_MAX;
  if (single) {
    const float cx = lx, cy = ly;
    if (style & HidNpadStyleTag_NpadJoyLeft)
      lx = -cy, ly = cx;
    else
      lx = cy, ly = -cx;
  }
  if (st4)
    st4[0] = lx, st4[1] = ly, st4[2] = rx, st4[3] = ry;
  if (i >= 1 && i < 8)
    b |= g_ts_held_p[i];
  return b;
}

/* a scripted player (2-8) stays plugged in once the script has pressed its buttons */
static u8 g_ts_seen_p[8];
static int ts_player_seen(int slot) {
  if (slot >= 1 && slot < 8 && g_ts_held_p[slot])
    g_ts_seen_p[slot] = 1;
  return slot >= 1 && slot < 8 && g_ts_seen_p[slot];
}

/* -1: the usual choice below; else the slot that drives the game's pad */
static int g_game_slot = -1;
void dcr_input_set_game_slot(int slot) {
  if (slot != g_game_slot)
    debugPrintf("[input] the game's own pad now follows %s\n", slot < 0 ? "the usual order" : slot == 8 ? "the attached Joy-Cons" : "one player");
  g_game_slot = slot < -1 || slot >= NSLOTS ? -1 : slot;
}

/* For the port's C# (dcr_mod.c): a slot's style set (0 = none), buttons and
 * sticks as held. Slot 0 includes the test script's player-1 presses. */
int dcr_input_slot(int slot, uint64_t *buttons, float *sticks) {
  if (slot < 0 || slot >= NSLOTS) {
    *buttons = 0;
    return 0;
  }
  const u64 style = padGetStyleSet(&g_pads[slot]);
  *buttons = slot_read(slot, sticks) | (slot == 0 ? g_ts_held : 0);
  int st = (int)(style & 0x7fffffff);
  if (!st && ((slot == 0 && g_ts_held) || ts_player_seen(slot)))
    st = HidNpadStyleTag_NpadFullKey; /* the test script's player */
  return st;
}

/* The Switch's controller screen for min..max players, one Joy-Con each
 * allowed (held sideways: the hold type must be set BEFORE the applet, or it
 * fails at once in handheld mode -- the Sonic Racing port on hardware).
 * Blocking; returns the number of players connected after it, or -1. */
static int g_ts_n; /* test script steps (below) */
int dcr_input_controller_applet(int min, int max) {
  if (g_ts_n) /* a scripted run (emulator): the script's players, no applet */
    return max;
  hidSetNpadJoyHoldType(HidNpadJoyHoldType_Horizontal);
  HidLaControllerSupportArg arg;
  hidLaCreateControllerSupportArg(&arg);
  arg.hdr.player_count_min = (s8)min;
  arg.hdr.player_count_max = (s8)max;
  arg.hdr.enable_permit_joy_dual = 1;
  arg.hdr.enable_single_mode = 0;
  arg.hdr.enable_identification_color = 1;
  static const HidLaControllerSupportArgColor col[4] = {
      {0x2e, 0x9b, 0xe8, 0xff}, {0xe8, 0x4a, 0x3a, 0xff}, {0x5c, 0xc8, 0x3c, 0xff}, {0xf0, 0xc0, 0x20, 0xff}};
  for (int i = 0; i < 4; i++)
    arg.identification_color[i] = col[i];
  HidLaControllerSupportResultInfo info;
  memset(&info, 0, sizeof info);
  const u64 t0 = armGetSystemTick();
  void dcr_applet_busy(int on); /* watchdog.c */
  dcr_applet_busy(1);
  const Result rc = hidLaShowControllerSupport(&info, &arg);
  dcr_applet_busy(0);
  dcr_input_mute_held();
  for (int i = 0; i < NSLOTS; i++)
    padUpdate(&g_pads[i]);
  int n = 0;
  for (int i = 0; i < 8; i++)
    n += padIsConnected(&g_pads[i]);
  debugPrintf("[input] the controller screen (%d-%d players): 0x%x, %d player(s), %d controllers now, after %llu ms\n",
              min, max, rc, info.player_count, n,
              (unsigned long long)(armTicksToNs(armGetSystemTick() - t0) / 1000000ull));
  return R_SUCCEEDED(rc) ? n : -1;
}

/* HD rumble on one slot for ms milliseconds (stopped by dcr_input_frame). */
static HidVibrationDeviceHandle g_vib[NSLOTS][2];
static s8 g_vib_ok[NSLOTS];
static u32 g_vib_style[NSLOTS];
static u64 g_vib_until[NSLOTS];
static void vib_send(int slot, float amp) {
  HidVibrationValue v = {.amp_low = amp, .freq_low = 160.0f, .amp_high = amp * 0.7f, .freq_high = 320.0f};
  HidVibrationValue vv[2] = {v, v};
  hidSendVibrationValues(g_vib[slot], vv, is_single(g_vib_style[slot]) ? 1 : 2);
}
void dcr_input_rumble(int slot, float amp, int ms) {
  if (slot < 0 || slot >= NSLOTS)
    return;
  const u32 style = (u32)padGetStyleSet(&g_pads[slot]);
  if (!style)
    return;
  if (style != g_vib_style[slot]) {
    static const u32 tags[] = {HidNpadStyleTag_NpadHandheld, HidNpadStyleTag_NpadFullKey, HidNpadStyleTag_NpadJoyDual,
                               HidNpadStyleTag_NpadJoyLeft, HidNpadStyleTag_NpadJoyRight};
    u32 tag = 0;
    for (unsigned k = 0; k < sizeof tags / sizeof tags[0] && !tag; k++)
      if (style & tags[k])
        tag = tags[k];
    g_vib_style[slot] = style;
    g_vib_ok[slot] = tag && R_SUCCEEDED(hidInitializeVibrationDevices(g_vib[slot], is_single(style) ? 1 : 2,
                                                                      slot == 8 ? HidNpadIdType_Handheld : (HidNpadIdType)slot, tag))
                         ? 1 : -1;
  }
  if (g_vib_ok[slot] < 0)
    return;
  vib_send(slot, amp < 0 ? 0 : amp > 1 ? 1 : amp);
  g_vib_until[slot] = armGetSystemTick() + armNsToTicks((u64)(ms > 2000 ? 2000 : ms) * 1000000ull);
}

static void rumble_frame(void) {
  const u64 now = armGetSystemTick();
  for (int i = 0; i < NSLOTS; i++)
    if (g_vib_until[i] && now >= g_vib_until[i]) {
      g_vib_until[i] = 0;
      if (g_vib_ok[i] > 0)
        vib_send(i, 0.0f);
    }
}

/* The slot in control: player 1, the attached Joy-Cons, then players 2-8. */
static int pick_active(void) {
  static const int order[NSLOTS] = {0, 8, 1, 2, 3, 4, 5, 6, 7};
  if (g_game_slot >= 0)
    return padIsConnected(&g_pads[g_game_slot]) || (g_game_slot == 0 && g_ts_held) ? g_game_slot : -1;
  for (int k = 0; k < NSLOTS; k++)
    if (padIsConnected(&g_pads[order[k]]))
      return order[k];
  return -1;
}

/* Which controllers are connected, and who is playing, when that changes. */
static void log_controllers(int active) {
  static int seen_active = -2;
  static u32 seen_connected = ~0u;
  u32 connected = 0;
  for (int i = 0; i < NSLOTS; i++)
    if (padIsConnected(&g_pads[i]))
      connected |= 1u << i;
  if (active == seen_active && connected == seen_connected)
    return;
  seen_active = active;
  seen_connected = connected;
  char list[200] = "", nm[32];
  size_t o = 0;
  for (int i = 0; i < NSLOTS && o < sizeof list; i++)
    if (connected & (1u << i)) {
      int k = snprintf(list + o, sizeof list - o, "%s%s (%s)", o ? ", " : "", slot_name(i, nm, sizeof nm),
                       style_name(padGetStyleSet(&g_pads[i])));
      o += k > 0 ? (size_t)k : 0;
    }
  debugPrintf("[input] controllers: %s; playing: %s\n", connected ? list : "none",
              active < 0 ? "nobody" : slot_name(active, nm, sizeof nm));
}

static void pad_frame(void) {
  for (int i = 0; i < NSLOTS; i++)
    padUpdate(&g_pads[i]);
  int active = pick_active();
  log_controllers(active);
  PadState *pad = active >= 0 ? &g_pads[active] : NULL;
  float st4[4] = {0};
  u64 held = (pad ? slot_read(active, st4) : 0) | g_ts_held; /* + the test script's */
  /* against what the game last saw: a change of controller releases the old
   * one's buttons and presses the new one's */
  u64 down = held & ~g_held_sent;
  u64 up = g_held_sent & ~held;
  g_held_sent = held;

  /* Minus is the pad's Select; a screenshot only with [debug] minus_screenshot
   * (the read-back and the write hold the game for a moment) */
  if ((down & HidNpadButton_Minus) && dcr_config()->minus_capture)
    dcr_gl_request_capture();

  for (unsigned i = 0; i < sizeof k_pad_keys / sizeof k_pad_keys[0]; i++) {
    if (down & k_pad_keys[i].button)
      send_key(0 /* ACTION_DOWN */, k_pad_keys[i].keycode);
    if (up & k_pad_keys[i].button)
      send_key(1 /* ACTION_UP */, k_pad_keys[i].keycode);
  }

  /* Android's axis conventions: stick and hat Y point DOWN. */
  float axis[DCR_AXES] = {0};
  axis[AXIS_X] = st4[0];
  axis[AXIS_Y] = -st4[1];
  axis[AXIS_Z] = st4[2];
  axis[AXIS_RZ] = -st4[3];
  axis[AXIS_HAT_X] = (held & HidNpadButton_Left) ? -1.0f : (held & HidNpadButton_Right) ? 1.0f : 0.0f;
  axis[AXIS_HAT_Y] = (held & HidNpadButton_Up) ? -1.0f : (held & HidNpadButton_Down) ? 1.0f : 0.0f;
  /* The game handles one new press per frame (KlicktockInput.InputManager:
   * `if A pressed ... else if Right pressed ...`), so A and a D-pad direction
   * pressed in the same frame would lose the direction. The direction goes to
   * the engine one frame later instead. */
  if ((down & HidNpadButton_A) && g_axis_sent[AXIS_HAT_X] == 0.0f && g_axis_sent[AXIS_HAT_Y] == 0.0f) {
    axis[AXIS_HAT_X] = 0.0f;
    axis[AXIS_HAT_Y] = 0.0f;
  }
  /* triggers: the 2017 profile reads GAS/BRAKE, SEA's older one L/RTRIGGER */
  axis[AXIS_BRAKE] = axis[AXIS_LTRIGGER] = (held & HidNpadButton_ZL) ? 1.0f : 0.0f;
  axis[AXIS_GAS] = axis[AXIS_RTRIGGER] = (held & HidNpadButton_ZR) ? 1.0f : 0.0f;
  if (!g_pad_announced || memcmp(axis, g_axis_sent, sizeof axis) != 0) {
    if (!g_pad_announced)
      debugPrintf("[input] gamepad \"%s\" (device %d) announced to the engine\n", DCR_PAD_NAME,
                  DCR_PAD_DEVICE_ID);
    g_pad_announced = 1;
    send_axes(axis);
  }
}

/* ------------------------------------------------------------ test script
 * A test aid for runs without hands (an emulator): <root>/test_script.txt,
 * one step a line, times in seconds since the first frame:
 *   <t> tap <x> <y>      touch down now, up 3 frames later (x, y in
 *                        thousandths of the screen)
 *   <t> press <button>   a b x y l r zl zr plus minus up down left right: held
 *                        for 3 frames
 *   <t> cap              save the next frame (capture-NNN.bmp)
 *   <t> quit             leave the frame loop
 * Absent file (every normal launch): nothing happens. */
extern volatile int g_dcr_quit_requested;
const char *dcr_game_root(void);
#define TS_MAX 512
static struct { float t; char op; float x, y; u64 btn; int done; } g_ts[TS_MAX];
static int g_ts_n, g_ts_tap_frames, g_ts_btn_frames;
static int64_t g_ts_t0;

static u64 ts_button(const char *n) {
  static const struct { const char *n; u64 b; } k[] = {
      {"a", HidNpadButton_A}, {"b", HidNpadButton_B}, {"x", HidNpadButton_X}, {"y", HidNpadButton_Y},
      {"l", HidNpadButton_L}, {"r", HidNpadButton_R}, {"zl", HidNpadButton_ZL}, {"zr", HidNpadButton_ZR},
      {"plus", HidNpadButton_Plus},
      {"minus", HidNpadButton_Minus}, {"up", HidNpadButton_Up}, {"down", HidNpadButton_Down},
      {"left", HidNpadButton_Left}, {"right", HidNpadButton_Right}};
  for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++)
    if (!strcmp(n, k[i].n))
      return k[i].b;
  return 0;
}

static void ts_load(void) {
  char path[300], line[128];
  snprintf(path, sizeof path, "%s/test_script.txt", dcr_game_root());
  FILE *f = fopen(path, "r");
  if (!f)
    return;
  while (g_ts_n < TS_MAX && fgets(line, sizeof line, f)) {
    float t, x = 0, y = 0;
    char op[16] = "", arg[16] = "";
    if (line[0] == '#' || sscanf(line, "%f %15s", &t, op) < 2)
      continue;
    g_ts[g_ts_n].t = t;
    if (!strcmp(op, "tap") && sscanf(line, "%*f %*s %f %f", &x, &y) == 2) {
      g_ts[g_ts_n].op = 't', g_ts[g_ts_n].x = x, g_ts[g_ts_n].y = y;
    } else if (!strcmp(op, "press") && sscanf(line, "%*f %*s %15s", arg) == 1 && ts_button(arg)) {
      char who[8] = "";
      int pl = 1;
      if (sscanf(line, "%*f %*s %*s %7s", who) == 1 && who[0] == 'p')
        pl = atoi(who + 1);
      g_ts[g_ts_n].op = 'p', g_ts[g_ts_n].btn = ts_button(arg);
      g_ts[g_ts_n].x = (float)(pl < 1 || pl > 8 ? 1 : pl);
    } else if (!strcmp(op, "cap")) {
      g_ts[g_ts_n].op = 'c';
    } else if (!strcmp(op, "quit")) {
      g_ts[g_ts_n].op = 'q';
    } else {
      continue;
    }
    g_ts_n++;
  }
  fclose(f);
  debugPrintf("[test] test_script.txt: %d steps\n", g_ts_n);
}

static void ts_frame(void) {
  if (!g_ts_n)
    return;
  if (!g_ts_t0)
    g_ts_t0 = now_ms();
  const float t = (float)(now_ms() - g_ts_t0) / 1000.0f;
  if (g_ts_tap_frames && --g_ts_tap_frames == 0)
    ptr_up(99);
  if (g_ts_btn_frames && --g_ts_btn_frames == 0)
    g_ts_held = 0;
  for (int p = 1; p < 8; p++) /* each scripted player's press ends on its own */
    if (g_ts_frames_p[p] && --g_ts_frames_p[p] == 0)
      g_ts_held_p[p] = 0;
  for (int i = 0; i < g_ts_n; i++) {
    if (g_ts[i].done || g_ts[i].t > t)
      continue;
    g_ts[i].done = 1;
    int w, h;
    dcr_window_size(&w, &h);
    switch (g_ts[i].op) {
    case 't':
      if (!g_ts_tap_frames) {
        ptr_down(99, g_ts[i].x * (float)w / 1000.0f, g_ts[i].y * (float)h / 1000.0f);
        g_ts_tap_frames = 3;
      }
      break;
    case 'p':
      if (g_ts[i].x > 1.5f) { /* presses due in the same frame add up */
        g_ts_held_p[(int)g_ts[i].x - 1] |= g_ts[i].btn;
        g_ts_frames_p[(int)g_ts[i].x - 1] = 3;
      } else {
        g_ts_held |= g_ts[i].btn;
        g_ts_btn_frames = 3;
      }
      break;
    case 'c':
      dcr_gl_request_capture();
      break;
    default:
      g_dcr_quit_requested = 1;
      break;
    }
    debugPrintf("[test] %.1f s: step %d (%c)\n", t, i, g_ts[i].op);
  }
}

void dcr_input_frame(void) {
  if (!g_ready)
    return;
  static int loaded;
  if (!loaded++)
    ts_load();
  ts_frame();
  touch_frame();
  pad_frame();
  rumble_frame();
}
