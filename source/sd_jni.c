/* sd_jni.c -- the Java surface Sonic Dash reaches, and what it is told.
 *
 * Forked from battd_nx's bp_jni.c (MIT): same dispatch contract, same
 * effective-class resolution, same "present but dormant" SDK policy. The
 * CLASSES and ANSWERS are this game's, read out of its own global-metadata.dat
 * and dump.cs -- 193 Java class names after removing /proc/net paths.
 *
 * ---------------------------------------------------------------------------
 * WHAT THE DONOR TAUGHT, AND WHAT IT GOT WRONG
 * ---------------------------------------------------------------------------
 * Kept from battd_nx:
 *
 *  1. NULL IS THE WRONG ANSWER FOR AN SDK OBJECT. A C# wrapper that binds a
 *     delegate to a null Java object makes IL2CPP invoke it with every
 *     argument shifted one register -- the deterministic crash in both of
 *     their hardware runs. Object-returning SDK calls get an INERT object of
 *     the SDK's own class, strings get "", so every later call on that handle
 *     is answered by the same policy.
 *
 *  2. THE EFFECTIVE CLASS. Unity 6 keys most method IDs to java/lang/Object,
 *     so the receiver's own class is consulted when id->cls says Object.
 *
 * Corrected here:
 *
 *  3. bp_jni.c reads arguments with va_arg. In this host, the ...MethodA
 *     (jvalue array) variants forward to the variadic ones with NO arguments,
 *     so any va_list reached that way is garbage -- and Unity 6 uses the A
 *     forms heavily (AndroidJavaObject.Call, and libunity's own jni:: layer).
 *     This module reads arguments ONLY from an explicit jvalue array or from a
 *     va_list the caller vouches for; jni_fake.c intercepts the A variants for
 *     claimed classes before they forward.
 *
 * ---------------------------------------------------------------------------
 * THE TWO BOOT GATES
 * ---------------------------------------------------------------------------
 *  PLAY ASSET DELIVERY. libunity itself -- not game C# -- calls
 *  PlayAssetDeliveryUnityWrapper.init(UnityPlayer, Context) and then
 *  getAssetPackPath("UnityDataAssetPack") at boot to find its data. This title
 *  ships its Unity data in that install-time pack. The answer is the staged
 *  assets/ root. Note the class sits under com/unity3d/player/, so it must be
 *  claimed HERE, ahead of unity_jni.c, or Unity's generic handler swallows it.
 *
 *  CONSENT. Hardlight's ComplianceCMPRequestState is a boot-FSM state whose
 *  HasFinished() only becomes true when the Java CMP calls back into the
 *  GameObject "ComplianceCallback" via UnitySendMessage. An inert consent stub
 *  never calls back, so the boot FSM WAITS THERE FOREVER. RequestConsent()
 *  therefore queues Native_OnRequestConsentSuccess for the next frame, and
 *  GetConsentStatus() answers NotRequired (1): truthful, because there are no
 *  ads and no data collection. It does NOT answer Obtained (3) -- that would
 *  assert a consent the player never gave.
 *
 * ---------------------------------------------------------------------------
 * THE LINE THAT IS NOT CROSSED
 * ---------------------------------------------------------------------------
 * Nothing here asserts an entitlement. Ads are unavailable, purchases fail,
 * logins decline, personalised ads are refused. IAP bypass, unlock-all and
 * rewarded-ad auto-grant are game-logic modifications, not interoperability,
 * and they do not belong in a loader.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "sd_jni.h"
#include "sd_config.h"
#include "util.h"
#include "config.h"
#include "jni_fake.h"

/* Must match jni_fake.c's anonymous FakeID typedef EXACTLY. A tagged struct
 * with the same layout is a distinct type rather than a redefinition, which is
 * what lets it cross the dispatch boundary as const void *. If the host layout
 * changes, every method name is misread -- hence the size assertion. */
struct FakeID { uint32_t tag; char cls[96]; char name[64]; char sig[160]; };
_Static_assert(sizeof(struct FakeID) == 4 + 96 + 64 + 160,
               "FakeID layout drifted from jni_fake.c");

extern const char *jni_string_utf(void *jstr);   /* jni_fake.c, exported */

#define TAG_CLASS  0x434c5331u   /* 'CLS1' -- jni_fake.c */
#define TAG_OBJECT 0x4f424a31u   /* 'OBJ1' */
#define TAG_STRING 0x53545231u   /* 'STR1' */

/* ------------------------------------------------------------- matching */

/* Java class names arrive slashed from FindClass and dotted from some C#
 * paths. Compare treating '.' and '/' as the same character. */
static int path_starts(const char *s, const char *prefix) {
  if (!s) return 0;
  for (; *prefix; s++, prefix++) {
    char a = (*s == '.') ? '/' : *s, b = (*prefix == '.') ? '/' : *prefix;
    if (a != b) return 0;
  }
  return 1;
}
static int path_is(const char *s, const char *full) {
  return path_starts(s, full) && s[strlen(full)] == '\0';
}
static int has(const char *h, const char *n) { return h && n && strstr(h, n); }

static const char *ret_type(const struct FakeID *id) {
  const char *r = strrchr(id->sig, ')');
  return r ? r + 1 : "";
}

const char *sd_jni_eff_cls(const void *id_, void *recv) {
  const struct FakeID *id = id_;
  const char *c = id ? id->cls : "";
  if (c && *c && strcmp(c, "java/lang/Object")) return c;
  if (recv) {
    uint32_t tag = *(const uint32_t *)recv;
    if (tag == TAG_CLASS || tag == TAG_OBJECT) {
      const char *rc = (const char *)recv + 4;   /* FakeClass.name / FakeObject.label */
      if (*rc) return rc;
    }
  }
  return c ? c : "";
}

/* ------------------------------------------------------ argument access */

/* The nth parameter of type String, or NULL. Reads from the jvalue array
 * when present, otherwise from a caller-vouched va_list (copied, so the
 * caller's list is not consumed). Never both, never neither-and-guess. */
/* The nth OBJECT argument (a listener, say): same signature walk as str_arg. */
static void *obj_arg(const struct FakeID *id, int nth, const void *jv, va_list *va) {
  const char *p = id->sig;
  if (*p != '(') return NULL;
  p++;
  va_list cp;
  if (!jv && va) va_copy(cp, *va);
  void *out = NULL;
  int idx = 0, seen = 0;
  while (*p && *p != ')') {
    int is_obj = 0, kind = *p;
    if (*p == '[') { while (*p == '[') p++; if (*p == 'L') { while (*p && *p != ';') p++; } is_obj = 1; }
    else if (*p == 'L') { while (*p && *p != ';') p++; is_obj = 1; }
    p++;
    void *o = NULL;
    if (jv) { if (is_obj) o = ((void *const *)jv)[idx]; }
    else if (va) {
      if (is_obj)                          o = va_arg(cp, void *);
      else if (kind == 'J')                (void)va_arg(cp, long long);
      else if (kind == 'F' || kind == 'D') (void)va_arg(cp, double);
      else                                 (void)va_arg(cp, int);
    }
    if (is_obj) { if (seen == nth) { out = o; break; } seen++; }
    idx++;
  }
  if (!jv && va) va_end(cp);
  return out;
}

static const char *str_arg(const struct FakeID *id, int nth, const void *jv, va_list *va) {
  const char *p = id->sig;
  if (*p != '(') return NULL;
  p++;
  va_list cp;
  if (!jv && va) va_copy(cp, *va);
  const char *out = NULL;
  int idx = 0, seen = 0;
  while (*p && *p != ')') {
    int is_obj = 0, is_str = 0, kind = *p;
    if (*p == '[') {                       /* arrays are one reference slot */
      while (*p == '[') p++;
      if (*p == 'L') { while (*p && *p != ';') p++; }
      is_obj = 1;
    } else if (*p == 'L') {
      is_str = !strncmp(p, "Ljava/lang/String;", 18);
      while (*p && *p != ';') p++;
      is_obj = 1;
    }
    p++;
    void *obj = NULL;
    if (jv) {
      if (is_obj) obj = ((void *const *)jv)[idx];
    } else if (va) {
      if (is_obj)                          obj = va_arg(cp, void *);
      else if (kind == 'J')                (void)va_arg(cp, long long);
      else if (kind == 'F' || kind == 'D') (void)va_arg(cp, double);
      else                                 (void)va_arg(cp, int);
    } else break;
    if (is_str) {
      if (seen++ == nth) {
        if (obj && *(const uint32_t *)obj == TAG_STRING) out = jni_string_utf(obj);
        break;
      }
    }
    idx++;
  }
  if (!jv && va) va_end(cp);
  return out;
}

/* --------------------------------------------------- deferred callbacks */

typedef void (*usm_fn)(const char *, const char *, const char *);
static usm_fn g_usm;

typedef struct { char obj[64], method[64], msg[128]; } Deferred;
#define SD_DEFER_MAX 8
static Deferred s_q[SD_DEFER_MAX];
static int      s_qn;
static Mutex    s_qlock;

static void defer_usm(const char *obj, const char *method, const char *msg) {
  mutexLock(&s_qlock);
  if (s_qn < SD_DEFER_MAX) {
    Deferred *d = &s_q[s_qn++];
    strncpy(d->obj, obj, sizeof d->obj - 1);         d->obj[sizeof d->obj - 1] = 0;
    strncpy(d->method, method, sizeof d->method - 1); d->method[sizeof d->method - 1] = 0;
    strncpy(d->msg, msg ? msg : "", sizeof d->msg - 1); d->msg[sizeof d->msg - 1] = 0;
  }
  mutexUnlock(&s_qlock);
}

void sd_jni_init(usm_fn usm) {
  g_usm = usm;
  diag_log("[sdjni] UnitySendMessage %s", usm ? "resolved -- deferred callbacks live"
           : "NOT RESOLVED -- the consent callback cannot be delivered and boot "
             "will wait at ComplianceCMPRequestState");
}

void sd_jni_tick(void) {
  Deferred local[SD_DEFER_MAX];
  int n;
  mutexLock(&s_qlock);
  n = s_qn;
  if (n) memcpy(local, s_q, sizeof(Deferred) * (size_t)n);
  s_qn = 0;
  mutexUnlock(&s_qlock);
  for (int i = 0; i < n; i++) {
    diag_log("[sdjni] -> UnitySendMessage(\"%s\", \"%s\", \"%s\")",
             local[i].obj, local[i].method, local[i].msg);
    if (g_usm) g_usm(local[i].obj, local[i].method, local[i].msg);
  }
}

/* --------------------------------------------------------------- locale */

/* The Switch's system language, e.g. "en-US", "ja", "fr-CA", "zh-Hans".
 * setGetSystemLanguage packs it as ASCII into a u64. Returned in Java's
 * Locale.toString() form ("en_US"), the common Android idiom; HLUnityCore
 * exposes both a "Raw" and a normalised getter on the C# side, so it parses
 * what arrives. If the game ignores the language, try SD_LOCALE_HYPHEN 1. */
#ifndef SD_LOCALE_HYPHEN
#define SD_LOCALE_HYPHEN 0
#endif
static const char *switch_locale(void) {
  /* Language from config.txt (sd_lang: auto = the Switch's, when the game ships
   * it), country pinned (SD_COUNTRY_ISO2): "en_AU", "fr_AU" ... */
  static char loc[16];
  if (!loc[0]) snprintf(loc, sizeof loc, "%s%c%s", sd_lang(), SD_LOCALE_HYPHEN ? '-' : '_', SD_COUNTRY_ISO2);
  return loc;
}
static const char *switch_language(void) { return sd_lang(); }   /* config.txt */

/* ------------------------------------------------ SLGlobal's identifiers */

/* ANDROID_ID stand-in. The game's player ID is SLGlobal.GetAndroidID()
 * (UserIdentification.CurrentDeviceIdentifier, read from the binary). An empty
 * answer made the player ID "null or empty" forever -- UserIdentification
 * retried every few seconds (45 log lines per session). A real device returns a
 * 16-hex-digit value that is stable for the install; so does this: random (not
 * derived from any hardware serial), generated once and kept in GAME_HOME so the
 * ID -- and anything the game keys to it -- survives restarts. */
static const char *install_id(void) {
  static char id[17];
  if (id[0]) return id;
  char path[256];
  snprintf(path, sizeof path, "%s/android_id", GAME_HOME);
  FILE *f = fopen(path, "rb");
  if (f) {
    size_t n = fread(id, 1, 16, f);
    fclose(f);
    int ok = (n == 16);
    for (int i = 0; ok && i < 16; i++)
      ok = (id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f');
    if (ok) { id[16] = 0; return id; }
  }
  uint8_t r[8];
  randomGet(r, sizeof r);
  for (int i = 0; i < 8; i++) snprintf(id + i * 2, 3, "%02x", r[i]);
  f = fopen(path, "wb");
  if (f) { fwrite(id, 1, 16, f); fclose(f); }
  diag_log("[sdjni] new install ID %s (%s %s)", id, f ? "saved to" : "COULD NOT SAVE to", path);
  return id;
}

/* ISO 639-2 three-letter LANGUAGE (java.util.Locale.getISO3Language). NOTE: not
 * what SLGlobal.GetISOLocale_ThreeDigit wants -- that feeds HLLanguageState's
 * ISOLocale enum, whose members are ISO-3166 COUNTRY codes (AUS = 6). An earlier
 * revision answered it with this function; see SD_COUNTRY_ISO3 instead. */
static const char *switch_iso3(void) {
  static const struct { const char *two, *three; } t[] = {
    {"en","eng"},{"ja","jpn"},{"fr","fra"},{"de","deu"},{"it","ita"},{"es","spa"},
    {"nl","nld"},{"pt","por"},{"ru","rus"},{"ko","kor"},{"zh","zho"},
  };
  const char *l = switch_language();
  for (size_t i = 0; i < sizeof t / sizeof t[0]; i++)
    if (!strcmp(l, t[i].two)) return t[i].three;
  return "eng";
}

/* The SD folder as Android-style absolute paths see it: GAME_HOME without the
 * "sdmc:" device prefix -- the same root Unity's own getFilesDir /
 * getExternalFilesDir answers use (unity_jni.c managed_root), so SEGA's code and
 * Unity agree where persistent data lives. */
static const char *game_root(void) {
  const char *h = GAME_HOME;
  return strncmp(h, "sdmc:", 5) ? h : h + 5;
}

/* java.util.Locale, answered for the language on the Switch and the pinned
 * country. Claimed by EFFECTIVE class, so it works on the Locale objects the
 * return-type-labelled fallback now hands out for Locale.getDefault(). */
static void *locale_answer(const char *m, const char *r) {
  static char tag[16], disp[48];
  const char *l = switch_language();
  static const struct { const char *c, *name; } langs[] = {
    {"en","English"},{"ja","Japanese"},{"fr","French"},{"de","German"},{"it","Italian"},
    {"es","Spanish"},{"nl","Dutch"},{"pt","Portuguese"},{"ru","Russian"},{"ko","Korean"},{"zh","Chinese"} };
  const char *lname = "English";
  for (size_t i = 0; i < sizeof langs / sizeof langs[0]; i++) if (!strcmp(l, langs[i].c)) lname = langs[i].name;
  if (strcmp(r, "Ljava/lang/String;")) {
    if (has(r, "Ljava/util/Locale;")) return jni_make_object("java/util/Locale");  /* getDefault & co. */
    return NULL;
  }
  if (!strcmp(m, "getLanguage"))        return jni_make_string(l);
  if (!strcmp(m, "getCountry"))         return jni_make_string(SD_COUNTRY_ISO2);
  if (!strcmp(m, "getISO3Language"))    return jni_make_string(switch_iso3());
  if (!strcmp(m, "getISO3Country"))     return jni_make_string(SD_COUNTRY_ISO3);
  if (!strcmp(m, "toLanguageTag"))      { snprintf(tag, sizeof tag, "%s-%s", l, SD_COUNTRY_ISO2); return jni_make_string(tag); }
  if (!strcmp(m, "getDisplayLanguage")) return jni_make_string(lname);
  if (!strcmp(m, "getDisplayCountry"))  return jni_make_string(!strcmp(SD_COUNTRY_ISO2, "AU") ? "Australia" : SD_COUNTRY_ISO2);
  if (!strcmp(m, "getDisplayName"))     { snprintf(disp, sizeof disp, "%s (%s)", lname, !strcmp(SD_COUNTRY_ISO2, "AU") ? "Australia" : SD_COUNTRY_ISO2); return jni_make_string(disp); }
  if (!strcmp(m, "getVariant") || !strcmp(m, "getScript")) return jni_make_string("");
  return jni_make_string(switch_locale());   /* toString and anything else: "en_AU" */
}

/* HLLanguageState.Locale -- the int SLGlobal.GetCurrentLocale() returns. Read
 * from the game's enum: Other=1, US=2, Brazil=4, Japan=8, Korea=16, China=32.
 * There is NO member 0: answering 0 (as the generic int path did) handed the
 * game an undefined locale. Australia is "Other". */
static int locale_enum(void) {
  const char *c = SD_COUNTRY_ISO2;
  if (!strcmp(c, "US")) return 2;
  if (!strcmp(c, "BR")) return 4;
  if (!strcmp(c, "JP")) return 8;
  if (!strcmp(c, "KR")) return 16;
  if (!strcmp(c, "CN")) return 32;
  return 1;
}


/* ---------------------------------------------- first-sight method trace
 * Every distinct Java method the game calls is logged once, as [jnim]. The
 * [jnicls] log showed which classes were touched; this shows which METHODS --
 * which is what it took to find that SLGlobal.GetAndroidID was the player ID,
 * and what will show the compliance flow's Java calls after the terms screen.
 * Tag is [jnim], not [jni]: util.c's log_is_noisy() drops "[jni]" lines. */
static uint32_t s_seen[4096];     /* stops inserting at 75% -- see first_sight() */
static int      s_seen_n;
static Mutex    s_seen_lock;
static int      s_traced;

static int first_sight(const char *a, const char *b) {
  uint32_t h = 2166136261u;
  for (const char *p = a; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
  h = (h ^ '.') * 16777619u;
  for (const char *p = b; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
  if (!h) h = 1;
  int fresh = 0;
  mutexLock(&s_seen_lock);
  for (uint32_t i = 0; i < 4096; i++) {
    uint32_t k = (h + i) & 4095;
    if (s_seen[k] == h) break;
    if (!s_seen[k]) {
      if (s_seen_n >= 3072) break;       /* 75%: never degrade into a full scan per call */
      s_seen[k] = h; s_seen_n++; fresh = 1; break;
    }
  }
  mutexUnlock(&s_seen_lock);
  return fresh;
}

void sd_jni_trace(const void *id_, void *recv) {
  const struct FakeID *id = id_;
#if !SD_JNI_LOG
  return;
#endif
  if (!id || s_traced >= 3000) return;
  const char *c = sd_jni_eff_cls(id, recv);
  if (first_sight(c, id->name)) {
    s_traced++;
    diag_log("[jnim] %s.%s%s", c, id->name, id->sig);
  }
}

/* ----------------------------------------------------- answer logging
 * [jniret] -- what we ANSWERED, for the first 3 calls of each method. The
 * player-ID and country bugs were both empty-string answers; a log of answers
 * shows that class of bug directly. The answer object is described by reading
 * its tag, so the kernel is asked first whether that address is readable: a
 * logging bug must never be able to crash the game. */
static int readable4(const void *p) {
  MemoryInfo mi; u32 pi;
  if (!p || ((uintptr_t)p & 3)) return 0;
  if (R_FAILED(svcQueryMemory(&mi, &pi, (u64)(uintptr_t)p))) return 0;
  return mi.type != MemType_Unmapped && (mi.perm & Perm_R) &&
         (uintptr_t)p + 8 <= mi.addr + mi.size;
}

static int call_index(const char *c, const char *n) {
  static struct { uint32_t h; uint8_t k; } t[8192];
  static int used;                    /* stops inserting at 75% */
  static Mutex lk;
  uint32_t h = 2166136261u;
  for (const char *p = c; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
  h = (h ^ '#') * 16777619u;
  for (const char *p = n; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
  if (!h) h = 1;
  int k = 99;
  mutexLock(&lk);
  for (uint32_t i = 0; i < 8192; i++) {
    uint32_t j = (h + i) & 8191;
    if (t[j].h == h) { if (t[j].k < 250) t[j].k++; k = t[j].k; break; }
    if (!t[j].h) {
      if (used >= 6144) break;             /* 75%: k stays 99, nothing logged */
      t[j].h = h; t[j].k = 1; used++; k = 1; break;
    }
  }
  mutexUnlock(&lk);
  return k;
}

void sd_jni_trace_ret(const void *id_, void *recv, char kind, uint64_t v, double f) {
#if SD_JNI_LOG
  const struct FakeID *id = id_;
  static unsigned total;
  if (!id || total >= 6000) return;
  const char *c = sd_jni_eff_cls(id, recv);
  int n = call_index(c, id->name);
  if (n > 3) return;
  total++;
  char val[180];
  if (kind == 'i')      snprintf(val, sizeof val, "%llu", (unsigned long long)v);
  else if (kind == 'f') snprintf(val, sizeof val, "%g", f);
  else if (kind == 'v') snprintf(val, sizeof val, "(void)");
  else if (!v)          snprintf(val, sizeof val, "null");
  else if (!readable4((const void *)(uintptr_t)v)) snprintf(val, sizeof val, "<%p>", (void *)(uintptr_t)v);
  else {
    uint32_t tag = *(const uint32_t *)(uintptr_t)v;
    if (tag == TAG_STRING) {
      const char *u = jni_string_utf((void *)(uintptr_t)v);
      snprintf(val, sizeof val, "\"%.120s\"", u ? u : "");
    } else if (tag == 0x4d494431u /* 'MID1': a reflected member = its method id */) {
      const struct FakeID *m = (const struct FakeID *)(uintptr_t)v;
      snprintf(val, sizeof val, "<member %.60s.%.40s%.60s>", m->cls, m->name, m->sig);
    } else if (tag == TAG_OBJECT || tag == TAG_CLASS) {
      snprintf(val, sizeof val, "<%s %.100s>", tag == TAG_CLASS ? "class" : "obj",
               (const char *)(uintptr_t)v + 4);
    } else snprintf(val, sizeof val, "<%p>", (void *)(uintptr_t)v);
  }
  diag_log("[jniret] %s.%s -> %s%s", c, id->name, val, n == 3 ? "   (later calls not logged)" : "");
#else
  (void)id_; (void)recv; (void)kind; (void)v; (void)f;
#endif
}

/* ------------------------------------------------------------ the claims */

#define C_PAD      "com/unity3d/player/PlayAssetDeliveryUnityWrapper"
#define C_CMP      "com/hardlight/hlcompliance/ConsentManagementPlatformGoogle"
#define C_HLCORE   "hardlight/hlcore/HLUnityCore"
#define C_HLOUT    "hardlight/hlcore/HLOutput"
#define C_SLGLOBAL "com/sega/gamelib/SLGlobal"
#define C_SLPERM   "com/sega/gamelib/SLPermissionsManager"
#define C_UPERM    "com/unity3d/player/UnityPermissions"
#define C_LOCALE   "java/util/Locale"
#define C_SOCIAL   "com/sega/gamelib/social/SLSocialInterface"
#define C_BILLING  "com/android/billingclient/api/BillingClient"
#define C_BILLRES  "com/android/billingclient/api/BillingResult"

int sd_jni_owns(const char *c) {
  return path_is(c, C_PAD)    || path_is(c, C_CMP)      || path_is(c, C_HLCORE) ||
         path_is(c, C_HLOUT)  || path_is(c, C_SLGLOBAL) || path_is(c, C_SLPERM) ||
         path_is(c, C_UPERM)  || path_is(c, C_LOCALE)  ||
         path_is(c, C_SOCIAL) || path_is(c, C_BILLING) || path_is(c, C_BILLRES);
}

/* SDK "present but dormant". Every prefix below is a family this game's
 * metadata names. Hardlight appears both with and without a com/ prefix, and
 * both are real. */
static const char *const k_inert[] = {
  "com/google/unity/", "com/google/android/", "com/google/ads/", "com/google/firebase/",
  "com/android/billingclient/", "com/unity/purchasing/", "com/unity/androidnotifications/",
  "com/unity3d/ads/", "com/appsflyer/", "com/applovin/", "com/moloco/", "com/mbridge/",
  "com/facebook/", "com/kidoz/", "com/superawesome/",
  "com/hardlight/hladvertisement/", "com/hardlight/hlanalytics/",
  "hardlight/hlcrashreport/", "hardlight/hlnotifications/", "hardlight/hlwebview/",
  "com/sega/gamelib/social/", "com/sega/gamelib/identifiers/", "com/sega/gamelib/CustomBackupAgent",
  NULL };
_Static_assert(sizeof k_inert / sizeof k_inert[0] - 1 <= 32, "one log bit per prefix");

int sd_sdk_is_inert(const char *c) {
  if (!c || !*c) return 0;
  for (int i = 0; k_inert[i]; i++) {
    if (path_starts(c, k_inert[i])) {
      /* Log each family once. An atomic bitmask rather than the donor's
       * "check nlog, then logged[nlog++]": that is two separate reads of nlog,
       * so two JNI threads racing through it could each pass the bound check
       * and write one slot past the end. One bit per prefix index, set with a
       * single atomic OR, has no such window. */
      static volatile uint32_t logged;
      const uint32_t bit = 1u << i;
      if (!(__atomic_fetch_or(&logged, bit, __ATOMIC_RELAXED) & bit))
        diag_log("[sdjni] SDK %s* -> dormant (objects inert, values false/0/\"\")", k_inert[i]);
      return 1;
    }
  }
  return 0;
}

void *sd_sdk_inert_object(const void *id_, const char *eff) {
  const struct FakeID *id = id_;
  const char *r = ret_type(id);
  if (!strcmp(r, "Ljava/lang/String;")) {
    /* An empty string was the player-ID bug. Say so, once per method, so the
     * next one is a log line rather than a hunt. */
    char key[160];
    snprintf(key, sizeof key, "=\"\"%s", id->name);
    if (first_sight(eff, key))
      diag_log("[sdjni] %s.%s() -> \"\" (no specific handler)", eff, id->name);
    return jni_make_string("");
  }
  if (r[0] == '[') return NULL;          /* an empty array would need a length */
  return jni_make_object(eff);           /* NON-NULL: see rule 1 above */
}

/* ----------------------------------------------------------- dispatchers */

static const char *s_cmp_listener = "ComplianceCallback";   /* dump.cs constant */
static char s_cmp_listener_buf[64];
static int  s_hlout_lines;
#define SD_HLOUT_MAX_LINES 2000

void *sd_jni_object(void *recv, const void *id_, const char *eff, const void *jv, va_list *va) {
  const struct FakeID *id = id_;
  const char *m = id->name, *r = ret_type(id);
  (void)recv;

  if (path_is(eff, C_PAD)) {
    if (has(r, "PlayAssetDeliveryUnityWrapper"))          /* static init(...) */
      return jni_make_object(C_PAD);
    if (has(m, "getAssetPackPath")) {
      const char *pack = str_arg(id, 0, jv, va);
      diag_log("[pad] getAssetPackPath(%s) -> %s/assets", pack ? pack : "?", GAME_HOME);
      { char a[320]; snprintf(a, sizeof a, "%s/assets", GAME_HOME); return jni_make_string(a); }
    }
  }

  if (path_is(eff, C_LOCALE)) {
    void *v = locale_answer(m, r);
    if (v) return v;
  }
  if (path_is(eff, C_BILLRES) && !strcmp(r, "Ljava/lang/String;"))
    return jni_make_string("Google Play Billing is not available on Nintendo Switch");

  if (path_is(eff, C_HLCORE) && !strcmp(r, "Ljava/lang/String;")) {
    /* Unity_GetISO2CountryCode: compliance country, region content, promos,
     * analytics region, the server's user country and the language variant all
     * read this. It answered "" -- see SD_COUNTRY_ISO2 in config.h. */
    if (has(m, "Country")) {
      static int logged;
      if (!logged++) diag_log("[sdjni] HLUnityCore.%s -> \"%s\" (pinned in config.h)", m, SD_COUNTRY_ISO2);
      return jni_make_string(SD_COUNTRY_ISO2);
    }
    if (has(m, "Locale"))   return jni_make_string(switch_locale());
    if (has(m, "Language")) return jni_make_string(switch_language());
  }

  if (path_is(eff, C_SLGLOBAL)) {
    if (has(r, "Landroid/app/Activity;") || has(r, "Landroid/content/Context;"))
      return jni_make_activity_object();
    /* SEGA's SLGlobal string getters, the complete set this game calls
     * (every AndroidJavaClass("com.sega.gamelib.SLGlobal") use in libil2cpp).
     * All four answered "" before; a phone answers each with a real value. */
    if (!strcmp(r, "Ljava/lang/String;")) {
      const char *v = NULL;
      if      (has(m, "GetAndroidID"))            v = install_id();
      else if (has(m, "GetAdvertisingID"))        v = "00000000-0000-0000-0000-000000000000"; /* Android's opted-out value */
      else if (has(m, "GetISOLocale_ThreeDigit")) v = SD_COUNTRY_ISO3;   /* a COUNTRY: ISOLocale.AUS */
      else if (has(m, "StoragePath"))             v = game_root();       /* Internal/External */
      else if (has(m, "Locale"))                  v = switch_locale();
      if (v) {
        static int logged;
        if (logged++ < 8) diag_log("[sdjni] SLGlobal.%s -> \"%s\"", m, v);
        return jni_make_string(v);
      }
    }
  }

  return sd_sdk_inert_object(id, eff);   /* everything else on a claimed class */
}

uint64_t sd_jni_int(void *recv, const void *id_, const char *eff, const void *jv, va_list *va) {
  const struct FakeID *id = id_;
  const char *m = id->name;
  (void)recv; (void)jv; (void)va;

  if (path_is(eff, C_CMP)) {
    if (has(m, "GetConsentStatus"))          return 1;   /* CMPConsentStatus.NotRequired */
    if (has(m, "IsConsentFlowComplete"))     return 1;
    if (has(m, "CanRequestPersonalisedAds")) return 0;   /* no -- and no ads at all */
    return 0;
  }
  /* BillingResult.getResponseCode -> BillingUnavailable (see startConnection). */
  if (path_is(eff, C_BILLRES) && has(m, "getResponseCode")) return 3;
  if (path_is(eff, C_SOCIAL) || path_is(eff, C_BILLING) || path_is(eff, C_BILLRES)) return 0;

  /* SLGlobal.GetCurrentLocale()I -> HLLanguageState.Locale (see locale_enum). */
  if (path_is(eff, C_SLGLOBAL) && has(m, "GetCurrentLocale")) return (uint64_t)locale_enum();

  /* HLUnityCore.Unity_GetPixelsFromNativeUnitDistance(float) -- Android native
   * units are dp, so px = dp x density. It answered 0, which collapses anything
   * the game sizes this way. The float arrives in jvalue.f on the ...A path, and
   * promoted to double on the variadic path. */
  if (path_is(eff, C_HLCORE) && has(m, "GetPixelsFromNativeUnitDistance")) {
    float dp = 0.0f;
    if (jv)      memcpy(&dp, jv, sizeof dp);
    else if (va) { va_list cp; va_copy(cp, *va); dp = (float)va_arg(cp, double); va_end(cp); }
    return (uint64_t)(int64_t)(dp * jni_display_density() + 0.5f);
  }

  /* Permissions: nothing is granted. POST_NOTIFICATIONS is the one this game
   * asks for, and "denied" simply leaves local notifications off. */
  if (path_is(eff, C_SLPERM) || path_is(eff, C_UPERM)) return 0;

  /* HLUnityCore: tracking authorisation is an iOS concept. Its Android path
   * begins with CanDeviceRequestTrackingAuthorisation(), so false is both the
   * truthful answer and the one that stops the flow before any callback. */
  return 0;
}

void sd_jni_void(void *recv, const void *id_, const char *eff, const void *jv, va_list *va) {
  const struct FakeID *id = id_;
  const char *m = id->name;
  (void)recv;

  /* ---- SEGA sign-in: com.sega.gamelib.social.SLSocialInterface -------------
   * The loading-screen hang. Read from the game's own Java (classes7.dex,
   * GooglePlayInterface): GPNotifySignIn(ok) stores "true\n<id>..." or "false";
   * SetUnityTargetActive() marks Unity ready and flushes a stored result;
   * SendAuthResponse() does UnitySendMessage("Community",
   * "OnGameCenterAuthenticateComplete", result). A phone offline fails sign-in
   * and sends "false"; the game continues as a guest. The dormant stub sent
   * nothing, so Community never heard back. No Google Play Games on a Switch:
   * "false" is also simply true. Same ordering as the Java. */
  if (path_is(eff, C_SOCIAL)) {
    static int target_active;
    static const char *pending;
    if (has(m, "SetUnityTargetActive")) {
      target_active = 1;
      if (pending) { defer_usm("Community", "OnGameCenterAuthenticateComplete", pending); pending = NULL; }
      return;
    }
    if (!strcmp(m, "Authenticate")) {
      diag_log("[social] Authenticate -> \"false\" (no Google Play Games; guest, as a phone offline)%s",
               target_active ? "" : " -- held until SetUnityTargetActive");
      if (target_active) defer_usm("Community", "OnGameCenterAuthenticateComplete", "false");
      else pending = "false";
      return;
    }
    return;   /* Initialise, logout, achievements...: nothing to do */
  }

  /* ---- Google Play Billing: startConnection(listener) -----------------------
   * Unity IAP waits for BillingClientStateListener.onBillingSetupFinished. The
   * dormant stub never called it. Answer as a device without Google Play does:
   * a BillingResult with GoogleBillingResponseCode.BillingUnavailable (3), so
   * IAP reports "purchasing unavailable" instead of waiting forever. */
  if (path_is(eff, C_BILLING) && !strcmp(m, "startConnection")) {
#if !SD_BILLING_CALLBACK
    diag_log("[billing] startConnection -- callback disabled (SD_BILLING_CALLBACK 0)");
    return;
#endif
    void *listener = obj_arg(id, 0, jv, va);
    diag_log("[billing] startConnection -> onBillingSetupFinished(BillingUnavailable) to %p", listener);
    jni_proxy_call_later(listener, "com/android/billingclient/api/BillingClientStateListener",
                         "onBillingSetupFinished", "(Lcom/android/billingclient/api/BillingResult;)V",
                         jni_make_object(C_BILLRES));
    return;
  }


  if (path_is(eff, C_CMP)) {
    if (has(m, "Initialise")) {
      const char *l = str_arg(id, 0, jv, va);
      if (l && *l) {
        strncpy(s_cmp_listener_buf, l, sizeof s_cmp_listener_buf - 1);
        s_cmp_listener = s_cmp_listener_buf;
      }
      diag_log("[cmp] Initialise -> callbacks go to \"%s\"", s_cmp_listener);
      return;
    }
    if (has(m, "RequestConsent")) {
      diag_log("[cmp] RequestConsent -> Native_OnRequestConsentSuccess next frame "
               "(status NotRequired: no ads, no collection)");
      defer_usm(s_cmp_listener, "Native_OnRequestConsentSuccess", "");
      return;
    }
    return;
  }

  if (path_is(eff, C_HLOUT)) {
    /* The game's own log stream -- LogError, LogException, breadcrumbs --
     * routed into debug.log. Errors are always kept; the rest is capped so a
     * chatty build cannot turn every frame into an SD-card write. */
    const char *text = str_arg(id, 0, jv, va);
    if (!text) return;
    int important = has(m, "rror") || has(m, "xception") || has(text, "Exception");
    if (important || s_hlout_lines < SD_HLOUT_MAX_LINES) {
      s_hlout_lines++;
      diag_log("[game] %s: %s", m, text);
    } else if (s_hlout_lines++ == SD_HLOUT_MAX_LINES) {
      diag_log("[game] (further non-error output suppressed)");
    }
    return;
  }

  if (path_is(eff, C_PAD)) {
    /* getAssetPackStates / downloadAssetPacks / requestToUseMobileData take
     * AndroidJavaProxy callbacks. Only game C# would call them, and this
     * game's code never does (its 8 AndroidAssetPacks references are all
     * inside Unity's own class). Logged so a future update that does is
     * visible rather than silently waiting on a callback. */
    diag_log("[pad] %s -- callback not driven (install-time pack already present)", m);
    return;
  }

  if ((path_is(eff, C_SLPERM) || path_is(eff, C_UPERM)) && has(m, "equest")) {
    diag_log("[perm] %s -> no callback fired; permission stays denied", m);
    return;
  }
}
