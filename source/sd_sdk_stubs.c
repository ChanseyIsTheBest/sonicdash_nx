/* sd_sdk_stubs.c -- in-process answers for the native SDK Sonic Dash P/Invokes
 * into but that is deliberately NOT loaded: SEGA's MariaUpload
 * (libmariaUpload.so).
 *
 * Forked from battd_nx's bp_sdk_stubs.c (MIT): same mechanism -- dlsym_fake()
 * asks us after a symbol is found neither in a loaded module nor in the shim
 * tables -- but the answers here are per-function rather than by stub class,
 * because every one of the 22 managed entry points and the enums they return
 * were read out of this game's own metadata.
 *
 * WHY NOT LOAD THE REAL LIBRARY
 * libmariaUpload.so is 5 MB and needs only libandroid/liblog/libz/libm/libdl/
 * libc -- so its HTTP/TLS stack is linked in statically. It is SEGA's
 * telemetry-and-account-auth client. Loaded, it would attempt network auth
 * against SEGA's servers from a homebrew process. The port runs offline by
 * policy, and this is the one library where "offline" has to be enforced by
 * not loading it at all.
 *
 * THE ONE THAT MATTERS: THE AUTH LOOP
 * Maria_LoopInitializeAuthSeq returns an int that MariaUpload.Util casts to
 * InitializeAuthResult (read from dump.cs):
 *     -1 InapropriateCall  0 Pending  1 Success  2 Error  3 Cancelled
 *      4 NeedRetry  5 NoUsers  6 FailAuth  7 AuthServer  8 NSAUnavailable
 *      9 Unregistered
 * A generic "return 0" stub reads as PENDING and the game polls it forever --
 * an infinite boot loop with no error. NeedRetry is a retry loop. Success
 * would claim a handshake with SEGA that never happened. ERROR (2) is terminal,
 * truthful, and exactly what a real phone in airplane mode gets -- the one
 * path SEGA's developers had to make survivable.
 *
 * If first boot shows the game refusing to continue past a connection dialog,
 * SD_MARIA_AUTH_RESULT is the knob. Read the note beside it before changing
 * it: this is telemetry auth, not a purchase, but Success is still a claim of
 * server contact that did not occur.
 *
 * NOTHING HERE ASSERTS AN ENTITLEMENT. Account links report "not linked",
 * logging reports "cannot start", the auth sequence reports "error".
 */
#include <stdint.h>
#include <string.h>
#include "util.h"
#include "config.h"

#ifndef SD_MARIA_AUTH_RESULT
#define SD_MARIA_AUTH_RESULT   2   /* InitializeAuthResult.Error -- see above */
#endif
#ifndef SD_MARIA_PREINIT_OK
#define SD_MARIA_PREINIT_OK    1   /* pre-init is local setup; it succeeds offline */
#endif

/* P/Invoke `bool` returns are read either as a 4-byte BOOL or a 1-byte I1
 * depending on marshalling attributes. An int 0/1 is correct under both. */

static int s_evt_tables;

static const char *nz(const char *s) { return s ? s : "(null)"; }

/* ---- Lib ------------------------------------------------------------- */
static void Maria_SetAppInformations(const char *app, const char *plat,
                                     const char *lang, const char *region) {
  diag_log("[maria] SetAppInformations app=%s platform=%s lang=%s region=%s",
           nz(app), nz(plat), nz(lang), nz(region));
}
/* The user ID is personal data. Log that it arrived, not what it is. */
static void Maria_SetUserID(const char *id)        { diag_log("[maria] SetUserID (%s)", id && *id ? "set" : "empty"); }
static void Maria_SetUserAge(int age)              { diag_log("[maria] SetUserAge %d", age); }
static void Maria_SetAppVersion(const char *v)     { diag_log("[maria] SetAppVersion %s", nz(v)); }
static int  Maria_CanStartLogging(void)            { return 0; }
static int  Maria_StartLogging(void)               { diag_log("[maria] StartLogging -> false (offline)"); return 0; }
static void Maria_ShutdownLogging(void)            { }
static void Maria_AddUserParameterTable(void *k, void *v, int n)  { (void)k; (void)v; (void)n; }
static void Maria_AddEventParameterTable(void *k, void *v, int n) {
  (void)k; (void)v; (void)n;
  /* Fired per analytics event. Say so once, then count silently -- logging
   * each one would flush the SD card on every coin pickup. */
  if (s_evt_tables++ == 0) diag_log("[maria] AddEventParameterTable: telemetry discarded (offline)");
}
static void Maria_SetActive(int a)                 { (void)a; }
static void Maria_SetSuspended(int s)              { (void)s; }

/* ---- Account --------------------------------------------------------- */
static int  Maria_SetSegaAccountLinkTarget(int t)  { (void)t; return 1; }  /* a setter: accepted */
static int  Maria_IsSegaAccountLinkPending(void)   { return 0; }
static int  Maria_CheckSegaAccountLinkStatus(void) { return 0; }           /* not linked */

/* ---- Util ------------------------------------------------------------ */
static int Maria_PreInitializeAuthSeq(int phase) {
  diag_log("[maria] PreInitializeAuthSeq(%d) -> %d", phase, SD_MARIA_PREINIT_OK);
  return SD_MARIA_PREINIT_OK;
}
static int Maria_LoopInitializeAuthSeq(void *uri_cb, void *id_cb) {
  /* The callbacks exist to hand a URI / account ID back during a real auth.
   * There is no real auth, so they are never invoked. */
  (void)uri_cb; (void)id_cb;
  static int n;
  if (n++ < 3)
    diag_log("[maria] LoopInitializeAuthSeq -> %d (terminal; 0 would be Pending forever)",
             SD_MARIA_AUTH_RESULT);
  return SD_MARIA_AUTH_RESULT;
}
static void Maria_PostInitializeAuthSeq(void)      { }
static const char s_platform[] = "android";
static const char *Maria_GetPlatformString(void)   { return s_platform; }   /* never NULL */
static void Maria_SetDevMode(int a, int b)         { (void)a; (void)b; }

/* ---- Develop / Internal ---------------------------------------------- */
static void Maria_SetLogCategoryThreshold(int c)   { (void)c; }
static void Maria_SetDummySegaAccountStatus(int s) { (void)s; }
static void Maria_SetUnityThreadFunctions(void *c, void *j, void *d) { (void)c; (void)j; (void)d; }

/* Anything else in the Maria_ namespace. The library exports 147 and the game
 * P/Invokes 22; a future game update could reach for one more. Zero is the
 * benign answer for every void/bool/count in this API, and saying so once
 * turns a silent new dependency into a log line. */
static long Maria_unknown(void) { return 0; }

typedef struct { const char *name; void *fn; } SdStub;
static const SdStub k_maria[] = {
  { "Maria_SetAppInformations",        (void *)Maria_SetAppInformations },
  { "Maria_SetUserID",                 (void *)Maria_SetUserID },
  { "Maria_SetUserAge",                (void *)Maria_SetUserAge },
  { "Maria_SetAppVersion",             (void *)Maria_SetAppVersion },
  { "Maria_CanStartLogging",           (void *)Maria_CanStartLogging },
  { "Maria_StartLogging",              (void *)Maria_StartLogging },
  { "Maria_ShutdownLogging",           (void *)Maria_ShutdownLogging },
  { "Maria_AddUserParameterTable",     (void *)Maria_AddUserParameterTable },
  { "Maria_AddEventParameterTable",    (void *)Maria_AddEventParameterTable },
  { "Maria_SetActive",                 (void *)Maria_SetActive },
  { "Maria_SetSuspended",              (void *)Maria_SetSuspended },
  { "Maria_SetSegaAccountLinkTarget",  (void *)Maria_SetSegaAccountLinkTarget },
  { "Maria_IsSegaAccountLinkPending",  (void *)Maria_IsSegaAccountLinkPending },
  { "Maria_CheckSegaAccountLinkStatus",(void *)Maria_CheckSegaAccountLinkStatus },
  { "Maria_PreInitializeAuthSeq",      (void *)Maria_PreInitializeAuthSeq },
  { "Maria_LoopInitializeAuthSeq",     (void *)Maria_LoopInitializeAuthSeq },
  { "Maria_PostInitializeAuthSeq",     (void *)Maria_PostInitializeAuthSeq },
  { "Maria_GetPlatformString",         (void *)Maria_GetPlatformString },
  { "Maria_SetDevMode",                (void *)Maria_SetDevMode },
  { "Maria_SetLogCategoryThreshold",   (void *)Maria_SetLogCategoryThreshold },
  { "Maria_SetDummySegaAccountStatus", (void *)Maria_SetDummySegaAccountStatus },
  { "Maria_SetUnityThreadFunctions",   (void *)Maria_SetUnityThreadFunctions },
};

void *sd_sdk_stub_lookup(const char *symbol) {
  if (!symbol || strncmp(symbol, "Maria_", 6)) return NULL;
  for (size_t i = 0; i < sizeof k_maria / sizeof k_maria[0]; i++)
    if (!strcmp(symbol, k_maria[i].name)) return k_maria[i].fn;
  diag_log("[maria] UNLISTED %s -> zero stub (new dependency? add it to sd_sdk_stubs.c)", symbol);
  return (void *)Maria_unknown;
}
