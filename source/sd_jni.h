/* sd_jni.h -- the Java classes Sonic Dash reaches, answered by name.
 *
 * Forked from battd_nx's bp_jni.h (MIT) and slotted into jni_fake.c's
 * dispatch chain on the same contract: claim a class, answer by return kind,
 * ahead of unity_jni.c. See sd_jni.c for what each class is told and why.
 */
#ifndef SD_JNI_H
#define SD_JNI_H

#include <stdarg.h>
#include <stdint.h>

/* Unity 6 hands jni_fake ~90% of method IDs keyed to java/lang/Object,
 * because GetObjectClass() answers java/lang/Object for any non-Bitmap
 * object. Gating on id->cls alone would therefore never fire for instance
 * calls. The EFFECTIVE class falls back to the receiver's own class name. */
const char *sd_jni_eff_cls(const void *id, void *recv);

int  sd_jni_owns(const char *eff);          /* explicitly answered classes      */
int  sd_sdk_is_inert(const char *eff);      /* SDK "present but dormant" policy */
void *sd_sdk_inert_object(const void *id, const char *eff);

/* jv: the jvalue array when entered from a ...MethodA call, else NULL.
 * va: a VALID va_list when entered from a variadic/...V call, else NULL.
 * Never both. The host's ...A variants forward to the variadic ones WITHOUT
 * arguments, so a va_list reached that way holds garbage -- which is why the
 * A variants are intercepted before they forward (see jni_fake.c). */
void    *sd_jni_object(void *recv, const void *id, const char *eff, const void *jv, va_list *va);
uint64_t sd_jni_int   (void *recv, const void *id, const char *eff, const void *jv, va_list *va);
void     sd_jni_void  (void *recv, const void *id, const char *eff, const void *jv, va_list *va);

/* UnitySendMessage from libunity's exports; NULL disables deferred callbacks. */
void sd_jni_init(void (*unity_send_message)(const char *, const char *, const char *));

/* Main thread, once per frame, before nativeRender: delivers deferred
 * callbacks (the consent result) the way Android's Looper would -- on a later
 * frame, never re-entrantly inside the JNI call that requested them. */
void sd_jni_tick(void);

/* Log each distinct Java method once as [jnim] (called by jni_fake.c). */
void sd_jni_trace(const void *id, void *recv);

/* [jniret]: log our answer (kind o/i/f/v), first 3 calls per method. */
void sd_jni_trace_ret(const void *id, void *recv, char kind, uint64_t v, double f);

#endif /* SD_JNI_H */
