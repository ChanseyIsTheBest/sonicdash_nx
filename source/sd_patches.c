/* sd_patches.c -- engine patches and the engine clock, for Sonic Dash.
 *
 * A port of colorsheep_nx's cs_patches.c onto offsets derived from THIS
 * libunity.so (see sd_offsets.h for provenance of every value).
 *
 * WHY THIS FILE WAS REWRITTEN
 * An earlier revision replaced cs_patches.c with a smaller file that only
 * answered the Android probes. That silently dropped four things main.c still
 * depended on: the TimeManager clock detour, Choreographer free-run, the
 * procfs reader stubs, and the 60 Hz clock thread (main.c kept calling
 * cs_start_clock_thread(), whose definition no longer existed). It also
 * stubbed GetBigLittleConfiguration to 0, which reports zero cores and crashes
 * Unity's job scheduler. All of that is restored or corrected here.
 *
 * THE RULE: every write is preceded by a guard check against the words
 * actually present in YOUR binary. A mismatch is logged and REFUSED. A stale
 * offset after a game update costs one log line, not a jump into an unrelated
 * function.
 */
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <switch.h>

#include "so_util.h"
#include "util.h"
#include "config.h"
#include "jni_fake.h"
#include "sd_offsets.h"

static uintptr_t unity_base;
static int s_applied, s_refused;

/* --------------------------------------------------------------- helpers */

static inline volatile uint32_t *at(uintptr_t off) {
  return (volatile uint32_t *)(unity_base + off);
}

static int guard(const char *what, uintptr_t off, uintptr_t delta, uint32_t want) {
  uint32_t got = at(off)[delta / 4];
  if (got == want) return 1;
  diag_log("[patch] REFUSED %s @0x%lx: +0x%lx is 0x%08x, expected 0x%08x",
           what, (unsigned long)off, (unsigned long)delta, got, want);
  s_refused++;
  return 0;
}

static void write_words(const char *what, uintptr_t off, const uint32_t *w, size_t n) {
  so_patch_code((void *)(unity_base + off), w, n * sizeof(uint32_t));
  diag_log("[patch] %s @libunity+0x%lx (%u word%s)", what, (unsigned long)off,
           (unsigned)n, n == 1 ? "" : "s");
  s_applied++;
}

/* movz/movk x0 <- 64-bit value ; ret */
static void body_ret_x0(uint32_t out[5], uint64_t v) {
  out[0] = 0xd2800000u | ((uint32_t)( v        & 0xffff) << 5);
  out[1] = 0xf2a00000u | ((uint32_t)((v >> 16) & 0xffff) << 5);
  out[2] = 0xf2c00000u | ((uint32_t)((v >> 32) & 0xffff) << 5);
  out[3] = 0xf2e00000u | ((uint32_t)((v >> 48) & 0xffff) << 5);
  out[4] = 0xd65f03c0u;
}

/* movz w0, #imm16 ; ret */
static void patch_ret_w0(const char *what, uintptr_t off, uint32_t w0, uint32_t imm) {
  if (!guard(what, off, 0, w0)) return;
  uint32_t code[2] = { 0x52800000u | ((imm & 0xffff) << 5), 0xd65f03c0u };
  write_words(what, off, code, 2);
}

/* ================================================================ clock */
/* TimeManager::Update is detoured so the engine's clock is driven by real
 * monotonic time rather than whatever the Android frame source would have
 * supplied. Without it the clock does not advance and the game sits on a
 * black screen with frames rendering and nothing moving. */

static void (*g_tm_update_body)(void *, double);
static uint64_t g_clk_base_ns;
static void    *g_tm;
static Mutex    g_clock_lock;
static volatile uint64_t g_last_main_tick_ns;
#define CLOCK_STALL_NS 100000000ull   /* 100 ms of main-thread silence == stalled */

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void clock_tick(void *tm) {
  uint64_t n = now_ns();
  if (!g_clk_base_ns) g_clk_base_ns = n;
  if (g_tm_update_body) g_tm_update_body(tm, (double)(n - g_clk_base_ns) / 1e9);
}

/* Detour target. The 16-byte redirect overwrites the first four instructions
 * of Update, so their work is replayed here exactly: frameCount++ and aux++
 * happen BEFORE the pause test, as in the original, and a paused manager
 * returns without ticking. */
void sd_time_update_hook(void *tm, double ignored) {
  (void)ignored;
  g_tm = tm;
  g_last_main_tick_ns = now_ns();
  *(volatile uint64_t *)((char *)tm + SD_TM_FIELD_FRAMECOUNT) += 1;
  *(volatile uint32_t *)((char *)tm + SD_TM_FIELD_AUX)        += 1;
  if (*(volatile uint8_t *)((char *)tm + SD_TM_FIELD_PAUSE)) return;
  mutexLock(&g_clock_lock);
  clock_tick(tm);
  mutexUnlock(&g_clock_lock);
}

static void install_timemanager_clock(void) {
  const uintptr_t e = SD_OFF_TimeManager_Update;
  if (!guard("TimeManager::Update", e, 0, SD_TM_Update_W0) ||
      !guard("TimeManager::Update", e, 4, SD_TM_Update_W1) ||
      !guard("TimeManager::Update body", e, SD_TM_UPDATE_BODY_ENTRY, SD_TM_BODY_W0))
    return;
  g_tm_update_body = (void (*)(void *, double))(unity_base + e + SD_TM_UPDATE_BODY_ENTRY);
  uintptr_t hook = (uintptr_t)&sd_time_update_hook;
  uint32_t stub[4] = { 0x58000050u,               /* ldr x16, #8 */
                       0xd61f0200u,               /* br  x16     */
                       (uint32_t)(hook & 0xffffffffu), (uint32_t)(hook >> 32) };
  write_words("TimeManager::Update -> monotonic clock", e, stub, 4);
}

/* ======================================================= frame pacing */

static void install_choreographer_freerun(void) {
  const uintptr_t o = SD_OFF_ChoreographerBase_Get;
  if (!guard("ChoreographerBase::Get", o, 0, SD_CHOREO_GET_W0) ||
      !guard("ChoreographerBase::Get", o, 4, SD_CHOREO_GET_W1))
    return;
  uint32_t code[2] = { 0xd2800000u /* movz x0, #0 */, 0xd65f03c0u /* ret */ };
  write_words("ChoreographerBase::Get -> NULL (free-run)", o, code, 2);
}

static volatile uint64_t *g_vsync_counter;
static uintptr_t         *g_vsync_cond_slot;

static void wire_vsync_counter(void) {
  /* Trust the counter address only if WaitVSync still loads it. */
  if (!guard("WaitVSync counter load", SD_OFF_WaitVSync_ldr, 0, SD_WAITVSYNC_LDR) ||
      !guard("WaitVSync counter page", SD_OFF_WaitVSync_ldr - 4, 0, SD_WAITVSYNC_ADRP)) {
    diag_log("[vsync] counter NOT wired -- WaitVSync will rely on the 16 ms "
             "cond_wait cap alone, and pacing will be poor");
    return;
  }
  g_vsync_counter   = (volatile uint64_t *)(unity_base + SD_OFF_ANDROID_VSYNC_COUNTER);
  g_vsync_cond_slot = (uintptr_t *)(unity_base + SD_OFF_VSYNC_COND);
  diag_log("[vsync] live counter wired @libunity+0x%x", SD_OFF_ANDROID_VSYNC_COUNTER);
}

/* =================================================== Android probes */

static void install_biglittle(void) {
  const uintptr_t o = SD_OFF_GetBigLittleConfiguration;
  if (!guard("GetBigLittleConfiguration", o, 0, SD_GBL_W0)) return;
  /* x0 = {big=3, little=0}, x1 = {mask 0x7, 0}. NOT 0 -- see sd_offsets.h. */
  uint32_t code[3] = { 0xd2800000u | ((uint32_t)(SD_GBL_X0 & 0xffff) << 5),   /* movz x0,#3 */
                       0xd2800001u | ((uint32_t)(SD_GBL_X1 & 0xffff) << 5),   /* movz x1,#7 */
                       0xd65f03c0u };
  write_words("GetBigLittleConfiguration -> 3 cores", o, code, 3);
}

/* The cached /proc readers return a POINTER to a struct, so they are answered
 * with a pointer to a static struct we own. Consumers read [+0] available and
 * [+8] total (system), or [+8] (process). This removes a real /proc parse on
 * every caller. (colorsheep_nx first believed this was the fix for their random
 * boot crash; it was not -- the region patch was -- but it is kept for the
 * same reason they kept it.) */
static uint64_t g_sysmem[4]  __attribute__((aligned(16)));
static uint64_t g_procmem[4] __attribute__((aligned(16)));

static void install_procfs_readers(void) {
  g_sysmem[0]  = SD_SYSMEM_AVAIL_BYTES;
  g_sysmem[1]  = SD_SYSMEM_TOTAL_BYTES;
  g_procmem[0] = SD_PROCMEM_RESIDENT_BYTES;
  g_procmem[1] = SD_PROCMEM_RESIDENT_BYTES;
  uint32_t code[5];
  if (guard("GetCachedSystemMemoryInfo", SD_OFF_GetCachedSystemMemoryInfo, 0, SD_PROCFS_READER_W0)) {
    body_ret_x0(code, (uint64_t)(uintptr_t)g_sysmem);
    write_words("GetCachedSystemMemoryInfo -> static", SD_OFF_GetCachedSystemMemoryInfo, code, 5);
  }
  if (guard("GetCachedProcessMemoryInfo", SD_OFF_GetCachedProcessMemoryInfo, 0, SD_PROCFS_READER_W0)) {
    body_ret_x0(code, (uint64_t)(uintptr_t)g_procmem);
    write_words("GetCachedProcessMemoryInfo -> static", SD_OFF_GetCachedProcessMemoryInfo, code, 5);
  }
}

/* =============================================================== audio */

static void install_audio(void) {
  patch_ret_w0("GetAndroidAudioOutputType -> OpenSL", SD_OFF_GetAndroidAudioOutputType,
               SD_AUDIOOUT_W0, SD_AUDIOOUT_OPENSL);

  /* Both getters share their first two words; +0x10 is the BL that proves
   * which is which. Checked before either is touched. */
  if (guard("GetNativeOutputSampleRate", SD_OFF_GetNativeOutputSampleRate, 0x10, SD_AUDIOPROP_RATE_W4))
    patch_ret_w0("GetNativeOutputSampleRate", SD_OFF_GetNativeOutputSampleRate,
                 SD_AUDIOPROP_W0, SD_AUDIO_NATIVE_SAMPLE_RATE);
  if (guard("GetNativeOutputFramesPerBuffer", SD_OFF_GetNativeOutputFramesPerBuffer, 0x10, SD_AUDIOPROP_FRAMES_W4))
    patch_ret_w0("GetNativeOutputFramesPerBuffer", SD_OFF_GetNativeOutputFramesPerBuffer,
                 SD_AUDIOPROP_W0, SD_AUDIO_NATIVE_FRAMES_PER_BUFFER);

  /* FMOD's own setOutput argument. Guarded on the load AND the BL after it. */
  if (guard("FMOD setOutput site", SD_OFF_FMOD_SETOUTPUT_SITE, 0, SD_FMOD_SETOUTPUT_FROM) &&
      guard("FMOD setOutput site", SD_OFF_FMOD_SETOUTPUT_SITE, 4, SD_FMOD_SETOUTPUT_NEXT)) {
    uint32_t to = SD_FMOD_SETOUTPUT_TO;
    write_words("FMOD setOutput -> OpenSL (0x16)", SD_OFF_FMOD_SETOUTPUT_SITE, &to, 1);
  }
}

/* ===================================================== probes & misc */

static void install_misc(void) {
  patch_ret_w0("GetPhysicalMemoryMB", SD_OFF_GetPhysicalMemoryMB, SD_PHYSMEM_W0, SD_PHYSMEM_MB);

  if (guard("Swappy::UpdateSwapInterval", SD_OFF_Swappy_UpdateSwapInterval, 0, SD_SwappyUSI_W0)) {
    uint32_t r = 0xd65f03c0u;
    write_words("Swappy::UpdateSwapInterval -> ret", SD_OFF_Swappy_UpdateSwapInterval, &r, 1);
  }

#if SD_FORCE_60FPS
  /* Returns a FLOAT in s0. An earlier revision wrote an integer into w0 here,
   * which the caller never reads. 60.0f is not encodable as an fmov immediate
   * (the range tops out at 31.0), so it is built in w8 and moved across. */
  if (guard("GetVSyncBasedTargetFrameRate", SD_OFF_GetVSyncBasedTargetFrameRate, 0, SD_VSFR_W0) &&
      guard("GetVSyncBasedTargetFrameRate", SD_OFF_GetVSyncBasedTargetFrameRate, 4, SD_VSFR_W1)) {
    uint32_t code[3] = { 0x52a84e08u,   /* movz w8, #0x4270, lsl #16  (60.0f) */
                         0x1e270100u,   /* fmov s0, w8                        */
                         0xd65f03c0u }; /* ret                                */
    write_words("GetVSyncBasedTargetFrameRate -> 60.0f", SD_OFF_GetVSyncBasedTargetFrameRate, code, 3);
  }
#endif
}

/* ------------------------------------------------------------------------ */

int sd_install_engine_patches(uintptr_t base) {
  unity_base = base;
  s_applied = s_refused = 0;
  diag_log("[patch] libunity base 0x%lx", (unsigned long)base);

  install_timemanager_clock();
  install_choreographer_freerun();
  wire_vsync_counter();
  install_biglittle();
  install_procfs_readers();
  install_audio();
  install_misc();

  diag_log("[patch] %d applied, %d refused", s_applied, s_refused);
  if (s_refused)
    diag_log("[patch] GUARD FAILURES -- this libunity.so is not the build these offsets "
             "were derived from. Re-run tools/derive2.py; do not hand-edit sd_offsets.h.");
  return s_applied;
}

/* =========================================================== clock thread */
/* Two jobs, one thread:
 *   1. Tick the live vsync counter at 60 Hz, and wake WaitVSync.
 *   2. Drive the engine clock while the main thread is parked in a synchronous
 *      scene load (otherwise the per-frame hook owns the clock).
 *
 * No choreographer callbacks are dispatched here, because none can exist:
 * ChoreographerBase::Get is patched to NULL (free-run), and even if that patch
 * were refused, the inherited AChoreographer_getInstance returns NULL, which
 * ChoreographerNDK::Enable null-checks and falls back to loop cadence. */

extern int  pthread_cond_broadcast_fake(pthread_cond_t **cnd);

static Thread g_clock_thr;

static void sd_clock_thread(void *arg) {
  (void)arg;
  static uint8_t tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
  install_bionic_tls(tls);
  uint64_t last = 0;

  while (!jni_quit_requested) {
    svcSleepThread(4000000ull);                        /* ~4 ms poll */
    uint64_t n = now_ns();
    if (!last) last = n;

    int ticked = 0;
    while ((int64_t)(n - last) >= (int64_t)SD_VSYNC_PERIOD_NS) {
      if (g_vsync_counter) __atomic_add_fetch(g_vsync_counter, 1, __ATOMIC_RELAXED);
      last += SD_VSYNC_PERIOD_NS;
      ticked = 1;
    }
    if (ticked) {
      /* Wake WaitVSync now instead of when its 16 ms cap expires -- but only
       * once Unity has initialised this condvar itself (slot holds a real
       * heap pointer). Racing it through ensure_cond() could create two
       * conds and strand a waiter on the leaked one. */
      if (g_vsync_cond_slot && *(volatile uintptr_t *)g_vsync_cond_slot >= 0x10000)
        pthread_cond_broadcast_fake((pthread_cond_t **)g_vsync_cond_slot);
    }

    void *tm = g_tm;
    if (tm && g_tm_update_body && (now_ns() - g_last_main_tick_ns) > CLOCK_STALL_NS &&
        mutexTryLock(&g_clock_lock)) {                /* trylock: never invert order */
      clock_tick(tm);
      mutexUnlock(&g_clock_lock);
    }
  }
}

void sd_start_clock_thread(void) {
  if (R_SUCCEEDED(threadCreate(&g_clock_thr, sd_clock_thread, NULL, NULL, 0x8000, 0x2C, -2)))
    threadStart(&g_clock_thr);
  else
    diag_log("[clock] threadCreate FAILED -- no vsync, WaitVSync will crawl");
}
