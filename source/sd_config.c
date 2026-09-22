/* sd_config.c -- config.txt: the player's settings, read at every launch.
 *
 * Modelled on bloonspop_nx's bp_config.c. The file lives next to the .nro
 * (GAME_HOME) and is written, documented inline, on first launch. A config.txt
 * from an older build that lacks an option gets that option's block appended,
 * so new settings appear without the player deleting the file.
 *
 *   resolution  the SHORT side in pixels, 720 (default) .. 1080. One setting for
 *               handheld and docked -- the Switch scales the picture to the
 *               panel or the TV. Snapped to a multiple of 18 so the 16:9 shape
 *               is exact and every buffer divides evenly (bloonspop_nx's rule).
 *   rotation    0 = none (landscape), 1 = 90 clockwise, 2 = 90 counter-
 *               clockwise: the game gets a portrait screen, rotated onto the
 *               landscape panel (sd_tate.c) -- for holding the console upright.
 *   language    auto, or one of the languages the game ships: en fr de it pt
 *               ru es. auto follows the Switch's language when the game has it,
 *               and falls back to English otherwise (bloonspop_nx dropped its
 *               option after the console's language handed its game one it
 *               could not render; restricting auto to the shipped set is the
 *               fix, not removing the choice).
 * MIT.
 */
#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "config.h"
#include "sd_config.h"
#include "util.h"

int sd_cfg_res      = 720;
int sd_cfg_rotation = 0;
static char s_lang[8] = "auto";

/* The languages Sonic Dash ships (its store listing): English, French,
 * German, Italian, Portuguese, Russian, Spanish. */
static const struct { const char *code, *name; } k_langs[] = {
  { "en", "english" }, { "fr", "french" },     { "de", "german" }, { "it", "italian" },
  { "pt", "portuguese" }, { "ru", "russian" }, { "es", "spanish" },
};

static const struct { const char *key; const char *block; } SECTIONS[] = {
  { "resolution",
    "# --- resolution --------------------------------------------------------\n"
    "# The picture's short side, in pixels:\n"
    "#    720  -> 1280 x  720   (default)\n"
    "#    810  -> 1440 x  810\n"
    "#    900  -> 1600 x  900\n"
    "#    990  -> 1760 x  990\n"
    "#   1080  -> 1920 x 1080\n"
    "# (sideways when rotated). Any value from 720 to 1080 is accepted and rounded\n"
    "# to the nearest size that keeps the exact 16:9 shape. One setting for both\n"
    "# handheld and docked. Higher is sharper, most visibly on a TV, but costs\n"
    "# performance.\n"
    "resolution = 720\n" },
  { "rotation",
    "# --- rotation ----------------------------------------------------------\n"
    "# Turn the picture to play with the console held upright:\n"
    "#   0 = none (default)\n"
    "#   1 = 90 degrees clockwise (right Joy-Con up)\n"
    "#   2 = 90 degrees counter-clockwise (left Joy-Con up)\n"
    "# Rotated, the game uses its phone-style portrait layout.\n"
    "rotation = 0\n" },
  { "language",
    "# --- language ----------------------------------------------------------\n"
    "# auto follows the Switch's language when the game has it, else English.\n"
    "# Or pick one: en (English), fr (French), de (German), it (Italian),\n"
    "# pt (Portuguese), ru (Russian), es (Spanish).\n"
    "language = auto\n" },
};

static void write_header(FILE *f) {
  fputs("# config.txt -- Sonic Dash (sonicdash_nx) settings, read at every launch.\n"
        "# Edit a value after the '='; anything after a '#' is a comment.\n\n", f);
}

/* Is there a line "key =" or "#key =" (any spacing)? Prose that merely
 * mentions the word does not count. */
static int has_key_line(const char *text, const char *key) {
  const size_t kl = strlen(key);
  for (const char *p = text; *p; ) {
    const char *q = p;
    while (*q == ' ' || *q == '\t' || *q == '#') q++;
    if (!strncmp(q, key, kl)) {
      const char *r = q + kl;
      while (*r == ' ' || *r == '\t') r++;
      if (*r == '=') return 1;
    }
    const char *nl = strchr(p, '\n');
    if (!nl) break;
    p = nl + 1;
  }
  return 0;
}

static char *trim(char *p) {
  while (isspace((unsigned char)*p)) p++;
  char *e = p + strlen(p);
  while (e > p && isspace((unsigned char)e[-1])) *--e = 0;
  return p;
}

static void apply(const char *key, const char *val) {
  if (!strcmp(key, "resolution")) {
    char *end; long v = strtol(val, &end, 10);
    if (*end || end == val) { debugPrintf("[config] resolution \"%s\" is not a number -- using %d\n", val, sd_cfg_res); return; }
    if (v < 720) v = 720;
    if (v > 1080) v = 1080;
    long s = ((v + 9) / 18) * 18;                          /* nearest exact 16:9 size */
    if (s != v) debugPrintf("[config] resolution %ld -> %ld (nearest exact 16:9 size)\n", v, s);
    sd_cfg_res = (int)s;
  } else if (!strcmp(key, "rotation")) {
    if (!strcmp(val, "0") || !strcmp(val, "1") || !strcmp(val, "2")) sd_cfg_rotation = val[0] - '0';
    else debugPrintf("[config] rotation \"%s\" -- use 0, 1 or 2; keeping %d\n", val, sd_cfg_rotation);
  } else if (!strcmp(key, "language")) {
    char v[16]; size_t i = 0;
    for (; val[i] && i < sizeof v - 1; i++) v[i] = (char)tolower((unsigned char)val[i]);
    v[i] = 0;
    if (!strcmp(v, "auto")) { strcpy(s_lang, "auto"); return; }
    for (size_t k = 0; k < sizeof k_langs / sizeof k_langs[0]; k++)
      if (!strcmp(v, k_langs[k].code) || !strcmp(v, k_langs[k].name)) { strcpy(s_lang, k_langs[k].code); return; }
    strcpy(s_lang, "auto");                                /* do what the log says */
    debugPrintf("[config] language \"%s\" is not one the game ships (en fr de it pt ru es) -- using auto\n", val);
  } else if (!strcmp(key, "screen_width") || !strcmp(key, "screen_height")) {
    debugPrintf("[config] %s is from an older build and is ignored -- use resolution\n", key);
  } else {
    debugPrintf("[config] unknown setting \"%s\" -- ignored\n", key);
  }
}

void sd_config_load(void) {
  char path[320]; snprintf(path, sizeof path, "%s/config.txt", GAME_HOME);
  char *text = NULL; long n = 0;
  FILE *f = fopen(path, "rb");
  if (f) {
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n > 0 && n < (1 << 20) && (text = malloc((size_t)n + 1))) {
      n = (long)fread(text, 1, (size_t)n, f); text[n] = 0;
    }
    fclose(f);
  }
  if (!text) {                                             /* first launch: full template */
    f = fopen(path, "w");
    if (f) {
      write_header(f);
      for (size_t i = 0; i < sizeof SECTIONS / sizeof SECTIONS[0]; i++) { fputs(SECTIONS[i].block, f); fputs("\n", f); }
      fclose(f);
      debugPrintf("[config] wrote %s\n", path);
    }
  } else {
    int appended = 0;                                      /* older file: add what it lacks */
    for (size_t i = 0; i < sizeof SECTIONS / sizeof SECTIONS[0]; i++) {
      if (has_key_line(text, SECTIONS[i].key)) continue;
      if (!appended && !(f = fopen(path, "a"))) break;
      if (!appended) fputs("\n", f);
      fputs(SECTIONS[i].block, f); fputs("\n", f);
      appended++;
    }
    if (appended) { fclose(f); debugPrintf("[config] added %d new option(s) to %s\n", appended, path); }
    for (char *ln = strtok(text, "\n"); ln; ln = strtok(NULL, "\n")) {
      char *hash = strchr(ln, '#');
      if (hash) *hash = 0;                                 /* inline comments allowed */
      char *eq = strchr(ln, '=');
      if (!eq) continue;
      *eq = 0;
      char *k = trim(ln), *v = trim(eq + 1);
      if (*k && *v) apply(k, v);
    }
    free(text);
  }
  debugPrintf("[config] resolution %dp, rotation %d, language %s (-> %s)\n",
              sd_cfg_res, sd_cfg_rotation, s_lang, sd_lang());
}

/* The two-letter language the whole port reports (Java Locale, SEGA's SLGlobal,
 * Hardlight's HLUnityCore). One answer, so the layers cannot disagree. */
const char *sd_lang(void) {
  if (strcmp(s_lang, "auto")) return s_lang;
  static char a[3];
  if (!a[0]) {
    char loc[9] = { 0 };
    u64 code = 0;
    if (R_SUCCEEDED(setInitialize())) {
      if (R_SUCCEEDED(setGetSystemLanguage(&code))) memcpy(loc, &code, 8);
      setExit();
    }
    a[0] = (char)tolower((unsigned char)loc[0]);
    a[1] = (char)tolower((unsigned char)loc[1]);
    int ok = 0;
    for (size_t k = 0; k < sizeof k_langs / sizeof k_langs[0]; k++) ok |= !strcmp(a, k_langs[k].code);
    if (!ok) { a[0] = 'e'; a[1] = 'n'; }                   /* ja zh ko nl ... -> English */
  }
  return a;
}
