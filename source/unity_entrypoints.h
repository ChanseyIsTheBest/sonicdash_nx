/* unity_entrypoints.h -- UnityPlayer native method typedefs
 * (Unity 6000.0.72f1, arm64-v8a).
 *
 * Unlike the laytonbmr_nx base, this port resolves the UnityPlayer natives with
 * NO hardcoded offsets:
 *   - JNI_OnLoad is an EXPORTED symbol (readelf: JNI_OnLoad@@libunity) -> resolve
 *     by name via so_try_find_addr_rx(&unity_mod, "JNI_OnLoad").
 *   - initJni / nativeRender / nativeResume / ... are file-local, but libunity's
 *     JNI_OnLoad hands their {name -> fn} table to RegisterNatives, which our fake
 *     JNI captures (jni_fake.c: jni_lookup_unity_native). So we recover the exact
 *     runtime pointers by name after calling JNI_OnLoad -- version-proof, no diff.
 *
 * (The cross-version diff in tools/find_offsets.py placed these around 0x735xxx,
 * but the native* thunks share prologues and can't be disambiguated by diffing;
 * the RegisterNatives table is authoritative, so we use it.)
 *
 * Signatures match the Unity-6 UnityPlayer JNI ABI:
 *   - initJni is (env,thiz,Context,int)  -- 4-arg (the trailing int is a flags value)
 *   - nativeInjectEvent is (env,thiz,InputEvent)->Z  -- 3-arg, no trailing int
 */
#ifndef UNITY_ENTRYPOINTS_H
#define UNITY_ENTRYPOINTS_H

#include <stdint.h>
#include "so_util.h"

typedef void     (*fn_initJni)(void*,void*,void*,int32_t);   /* env,thiz,Context,int */
typedef void     (*fn_gfxstate)(void*,void*,int32_t,void*);
typedef void     (*fn_v)(void*,void*);
typedef uint8_t  (*fn_z)(void*,void*);
typedef void     (*fn_vz)(void*,void*,int32_t);
typedef uint8_t  (*fn_inject)(void*,void*,void*,int32_t);
typedef void     (*fn_orient)(void*,void*,int32_t,int32_t);
typedef int      (*fn_jnionload)(void* /*vm*/, void* /*reserved*/);

/* Names as they appear in the UnityPlayer RegisterNatives table. main.c looks each
 * up via jni_lookup_unity_native() after JNI_OnLoad has run. */
#define UNITY_NATIVE_initJni                  "initJni"
#define UNITY_NATIVE_nativeRecreateGfxState   "nativeRecreateGfxState"
#define UNITY_NATIVE_nativeSendSurfaceChanged "nativeSendSurfaceChangedEvent"
#define UNITY_NATIVE_nativeRender             "nativeRender"
#define UNITY_NATIVE_nativeInjectEvent        "nativeInjectEvent"
#define UNITY_NATIVE_nativeResume             "nativeResume"
#define UNITY_NATIVE_nativePause              "nativePause"
#define UNITY_NATIVE_nativeFocusChanged       "nativeFocusChanged"
#define UNITY_NATIVE_nativeDone               "nativeDone"
#define UNITY_NATIVE_nativeApplicationUnload  "nativeApplicationUnload"
#define UNITY_NATIVE_nativeUnityPlayerSetRunning "nativeUnityPlayerSetRunning"

#endif /* UNITY_ENTRYPOINTS_H */
