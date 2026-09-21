/* main.c -- Sonic Dash Nintendo Switch wrapper entry point.
 *
 * Unity 6000.0.72f1 / IL2CPP, arm64-v8a. Loads libmain + libunity + libil2cpp,
 * resolves their imports against native Switch implementations, and drives the
 * lifecycle the Java UnityPlayer normally runs (JNI_OnLoad -> initJni -> recreate
 * GFX -> surface changed -> resume/focus -> render loop). The UnityPlayer natives
 * are recovered BY NAME from libunity's RegisterNatives table (jni_fake.c), so no
 * per-function offsets are needed for them.
 *
 * Where the game-specific pieces live -- all derived from THIS game's binaries:
 *   sd_offsets.h / sd_patches.c   libunity engine patches, clock, vsync thread
 *   sd_region_patch.h             256MB->64MB allocator retile (16 sites)
 *   sd_gc.h / sd_gc.c             Boehm stop-the-world bridge (libil2cpp)
 *   sd_il2cpp_offsets.h           PlayerPrefs + Input hook targets (libil2cpp)
 *   sd_jni.c / sd_sdk_stubs.c     the game's Java and native-SDK surface
 * Every write into a game module is guarded by the instruction words actually
 * present at the site; tools/audit_guards.py verifies all of them offline.
 *
 * Forked from colorsheep_nx (itself from laytonbmr_nx / vln_nx, MIT). The
 * memory/heap/overcommit scaffolding below is inherited unchanged: it is an
 * engine-generation property, not a game property. The asset layout is this
 * game's: Addressables, no data.unity3d -- see check_data().
 */
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include <sys/stat.h>
#include <switch.h>
#include <SDL2/SDL.h>

#include "config.h"
#include "sd_gc.h"
#include "sd_saveedit.h"
#include "sd_config.h"
#include "sd_jni.h"
#include "util.h"
#include "error.h"
#include "so_util.h"
#include "imports.h"
#include "jni_fake.h"
#include "android_native_unity.h"
#include "opensles.h"
#include "unity_entrypoints.h"
#include "sd_region_patch.h"
#include "unity_input_hook.h"
#include "diag.h"
#include "sd_offsets.h"

#define DATA_ROOT  GAME_HOME
#define LIB_MAIN   "libmain.so"
#define LIB_UNITY  "libunity.so"
#define LIB_IL2CPP "libil2cpp.so"

/* sd_patches.c -- verify-before-patch engine patches, keyed on sd_offsets.h */
int  sd_install_engine_patches(uintptr_t unity_base);
void sd_start_clock_thread(void);   /* sd_patches.c */
/* jni_fake.c: recover a UnityPlayer native by name from the captured RegisterNatives table */
void *jni_lookup_unity_native(const char *name);
/* unity_glue.c */
void unity_environment_init(const char *data_root);

static void *heap_so_base = NULL;
static size_t heap_so_limit = 0;

/* mmap arena (consumed by mmap_fake/munmap_fake in libc_shim.c). */
void  *g_mmap_arena_base = NULL;
size_t g_mmap_arena_size = 0;
int    g_overcommit      = 0;
u64    g_alias_base = 0, g_alias_size = 0;
unsigned g_oc_heap_mb = 0, g_oc_freed_mb = 0;
int      g_oc_hint_map = 0, g_oc_hint_unmap = 0;
unsigned g_oc_alias_mb = 0;
void    *g_oc_win = NULL;
int      g_oc_probe_tried = 0, g_oc_shrink_tried = 0;
extern int oc_arena_init(void *window, size_t window_bytes, void *pool, size_t pool_bytes);
unsigned g_oc_probe_rc = 0, g_oc_shrink_rc = 0;
unsigned long g_oc_win_addr = 0;
u64      g_oc_sysres = 0;

so_module main_mod, unity_mod, il2cpp_mod;

/* This NRO's own image, captured once at boot by nro_range_init(). The first
 * hardware crash landed in our own code (sd_gc_arm) and the dump showed only raw
 * addresses, so the load base had to be solved for from register values before
 * anything could be symbolized. With the range known, the crash handler prints
 * "sonicdash_nx+0xOFFSET", and because the ELF is linked at address 0, OFFSET is
 * directly an address in sonicdash_nx.elf (e.g. aarch64-none-elf-addr2line). */
static uintptr_t g_nro_base, g_nro_end;

static void nro_range_init(void) {
  MemoryInfo mi; u32 pi;
  if (R_FAILED(svcQueryMemory(&mi, &pi, (u64)(uintptr_t)&nro_range_init))) return;
  g_nro_base = mi.addr;
  g_nro_end  = mi.addr + mi.size;
  /* text, then rodata, then data: contiguous code-typed regions */
  for (int i = 0; i < 4; i++) {
    if (R_FAILED(svcQueryMemory(&mi, &pi, g_nro_end))) break;
    if (mi.addr != g_nro_end || (mi.type != MemType_CodeStatic && mi.type != MemType_CodeMutable)) break;
    g_nro_end = mi.addr + mi.size;
  }
  debugPrintf("[boot] nro image 0x%lx..0x%lx -- crash addresses in it print as "
              "sonicdash_nx+offset (= address in sonicdash_nx.elf)\n",
              (unsigned long)g_nro_base, (unsigned long)g_nro_end);
}

/* Strong override of nx_crash_handler.c's weak stub: name addresses that fall inside
 * our loaded .so images (creport can't, since they aren't real modules) -- and
 * inside this NRO itself. */
int crash_resolve_module(uintptr_t addr, char *name_out, size_t name_cap, uintptr_t *base_out) {
  if (g_nro_base && addr >= g_nro_base && addr < g_nro_end) {
    snprintf(name_out, name_cap, "sonicdash_nx");
    *base_out = g_nro_base;
    return 1;
  }
  const struct { const char *n; so_module *m; } mods[] = {
    { "libmain.so", &main_mod }, { "libunity.so", &unity_mod }, { "libil2cpp.so", &il2cpp_mod },
  };
  for (unsigned i = 0; i < sizeof(mods)/sizeof(*mods); i++) {
    uintptr_t b = (uintptr_t)mods[i].m->load_virtbase;
    if (b && addr >= b && addr < b + mods[i].m->load_size) {
      snprintf(name_out, name_cap, "%s", mods[i].n);
      *base_out = b;
      return 1;
    }
  }
  return 0;
}

extern void nx_sd_flush(void);        /* libc_shim.c: periodic SD commit       */

/* audio warmup gate for opensles.c (frames since boot) */
static volatile uint32_t g_frame_count = 0;
uint32_t port_frame_count(void) { return g_frame_count; }

/* libunity ~19M + libil2cpp ~48M + headroom for relocated segments */
#define SO_REGION_BYTES (240u * 1024 * 1024)

/* ==========================================================================
 * Inherited memory/heap/overcommit scaffolding (verbatim from the vln/cr3_nx
 * base, MIT). Engine-generation-generic; not Color-Sheep-specific.
 * ========================================================================== */
static void *oc_find_stack_window(size_t want, size_t *out_size) {
  *out_size = 0;
  u64 sbase = 0, ssize = 0;
  svcGetInfo(&sbase, InfoType_StackRegionAddress, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&ssize, InfoType_StackRegionSize,    CUR_PROCESS_HANDLE, 0);
  if (!sbase || !ssize) return NULL;
  u64 end = sbase + ssize, a = sbase, best_a = 0, best_l = 0;
  int holes = 0, mapped = 0;
  while (a < end) {
    MemoryInfo mi; u32 pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, a))) break;
    u64 ms = mi.addr, me = mi.addr + mi.size;
    if (me <= a) break;
    if (mi.type == MemType_Unmapped) {
      u64 hs = ms < sbase ? sbase : ms, he = me > end ? end : me;
      if (he > hs) {
        if (he - hs > best_l) { best_l = he - hs; best_a = hs; }
        if (holes < 8)
          debugPrintf("[oc] stack hole %d: %p .. %p (%u MB)\n",
                      holes++, (void *)hs, (void *)he, (unsigned)((he - hs) >> 20));
      }
    } else mapped++;
    a = me;
  }
  debugPrintf("[oc] stack scan: base=%p size=%u MB, %d holes, %d mapped spans, largest=%u MB\n",
              (void *)sbase, (unsigned)(ssize >> 20), holes, mapped, (unsigned)(best_l >> 20));
  if (!best_a) return NULL;
  u64 aligned = (best_a + (MMAP_ARENA_ALIGN - 1)) & ~(MMAP_ARENA_ALIGN - 1);
  if (aligned >= best_a + best_l) return NULL;
  u64 avail = ((best_a + best_l) - aligned) & ~(MMAP_ARENA_ALIGN - 1);
  if (!avail) return NULL;
  if (avail > want) avail = want;
  *out_size = avail;
  return (void *)aligned;
}

static int overcommit_setup(void *addr, size_t size, size_t so_zone,
                            void **out_addr, size_t *out_fake) {
  (void)addr; (void)size; (void)so_zone; (void)out_addr; (void)out_fake;
  g_oc_hint_map   = envIsSyscallHinted(0x2c);
  g_oc_hint_unmap = envIsSyscallHinted(0x2d);
  svcGetInfo(&g_alias_base, InfoType_AliasRegionAddress, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&g_alias_size, InfoType_AliasRegionSize,    CUR_PROCESS_HANDLE, 0);
  g_oc_alias_mb = (unsigned)(g_alias_size >> 20);
  svcGetInfo(&g_oc_sysres, InfoType_SystemResourceSizeTotal, CUR_PROCESS_HANDLE, 0);
  return 0;   /* no system resource -> svcMapPhysicalMemory unusable; heap-backed */
}

void __libnx_initheap(void) {
  void *addr;
  size_t size = 0;
  size_t mem_available = 0, mem_used = 0;

  if (envHasHeapOverride()) {
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
  } else {
    svcGetInfo(&mem_available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    if (mem_available > mem_used + 0x200000)
      size = (mem_available - mem_used - 0x200000) & ~0x1FFFFF;
    if (size == 0)
      size = 0x2000000 * 16;
    Result rc = svcSetHeapSize(&addr, size);
    if (R_FAILED(rc))
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }

  const size_t MB = 1024 * 1024;
  size_t so_zone = SO_REGION_BYTES;
  if (so_zone > size / 2)
    so_zone = size / 2;

  extern char *fake_heap_start;
  extern char *fake_heap_end;

  void *oc_addr; size_t oc_fake;
  if (overcommit_setup(addr, size, so_zone, &oc_addr, &oc_fake)) {
    fake_heap_start = (char *)oc_addr;
    fake_heap_end   = (char *)oc_addr + oc_fake;
    heap_so_base    = (void *)ALIGN_MEM((uintptr_t)oc_addr + oc_fake, 0x1000);
    heap_so_limit   = so_zone;
    return;
  }

  const size_t big_align    = MMAP_ARENA_ALIGN;
  const size_t newlib_floor = 384 * MB;
  size_t arena_sz = MMAP_ARENA_RESERVE;
  size_t fake_heap_size;

  if (size > so_zone + big_align + newlib_floor + 256 * MB) {
    size_t avail = size - so_zone - big_align - newlib_floor;
    if (arena_sz > avail) arena_sz = avail & ~(big_align - 1);
    size_t usable    = size - so_zone - big_align;
    size_t arena_cap = ((usable * 30) / 100) & ~(big_align - 1);
    if (arena_sz > arena_cap) arena_sz = arena_cap;
    fake_heap_size = size - so_zone - arena_sz - big_align;
  } else {
    fake_heap_size = (size > so_zone) ? size - so_zone : size / 2;
    arena_sz = 0;
  }

  fake_heap_start = (char *)addr;
  fake_heap_end   = (char *)addr + fake_heap_size;

  heap_so_base  = (void *)ALIGN_MEM((uintptr_t)addr + fake_heap_size, 0x1000);
  heap_so_limit = so_zone;

  if (arena_sz) {
    g_mmap_arena_base = (void *)ALIGN_MEM((uintptr_t)heap_so_base + so_zone, big_align);
    g_mmap_arena_size = arena_sz;
  }
}

static void check_syscalls(void) {
  if (!envIsSyscallHinted(0x77)) fatal_error("svcMapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x78)) fatal_error("svcUnmapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x73)) fatal_error("svcSetProcessMemoryPermission is unavailable.");
  if (envGetOwnProcessHandle() == INVALID_HANDLE) fatal_error("Own process handle is unavailable.");
}

/* ==========================================================================
 * Color-Sheep-specific: data layout, module load, region no-op.
 * ========================================================================== */

/* Boot-time data check. This game uses Addressables: loose engine data under
 * assets/bin/Data/ (base APK) plus a catalogue and bundles under assets/aa/
 * (asset-pack APK). There is no data.unity3d -- the donor's single-bundle
 * layout does not apply here. */
static void check_data(void) {
  /* SONIC DASH HAS NO data.unity3d. The donor used the single-bundle layout;
   * this title is Addressables, so the engine data is loose under
   * assets/bin/Data/ (from the BASE apk) and the content lives in
   * assets/aa/ (from the ASSET PACK apk). Checking for data.unity3d here --
   * as the inherited list did -- fails at boot with a misleading message
   * naming a file that is not supposed to exist. */
  const char *files[] = {
    LIB_MAIN, LIB_UNITY, LIB_IL2CPP,
    "assets/bin/Data/globalgamemanagers",
    "assets/bin/Data/boot.config",
    "assets/bin/Data/unity_app_guid",
    "assets/bin/Data/Managed/Metadata/global-metadata.dat",
    /* The Addressables catalogue. Its absence means the asset pack was not
     * staged, which is the single most likely SD-card mistake: the base APK
     * alone looks complete and is not. */
    "assets/aa/catalog.bin",
    "assets/aa/catalog.hash",
    "assets/aa/settings.json",
  };
  char path[768];
  struct stat st;
  for (unsigned i = 0; i < sizeof(files)/sizeof(*files); i++) {
    snprintf(path, sizeof path, "%s/%s", DATA_ROOT, files[i]);
    if (stat(path, &st) < 0)
      fatal_error("Missing data file:\n%s\nCheck your SD card layout (see README.md).\n"
                  "Run tools/stage_sd.py to assemble it from your own APK(s).", files[i]);
  }

  /* Count the Addressables bundles. A catalogue with no bundles beside it is a
   * half-staged card -- it passes every check above and then fails much later,
   * inside the engine, as a content-load error with no mention of the SD card. */
  {
    char dir[768];
    snprintf(dir, sizeof dir, "%s/assets/aa/Android", DATA_ROOT);
    DIR *d = opendir(dir);
    int n = 0;
    if (d) {
      struct dirent *e;
      while ((e = readdir(d))) {
        const char *dot = strrchr(e->d_name, '.');
        if (dot && !strcmp(dot, ".bundle")) n++;
      }
      closedir(d);
    }
    if (n == 0)
      fatal_error("No Addressables bundles found in:\n%s\n\n"
                  "You almost certainly staged only the base APK. This title's\n"
                  "content ships in UnityDataAssetPack.apk -- pass it to\n"
                  "tools/stage_sd.py with --pack.", dir);
    debugPrintf("[boot] addressables: %d bundle(s)\n", n);
  }
}

static int load_module(so_module *mod, const char *name) {
  char path[768];
  snprintf(path, sizeof path, "%s/%s", DATA_ROOT, name);
  if (so_load(mod, path, heap_so_base, heap_so_limit) < 0)
    return -1;
  size_t used = ALIGN_MEM(mod->load_size, 0x1000);
  heap_so_base = (char *)heap_so_base + used;
  heap_so_limit -= used;
  crx_resolve_imports(mod);
  return 0;
}

/* Region-granularity patch: Unity's allocator re-tiled from 256 MB to 64 MB
 * blocks so its reservations fit a 4 GB console. 16 sites, derived from THIS
 * libunity.so by tools/scan_granularity.py and swept for completeness (see
 * sd_region_patch.h). ALL-OR-NOTHING: every `from` word is verified first and
 * nothing is written if any differs, because a partial patch that mixes 256 MB
 * and 64 MB paths corrupts the heap silently. */
static int nx_patch_unity_regions(uintptr_t ub) {
  int bad = 0;
  for (int i = 0; i < SD_REGION_PATCH_N; i++) {
    uint32_t cur = *(volatile uint32_t *)(ub + SD_REGION_PATCH[i].off);
    if (cur != SD_REGION_PATCH[i].from) {
      debugPrintf("[region] mismatch[%d] @+0x%x: have 0x%08x want 0x%08x\n",
                  i, (unsigned)SD_REGION_PATCH[i].off, cur, SD_REGION_PATCH[i].from);
      bad++;
    }
  }
  if (bad) {   /* all-or-nothing: a partial patch would mix 256MB/64MB paths -> corruption */
    debugPrintf("[region] %d/%d sites mismatched -> patching NOTHING (stock 256MB regions)\n",
                bad, SD_REGION_PATCH_N);
    return 0;
  }
  for (int i = 0; i < SD_REGION_PATCH_N; i++)
    so_patch_code((void *)(ub + SD_REGION_PATCH[i].off),
                  &SD_REGION_PATCH[i].to, sizeof SD_REGION_PATCH[i].to);
  debugPrintf("[region] granularity 256MB->64MB patched in-memory (%d sites)\n", SD_REGION_PATCH_N);
  return SD_REGION_PATCH_N;
}

/* engine entry points (resolved by name from RegisterNatives, post-JNI_OnLoad) */
static fn_initJni  Unity_initJni;
static fn_gfxstate Unity_nativeRecreateGfxState;
static fn_v        Unity_nativeSendSurfaceChanged;
static fn_z        Unity_nativeRender;
static fn_v        Unity_nativeResume;
static fn_vz       Unity_nativeFocusChanged;
static fn_z        Unity_nativeDone;
static fn_v        Unity_nativeApplicationUnload;
static fn_vz       Unity_nativeUnityPlayerSetRunning;

/* Save persistence: commit the SD periodically. (Saves go via PlayerPrefs -> prefs.kv;
 * the managed PlayerPrefs flush hook is a TODO -- see below -- but committing the SD
 * still persists whatever reached the prefs file.) */
static AppletHookCookie g_applet_cookie;
static void nx_applet_hook(AppletHookType hook, void *param) {
  (void)param;
  if (hook == AppletHookType_OnFocusState || hook == AppletHookType_OnExitRequest)
    nx_sd_flush();
}

int main(int argc, char *argv[]) {
  (void)argc; (void)argv;
  socketInitializeDefault();
  debugPrintf("[boot] === sonicdash_nx start (Unity 6000.0.72f1, libunity BuildID 6618ff361333edc4) ===\n");
  nro_range_init();

  /* CWD fix: title-override leaves cwd at the .nro folder or SD root; Unity reads many
   * files via relative paths, so chdir into DATA_ROOT. */
  {
    char cwd[256] = {0};
    getcwd(cwd, sizeof cwd);
    int rc = chdir(DATA_ROOT);
    struct stat st;
    int reach_meta = stat("assets/bin/Data/Managed/Metadata/global-metadata.dat", &st) == 0;
    int reach_data = stat("assets/aa/catalog.bin", &st) == 0;   /* Addressables catalogue */
    debugPrintf("[boot] cwd '%s' -> chdir(%s)=%d; reachable(rel): metadata=%d aa/catalog.bin=%d\n",
                cwd, DATA_ROOT, rc, reach_meta, reach_data);
  }

  /* Render resolution is fixed to 1080 in both docked and handheld, so we do NOT
   * generate a config.txt. A manually-placed config.txt is still honoured (a power
   * user can override without recompiling), but nothing is ever written -- absent
   * file just means "use the fixed default" rather than "create one". */
  sd_config_load();   /* config.txt: resolution, rotation, language -- before anything is sized */

  check_syscalls();
  debugPrintf("[boot] syscalls ok\n");
  {
    extern char *fake_heap_start, *fake_heap_end;
    debugPrintf("[boot] mem: newlib=%u MB, mmap arena=%u MB @ %p\n",
                (unsigned)((fake_heap_end - fake_heap_start) / (1024 * 1024)),
                (unsigned)(g_mmap_arena_size / (1024 * 1024)), g_mmap_arena_base);
    u64 tot = 0, used = 0;
    svcGetInfo(&tot,  InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize,  CUR_PROCESS_HANDLE, 0);
    debugPrintf("[boot] phys: total=%u MB used=%u MB free=%u MB\n",
                (unsigned)(tot >> 20), (unsigned)(used >> 20), (unsigned)((tot - used) >> 20));
  }

  /* Stack-region overcommit arena. Any failure -> heap-backed arena. */
  {
    void *pool = NULL;
    size_t winsz = 0;
    void *win = oc_find_stack_window(OC_WINDOW_BYTES, &winsz);
    VirtmemReservation *rv = NULL;
    if (win && winsz) {
      virtmemLock();
      rv = virtmemAddReservation(win, winsz);
      virtmemUnlock();
    }
    if (win && rv && winsz) {
      pool = memalign(0x1000, OC_POOL_BYTES);
      if (pool && oc_arena_init(win, winsz, pool, OC_POOL_BYTES))
        debugPrintf("[oc] ARMED: window %u MB @ %p, pool %u MB @ %p\n",
                    (unsigned)(winsz >> 20), win, (unsigned)(OC_POOL_BYTES >> 20), pool);
      else
        debugPrintf("[oc] DISABLED: pool=%p init failed -> heap-backed only\n", pool);
    } else {
      debugPrintf("[oc] DISABLED: no usable stack hole -> heap-backed only\n");
    }
  }

  /* Render resolution. Fixed at 1920x1080 in both modes (supersampled down on a
   * 720p handheld). No config.txt is generated; the only way to change this is to
   * drop a config.txt in by hand, which the block above still reads. */
  /* Resolution from config.txt -- the SAME in handheld and docked; the Switch
   * scales the picture to the panel or the TV. Rotated, the game gets a portrait
   * screen (p x p*16/9) that sd_tate.c turns onto the landscape window. */
  {
    const int p = sd_cfg_res, l = p * 16 / 9;
    if (sd_cfg_rotation) { screen_width = p; screen_height = l; }
    else                 { screen_width = l; screen_height = p; }
  }
  debugPrintf("[gfx] boot mode=%s render=%dx%d\n",
              appletGetOperationMode() == AppletOperationMode_Console ? "DOCKED" : "HANDHELD",
              screen_width, screen_height);

  SDL_SetMainReady();
  if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0)
    debugPrintf("SDL_Init failed: %s\n", SDL_GetError());

  check_data();

  sd_saveedit_run();   /* save_edit.txt -> save.txt, before the game reads it */

  /* Sweep Unity's case-sensitivity probe files: CASESENSITIVETEST<guid> strays from older
   * builds, plus the single hidden scratch the probe is now redirected to (libc_shim.c
   * casetest_redirect). Prevents junk accumulating on the SD card each launch. */
  {
    DIR *dd = opendir(DATA_ROOT);
    int swept = 0;
    if (dd) {
      struct dirent *de;
      while ((de = readdir(dd))) {
        if (strncasecmp(de->d_name, "CASESENSITIVETEST", 17) == 0 ||
            strcmp(de->d_name, ".casetest") == 0) {
          char pth[320]; snprintf(pth, sizeof pth, DATA_ROOT "/%s", de->d_name);
          if (unlink(pth) == 0) swept++;
        }
      }
      closedir(dd);
    }
    if (swept) debugPrintf("[boot] swept %d case-sensitivity probe file(s)\n", swept);
  }

  debugPrintf("[boot] loading modules...\n");
  if (load_module(&main_mod,   LIB_MAIN)   < 0) fatal_error("Could not load %s", LIB_MAIN);
  if (load_module(&unity_mod,  LIB_UNITY)  < 0) fatal_error("Could not load %s", LIB_UNITY);
  if (load_module(&il2cpp_mod, LIB_IL2CPP) < 0) fatal_error("Could not load %s", LIB_IL2CPP);
  debugPrintf("[boot] libmain=%p libunity=%p libil2cpp=%p\n",
              main_mod.load_virtbase, unity_mod.load_virtbase, il2cpp_mod.load_virtbase);

  so_finalize(&main_mod);   so_flush_caches(&main_mod);
  so_finalize(&unity_mod);  so_flush_caches(&unity_mod);
  so_finalize(&il2cpp_mod); so_flush_caches(&il2cpp_mod);
  debugPrintf("[boot] modules finalized + flushed\n");

  /* Arm the GC stop-the-world bridge. ORDER MATTERS: this reads guard words
   * through load_virtbase, and so_finalize() is what maps the image there --
   * before it, the bytes live only in the heap staging copy (load_base) and
   * load_virtbase is unmapped. The first hardware boot faulted exactly here
   * when this call sat above the finalize block. Arming here is still in time:
   * no libil2cpp code runs until so_execute_init_array(), below, so no
   * collection can have started. (battd_nx's rule, in its own words: "Set from
   * load_virtbase, not load_base: the offsets in the bridge are virtual
   * addresses within the mapped module.") */
  sd_gc_arm((uintptr_t)il2cpp_mod.load_virtbase, (size_t)il2cpp_mod.load_size);

  /* Engine patches (sd_patches.c): Android probes -> Switch-true answers.
   * Every site self-verifies its guard words against sd_offsets.h first and
   * REFUSES rather than writing on a mismatch. */
  {
    int n = sd_install_engine_patches((uintptr_t)unity_mod.load_virtbase);
    /* UnitySendMessage is a libunity export. sd_jni.c needs it to deliver the
     * consent callback that Hardlight's boot FSM waits on. */
    sd_jni_init((void (*)(const char *, const char *, const char *))
                so_try_find_addr_rx(&unity_mod, "UnitySendMessage"));
    debugPrintf("[boot] engine patches applied: %d (refusals, if any, are the [patch] REFUSED lines)\n", n);
  }

  /* Region granularity (no-op until re-derived; 8GB Switch runs fine at 256MB). */
  nx_patch_unity_regions((uintptr_t)unity_mod.load_virtbase);

  /* (A Colour Sheep patch stood here: it stubbed the Google Play Games plugin's
   * AndroidClient.Authenticate at a hardcoded RVA into THEIR libil2cpp.so. Sonic
   * Dash does not ship that plugin -- its sign-in is Hardlight's
   * SocialPlatformGooglePlayGames, which reaches Java and is answered by the SDK
   * dormant policy in sd_jni.c -- and the RVA was meaningless against this
   * module. Removed rather than retargeted.) */

  /* Main thread runs init_array + the engine lifecycle; give it a stable bionic TLS. */
  static uint8_t main_tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
  install_bionic_tls(main_tls);

  debugPrintf("[boot] running init arrays...\n");
  so_execute_init_array(&main_mod);
  so_execute_init_array(&unity_mod);
  so_execute_init_array(&il2cpp_mod);
  so_free_temp(&main_mod); so_free_temp(&unity_mod); so_free_temp(&il2cpp_mod);
  debugPrintf("[boot] init arrays done\n");

  jni_init();
  unity_environment_init(DATA_ROOT);
  android_native_update_mode();
  android_native_input_init();
  debugPrintf("[boot] jni + env + hid ready\n");

  install_bionic_tls(main_tls);

  extern void *fake_env, *fake_unityplayer_thiz, *fake_context_obj, *fake_surface_obj;
  extern void *fake_vm;

  /* Call libunity's real JNI_OnLoad(fake_vm) FIRST (exported symbol): runs
   * jni::Initialize() (caches the JavaVM) AND registers the UnityPlayer natives,
   * which our fake RegisterNatives captures for by-name resolution below. */
  {
    fn_jnionload Unity_JNI_OnLoad =
      (fn_jnionload)so_try_find_addr_rx(&unity_mod, "JNI_OnLoad");
    if (!Unity_JNI_OnLoad) fatal_error("libunity JNI_OnLoad not found");
    debugPrintf("[boot] calling libunity JNI_OnLoad(fake_vm)...\n");
    int jver = Unity_JNI_OnLoad(fake_vm, NULL);
    debugPrintf("[boot] JNI_OnLoad returned 0x%x\n", jver);
  }

  /* Register the JavaVM with the il2cpp runtime. We call libil2cpp's exported
   * JNI_OnLoad by name (simpler + version-proof vs. hardcoding the VM-global RVAs
   * the reference replicated). TODO: if this mis-binds (a PLT log call at its head
   * bound wrong), fall back to replicating the two VM-global stores directly --
   * derive their RVAs from this libil2cpp's JNI_OnLoad. */
  {
    fn_jnionload Il2cpp_JNI_OnLoad =
      (fn_jnionload)so_try_find_addr_rx(&il2cpp_mod, "JNI_OnLoad");
    if (Il2cpp_JNI_OnLoad) {
      debugPrintf("[boot] calling libil2cpp JNI_OnLoad(fake_vm)...\n");
      Il2cpp_JNI_OnLoad(fake_vm, NULL);
    } else {
      debugPrintf("[boot] WARNING: libil2cpp JNI_OnLoad not found (managed JNI may fail)\n");
    }
  }

  /* Resolve the UnityPlayer natives BY NAME from the captured RegisterNatives table. */
  #define RESOLVE_NATIVE(var, cast, name) do {                         \
      var = (cast)jni_lookup_unity_native(name);                       \
      if (!(var)) fatal_error("UnityPlayer native not captured: %s", name); \
    } while (0)
  RESOLVE_NATIVE(Unity_initJni,                  fn_initJni,  UNITY_NATIVE_initJni);
  RESOLVE_NATIVE(Unity_nativeRecreateGfxState,   fn_gfxstate, UNITY_NATIVE_nativeRecreateGfxState);
  RESOLVE_NATIVE(Unity_nativeSendSurfaceChanged, fn_v,        UNITY_NATIVE_nativeSendSurfaceChanged);
  RESOLVE_NATIVE(Unity_nativeRender,             fn_z,        UNITY_NATIVE_nativeRender);
  RESOLVE_NATIVE(Unity_nativeResume,             fn_v,        UNITY_NATIVE_nativeResume);
  RESOLVE_NATIVE(Unity_nativeFocusChanged,       fn_vz,       UNITY_NATIVE_nativeFocusChanged);
  RESOLVE_NATIVE(Unity_nativeDone,               fn_z,        UNITY_NATIVE_nativeDone);
  RESOLVE_NATIVE(Unity_nativeApplicationUnload,  fn_v,        UNITY_NATIVE_nativeApplicationUnload);
  /* Optional (Unity 6 lifecycle) -- may be absent; don't fatal if so. */
  Unity_nativeUnityPlayerSetRunning =
    (fn_vz)jni_lookup_unity_native(UNITY_NATIVE_nativeUnityPlayerSetRunning);
  #undef RESOLVE_NATIVE
  debugPrintf("[boot] natives resolved (initJni=%p render=%p)\n",
              (void *)Unity_initJni, (void *)Unity_nativeRender);

  install_bionic_tls(main_tls);

  /* Unity 6 initJni is (Context,int); pass 0 for the trailing flags value. */
  debugPrintf("[boot] initJni(ctx,0)...\n");
  Unity_initJni(fake_env, fake_unityplayer_thiz, fake_context_obj, 0);
  debugPrintf("[boot] nativeRecreateGfxState...\n");
  Unity_nativeRecreateGfxState(fake_env, fake_unityplayer_thiz, 0, fake_surface_obj);
  Unity_nativeSendSurfaceChanged(fake_env, fake_unityplayer_thiz);
  debugPrintf("[boot] surface changed; resume + focus\n");

  Unity_nativeResume(fake_env, fake_unityplayer_thiz);
  Unity_nativeFocusChanged(fake_env, fake_unityplayer_thiz, 1 /* hasFocus */);
  if (Unity_nativeUnityPlayerSetRunning)
    Unity_nativeUnityPlayerSetRunning(fake_env, fake_unityplayer_thiz, 1);
  debugPrintf("[boot] resumed + focus=true\n");

  appletHook(&g_applet_cookie, nx_applet_hook, NULL);

  /* CRITICAL ORDER: disable the Boehm GC + install the clock BEFORE the first
   * nativeRender. First-frame managed allocs can trigger a GC whose stop-the-world
   * uses POSIX signals Switch never delivers -> nativeRender would never return. */
  {
    typedef void (*fn_set_mode)(int);
    typedef void (*fn_void)(void);
    fn_set_mode il2cpp_gc_set_mode = (fn_set_mode)so_try_find_addr_rx(&il2cpp_mod, "il2cpp_gc_set_mode");
    fn_void     il2cpp_gc_disable  = (fn_void)    so_try_find_addr_rx(&il2cpp_mod, "il2cpp_gc_disable");
    if (il2cpp_gc_set_mode) { il2cpp_gc_set_mode(1); debugPrintf("[boot] il2cpp_gc_set_mode(DISABLED)\n"); }
    else debugPrintf("[boot] WARNING: il2cpp_gc_set_mode not found\n");
    if (il2cpp_gc_disable)  { il2cpp_gc_disable();   debugPrintf("[boot] il2cpp_gc_disable() -> GC OFF\n"); }
    else debugPrintf("[boot] WARNING: il2cpp_gc_disable not found\n");
  }

  /* il2cpp method hooks (RVAs generated from THIS build's dump.cs -- sd_il2cpp_offsets.h):
   *  - UnityEngine.Input.* : return our Switch touch state (Unity 6's nativeInjectEvent
   *    accepts events but never reads coords, so Input would otherwise stay empty);
   *  - UnityEngine.PlayerPrefs.* : route the game's save through our persistent prefs.kv,
   *    since Unity's native PlayerPrefs never reaches disk on Switch.
   * (The reference's LanguageParam hook is dropped -- that class is Layton-specific.) */
  nx_install_input_hooks((uintptr_t)il2cpp_mod.load_virtbase);
  nx_install_tweak_hooks((uintptr_t)il2cpp_mod.load_virtbase);   /* render scale, analytics (config.h) */
  nx_install_playerprefs_hooks((uintptr_t)il2cpp_mod.load_virtbase,
                               (void *)so_try_find_addr_rx(&il2cpp_mod, "il2cpp_string_new"));

  sd_start_clock_thread();
  debugPrintf("[boot] GC off + clock thread started; entering render loop\n");

  diag_thread_register(NULL, 0);
  diag_set_name(NULL, "NX_UIMain");
  diag_watchdog_start();

  int frame = 0;
  uint64_t next_frame_ns = 0;
  {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    next_frame_ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
  }
  while (appletMainLoop() && !jni_quit_requested) {
    diag_frame(frame);
    g_frame_count++;
    android_native_update_mode();
    android_native_feed_hid();
    sd_jni_tick();        /* deferred Java->Unity callbacks, on the main thread */
    if (!Unity_nativeRender(fake_env, fake_unityplayer_thiz)) break;
    if (frame < 5 || (frame % 120) == 0) debugPrintf("[boot] frame %d rendered\n", frame);
    frame++;
    if ((frame % 120) == 0) nx_sd_flush();   /* commit any pending save ~every 2s */
    /* FRAME LIMITER (~60fps). No real display vsync -> pace to 16.6ms so we don't
     * flood the compositor (an unthrottled present flood crashes vi). */
    next_frame_ns += 16666667ULL;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    if ((int64_t)(next_frame_ns - now) > 0) svcSleepThread(next_frame_ns - now);
    else next_frame_ns = now;
  }

  Unity_nativeApplicationUnload(fake_env, fake_unityplayer_thiz);
  Unity_nativeDone(fake_env, fake_unityplayer_thiz);

  opensles_shutdown();
  SDL_Quit();
  socketExit();

  extern void NX_NORETURN __libnx_exit(int rc);
  __libnx_exit(0);
  return 0;
}
