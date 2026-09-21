/* util.c -- misc utility functions
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#include "util.h"
#include "config.h"

// File-only logger (DEBUG_LOG builds only): open once + flush per line so the tail survives a
// crash, mutex-serialised across engine threads. Drops the high-frequency dlsym/dlopen/JNI spam.
#if DEBUG_LOG
static Mutex g_log_lock;
static int log_is_noisy(const char *t) {
#if SD_JNI_LOG
  /* JNI logging is ON (config.h): nothing is dropped. This filter used to hide
   * every "[jni]" / "JNI:" / "dlopen" / "dlsym" line -- including the class-pool
   * and id-pool exhaustion warnings and every UNIMPL-slot hit. */
  (void)t;
  return 0;
#else
  return !strncmp(t, "dlsym", 5) || !strncmp(t, "dlopen", 6) ||
         !strncmp(t, "JNI ", 4)  || !strncmp(t, "JNI:", 4) || !strncmp(t, "[jni]", 5);
#endif
}
#endif

/* Take and release the log lock. Used by the GC bridge before it freezes a
 * thread: if we can take this, nobody is mid-debugPrintf holding the stdio and
 * heap locks, so freezing them will not deadlock the collector against its own
 * diagnostics. A thread that starts logging after we return blocks on a lock
 * we are not holding, which is fine.
 *
 * An accessor rather than un-static'ing g_log_lock, so the lock stays owned by
 * the one file that uses it. */
void log_lock_barrier(void) {
#if DEBUG_LOG
  mutexLock(&g_log_lock);
  mutexUnlock(&g_log_lock);
#endif
}

/* diag_log -- one formatted line, newline appended.
 *
 * sd_patches.c, sd_imports.c and sd_jni.c were written against this name and
 * it did not exist anywhere in the tree, so none of the three would link. It
 * is a thin wrapper rather than a rename because every call site passes a
 * message WITHOUT a trailing newline, and debugPrintf writes exactly what it
 * is given -- aliasing the two would run every patch line into the next. */
void diag_log(const char *fmt, ...) {
#if DEBUG_LOG
  char line[512];
  va_list va;
  va_start(va, fmt);
  int n = vsnprintf(line, sizeof line - 1, fmt, va);
  va_end(va);
  if (n < 0) return;
  if (n > (int)sizeof line - 2) n = (int)sizeof line - 2;
  line[n] = '\n'; line[n + 1] = '\0';
  debugPrintf("%s", line);
#else
  (void)fmt;
#endif
}

int debugPrintf(char *text, ...) {
#if DEBUG_LOG
  static FILE *f = NULL;
  va_list list;
  if (log_is_noisy(text)) return 0;
  mutexLock(&g_log_lock);
  if (!f) f = fopen(LOG_NAME, "a");
  if (f) { va_start(list, text); vfprintf(f, text, list); va_end(list); fflush(f); }
  mutexUnlock(&g_log_lock);
#else
  (void)text;
#endif
  return 0;
}

// Per-thread bionic TLS. The engine reads its stack canary from tpidr_el0+0x28;
// every thread that runs engine code needs its OWN zeroed block here. A single
// shared block races: one thread's TLS writes (including the guard slot) corrupt
// another thread's in-flight canary, tripping a false __stack_chk_fail. `buf`
// must outlive the thread (TPIDR_EL0 points into it until the thread exits).
void install_bionic_tls(void *buf) {
  memset(buf, 0, BIONIC_TLS_SIZE);
  armSetTlsRw((uint8_t *)buf + BIONIC_TLS_TP_OFFSET);
}

// boost the CPU to 1785MHz while loading
void cpu_boost(int on) {
  appletSetCpuBoostMode(on ? ApmCpuBoostMode_FastLoad : ApmCpuBoostMode_Normal);
}

int ret0(void) { return 0; }

int retm1(void) { return -1; }
