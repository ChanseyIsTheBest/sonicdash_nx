/* android_native_unity.c -- the 27 NDK symbols libunity.so imports, for the
 * ZOOKEEPER DX Switch port. Unity is NOT a NativeActivity, so unlike cr3_nx's
 * android_native.c there is no ANativeActivity glue / android_main / AInputQueue
 * here: the engine is driven by the JNI-registered natives (see main.c). We only
 * provide the raw NDK functions libunity calls directly:
 *
 *   ANativeWindow_acquire/_release/_fromSurface/_setBuffersGeometry/
 *                _getWidth/_getHeight/_getFormat      -> libnx NWindow
 *   ALooper_prepare/_acquire/_release/_pollOnce/_wake/_forThread
 *                                                     -> condvar wait/wake
 *   ASensorManager_ , ASensorEventQueue_ , ASensor_   -> "no sensors"
 *
 * IMPORTANT context-ownership note: the engine creates its OWN EGL context from
 * the ANativeWindow (cr3_nx's main.c creates none). The host must NOT create an
 * SDL_GL / EGL context. Use SDL for audio + HID only. Delete the
 * SDL_GL_SetAttribute/SDL_GL_CreateContext/SDL_GL_SwapWindow calls from the
 * earlier main_skeleton.c; the engine calls eglSwapBuffers itself.
 *
 * Needs devkitA64 + libnx (switch.h) + switch-mesa. Not host-compilable.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <switch.h>
#include "util.h"   /* debugPrintf */
#include "config.h" /* screen_width / screen_height */
#include "unity_input_hook.h"
#include "nx_pointer.h"
#include "sd_tate.h"
#include "libc_shim.h"  /* fopen_fake / fclose_fake (locked file I/O) */

#ifndef AWINDOW_FORMAT_RGBA_8888
#define AWINDOW_FORMAT_RGBA_8888 1
#endif

/* opaque NDK types -> concrete libnx instances */
typedef struct ANativeWindow ANativeWindow;     /* == NWindow* at runtime */
typedef struct ALooper       ALooper;

/* ==========================================================================
 * dock-aware screen state (also read by unity_jni.c's Display getters)
 * ========================================================================== */
static u32 g_w = 1280, g_h = 720;         /* RENDER size: what the engine believes it has */
static u32 g_win_w = 1280, g_win_h = 720; /* WINDOW size: always landscape -- the Switch will
                                           * not scan out a portrait layer; sd_tate.c rotates
                                           * the portrait render onto it when config.txt says so */
extern int screen_width, screen_height;   /* the render resolution main.c resolved (config/auto) */

void android_native_update_mode(void){
  /* The NWindow buffer MUST match the resolution Unity renders/reports (screen_width/height),
   * or the game renders at one size into a buffer of another -> zoom/crop. main.c sets those
   * from config.txt or the 1440x2560 default; mirror them here. */
  /* One size in both modes (config.txt resolution); the Switch scales it. */
  if (screen_width > 0 && screen_height > 0) { g_w = (u32)screen_width; g_h = (u32)screen_height; }
  if (sd_tate_enabled()) { g_win_w = g_h; g_win_h = g_w; }   /* portrait render, landscape window */
  else                   { g_win_w = g_w; g_win_h = g_h; }
}
u32 android_native_window_width(void)  { return g_win_w; }
u32 android_native_window_height(void) { return g_win_h; }
u32 android_native_width(void)  { return g_w; }
u32 android_native_height(void) { return g_h; }

/* ==========================================================================
 * ANativeWindow  ->  libnx NWindow
 * ========================================================================== */
/* fbstub45: pin the displayed region to exactly the dimensions Unity renders
 * into. nwindowSetDimensions may allocate a width-aligned (e.g. 720 -> 768)
 * swapchain buffer; without a matching crop the compositor can scan the extra
 * uninitialized columns, which shows up as the image being "cut off" / garbage
 * on the right edge. Cropping to (0,0,bw,bh) guarantees only the rendered
 * content is presented. */
/* LANDSCAPE, NO ROTATION. Layton renders landscape (confirmed on hardware), so the buffer
 * (bw x bh, 16:9) maps 1:1 to the landscape panel with the identity transform -- no compositor
 * rotation. (The port originally assumed portrait and rotated 90deg, which was the whole
 * "zoomed/tiny" bug.) */
/* Remember the last geometry so we can re-assert it (see nx_window_reassert). */
static u32 g_geom_bw = 1920, g_geom_bh = 1080;

static void nx_window_set_geom(NWindow *w, u32 bw, u32 bh) {
  g_geom_bw = bw; g_geom_bh = bh;
  nwindowSetDimensions(w, bw, bh);
  nwindowSetCrop(w, 0, 0, bw, bh);
  nwindowSetTransform(w, 0u);   /* identity: landscape buffer -> landscape panel */
  static int nlog = 0;
  if (nlog < 6) {
    nlog++;
    u32 aw = 0, ah = 0;
    nwindowGetDimensions(w, &aw, &ah);
    debugPrintf("[gfx] set_geom: render %ux%u, nwindow reports %ux%u, transform=0 (landscape)\n",
                bw, bh, aw, ah);
  }
}

/* switch-mesa creates its swapchain inside eglCreateWindowSurface and may reset the NWindow's
 * crop/dimensions to its own defaults; re-assert our crop (and identity transform) afterwards
 * so only the rendered content is presented. Cheap: two property writes. */
void nx_window_reassert(void) {
  NWindow *w = nwindowGetDefault();
  nwindowSetCrop(w, 0, 0, g_geom_bw, g_geom_bh);
  nwindowSetTransform(w, 0u);
}

ANativeWindow *android_native_window(void){
  NWindow *w = nwindowGetDefault();
  nx_window_set_geom(w, g_win_w, g_win_h);   /* the WINDOW, landscape even when rotated */
  return (ANativeWindow *)w;
}
void     ANativeWindow_acquire(ANativeWindow *w){ (void)w; }                 /* singleton: refcount no-op */
void     ANativeWindow_release(ANativeWindow *w){ (void)w; }
ANativeWindow *ANativeWindow_fromSurface(void *env, void *surface){
  (void)env; (void)surface; return android_native_window();               /* one surface == our window */
}
int32_t  ANativeWindow_getWidth (ANativeWindow *w){ (void)w; return (int32_t)g_w; }
int32_t  ANativeWindow_getHeight(ANativeWindow *w){ (void)w; return (int32_t)g_h; }
int32_t  ANativeWindow_getFormat(ANativeWindow *w){ (void)w; return AWINDOW_FORMAT_RGBA_8888; }
static u32 g_req_w, g_req_h;   /* last size the game asked to render at; 0 = none yet */
int32_t  ANativeWindow_setBuffersGeometry(ANativeWindow *w, int32_t width, int32_t height, int32_t format){
  (void)format;
  /* FIXED-SIZE WINDOW -- datadefense_nx's rule. The second hardware boot
   * showed the splash zoomed in: Sonic Dash asked for a 75% render scale
   * (1440x810). nwindowSetDimensions could not resize the live swapchain, so the
   * window kept reporting 1920x1080 -- but nwindowSetCrop DID take, and the
   * panel showed only the top-left 1440x810 of each frame. Meanwhile Unity, seeing
   * the size unchanged, logged "Hardware resolution scaling not supported,
   * falling back to software scaling (blit)" and filled the whole 1920x1080.
   * Android devices with no hardware scaler behave exactly like this rule: the
   * call succeeds, the readback stays native, and Unity's blit takes over. And
   * actually resizing is worse -- datadefense_nx records mesa building a
   * swapchain the display never consumes, hanging the first eglSwapBuffers. */
  if (width > 0 && height > 0) {
    g_req_w = (u32)width; g_req_h = (u32)height;   /* the game's render size -- touch fallback */
    if ((u32)width != g_w || (u32)height != g_h) {
      static int logged;
      if (logged++ < 4)
        debugPrintf("[gfx] setBuffersGeometry %dx%d REJECTED (fixed-size window stays %ux%u; "
                    "Unity blit-scales)\n", width, height, g_w, g_h);
      return 0;
    }
    nx_window_set_geom((NWindow *)w, g_win_w, g_win_h);   /* render size requested; window applied */
  }
  return 0;
}

/* ==========================================================================
 * ALooper -- Unity uses it as a per-thread wait/wake primitive (not real fd
 * polling), so a condvar-backed looper is sufficient. If the engine turns out
 * to register real fds, port cr3_nx's fake-fd PollItem layer in here.
 * ========================================================================== */
#define ALOOPER_POLL_WAKE     (-1)
#define ALOOPER_POLL_TIMEOUT  (-3)
#define MAX_LOOPERS 16

struct ALooper { Mutex m; CondVar cv; int signaled; int refs; u32 owner; int used; };
static struct ALooper g_loopers[MAX_LOOPERS];
static Mutex g_loopers_lock;
static int   g_loopers_init = 0;

static void loopers_once(void){ if(!g_loopers_init){ mutexInit(&g_loopers_lock); g_loopers_init=1; } }

static struct ALooper *looper_for(u32 tid, int create){
  loopers_once();
  mutexLock(&g_loopers_lock);
  for (int i=0;i<MAX_LOOPERS;i++) if (g_loopers[i].used && g_loopers[i].owner==tid){
    struct ALooper *l=&g_loopers[i]; mutexUnlock(&g_loopers_lock); return l; }
  if (create) for (int i=0;i<MAX_LOOPERS;i++) if (!g_loopers[i].used){
    struct ALooper *l=&g_loopers[i];
    l->used=1; l->owner=tid; l->signaled=0; l->refs=1;
    mutexInit(&l->m); condvarInit(&l->cv);
    mutexUnlock(&g_loopers_lock); return l; }
  mutexUnlock(&g_loopers_lock);
  return NULL;
}
static u32 cur_tid(void){ return (u32)(uintptr_t)threadGetCurHandle(); }

ALooper *ALooper_prepare(int opts){ (void)opts; return (ALooper *)looper_for(cur_tid(), 1); }
/* Unity 6's InitializeUILooper() does `if (ALooper_forThread()==NULL) { log
 * "Couldn't retrieve native ALooper for UI thread"; return; }` -- and leaves the
 * global NdkLooper singleton NULL. A later NdkLooper::CreateHandler() then calls
 * WaitForCreation() on that null pointer -> Data Abort (ldrb [null+0xe8]). On real
 * Android the UI thread already owns a Java-created looper; we have none, so return
 * a create-on-demand looper (like ALooper_pollOnce already does) so the NdkLooper
 * is constructed (its ctor sets the [+0xe8] "created" flag WaitForCreation reads). */
ALooper *ALooper_forThread(void){  return (ALooper *)looper_for(cur_tid(), 1); }
void     ALooper_acquire(ALooper *l){ struct ALooper *L=(void*)l; if(L){ mutexLock(&L->m); L->refs++; mutexUnlock(&L->m);} }
void     ALooper_release(ALooper *l){ struct ALooper *L=(void*)l; if(L){ mutexLock(&L->m); if(--L->refs<=0) L->used=0; mutexUnlock(&L->m);} }

void ALooper_wake(ALooper *l){
  struct ALooper *L=(void*)l; if(!L) return;
  mutexLock(&L->m); L->signaled=1; condvarWakeAll(&L->cv); mutexUnlock(&L->m);
}
int ALooper_pollOnce(int timeoutMillis, int *outFd, int *outEvents, void **outData){
  struct ALooper *L = (void*)looper_for(cur_tid(), 1);
  if (outFd) *outFd=0;
  if (outEvents) *outEvents=0;
  if (outData) *outData=NULL;
  mutexLock(&L->m);
  if (!L->signaled){
    if (timeoutMillis==0){ mutexUnlock(&L->m); return ALOOPER_POLL_TIMEOUT; }
    if (timeoutMillis<0)  condvarWait(&L->cv,&L->m);
    else condvarWaitTimeout(&L->cv,&L->m,(u64)timeoutMillis*1000000ull);
  }
  int was = L->signaled; L->signaled=0;
  mutexUnlock(&L->m);
  return was ? ALOOPER_POLL_WAKE : ALOOPER_POLL_TIMEOUT;
}
/* Unity rarely uses these two, but provide them for completeness. */
int ALooper_addFd(ALooper *l,int fd,int ident,int events,void *cb,void *data){
  (void)l;(void)fd;(void)ident;(void)events;(void)cb;(void)data; return 1; }
int ALooper_removeFd(ALooper *l,int fd){ (void)l;(void)fd; return 1; }

/* ==========================================================================
 * Sensors -- report none. (CR3 imported no ASensorManager; Unity does, so these
 * must exist and return a clean empty state rather than be missing symbols.)
 * ========================================================================== */
void *ASensorManager_getInstance(void){ static int x; return &x; }
void *ASensorManager_getInstanceForPackage(const char *p){ (void)p; return ASensorManager_getInstance(); }
int   ASensorManager_getSensorList(void *m, void **list){ (void)m; if(list)*list=NULL; return 0; }
void *ASensorManager_getDefaultSensor(void *m, int type){ (void)m;(void)type; return NULL; }
void *ASensorManager_createEventQueue(void *m, void *looper, int ident, void *cb, void *data){
  (void)m;(void)looper;(void)ident;(void)cb;(void)data; static int q; return &q; }
int   ASensorManager_destroyEventQueue(void *m, void *q){ (void)m;(void)q; return 0; }

int   ASensorEventQueue_enableSensor (void *q, const void *s){ (void)q;(void)s; return -1; }
int   ASensorEventQueue_disableSensor(void *q, const void *s){ (void)q;(void)s; return 0; }
int   ASensorEventQueue_setEventRate (void *q, const void *s, int32_t us){ (void)q;(void)s;(void)us; return 0; }
int   ASensorEventQueue_getEvents    (void *q, void *ev, size_t n){ (void)q;(void)ev;(void)n; return 0; }
int   ASensorEventQueue_hasEvents    (void *q){ (void)q; return 0; }

const char *ASensor_getName      (const void *s){ (void)s; return ""; }
const char *ASensor_getVendor    (const void *s){ (void)s; return ""; }
int         ASensor_getType      (const void *s){ (void)s; return 0; }
float       ASensor_getResolution(const void *s){ (void)s; return 0.0f; }
int         ASensor_getMinDelay  (const void *s){ (void)s; return 0; }

/* cr3 dead-handler stub: no orientation sensor -> report level. */
void android_get_orientation(float *x, float *y, float *z){
  if (x) *x = 0.0f;
  if (y) *y = 0.0f;
  if (z) *z = 0.0f;
}

/* ==========================================================================
 * HID polling -> Unity input.
 * Unity ingests input through the Java UnityPlayer (touch -> nativeInjectEvent /
 * key path). The exact native event struct is engine-internal: recover the
 * registered "injectEvent"/"nativePointer*" method from libunity's JNI_OnLoad
 * (PORTING_PLAN.md S4) and fill in feed_one_touch(). Until then this reads HID
 * but doesn't yet hand it to the engine.
 * ========================================================================== */
/* HID -> Unity input. TOUCH ONLY: the Switch touchscreen passes straight through (all fingers)
 * into il2cpp UnityEngine.Input via the hooks in unity_input_hook.c.
 *
 * There is deliberately NO virtual cursor. This is a touch title: the stick-driven
 * cursor was removed in both docked and handheld (it cluttered the screen and the game isn't
 * playable that way). Docked therefore has no pointer input -- play in handheld. */

static float g_last_tx = 360, g_last_ty = 640;    /* last touch (game space), reused on release */

/* Bridge the dual-pointer module's line logger to the port's debug log. */
static void nxp_log_line(const char *msg){ debugPrintf("%s", msg); }

void android_native_input_init(void){
  /* ONE on-screen cursor, identical to bloonspop_nx's (nx_pointer.c, copied
   * unchanged apart from the rotation calls). It owns the pad, the touchscreen,
   * a USB mouse and the gyro; everything arrives through nxp_poll() below.
   *   +  cursor on/off (on by default docked, off handheld)   -  gyro on/off
   *   left stick moves   L/R recenter   A/ZR/ZL tap   D-pad up/down sensitivity
   * Sensitivities persist in pointer.cfg; an optional cursor.png is used if present. */
  NxpConfig cfg = {
    .screen_w = 0, .screen_h = 0,                   /* set below: the ENGINE render size    */
    .panel_w  = 1280, .panel_h  = 720,              /* Switch touch panel space             */
    .data_dir = NULL,   /* filled below: GAME_HOME, next to the .nro */
    .cursor_id = 1000,                              /* reserved id: cannot clash with a finger */
    .max_touch_slots = NX_MAX_TOUCH,
    .stick_speed = 0.0f,                            /* 0 => module default (14 px/frame)    */
    .mouse_sens  = 0.0f,
    .log = nxp_log_line,
    .fopen_fn  = fopen_fake,
    .fclose_fn = fclose_fake,
  };
  cfg.data_dir = GAME_HOME;
  /* RENDER space (portrait when rotated): touches arrive through
   * sd_tate_map_pointer already rotated into it, and the cursor is drawn into
   * the portrait framebuffer. main.c has set screen_width/height by now. */
  cfg.screen_w = screen_width  > 0 ? screen_width  : 1280;
  cfg.screen_h = screen_height > 0 ? screen_height : 720;
  debugPrintf("[input] pointer space %dx%d (render), panel %dx%d\n", cfg.screen_w, cfg.screen_h, cfg.panel_w, cfg.panel_h);
  nxp_init(&cfg);   /* also starts the six-axis (gyro) sensors */
}

void android_native_feed_hid(void){
  /* One update for the cursors; feeds the pad, moves cursors, tracks taps. */
  nxp_update();                                     /* reads pad/touch/mouse/gyro, builds events */

  const float SW = (screen_width  > 0) ? (float)screen_width  : 1280.0f;
  const float SH = (screen_height > 0) ? (float)screen_height : 720.0f;

  /* bloonspop_nx's conversion, verbatim: nx_pointer reports events in RENDER
   * space with a TOP-LEFT origin; Unity wants BOTTOM-LEFT, so flip Y and hand
   * every event to the multi-touch tracker. Touchscreen fingers and the cursor
   * share one path; a lifted pointer drops out next frame and the tracker emits
   * its Ended itself, so a quick tap is never lost. */
  NxpEvent ev[NX_MAX_TOUCH];
  int n = nxp_poll(ev, NX_MAX_TOUCH);
  NxTouchIn tin[NX_MAX_TOUCH];
  int tn = 0;
  for (int i = 0; i < n && tn < NX_MAX_TOUCH; i++) {
    float gx = ev[i].x;
    float gy = SH - ev[i].y;                         /* flip Y: top-left -> bottom-left */
    if (gx < 0.0f)      gx = 0.0f;
    if (gx > SW - 1.0f) gx = SW - 1.0f;
    if (gy < 0.0f)      gy = 0.0f;
    if (gy > SH - 1.0f) gy = SH - 1.0f;
    tin[tn].id = ev[i].id;
    tin[tn].x  = gx;
    tin[tn].y  = gy;
    tn++;
  }
  if (tn > 0) { g_last_tx = tin[0].x; g_last_ty = tin[0].y; }

  /* Into the GAME's UI space. Everything above is in window pixels (1920x1080),
   * but when the game renders at a scaled size and Unity blit-scales it (this
   * game asks for 1440x810), Screen.width/height and touch coordinates are in
   * the scaled space. Native input would convert; our hooks bypass native input.
   * Unscaled, every touch lands 33% further from the bottom-left corner than
   * where the finger is -- missing almost everything while touch still "works".
   * Source of truth is the game's own Screen, refreshed about once a second;
   * the geometry request is the fallback until the game is up. */
  {
    static unsigned frames;
    static float sx = 1.0f, sy = 1.0f;
    static int logged_w, logged_h;
    if ((frames++ % 60) == 0) {
      int gw = 0, gh = 0;
      const char *src = "Screen";
      if (!nx_input_game_screen(&gw, &gh)) {
        gw = g_req_w ? (int)g_req_w : screen_width;
        gh = g_req_h ? (int)g_req_h : screen_height;
        src = g_req_w ? "geometry request" : "window";
      }
      sx = (float)gw / (float)screen_width;
      sy = (float)gh / (float)screen_height;
      if (gw != logged_w || gh != logged_h) {
        logged_w = gw; logged_h = gh;
        debugPrintf("[touch] game UI space %dx%d (from %s), window %dx%d -> touch scale %.3f x %.3f\n",
                    gw, gh, src, screen_width, screen_height, sx, sy);
      }
    }
    for (int i = 0; i < tn; i++) { tin[i].x *= sx; tin[i].y *= sy; }
  }

  /* tn == 0 -> nothing down: update_multi emits Ended for anything that lifted. */
  nx_input_hook_update_multi(tin, tn);
}
