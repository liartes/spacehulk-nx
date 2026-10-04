/* dcr_jni_unity.h -- Unity's side of the JNI world: Java proxies, loopers and
 * handlers (jni_proxy.c), on the runtime's JNI core (runtime/source/jni.h).
 * MIT.
 */
#ifndef DCR_JNI_UNITY_H
#define DCR_JNI_UNITY_H
#include "jni.h"

/* Java proxies and loopers (jni_proxy.c). */
int jni_is_proxy(const JObj *o);
jvalue jni_proxy_call(JObj *self, JMethod *m, const jvalue *args);
void jni_looper_bind_engine(void); /* the frame-loop thread owns the engine looper */
void jni_looper_run_engine(void);  /* run its due posts; call between frames */
JNI_H_DECL(jni_h_newInterfaceProxy); JNI_H_DECL(jni_h_disableInterfaceProxy);
JNI_H_DECL(jni_h_method_getName); JNI_H_DECL(jni_h_method_getDeclaringClass);
JNI_H_DECL(jni_h_method_getReturnType); JNI_H_DECL(jni_h_method_getParameterTypes);
JNI_H_DECL(jni_h_method_getModifiers); JNI_H_DECL(jni_h_method_equals);
JNI_H_DECL(jni_h_method_hashCode); JNI_H_DECL(jni_h_method_toString);
JNI_H_DECL(jni_h_findLibrary);
JNI_H_DECL(jni_h_getMainLooper); JNI_H_DECL(jni_h_myLooper); JNI_H_DECL(jni_h_runOnUiThread);
JNI_H_DECL(jni_h_handler_init); JNI_H_DECL(jni_h_handler_post); JNI_H_DECL(jni_h_handler_postDelayed);
JNI_H_DECL(jni_h_handler_removeCallbacks); JNI_H_DECL(jni_h_handler_getLooper);
JNI_H_DECL(jni_h_rh_getMethodID); JNI_H_DECL(jni_h_rh_getFieldID);
JNI_H_DECL(jni_h_rh_getConstructorID); JNI_H_DECL(jni_h_rh_newProxyInstance);

/* Drop the local refs of MotionEvent.obtain() copies made during an injection. */
void jni_release_obtained(void);

#endif
