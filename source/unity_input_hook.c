/* unity_input_hook.c -- feed Switch touch straight into UnityEngine.Input (Sonic Dash).
 *
 * Unity 6's nativeInjectEvent path is a dead end here: it ACCEPTS our fake MotionEvent
 * (inject_ret=1) but never reads its coordinates, so Input.touchCount / GetTouch /
 * mousePosition stay empty and the game's UI never sees a tap. Instead we bypass the JNI
 * event system entirely and patch the game's own il2cpp Input methods to return OUR touch
 * state directly. The game reads these every frame, so it sees the Switch touchscreen.
 *
 * RVAs come from sd_il2cpp_offsets.h, GENERATED from THIS build's dump.cs by
 * tools/derive_il2cpp_hooks.py (the inherited table held Colour Sheep's RVAs, which
 * lie past the end of this game's libil2cpp.so). runtime = il2cpp load_virtbase + RVA.
 * UnityEngine.Touch was checked against this build's dump.cs: 14 fields, 0x44 bytes,
 * offsets identical to NxTouch below -- and _Static_asserts now hold that in place.
 *
 * (The reference's LanguageParam.getCurrentLanguage hook is REMOVED: that is a Layton
 * game-specific class.)
 */
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "config.h"   /* SD_FULL_RES_RENDER / SD_ANALYTICS_OFF -- without it both
                       * #if blocks below silently evaluated to 0 and neither hook
                       * was ever built (found 2026-09; the audit now runs -Wundef) */
#include "unity_input_hook.h"

int   debugPrintf(char *fmt, ...);
extern int screen_width, screen_height;
int so_patch_code(void *dst, const void *src, unsigned long len);   /* so_util.c */

/* ---- touch state, written by android_native_feed_hid every frame ----------
 * Multi-touch: g_hook_touch[0..g_hook_count) are the fingers Unity will see this frame
 * (Color Sheep needs several at once -- you hold multiple colour pads together). The
 * mouse-emulation globals mirror the PRIMARY finger so the GetMouseButton* hooks and any
 * mouse-driven UI keep working exactly as before. */
typedef struct { int id; float x, y; int phase; float dx, dy; } HookTouch;  /* phase: 0 Began 1 Moved 2 Stationary 3 Ended */
static HookTouch g_hook_touch[NX_MAX_TOUCH];

int   g_hook_count = 0;                 /* Input.touchCount (0..NX_MAX_TOUCH)            */
int   g_hook_btn   = 0;                 /* Input.GetMouseButton(0)  (primary finger)     */
int   g_hook_btn_down = 0, g_hook_btn_up = 0;  /* GetMouseButtonDown/Up(0), 1-frame edges */
int   g_hook_phase = 3;                 /* primary finger's TouchPhase                   */
float g_hook_x = 0.0f, g_hook_y = 0.0f; /* Unity screen space (bottom-left origin, px)   */

/* ---- Unity value types (AArch64 return conventions matter) ---------------- */
typedef struct { float x, y; }    NxV2;
typedef struct { float x, y, z; } NxV3;                 /* HFA -> s0,s1,s2      */
typedef struct {                                        /* UnityEngine.Touch, 0x44 bytes */
  int32_t m_FingerId;       NxV2 m_Position;   NxV2 m_RawPosition; NxV2 m_PositionDelta;
  float   m_TimeDelta;      int32_t m_TapCount; int32_t m_Phase;   int32_t m_Type;
  float   m_Pressure;       float m_maxPressure; float m_Radius;   float m_RadiusVariance;
  float   m_AltitudeAngle;  float m_AzimuthAngle;
} NxTouch;                                               /* > 16 bytes -> sret (x8) */
/* UnityEngine.Touch in this build's dump.cs (6000.0.72f1). GetTouch returns this BY
 * VALUE, so a drifted layout would hand the game wrong positions or phases with no
 * crash to point at -- broken swipes. Fail the build instead. */
_Static_assert(sizeof(NxTouch) == 0x44,                      "UnityEngine.Touch is 0x44 bytes");
_Static_assert(__builtin_offsetof(NxTouch, m_Position) == 0x04, "Touch.m_Position @0x04");
_Static_assert(__builtin_offsetof(NxTouch, m_TapCount) == 0x20, "Touch.m_TapCount @0x20");
_Static_assert(__builtin_offsetof(NxTouch, m_Phase)    == 0x24, "Touch.m_Phase @0x24");
_Static_assert(__builtin_offsetof(NxTouch, m_Pressure) == 0x2C, "Touch.m_Pressure @0x2C");
_Static_assert(__builtin_offsetof(NxTouch, m_AzimuthAngle) == 0x40, "Touch.m_AzimuthAngle @0x40");

/* ---- the hooks: il2cpp calling convention = (real args..., MethodInfo*) ---- */
/* ---- touch-path instrumentation -------------------------------------------
 * Every link of the chain logs, rate-limited, so a single debug.log shows where
 * touch stops:  HID sees a finger  ->  hook state  ->  the game polls
 * Input.touchCount  ->  the game reads Input.GetTouch.  Added after a boot where
 * no UI responded and the log could not say which link had failed. */
static unsigned s_tc_calls, s_tc_nonzero, s_gt_logged, s_hid_logged;

static void touch_log_hid(const char *what, int id, float x, float y) {
  if (s_hid_logged < 12) {
    s_hid_logged++;
    debugPrintf("[touch] HID finger %d %s at (%.0f,%.0f)%s\n", id, what, x, y,
                s_tc_calls ? "" : "  -- but the game has NOT polled touchCount yet");
  }
}

static int32_t hk_touchCount(void *mi){
  (void)mi;
  s_tc_calls++;
  if (g_hook_count > 0) s_tc_nonzero++;
  if (s_tc_calls == 1)
    debugPrintf("[touch] game is polling Input.touchCount (first call)\n");
  else if ((s_tc_calls % 1200) == 0)   /* ~4 calls a frame -> about every 5 s */
    debugPrintf("[touch] touchCount polled %u times, %u of them with touches\n",
                s_tc_calls, s_tc_nonzero);
  return g_hook_count;
}

/* The gate outside our other hooks. The game's StandaloneInputModuleNoMouse
 * reports itself supported only if  m_ForceModuleActive || Input.mousePresent ||
 * Input.touchSupported,  and the EventSystem never runs an unsupported module --
 * so if libunity's native layer said "no touchscreen, no mouse", no touch could
 * reach any UI however well the other hooks worked. The Switch has a
 * touchscreen, so this is the truthful answer, not a workaround. */
static uint8_t hk_touchSupported(void *mi){
  (void)mi;
  static int logged;
  if (!logged) { logged = 1; debugPrintf("[touch] Input.touchSupported -> true (input module can activate)\n"); }
  return 1;
}
static NxV3    hk_mousePosition(void *mi){ (void)mi; NxV3 v = { g_hook_x, g_hook_y, 0.0f }; return v; }
static uint8_t hk_getMouseButton(int32_t b, void *mi){ (void)mi; return (b==0) ? (uint8_t)g_hook_btn : 0; }
static uint8_t hk_getMouseButtonDown(int32_t b, void *mi){ (void)mi; return (b==0) ? (uint8_t)g_hook_btn_down : 0; }
static uint8_t hk_getMouseButtonUp(int32_t b, void *mi){ (void)mi; return (b==0) ? (uint8_t)g_hook_btn_up : 0; }
static NxTouch hk_getTouch(int32_t index, void *mi){
  (void)mi; NxTouch t; memset(&t, 0, sizeof t);
  if (index >= 0 && index < g_hook_count) {
    const HookTouch *h = &g_hook_touch[index];
    t.m_FingerId   = h->id;                     /* stable per finger while it is down */
    t.m_Position.x = h->x; t.m_Position.y = h->y;
    t.m_RawPosition = t.m_Position;
    t.m_Phase = h->phase; t.m_TapCount = 1;
    t.m_Pressure = 1.0f; t.m_maxPressure = 1.0f;
    /* Unity's own UI derives drag deltas from positions, but custom widgets --
     * scroll pickers especially -- often read touch.deltaPosition directly. It
     * was always zero here, which lets taps work while every drag goes nowhere. */
    t.m_PositionDelta.x = h->dx; t.m_PositionDelta.y = h->dy;
    t.m_TimeDelta = 1.0f / 60.0f;
    if (s_gt_logged < 8) {
      s_gt_logged++;
      debugPrintf("[touch] game read GetTouch(%d): id=%d phase=%d pos=(%.0f,%.0f) delta=(%.1f,%.1f)\n",
                  (int)index, h->id, h->phase, h->x, h->y, h->dx, h->dy);
    }
  }
  return t;
}

/* Overwrite a method entry with an absolute long jump to `target`:
 *   ldr x16, #8 ; br x16 ; .quad target      (16 bytes) */
static void patch_jump(uintptr_t site, void *target) {
  uint32_t code[4];
  code[0] = 0x58000050u;                 /* ldr x16, #8  (load target from site+8) */
  code[1] = 0xd61f0200u;                 /* br  x16                                */
  memcpy(&code[2], &target, sizeof target);
  so_patch_code((void *)site, code, sizeof code);
}

/* ---- guarded hooks ------------------------------------------------------
 * The RVAs come from sd_il2cpp_offsets.h, GENERATED from this build's dump.cs
 * by tools/derive_il2cpp_hooks.py, which verifies the dump matches this
 * libil2cpp.so, bounds-checks every target into executable code, rejects
 * identical-code folding, and requires 16 bytes of room for the jump stub.
 *
 * THIS REPLACES A DANGEROUS INHERITED TABLE. It carried Colour Sheep's RVAs
 * (0x2A2D83C ...) with no guard. Sonic Dash's libil2cpp maps only 0x2615770
 * bytes, so those addresses were past the end of the module: every boot would
 * have written jump stubs into whatever memory followed it.
 *
 * Each hook re-checks its first two instruction words at runtime, and each set
 * is ALL-OR-NOTHING: nothing is written unless every required target verifies. */
#include "sd_il2cpp_offsets.h"

typedef struct { const char *what; uint32_t rva, w0, w1; void *fn; int required; } SdHook;
#define SD_HOOK(M, fn, req) { #M, SD_IL2CPP_##M##_RVA, SD_IL2CPP_##M##_W0, SD_IL2CPP_##M##_W1, (void *)(fn), (req) }

static int hooks_verify(uintptr_t base, const SdHook *h, int n, const char *set) {
  int ok = 1;
  for (int i = 0; i < n; i++) {
    if (!h[i].rva) {
      if (h[i].required) {
        debugPrintf("[%s] REQUIRED %s is not in this build\n", set, h[i].what);
        ok = 0;
      }
      continue;
    }
    const volatile uint32_t *p = (const volatile uint32_t *)(base + h[i].rva);
    if (p[0] != h[i].w0 || p[1] != h[i].w1) {
      debugPrintf("[%s] REFUSED %s @libil2cpp+0x%x: %08x %08x, expected %08x %08x\n",
                  set, h[i].what, h[i].rva, p[0], p[1], h[i].w0, h[i].w1);
      ok = 0;
    }
  }
  if (!ok)
    debugPrintf("[%s] NOT INSTALLED -- libil2cpp.so is not the build these offsets came "
                "from. Re-run tools/derive_il2cpp_hooks.py against your dump.cs.\n", set);
  return ok;
}

static int hooks_apply(uintptr_t base, const SdHook *h, int n) {
  int done = 0;
  for (int i = 0; i < n; i++)
    if (h[i].rva) { patch_jump(base + h[i].rva, h[i].fn); done++; }
  return done;
}

/* The game's own UI resolution, read from UnityEngine.Screen. Touch positions
 * must be in THIS space: when the game renders at a scaled size and Unity
 * software-blits it up to the window, Screen reports the scaled size and native
 * input converts touches into it -- a conversion our hooks bypass. Only called
 * once the game has polled touchCount, which proves il2cpp is running; the
 * getters' first two words are verified before the first call. */
static uintptr_t s_il2cpp_base;
int nx_input_game_screen(int *w, int *h) {
  static int ok = -1;                          /* -1 unchecked, 0 unusable, 1 verified */
  if (!s_il2cpp_base || !s_tc_calls) return 0;
  if (ok < 0) {
    const volatile uint32_t *pw = (const volatile uint32_t *)(s_il2cpp_base + SD_IL2CPP_SC_get_width_RVA);
    const volatile uint32_t *ph = (const volatile uint32_t *)(s_il2cpp_base + SD_IL2CPP_SC_get_height_RVA);
    ok = SD_IL2CPP_SC_get_width_RVA && SD_IL2CPP_SC_get_height_RVA &&
         pw[0] == SD_IL2CPP_SC_get_width_W0  && pw[1] == SD_IL2CPP_SC_get_width_W1 &&
         ph[0] == SD_IL2CPP_SC_get_height_W0 && ph[1] == SD_IL2CPP_SC_get_height_W1;
    if (!ok) debugPrintf("[touch] Screen getters failed their guards -- touch scale "
                         "falls back to the geometry request\n");
  }
  if (!ok) return 0;
  *w = ((int32_t (*)(void *))(s_il2cpp_base + SD_IL2CPP_SC_get_width_RVA))(NULL);
  *h = ((int32_t (*)(void *))(s_il2cpp_base + SD_IL2CPP_SC_get_height_RVA))(NULL);
  return *w > 0 && *h > 0;
}

void nx_install_input_hooks(uintptr_t il2cpp_base) {
  s_il2cpp_base = il2cpp_base;
  /* Sonic Dash reads legacy UnityEngine.Input (there is no UnityEngine.InputSystem
   * namespace in its dump), so these six are the whole of its touch input. */
  static const SdHook h[] = {
    SD_HOOK(IN_get_touchCount,     hk_touchCount,         1),
    SD_HOOK(IN_GetTouch,           hk_getTouch,           1),
    SD_HOOK(IN_get_mousePosition,  hk_mousePosition,      1),
    SD_HOOK(IN_GetMouseButton,     hk_getMouseButton,     1),
    SD_HOOK(IN_GetMouseButtonDown, hk_getMouseButtonDown, 1),
    SD_HOOK(IN_GetMouseButtonUp,   hk_getMouseButtonUp,   1),
    SD_HOOK(IN_get_touchSupported, hk_touchSupported,     0),
  };
  const int n = (int)(sizeof h / sizeof h[0]);
  if (!hooks_verify(il2cpp_base, h, n, "input")) return;
  debugPrintf("[input] %d/%d Input hooks installed (touchCount/GetTouch/mouse*)\n",
              hooks_apply(il2cpp_base, h, n), n);
}

/* Called by android_native_feed_hid with EVERY finger currently down, already mapped to Unity
 * screen space (bottom-left origin, game pixels). Phases are derived by matching HID finger ids
 * against last frame: a new id is Began, a known id is Moved (or Stationary if it didn't move),
 * and an id that disappeared is reported for exactly ONE more frame as Ended at its last
 * position -- Unity's contract, and what lets the game see a tap complete.
 * Fingers keep their slot order, so Input.GetTouch(i) is stable across frames. */
void nx_input_hook_update_multi(const NxTouchIn *in, int n) {
  static HookTouch prev[NX_MAX_TOUCH];
  static int prev_n = 0;
  HookTouch cur[NX_MAX_TOUCH];
  int cn = 0;

  if (n < 0) n = 0;
  if (n > NX_MAX_TOUCH) n = NX_MAX_TOUCH;

  /* 1) every finger that is down now: Began / Moved / Stationary */
  for (int i = 0; i < n; i++) {
    const HookTouch *was = NULL;
    for (int j = 0; j < prev_n; j++)
      if (prev[j].id == in[i].id && prev[j].phase != 3) { was = &prev[j]; break; }

    cur[cn].id = in[i].id;
    cur[cn].x  = in[i].x;
    cur[cn].y  = in[i].y;
    if (was) {
      float dx = in[i].x - was->x, dy = in[i].y - was->y;
      cur[cn].phase = (dx*dx + dy*dy > 0.25f) ? 1 /*Moved*/ : 2 /*Stationary*/;
      cur[cn].dx = dx; cur[cn].dy = dy;
    } else {
      cur[cn].phase = 0 /*Began*/;
      cur[cn].dx = cur[cn].dy = 0.0f;
      touch_log_hid("Began", in[i].id, in[i].x, in[i].y);
    }
    cn++;
  }

  /* 2) fingers that were down last frame and are now gone: one Ended frame each */
  for (int j = 0; j < prev_n && cn < NX_MAX_TOUCH; j++) {
    if (prev[j].phase == 3) continue;              /* already reported Ended -> drop it */
    int still_down = 0;
    for (int i = 0; i < n; i++)
      if (in[i].id == prev[j].id) { still_down = 1; break; }
    if (!still_down) {
      cur[cn] = prev[j];
      cur[cn].phase = 3 /*Ended*/;
      cur[cn].dx = cur[cn].dy = 0.0f;
      touch_log_hid("Ended", prev[j].id, prev[j].x, prev[j].y);
      cn++;
    }
  }

  /* 3) publish to the hooks */
  for (int i = 0; i < cn; i++) g_hook_touch[i] = cur[i];
  g_hook_count = cn;

  /* 4) mouse emulation mirrors the primary finger (unchanged behaviour for mouse-driven UI) */
  static int prev_active = 0;
  int active = (n > 0);
  g_hook_btn_down = (active && !prev_active);
  g_hook_btn_up   = (!active && prev_active);
  g_hook_btn      = active;
  if (cn > 0) {
    g_hook_x = cur[0].x; g_hook_y = cur[0].y;
    g_hook_phase = cur[0].phase;
  } else {
    g_hook_phase = 3;
  }
  prev_active = active;

  /* 5) remember this frame */
  for (int i = 0; i < cn; i++) prev[i] = cur[i];
  prev_n = cn;
}

/* Single-touch wrapper (stick-cursor / A-button path, which has only one pointer). */
void nx_input_hook_update(int active, float ux, float uy) {
  /* id 1000: deliberately outside the HID finger-id range (0..15) so switching between the
   * stick cursor and a real finger is seen as a new finger (Began), not a jump of an old one. */
  NxTouchIn t = { 1000, ux, uy };
  nx_input_hook_update_multi(active ? &t : NULL, active ? 1 : 0);
}

/* ---- PlayerPrefs persistence ---------------------------------------------
 * Saves go via UnityEngine.PlayerPrefs, but Unity's native PlayerPrefs never
 * reaches disk on Switch, so progress lived only in RAM. We replace Set/Get/Delete/Save so
 * the game's PlayerPrefs go through our persistent store (unity_jni.c prefs.kv), loaded on
 * boot and flushed to the SD. RVAs come from sd_il2cpp_offsets.h (this build's dump.cs). */
extern void        nx_prefs_set(char type, const char *key, const char *val);   /* unity_jni.c */
extern const char *nx_prefs_get(const char *key);
extern void        nx_prefs_del(const char *key);
extern void        nx_prefs_flush(void);

static void *(*g_il2cpp_string_new)(const char *);

/* il2cpp System.String (arm64): _stringLength @0x10 (int32), _firstChar (UTF-16) @0x14
 * (confirmed against dump.cs). Malloc'd UTF-8 copy (caller frees). */
static char *il2str_dup(void *s) {
  if (!s) return NULL;
  int32_t len = *(int32_t *)((char *)s + 0x10);
  if (len < 0) len = 0;
  char *out = (char *)malloc((size_t)len * 3 + 1);
  if (!out) return NULL;
  const uint16_t *ch = (const uint16_t *)((char *)s + 0x14);
  int o = 0;
  for (int i = 0; i < len; i++) {
    uint32_t c = ch[i];
    if (c < 0x80)        out[o++] = (char)c;
    else if (c < 0x800){ out[o++] = (char)(0xC0|(c>>6));  out[o++] = (char)(0x80|(c&0x3F)); }
    else               { out[o++] = (char)(0xE0|(c>>12)); out[o++] = (char)(0x80|((c>>6)&0x3F)); out[o++] = (char)(0x80|(c&0x3F)); }
  }
  out[o] = 0;
  return out;
}
static void *mkstr(const char *s) { return g_il2cpp_string_new ? g_il2cpp_string_new(s ? s : "") : (void *)0; }

/* il2cpp static-method ABI: (real args..., MethodInfo*). */
static void hk_pp_SetString(void *key, void *val, void *mi) {
  (void)mi; char *k = il2str_dup(key), *v = il2str_dup(val);
  if (k) nx_prefs_set('S', k, v ? v : "");
  free(k); free(v);
}
static void hk_pp_SetInt(void *key, int32_t val, void *mi) {
  (void)mi; char *k = il2str_dup(key), b[16];
  if (k) { snprintf(b, sizeof b, "%d", (int)val); nx_prefs_set('I', k, b); }
  free(k);
}
static void hk_pp_SetFloat(void *key, float val, void *mi) {
  (void)mi; char *k = il2str_dup(key), b[32];
  if (k) { snprintf(b, sizeof b, "%.9g", (double)val); nx_prefs_set('F', k, b); }
  free(k);
}
static void *hk_pp_GetString2(void *key, void *def, void *mi) {
  (void)mi; char *k = il2str_dup(key); const char *v = k ? nx_prefs_get(k) : NULL; free(k);
  return v ? mkstr(v) : def;
}
static void *hk_pp_GetString1(void *key, void *mi) {
  (void)mi; char *k = il2str_dup(key); const char *v = k ? nx_prefs_get(k) : NULL; free(k);
  return mkstr(v ? v : "");
}
static int32_t hk_pp_GetInt2(void *key, int32_t def, void *mi) {
  (void)mi; char *k = il2str_dup(key); const char *v = k ? nx_prefs_get(k) : NULL; free(k);
  return v ? (int32_t)atoi(v) : def;
}
static int32_t hk_pp_GetInt1(void *key, void *mi) {
  (void)mi; char *k = il2str_dup(key); const char *v = k ? nx_prefs_get(k) : NULL; free(k);
  return v ? (int32_t)atoi(v) : 0;
}
static float hk_pp_GetFloat2(void *key, float def, void *mi) {
  (void)mi; char *k = il2str_dup(key); const char *v = k ? nx_prefs_get(k) : NULL; free(k);
  return v ? (float)atof(v) : def;
}
static uint8_t hk_pp_HasKey(void *key, void *mi) {
  (void)mi; char *k = il2str_dup(key); int h = (k && nx_prefs_get(k)); free(k); return h ? 1 : 0;
}
static void hk_pp_DeleteKey(void *key, void *mi) {
  (void)mi; char *k = il2str_dup(key); if (k) nx_prefs_del(k); free(k);
}
static void hk_pp_Save(void *mi) { (void)mi; nx_prefs_flush(); }

/* PlayerPrefs -> prefs.kv. Unity's Android PlayerPrefs goes through
 * SharedPreferences over the fake JNI and never reaches disk, so without these
 * hooks progress is lost on exit.
 *
 * ALL-OR-NOTHING, and stricter than the donor. The inherited code hooked the
 * setters even when it had to skip GetString (il2cpp_string_new unresolved) --
 * writes then went to prefs.kv while reads went to Unity's empty native store,
 * so the game could never read back its own save. Here, if string_new is
 * missing or any required target fails its guard, NONE are installed and the
 * game uses the native path consistently.
 *
 * GetInt(string) has no compiled body in this build (see sd_il2cpp_offsets.h),
 * so it is optional; its callers reach GetInt(string,int), which is hooked. */
void nx_install_playerprefs_hooks(uintptr_t il2cpp_base, void *string_new) {
  static const SdHook h[] = {
    SD_HOOK(PP_SetString,  hk_pp_SetString,  1), SD_HOOK(PP_SetInt,     hk_pp_SetInt,     1),
    SD_HOOK(PP_SetFloat,   hk_pp_SetFloat,   1), SD_HOOK(PP_GetString2, hk_pp_GetString2, 1),
    SD_HOOK(PP_GetString1, hk_pp_GetString1, 1), SD_HOOK(PP_GetInt2,    hk_pp_GetInt2,    1),
    SD_HOOK(PP_GetInt1,    hk_pp_GetInt1,    0), SD_HOOK(PP_GetFloat2,  hk_pp_GetFloat2,  1),
    SD_HOOK(PP_HasKey,     hk_pp_HasKey,     1), SD_HOOK(PP_DeleteKey,  hk_pp_DeleteKey,  1),
    SD_HOOK(PP_Save,       hk_pp_Save,       1),
  };
  const int n = (int)(sizeof h / sizeof h[0]);
  if (!string_new) {
    debugPrintf("[prefs] il2cpp_string_new unresolved -- NO prefs hooks (a partial set "
                "would split writes and reads across two stores)\n");
    return;
  }
  g_il2cpp_string_new = (void *(*)(const char *))string_new;
  if (!hooks_verify(il2cpp_base, h, n, "prefs")) return;
  debugPrintf("[prefs] %d PlayerPrefs hooks installed -> prefs.kv\n", hooks_apply(il2cpp_base, h, n));
}

/* ---- game tweaks: each its own hook set, each behind a config.h switch ------ */

/* AppFlow.SetResolutionScale(float): the game lowers its own render resolution
 * (75% -> 1440x810 through Screen.SetResolution) and Unity blit-scales it back
 * up. Ignoring the call keeps rendering at the native 1920x1080. */
static void hk_setResolutionScale(void *self, float scale, void *mi) {
  (void)self; (void)mi;
  static int n;
  if (n++ < 3) debugPrintf("[tweak] AppFlow.SetResolutionScale(%.2f) ignored -- rendering at 100%%\n", scale);
}

/* AnalyticsSystem.CanAnalyticsEventsBeCollected(event): the gate every event
 * passes (CanPushAnalyticsEvent, CanPushAnalyticsEventToManualQueue). "No" is
 * the game's own opt-out path: nothing is queued, sent, failed -- or appended
 * to failed_events.log, which reached 187 KB in one offline session. */
static uint8_t hk_analyticsOff(void *self, void *ev, void *mi) {
  (void)self; (void)ev; (void)mi;
  return 0;
}

void nx_install_tweak_hooks(uintptr_t il2cpp_base) {
#if SD_FULL_RES_RENDER
  { static const SdHook h[] = { SD_HOOK(AF_SetResolutionScale, hk_setResolutionScale, 1) };
    if (hooks_verify(il2cpp_base, h, 1, "render-scale"))
      debugPrintf("[tweak] full-resolution rendering: %d hook installed\n", hooks_apply(il2cpp_base, h, 1)); }
#endif
#if SD_ANALYTICS_OFF
  { static const SdHook h[] = { SD_HOOK(AN_CanCollect, hk_analyticsOff, 1) };
    if (hooks_verify(il2cpp_base, h, 1, "analytics"))
      debugPrintf("[tweak] analytics collection off (no failed_events.log): %d hook installed\n", hooks_apply(il2cpp_base, h, 1)); }
#endif
  (void)il2cpp_base;
}
