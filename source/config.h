/* config.h -- Sonic Dash Switch wrapper configuration.
 *
 * Forked from the laytonbmr_nx / vln_nx SoLoader ports (MIT). The loader-tuning
 * constants (heap split, mmap arena, overcommit window) are inherited unchanged;
 * they are engine-generation properties, not game properties. The game-identity
 * and user-config parts below are Sonic-Dash-specific.
 *
 * This software may be modified and distributed under the terms of the MIT
 * license. See the LICENSE file for details.
 */
#ifndef __CONFIG_H__
#define __CONFIG_H__

/* Newlib heap for the engine/libc++/il2cpp managed heaps; the rest -> .so loader. */
#define MEMORY_MB 768

/* mmap arena. Unity reserves aligned pools by over-mmapping then trimming head/tail;
 * we back anon mmaps from an aligned arena with a per-page bitmap so sub-range munmap
 * frees only trimmed pages. ALIGN must match Unity's region granularity. */
#define MMAP_ARENA_ALIGN    ((size_t)64 * 1024 * 1024)
#define MMAP_ARENA_RESERVE  ((size_t)1792 * 1024 * 1024)  /* heap-backed cap (28x64MB) */

/* Stack-region overcommit arena (libc_shim.c). */
#define OC_WINDOW_BYTES     ((size_t)1536 * 1024 * 1024)
#define OC_POOL_BYTES       ((size_t) 384 * 1024 * 1024)
#define MMAP_VIRT_RESERVE   ((size_t)6144 * 1024 * 1024)
#define OVERCOMMIT_HEAP_MB  608u

/* --- inherited SoLoader leftovers (unused by Unity, kept for base parity) --- */
#define SO_NAME      "libcrx.so"
#define SO_CPP_NAME  "libc++_shared.so"
#define MAIN_MVGL    "main.10007.android.mvgl"

/* --- Sonic Dash game identity (JNI Context shim: getPackageName/versionCode) ---
 * Unity 6000.0.72f1 / IL2CPP / arm64-v8a, metadata v31.
 *
 * ASSET LAYOUT IS NOT THE DONOR'S. Colour Sheep used the single-bundle shape
 * (data.unity3d + sharedassets0.resource). Sonic Dash has NO data.unity3d at
 * all. It ships:
 *     assets/bin/Data/            loose engine data, from the BASE apk
 *                                 (globalgamemanagers, level0, sharedassets0,
 *                                  Managed/Metadata/global-metadata.dat)
 *     assets/aa/Android/          292 Addressables .bundle files
 *     assets/aa/catalog.json      the Addressables catalogue
 *     assets/bin/Data/<32-hex>    ~560 more files, from the ASSET PACK apk
 * The last two come from UnityDataAssetPack.apk, a Play Asset Delivery pack,
 * NOT from the base APK. Both APKs must be staged. See tools/stage_sd.py. */
#define SD_PACKAGE       "com.sega.sonicdash"
#define SD_VERSION_NAME  "10.3.1"
#define SD_VERSION_CODE  1726654940
#define SD_APP_GUID      "e9f8ebc9-6e7c-4fe0-8c68-5083e91f73da"  /* unity_app_guid */

#define CONFIG_NAME "config.txt"
#define LOG_NAME    "sdmc:/switch/sonicdash_nx/debug.log"

/* Game data root == the .nro's own folder (SoLoader SD convention):
 * nro + libs + assets all live in sdmc:/switch/sonicdash_nx/. */
#define GAME_HOME   "sdmc:/switch/sonicdash_nx"

/* flip to 1 (and rebuild) for on-hardware file logging (debug.log) */
#define DEBUG_LOG 0

extern int screen_width;
extern int screen_height;

/* =======================================================================
 * Sonic Dash additions
 * ======================================================================= */

/* Force GLES by refusing libvulkan.so at dlopen. This build ships BOTH
 * GLES3 and SPIR-V shader variants (189 GLES sources / 72 SPIR-V modules,
 * measured with tools/check_graphics_api.py), so the GLES path has real
 * shaders to compile. Leave this on: mesa/nouveau is OpenGL only. */
#define SD_REFUSE_VULKAN            1

/* 60 fps override. OFF by default -- see the warning in sd_patches.c and
 * PORTING.md section 6. An endless runner is exactly the genre where a
 * per-frame integration bug turns 60 fps into double speed. */
#define SD_FORCE_60FPS              0

/* Resolution is NOT set here: config.txt's `resolution` (sd_config.c), 720p by
 * default and the same in docked and handheld; `rotation` picks portrait. An
 * earlier revision defined SD_SCREEN_* and SD_LANDSCAPE knobs here that nothing
 * read; they were removed so that editing this file cannot appear to change
 * something it does not. */

/* COUNTRY, pinned. Every path that asks the "device" for its country answers
 * this, so compliance rules, region-gated content, promos and the server's user
 * country all agree. It is independent of LANGUAGE, which still follows the
 * Switch's system setting (locale strings become "<language>_AU").
 *
 * Why it matters: HLUnityCore.Unity_GetISO2CountryCode used to answer "", which
 * Hardlight's ComplianceUtility.FindCountry (String.IsNullOrWhiteSpace) turned
 * into the country "default" -- recorded in LegalPluginData.json -- where a phone
 * records its real country. The lookup is case-insensitive (String.Equals with
 * OrdinalIgnoreCase), so "AU" matches the list's "au".
 * The game's own regulation table: {"id":"au","age":15,"type":"coppa"}. */
#define SD_COUNTRY_ISO2  "AU"
#define SD_COUNTRY_ISO3  "AUS"

/* JNI LOGGING -- master switch. 1 = everything:
 *   - util.c's log_is_noisy() stops dropping "[jni]", "JNI:", "dlopen", "dlsym"
 *   - [jnim]   every distinct Java method, once        (sd_jni.c)
 *   - [jniret] what we ANSWERED, first 3 calls a method (sd_jni.c)
 *   - [jnicls] every class, once                        (always on)
 * Flood-guarded: UNIMPL slots and pool exhaustion log once, dlsym is capped.
 * Set 0 for a release build: the per-call answer bookkeeping then compiles away. */
#define SD_JNI_LOG  1

/* ANDROID VERSION reported to the game AND the engine (Build.VERSION.SDK_INT /
 * RELEASE). 32 = Android 12L: the newest level WITHOUT Android 13's runtime
 * notification permission. At 33+ the game's notification setup
 * (Hardlight.AndroidNotificationsNativeBridge.RequestPermissions) asks through
 * Unity's PermissionCallbacks proxy, and this port cannot yet call a Java proxy
 * back -- the request would stay pending forever. Before the Class.forName fix
 * the engine saw 0 and C# saw 33; both now see this one value. */
/* From the APK's own AndroidManifest.xml (read with pyaxmlparser). */
#define SD_APK_MIN_SDK      23
#define SD_APK_TARGET_SDK   35

#define SD_ANDROID_API      32
#define SD_ANDROID_RELEASE  "12"

/* RAM read-ahead cache for big read-only asset files (libc_shim.c): 8 slots x
 * a 1 MB window, turning Unity's thousands of tiny per-field reads into one
 * large SD read. Disabled while the stop-the-world freeze is investigated
 * (user request). Audited first: it is small (<= 8 MB) and correct -- close()
 * detaches the slot, so a reused fd is never served stale bytes. Expect
 * slower loading with it off; set 1 to restore. */
#define SD_RA_CACHE  0

/* Deliver Google Play Billing's onBillingSetupFinished(BillingUnavailable) to
 * Unity IAP (sd_jni.c). It runs C# on the proxy drain thread -- the first C#
 * to run on a thread this port created -- just before the first stop-the-world
 * freeze. 1 = deliver (default). Set 0 to bisect: if the freeze goes away,
 * C# on the drain thread is implicated. */
#define SD_BILLING_CALLBACK  1

/* Render at 100% of the config.txt resolution instead of the game's own 75%
 * (AppFlow.SetResolutionScale -> Screen.SetResolution). Sharper; costs GPU time
 * -- set 0 if frame rate suffers, or lower `resolution` in config.txt. */
#define SD_FULL_RES_RENDER  1

/* Analytics off: the game's own opt-out path (AnalyticsSystem.
 * CanAnalyticsEventsBeCollected -> false). Offline, every event failed and was
 * appended to failed_events.log forever. */
#define SD_ANALYTICS_OFF    1

/* Save editing (battd_nx design): save_edit.txt next to the game, applied to
 * save.txt at every launch and re-signed exactly as the game signs it.
 * sd_saveedit.c. */
/* Rotated presentation (config.txt rotation 1/2): the portrait image is blitted
 * 1:1 onto the window, so NEAREST sampling is exact. 1 = LINEAR (softer). */
#define SD_TATE_LINEAR  0

#define SD_SAVE_EDIT  1

#endif /* __CONFIG_H__ -- everything above is inside the guard */
