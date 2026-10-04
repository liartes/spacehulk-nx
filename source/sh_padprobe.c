/* sh_padprobe.c -- which Unity "joystick button N" each Switch button becomes.
 *
 * Space Hulk reads its pad by Unity button number (GamepadState.cs: its
 * InputManager axes "Start" = joystick button 7, and "Select" read as
 * "joystick button 10" on Android); the in-mission menu is the Start hotkey
 * of OptionsButton, the strategic view the Select hotkey (level6). How Unity
 * 5.3 numbers Android's KEYCODE_BUTTON_* is not documented, so it is asked:
 * with [debug] log_buttons, every frame the engine's own Input.GetKey
 * (the icall, captured in dcr_icall_hooks.c) is read for JoystickButton0-19
 * and Escape, and every change is logged next to the Android key code the
 * port sent (dcr_input.c). Main thread only (Unity's input API). MIT.
 */
#include <stdint.h>

#include "dcr_config.h"
#include "util.h"

typedef int (*get_key_fn)(int keycode);
get_key_fn sh_i_GetKeyInt;

#define KC_ESCAPE 27
#define KC_JOY0 330 /* KeyCode.JoystickButton0 */
#define NKEYS 21

void sh_padprobe_frame(void) {
  if (!dcr_config()->log_buttons || !sh_i_GetKeyInt)
    return;
  static uint8_t prev[NKEYS];
  for (int i = 0; i < NKEYS; i++) {
    int kc = i < 20 ? KC_JOY0 + i : KC_ESCAPE;
    uint8_t now = sh_i_GetKeyInt(kc) ? 1 : 0;
    if (now != prev[i]) {
      prev[i] = now;
      if (i < 20)
        debugPrintf("[pad] Unity: joystick button %d %s\n", i, now ? "DOWN" : "up");
      else
        debugPrintf("[pad] Unity: Escape %s\n", now ? "DOWN" : "up");
    }
  }
}
