/* sh_imports_extra.c -- imports Unity 5.3's libunity has that the Unity 5.6
 * port never needed (runtime/tools/gen_imports.py binds them by name).
 *
 *   AInputQueue_* / AMotionEvent_* / AInputEvent_getSource
 *                  the NativeActivity input path (UnityPlayerNativeActivity).
 *                  This port drives UnityPlayer.nativeInjectEvent instead
 *                  (dcr_input.c), so no queue is ever attached: the queue has
 *                  no events, and a motion event, were one ever handed over,
 *                  reads as empty.
 *   ANativeWindow_lock / unlockAndPost
 *                  software blits to the window: refused, the engine renders
 *                  with GLES.
 *   pthread_attr_setschedpolicy
 *                  accepted and ignored, as setschedparam is (dcr_sched.c
 *                  decides priorities and cores).
 *   isnan          bionic exports it as a function; newlib only as a macro.
 *   wprintf        stdout is the log; wide output is dropped.
 * MIT.
 */
#include <math.h>
#include <stdint.h>

#include "util.h"

/* ------------------------------------------------------------ input queue */
void b_AInputQueue_attachLooper(void *q, void *looper, int ident, void *cb, void *data) {}
void b_AInputQueue_detachLooper(void *q) {}
int32_t b_AInputQueue_getEvent(void *q, void **ev) {
  if (ev)
    *ev = NULL;
  return -1;
}
int32_t b_AInputQueue_preDispatchEvent(void *q, void *ev) { return 0; }
void b_AInputQueue_finishEvent(void *q, void *ev, int handled) {}

int32_t b_AInputEvent_getSource(const void *e) { return 0; }

/* ----------------------------------------------------------- motion events */
int32_t b_AMotionEvent_getAction(const void *e) { return 0; }
int32_t b_AMotionEvent_getFlags(const void *e) { return 0; }
int32_t b_AMotionEvent_getMetaState(const void *e) { return 0; }
int32_t b_AMotionEvent_getEdgeFlags(const void *e) { return 0; }
int64_t b_AMotionEvent_getDownTime(const void *e) { return 0; }
int64_t b_AMotionEvent_getEventTime(const void *e) { return 0; }
float b_AMotionEvent_getXPrecision(const void *e) { return 1.0f; }
float b_AMotionEvent_getYPrecision(const void *e) { return 1.0f; }
uint32_t b_AMotionEvent_getPointerCount(const void *e) { return 0; }
int32_t b_AMotionEvent_getPointerId(const void *e, uint32_t i) { return 0; }
float b_AMotionEvent_getX(const void *e, uint32_t i) { return 0.0f; }
float b_AMotionEvent_getY(const void *e, uint32_t i) { return 0.0f; }
float b_AMotionEvent_getPressure(const void *e, uint32_t i) { return 0.0f; }
float b_AMotionEvent_getSize(const void *e, uint32_t i) { return 0.0f; }
float b_AMotionEvent_getTouchMajor(const void *e, uint32_t i) { return 0.0f; }
float b_AMotionEvent_getTouchMinor(const void *e, uint32_t i) { return 0.0f; }
float b_AMotionEvent_getToolMajor(const void *e, uint32_t i) { return 0.0f; }
float b_AMotionEvent_getToolMinor(const void *e, uint32_t i) { return 0.0f; }
float b_AMotionEvent_getOrientation(const void *e, uint32_t i) { return 0.0f; }
size_t b_AMotionEvent_getHistorySize(const void *e) { return 0; }
int64_t b_AMotionEvent_getHistoricalEventTime(const void *e, size_t h) { return 0; }
float b_AMotionEvent_getHistoricalX(const void *e, uint32_t i, size_t h) { return 0.0f; }
float b_AMotionEvent_getHistoricalY(const void *e, uint32_t i, size_t h) { return 0.0f; }
float b_AMotionEvent_getHistoricalPressure(const void *e, uint32_t i, size_t h) { return 0.0f; }
float b_AMotionEvent_getHistoricalSize(const void *e, uint32_t i, size_t h) { return 0.0f; }

/* ------------------------------------------------------------------ window */
int32_t b_ANativeWindow_lock(void *w, void *buf, void *dirty) {
  debugPrintf("[ndk] ANativeWindow_lock refused (GLES only)\n");
  return -1;
}
int32_t b_ANativeWindow_unlockAndPost(void *w) { return -1; }

/* ------------------------------------------------------------------- libc */
int b_pthread_attr_setschedpolicy(void *attr, int policy) { return 0; }

int b_isnan(double x) { return __builtin_isnan(x); }

int b_wprintf(const void *fmt, ...) { return 0; }
