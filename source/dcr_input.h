/* dcr_input.h -- Switch input -> the Android events Unity 2017.4 reads.
 *
 * Unity 2017.4 receives input through UnityPlayer.nativeInjectEvent(InputEvent)
 * and then reads the event back through JNI (MotionEvent.getX(i), ...). So an
 * event is a Java object of class android/view/MotionEvent or KeyEvent whose
 * payload is one of these structs; jni_android.c serves the getters from it. */
#ifndef DCR_INPUT_H
#define DCR_INPUT_H
#include <stdint.h>

#define DCR_MAX_POINTERS 10

/* The controllers are ONE Android gamepad, the kind the game's InControl
 * profiles know: an NVIDIA Shield controller. Its name picks the profile
 * (NVidiaShield2017AndroidProfile: A B X Y = Unity joystick buttons 0-3,
 * sticks = axes X Y / Z RZ, D-pad = HAT_X HAT_Y, triggers = GAS BRAKE). */
#define DCR_PAD_DEVICE_ID 7
/* SEA 1.5.4's InControl predates the NVidiaShield2017 profile: its
 * NVidiaShieldAndroidProfile matches "NVIDIA Corporation NVIDIA Controller"
 * (a regex search), which this name also satisfies for the 2017 one. */
#define DCR_PAD_NAME "NVIDIA Corporation NVIDIA Controller v01.04"
#define DCR_PAD_SOURCES 0x01000511 /* JOYSTICK | GAMEPAD | DPAD | KEYBOARD */
#define DCR_SRC_JOYSTICK 0x01000010
#define DCR_SRC_GAMEPAD_KEYS 0x00000501 /* GAMEPAD | KEYBOARD, as a pad's KeyEvent */
#define DCR_AXES 24                   /* MotionEvent.AXIS_X (0) .. AXIS_BRAKE (23) */

typedef struct {
  int32_t action;          /* MotionEvent action incl. pointer index bits */
  int32_t count;
  int32_t id[DCR_MAX_POINTERS];
  float x[DCR_MAX_POINTERS], y[DCR_MAX_POINTERS];
  int64_t down_time_ms, event_time_ms;
  int32_t source;          /* 0x1002 touchscreen, or DCR_SRC_JOYSTICK */
  int32_t device_id;
  float axis[DCR_AXES];    /* joystick events: getAxisValue(AXIS_*) */
} DcrMotion;

typedef struct {
  int32_t action;          /* ACTION_DOWN 0 / ACTION_UP 1 */
  int32_t key_code;
  int32_t meta_state;
  int32_t repeat;
  int32_t unicode;
  int32_t source;          /* 0x501 gamepad | 0x101 keyboard */
  int32_t device_id;
  int64_t down_time_ms, event_time_ms;
} DcrKey;

/* The gamepad's axes, as InputDevice.getMotionRanges() lists them. */
extern const int dcr_pad_axes[];
extern const int dcr_pad_naxes;

void dcr_input_init(void);
void dcr_input_frame(void);   /* poll the pads/touch and inject events (main loop) */

#endif
