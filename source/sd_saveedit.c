/* ---------------------------------------------------------------------------
 * sd_saveedit.c -- edit save.txt at boot from save_edit.txt (battd_nx design).
 *
 * THE SAVE (all of it derived from libil2cpp's PropertyStore, verified on a
 * real save: 362 properties, checksum 2920496155)
 *     line 1   format version ("-3")
 *     line 2   number of key/value pairs
 *     ...      key, value, key, value ...
 *     last     checksum
 * checksum = CRC-32 (IEEE table, init 0xFFFFFFFF, NO final XOR) over every
 * line above it, each ending "\n", followed by the salt "Artifical Key"
 * (PropertyStore.CalculateSaltAndHashedString -> Hardlight.HLCRC32.Generate).
 * On a mismatch the game RESETS the save -- which is why hand-editing save.txt
 * loses progress, and why this file exists.
 *
 * HOW IT EDITS
 * save_edit.txt is written once, every setting commented out. An uncommented
 * setting is applied at EVERY launch while it stays uncommented. Values are
 * changed inside properties the save already holds, with ONE exception: a
 * char.<Name> line for a character on the game's roster that the save does not
 * have yet creates its CharID_<Name> entry first, exactly as the game creates
 * one (the six CharacterSaveData fields in the game's order, locked, level 1,
 * 50 cards),
 * appended at the end as the game appends new properties. Nothing else is ever
 * added. Character fields are then spliced inside that JSON.
 *
 * SAFETY, in the order it happens (battd_nx's sequence)
 *   1. parse save.txt; stop if the layout is wrong or the checksum does not
 *      match (unless fix_hand_edited_save is set and the layout is intact)
 *   2. apply only the uncommented settings; if nothing differs, stop without
 *      writing
 *   3. serialize, then PARSE THE RESULT: checksum must verify and every edited
 *      value must read back as written
 *   4. keep the untouched original once as save.txt.orig
 *   5. write save.txt.tmp and rename it over save.txt, then commit the SD card
 * Portable C: the core compiles and is tested on a PC against a real save.
 * ------------------------------------------------------------------------- */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <errno.h>
#include <strings.h>   /* strcasecmp */
#include "sd_saveedit.h"
#include "sd_character_ids.h"   /* GENERATED: the game's CharacterIDs roster */
static const char *canon_char(const char *name);   /* roster lookup, below */
static int write_file(const char *path, const char *b, size_t n);

#ifdef __SWITCH__
#include <switch.h>
#include "config.h"
#include "util.h"
#define SE_LOG(...) debugPrintf(__VA_ARGS__)
#else
#define SE_LOG(...) printf(__VA_ARGS__)
#endif

#define SE_MAX_FILE (4u << 20)
#define SE_SALT     "Artifical Key"

/* ------------------------------------------------------------ checksum */
static uint32_t crc_table[256];
static void crc_init(void) {
  if (crc_table[1]) return;
  for (uint32_t n = 0; n < 256; n++) {
    uint32_t c = n;
    for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    crc_table[n] = c;
  }
}
static uint32_t crc_feed(uint32_t c, const char *p, size_t n) {
  for (size_t i = 0; i < n; i++) c = crc_table[(c ^ (uint8_t)p[i]) & 0xFF] ^ (c >> 8);
  return c;
}
/* content = every line above the checksum, each with its "\n" */
static uint32_t save_checksum(const char *content, size_t len) {
  crc_init();
  uint32_t c = crc_feed(0xFFFFFFFFu, content, len);
  return crc_feed(c, SE_SALT, sizeof SE_SALT - 1);          /* NO final XOR */
}

/* ------------------------------------------------------------ the save */
typedef struct { char *key, *value; } Pair;
typedef struct { char *version; Pair *p; int n; char *stored; int valid; } Save;

static void save_free(Save *s) {
  if (!s) return;
  free(s->version); free(s->stored);
  for (int i = 0; i < s->n; i++) { free(s->p[i].key); free(s->p[i].value); }
  free(s->p);
  memset(s, 0, sizeof *s);
}

static char *dupn(const char *p, size_t n) {
  char *r = malloc(n + 1);
  if (r) { memcpy(r, p, n); r[n] = 0; }
  return r;
}

/* Parse; returns 0 and fills *s, or -1 with a reason in err. */
static int save_parse(const char *buf, size_t len, Save *s, char *err, size_t errsz) {
  memset(s, 0, sizeof *s);
  size_t nl = 0;
  for (size_t i = 0; i < len; i++) if (buf[i] == '\n') nl++;
  if (!len || buf[len - 1] != '\n') { snprintf(err, errsz, "does not end with a newline (truncated?)"); return -1; }
  char **line = calloc(nl, sizeof *line);
  size_t *ll = calloc(nl, sizeof *ll);
  if (!line || !ll) { free(line); free(ll); snprintf(err, errsz, "out of memory"); return -1; }
  size_t k = 0, start = 0;
  for (size_t i = 0; i < len; i++) if (buf[i] == '\n') {
    size_t e = i;
    if (e > start && buf[e - 1] == '\r') e--;                 /* tolerate CRLF */
    line[k] = (char *)buf + start; ll[k] = e - start; k++; start = i + 1;
  }
  int rc = -1;
  if (k < 3) { snprintf(err, errsz, "too short to be a Sonic Dash save"); goto out; }
  char tmp[32];
  snprintf(tmp, sizeof tmp, "%.*s", (int)(ll[1] < 31 ? ll[1] : 31), line[1]);
  char *endp; long count = strtol(tmp, &endp, 10);
  if (*endp || count < 0 || (size_t)count * 2 + 3 != k) {
    snprintf(err, errsz, "layout wrong: header says %ld properties, file holds %zu lines", count, k);
    goto out;
  }
  s->version = dupn(line[0], ll[0]);
  s->stored  = dupn(line[k - 1], ll[k - 1]);
  s->n = (int)count;
  s->p = calloc((size_t)count ? (size_t)count : 1, sizeof *s->p);
  if (!s->version || !s->stored || !s->p) { snprintf(err, errsz, "out of memory"); goto out; }
  for (long i = 0; i < count; i++) {
    s->p[i].key   = dupn(line[2 + 2 * i], ll[2 + 2 * i]);
    s->p[i].value = dupn(line[3 + 2 * i], ll[3 + 2 * i]);
    if (!s->p[i].key || !s->p[i].value) { snprintf(err, errsz, "out of memory"); goto out; }
  }
  /* checksum over the lines above it, exactly as they are in the file */
  { const size_t clen = (size_t)(line[k - 1] - buf);
    char want[16]; snprintf(want, sizeof want, "%u", save_checksum(buf, clen));
    s->valid = !strcmp(want, s->stored); }
  rc = 0;
out:
  free(line); free(ll);
  if (rc) save_free(s);
  return rc;
}

/* Serialize with a fresh checksum. Returns a malloc'd buffer (len in *out_len). */
static char *save_serialize(const Save *s, size_t *out_len) {
  size_t cap = strlen(s->version) + 48;
  for (int i = 0; i < s->n; i++) cap += strlen(s->p[i].key) + strlen(s->p[i].value) + 2;
  char *b = malloc(cap);
  if (!b) return NULL;
  size_t o = (size_t)snprintf(b, cap, "%s\n%d\n", s->version, s->n);
  for (int i = 0; i < s->n; i++) o += (size_t)snprintf(b + o, cap - o, "%s\n%s\n", s->p[i].key, s->p[i].value);
  o += (size_t)snprintf(b + o, cap - o, "%u\n", save_checksum(b, o));
  *out_len = o;
  return b;
}

static Pair *find_prop(Save *s, const char *key) {
  for (int i = 0; i < s->n; i++) if (!strcmp(s->p[i].key, key)) return &s->p[i];
  return NULL;
}

/* ------------------------------------------------------------ settings */
static char *trim(char *p) {
  while (isspace((unsigned char)*p)) p++;
  char *e = p + strlen(p);
  while (e > p && isspace((unsigned char)e[-1])) *--e = 0;
  return p;
}
static int is_int(const char *v) {
  if (*v == '-') v++;
  if (!*v || strlen(v) > 10) return 0;
  for (; *v; v++) if (!isdigit((unsigned char)*v)) return 0;
  return 1;
}
static int is_ascii_line(const char *v) {
  for (; *v; v++) if ((unsigned char)*v < 0x20 || (unsigned char)*v > 0x7E) return 0;
  return 1;
}

/* Friendly names -> the property they set. Rings is the wallet: in a real save
 * Banked Rings Total 602 = RingsBanked_Total 462 + RingsPurchased_Total 840 -
 * RingsSpent_Total 700. */
static const struct { const char *name, *prop; } k_alias[] = {
  { "rings",          "Banked Rings Total"    },
  { "red_star_rings", "Star Rings Total"      },
  { "gems",           "GemTotalProperty"      },
  { "flickies",       "FlickyStorageProperty" },
  { "xp",             "xpAmount"              },
};

/* Replace the number after "field": inside a JSON value; only if present. */
static int json_set_number(char **json, const char *field, long long v, int bitop /*0 set,1 or,2 andnot*/) {
  char pat[64]; snprintf(pat, sizeof pat, "\"%s\":", field);
  char *at = strstr(*json, pat);
  if (!at) return -1;
  char *num = at + strlen(pat), *e = num;
  if (*e == '-') e++;
  while (isdigit((unsigned char)*e)) e++;
  if (e == num) return -1;
  long long old = strtoll(num, NULL, 10), nv = bitop == 1 ? (old | v) : bitop == 2 ? (old & ~v) : v;
  char nb[32]; snprintf(nb, sizeof nb, "%lld", nv);
  size_t pre = (size_t)(num - *json), post = strlen(e);
  char *r = malloc(pre + strlen(nb) + post + 1);
  if (!r) return -1;
  memcpy(r, *json, pre); strcpy(r + pre, nb); strcpy(r + pre + strlen(nb), e);
  free(*json); *json = r;
  return old == nv ? 0 : 1;
}

/* Apply one "key = value"; returns 1 if the save changed, 0 if not, -1 skipped. */
/* A character's entry exactly as the game writes it: CharacterSaveData's six
 * serialized fields in declaration order (JsonUtility), with the starting
 * values every new character in a real save has -- locked (m_flags bit 0
 * clear), level 1, phase 0 -- and 50 cards, the editor's default (a cards
 * line overrides it). The name is not inside: it lives only in the key. */
#define NEW_CHARACTER_JSON \
  "{\"m_flags\":0,\"m_powerUps\":{\"m_data\":[]},\"m_upgrades\":{\"m_data\":[]},\"m_cards\":50,\"m_level\":1,\"m_phase\":0}"

/* Append a property at the end, as the game itself does the first time it
 * saves one. save_serialize writes the count from s->n, and step 3's read-back
 * compares every pair, so a new entry is verified like any other. */
static Pair *append_prop(Save *s, const char *key, const char *value) {
  Pair *np = realloc(s->p, (size_t)(s->n + 1) * sizeof *s->p);
  if (!np) return NULL;
  s->p = np;
  Pair *p = &s->p[s->n];
  p->key = dupn(key, strlen(key));
  p->value = dupn(value, strlen(value));
  if (!p->key || !p->value) { free(p->key); free(p->value); return NULL; }
  s->n++;
  return p;
}

/* Find a character's entry -- creating it the way the game does if needed --
 * and apply one ALREADY-VALIDATED field. quiet: no per-character lines (char.all
 * logs one summary). Returns 1 changed, 0 unchanged, -1 skipped; *made is set
 * when the entry was created. */
static int apply_char(Save *s, const char *id, int is_num, int is_level, long long num, int on, int quiet, int *made) {
  char prop[96]; snprintf(prop, sizeof prop, "CharID_%s", id);
  Pair *p = find_prop(s, prop);
  *made = 0;
  if (!p) {
    if (!(p = append_prop(s, prop, NEW_CHARACTER_JSON))) { SE_LOG("[saveedit] %s: out of memory -- skipped\n", prop); return -1; }
    if (!quiet) SE_LOG("[saveedit] created %s (locked, level 1, 50 cards): the game had not added %s yet\n", prop, id);
    *made = 1;
  }
  const int r = is_num ? json_set_number(&p->value, is_level ? "m_level" : "m_cards", num, 0)
                       : json_set_number(&p->value, "m_flags", 1, on ? 1 : 2);
  if (r < 0) { if (!quiet) SE_LOG("[saveedit] %s: field not present -- skipped\n", prop); return *made ? 1 : -1; }
  return (r > 0 || *made) ? 1 : 0;
}

/* char.all.<field> = value: the same validated setting for every character on
 * the roster (created if missing, exactly as for a single line) and for any
 * character already in the save that the roster does not know -- a later game
 * version. The caller applies char.all lines FIRST, so a line for one
 * character overrides them wherever it sits in the file. */
static int apply_all(Save *s, const char *key, const char *val, int is_num, int is_level, long long num, int on) {
  int n = 0, made_total = 0, changed = 0;
  for (int i = 0; i < SD_CHARACTER_COUNT; i++) {
    int made = 0; const int r = apply_char(s, SD_CHARACTER_IDS[i], is_num, is_level, num, on, 1, &made);
    if (r < 0) continue;
    n++; made_total += made; changed += r > 0;
  }
  const int saved_n = s->n;                            /* no creation below: s->p stays put */
  for (int k = 0; k < saved_n; k++) {
    if (strncmp(s->p[k].key, "CharID_", 7)) continue;
    const char *id = s->p[k].key + 7; int known = 0;
    for (int i = 0; i < SD_CHARACTER_COUNT && !known; i++) known = !strcmp(id, SD_CHARACTER_IDS[i]);
    if (known) continue;
    int made = 0; const int r = apply_char(s, id, is_num, is_level, num, on, 1, &made);
    if (r < 0) continue;
    n++; changed += r > 0;
  }
  SE_LOG("[saveedit] %s = %s: %d characters (%d created, %d changed)\n", key, val, n, made_total, changed);
  return changed ? 1 : 0;
}

/* PowerUpValues JSON, exactly as the game writes it: {"m_data":[e,e,...]} with
 * each entry (count << 9) | type. Decoded from the game's own serialiser:
 * OnBeforeSerialize packs `orr w8, w9, w8, lsl #9` (type | count << 9);
 * OnAfterDeserialize unpacks type = e & 0x1FF, count = e >> 9. count 0 removes
 * the type's entry. Every other entry is kept as it was, in order. Returns 1
 * changed, 0 unchanged, -1 if the value is not in that form. */
static int powerup_set(char **json, unsigned type, unsigned count) {
  const char *a = strstr(*json, "\"m_data\":[");
  if (!a) return -1;
  const char *p = a + 10, *close = strchr(p, ']');
  if (!close) return -1;
  unsigned vals[64]; int n = 0, found = -1;
  while (p < close) {
    while (p < close && (*p == ' ' || *p == ',')) p++;
    if (p >= close) break;
    char *e; const unsigned long v = strtoul(p, &e, 10);
    if (e == p || e > close || n >= 63) return -1;
    if ((v & 0x1FF) == type) found = n;
    vals[n++] = (unsigned)v; p = e;
  }
  const unsigned want = (count << 9) | type;
  if (count) {
    if (found >= 0) { if (vals[found] == want) return 0; vals[found] = want; }
    else vals[n++] = want;
  } else {
    if (found < 0) return 0;
    memmove(&vals[found], &vals[found + 1], (size_t)(n - found - 1) * sizeof vals[0]); n--;
  }
  const size_t pre = (size_t)(a + 10 - *json), post = strlen(close);
  char *out = malloc(pre + (size_t)n * 12 + post + 1);
  if (!out) return -1;
  memcpy(out, *json, pre); size_t o = pre;
  for (int i = 0; i < n; i++) o += (size_t)sprintf(out + o, i ? ",%u" : "%u", vals[i]);
  memcpy(out + o, close, post + 1);
  free(*json); *json = out;
  return 1;
}

static int apply_setting(Save *s, const char *key, const char *val) {
  for (size_t i = 0; i < sizeof k_alias / sizeof k_alias[0]; i++) {
    if (strcmp(key, k_alias[i].name)) continue;
    if (!is_int(val) || val[0] == '-') { SE_LOG("[saveedit] %s = %s: not a whole number >= 0 -- skipped\n", key, val); return -1; }
    Pair *p = find_prop(s, k_alias[i].prop);
    if (!p) { SE_LOG("[saveedit] %s: '%s' is not in save.txt yet -- skipped\n", key, k_alias[i].prop); return -1; }
    if (!strcmp(p->value, val)) return 0;
    SE_LOG("[saveedit] %s: %s -> %s\n", key, p->value, val);
    free(p->value); p->value = dupn(val, strlen(val));
    return 1;
  }
  /* free_revive / double_rings: PowerUpsInventory's two SinglePurchasePowerUps
   * (PowerUpType.FreeRevive = 10, DoubleRing = 7). Both are SharedPowerUps, so
   * they live in SharedPowerUpCount, not in a character's entry; owning one is
   * a count of 1. ShouldFreeReviveShow offers the free revive when the count is
   * >= 1, once per run, and using it does not consume it. */
  if (!strcasecmp(key, "free_revive") || !strcasecmp(key, "double_rings")) {
    const unsigned type = !strcasecmp(key, "free_revive") ? 10u : 7u;
    const int on = !strcasecmp(val, "true") || !strcmp(val, "1");
    if (!on && strcasecmp(val, "false") && strcmp(val, "0")) { SE_LOG("[saveedit] %s = %s: use true or false -- skipped\n", key, val); return -1; }
    Pair *p = find_prop(s, "SharedPowerUpCount");
    if (!p) { SE_LOG("[saveedit] %s: SharedPowerUpCount is not in save.txt yet (play one run first) -- skipped\n", key); return -1; }
    const int r = powerup_set(&p->value, type, on ? 1u : 0u);
    if (r < 0) { SE_LOG("[saveedit] %s: SharedPowerUpCount is not in the form the game writes -- skipped\n", key); return -1; }
    if (r > 0) SE_LOG("[saveedit] %s = %s\n", key, val);
    return r;
  }
  if (!strncmp(key, "char.", 5)) {
    char name[64], field[16];
    const char *dot = strrchr(key, '.');
    if (dot == key + 4 || (size_t)(dot - key - 5) >= sizeof name) { SE_LOG("[saveedit] %s: expected char.<Name>.<level|cards|owned> -- skipped\n", key); return -1; }
    snprintf(name, sizeof name, "%.*s", (int)(dot - key - 5), key + 5);
    snprintf(field, sizeof field, "%s", dot + 1);
    /* 1. validate the setting BEFORE touching the save, so a bad value can
     *    never leave a newly created entry behind */
    const int is_num = !strcasecmp(field, "level") || !strcasecmp(field, "cards");
    int on = 0;
    if (is_num) {
      if (!is_int(val) || val[0] == '-') { SE_LOG("[saveedit] %s = %s: not a whole number >= 0 -- skipped\n", key, val); return -1; }
    } else if (!strcasecmp(field, "owned")) {
      on = !strcasecmp(val, "true") || !strcmp(val, "1");
      if (!on && strcasecmp(val, "false") && strcmp(val, "0")) { SE_LOG("[saveedit] %s = %s: use true or false -- skipped\n", key, val); return -1; }
    } else { SE_LOG("[saveedit] %s: unknown character field '%s' (level, cards, owned) -- skipped\n", key, field); return -1; }
    const long long num = is_num ? atoll(val) : 0;
    const int is_level = !strcasecmp(field, "level");
    /* char.all.<field>: every character at once */
    if (!strcasecmp(name, "all")) return apply_all(s, key, val, is_num, is_level, num, on);
    /* 2. one character: a name neither on the roster nor in the save is a typo,
     *    and a typo must not invent a character */
    const char *cname = canon_char(name);
    if (cname == name) {
      char prop[96]; snprintf(prop, sizeof prop, "CharID_%s", name);
      if (!find_prop(s, prop)) {
        SE_LOG("[saveedit] %s: '%s' is not a character in this version of the game "
               "(see the list at the end of save_edit.txt) -- skipped\n", key, name);
        return -1;
      }
    }
    /* 3. find or create, then apply */
    int made = 0;
    const int r = apply_char(s, cname, is_num, is_level, num, on, 0, &made);
    if (r > 0) SE_LOG("[saveedit] %s = %s\n", key, val);
    return r;
  }
  if (!strncmp(key, "prop.", 5)) {
    Pair *p = find_prop(s, key + 5);
    if (!p) { SE_LOG("[saveedit] %s: no property '%s' in save.txt -- skipped (nothing is ever added)\n", key, key + 5); return -1; }
    if (!strcmp(p->value, val)) return 0;
    SE_LOG("[saveedit] %s: %s -> %s\n", key, p->value, val);
    free(p->value); p->value = dupn(val, strlen(val));
    return 1;
  }
  SE_LOG("[saveedit] unknown setting '%s' -- skipped\n", key);
  return -1;
}

/* ------------------------------------------------------------ files */
static char *read_file(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  char *b = malloc(SE_MAX_FILE + 1);
  size_t n = b ? fread(b, 1, SE_MAX_FILE + 1, f) : 0;
  fclose(f);
  if (!b || n > SE_MAX_FILE) { free(b); return NULL; }
  b[n] = 0; *len = n;
  return b;
}
static int write_file(const char *path, const char *b, size_t n) {
  FILE *f = fopen(path, "wb");
  if (!f) return 0;
  const int ok = fwrite(b, 1, n, f) == n;
  return (fclose(f) == 0) && ok;
}

/* The full roster, last in the file so the sections above stay short. The
 * marker also tells an older save_edit.txt (written before the roster existed)
 * that it still needs the list appended. */
#define ROSTER_MARKER "# --- all characters ---"
#define ALL_MARKER "# --- every character at once ---"
static const char *const ALL_BLOCK[] = {
  ALL_MARKER "--------------------------------------------",
  "# char.all.<field> sets it for EVERY character in the list below. Characters",
  "# not in your save yet are added first (locked, level 1, 50 cards). A line",
  "# for one character overrides it, wherever it is in the file.",
  "#char.all.level = 10",
  "#char.all.cards = 50",
  "#char.all.owned = true",
};
#define ALL_BLOCK_N ((int)(sizeof ALL_BLOCK / sizeof ALL_BLOCK[0]))
#define POWER_MARKER "# --- power-ups ---"
static const char *const POWER_BLOCK[] = {
  POWER_MARKER "-------------------------------------------------------",
  "# One-off purchases the game sells, kept in SharedPowerUpCount. free_revive",
  "# gives you one free revive in every run; double_rings doubles the rings",
  "# you collect. false takes either away again.",
  "#free_revive = true",
  "#double_rings = true",
};
#define POWER_BLOCK_N ((int)(sizeof POWER_BLOCK / sizeof POWER_BLOCK[0]))
#define OWNED_NOTE_1 "# owned = true unlocks a character: crossovers (LEGO, Movie, Sanrio, Pac-Man,"
#define OWNED_NOTE_2 "# Angry Birds...) are limited edition and only appear in the menu once owned."
static void write_roster(FILE *f) {
  fputs("\n", f);
  for (int i = 0; i < ALL_BLOCK_N; i++) { fputs(ALL_BLOCK[i], f); fputs("\n", f); }
  fprintf(f, "\n" ROSTER_MARKER "-------------------------------------------------\n"
             "# Every playable character in this version of the game (%d), in the order\n"
             "# they joined, by the exact name the save uses (the game's own CharacterIDs).\n"
             "# A character not in your save yet is added first (locked, level 1, 50 cards).\n"
             OWNED_NOTE_1 "\n" OWNED_NOTE_2 "\n",
          SD_CHARACTER_COUNT);
  for (int i = 0; i < SD_CHARACTER_COUNT; i++)
    fprintf(f, "#char.%s.level = 10\n#char.%s.cards = 50\n#char.%s.owned = true\n",
            SD_CHARACTER_IDS[i], SD_CHARACTER_IDS[i], SD_CHARACTER_IDS[i]);
}

/* Parse a roster-style line -- "char.<Name>.<field> = ..." with or without a
 * leading '#' -- into the roster index (-1 if the name is not on it) and the
 * field. Returns 0 if the line is not of that shape. */
static int parse_char_line(const char *p, const char *end, int *idx, char *field, size_t fsz) {
  while (p < end && (*p == ' ' || *p == '\t' || *p == '#')) p++;
  if (end - p < 6 || strncasecmp(p, "char.", 5)) return 0;
  p += 5;
  const char *dot = memchr(p, '.', (size_t)(end - p));
  if (!dot || dot == p || dot - p >= 64) return 0;
  char name[64]; memcpy(name, p, (size_t)(dot - p)); name[dot - p] = 0;
  const char *f = dot + 1, *fe = f;
  while (fe < end && *fe != ' ' && *fe != '\t' && *fe != '=') fe++;
  if (fe == f || (size_t)(fe - f) >= fsz) return 0;
  memcpy(field, f, (size_t)(fe - f)); field[fe - f] = 0;
  while (fe < end && (*fe == ' ' || *fe == '\t')) fe++;
  if (fe >= end || *fe != '=') return 0;
  *idx = -1;
  for (int i = 0; i < SD_CHARACTER_COUNT; i++) if (!strcasecmp(name, SD_CHARACTER_IDS[i])) { *idx = i; break; }
  return 1;
}

/* A roster written before owned lines existed gets "#char.<Name>.owned = true"
 * inserted under each character's cards line. Line-preserving: every existing
 * line is kept byte for byte -- your own settings included -- and only
 * COMMENTED lines are added, so no setting changes by itself. Line endings
 * follow the file's (CRLF stays CRLF). Written to .tmp and renamed over, like
 * save.txt. Returns the number of owned lines inserted. */
static int add_owned_lines(const char *edit_path, const char *text, size_t len) {
  const char *m = strstr(text, ROSTER_MARKER);
  if (!m) return 0;
  const char *end = text + len;
  const char *nl = strstr(text, "\r\n") ? "\r\n" : "\n";
  unsigned char have[SD_CHARACTER_COUNT] = { 0 };
  char field[16]; int idx, missing = 0;
  for (const char *p = m; p < end; ) {                 /* owned lines already in the roster */
    const char *e = memchr(p, '\n', (size_t)(end - p)); const char *le = e ? e : end;
    if (parse_char_line(p, le, &idx, field, sizeof field) && idx >= 0 && !strcasecmp(field, "owned")) have[idx] = 1;
    p = e ? e + 1 : end;
  }
  for (int i = 0; i < SD_CHARACTER_COUNT; i++) missing += !have[i];
  const int need_all = strstr(text, ALL_MARKER) == NULL;  /* the char.all block, above the list */
  const int need_power = strstr(text, POWER_MARKER) == NULL;  /* free_revive / double_rings */
  if (!missing && !need_all && !need_power) return 0;
  const int note = strstr(text, OWNED_NOTE_1) == NULL;  /* one line: matches CRLF files too */
  char *out = malloc(len + (size_t)missing * 96 + 2048);
  if (!out) return 0;
  size_t o = 0; int inserted = 0;
  const char *me = memchr(m, '\n', (size_t)(end - m));
  const char *head_end = me ? me + 1 : end;            /* up to and including the marker line */
  const char *ms = m;                                  /* start of the marker line */
  while (ms > text && ms[-1] != '\n') ms--;
  const char *cs = ms;                                 /* the power-ups block goes above "characters" */
  if (need_power) {
    const char *ch = strstr(text, "# --- characters");
    if (ch && ch < ms) { cs = ch; while (cs > text && cs[-1] != '\n') cs--; }
  }
  memcpy(out, text, (size_t)(cs - text)); o = (size_t)(cs - text);
  if (need_power) {
    for (int i = 0; i < POWER_BLOCK_N; i++) o += (size_t)sprintf(out + o, "%s%s", POWER_BLOCK[i], nl);
    o += (size_t)sprintf(out + o, "%s", nl);
  }
  memcpy(out + o, cs, (size_t)(ms - cs)); o += (size_t)(ms - cs);
  if (need_all) {                                      /* char.all block + blank line, above the list */
    for (int i = 0; i < ALL_BLOCK_N; i++) o += (size_t)sprintf(out + o, "%s%s", ALL_BLOCK[i], nl);
    o += (size_t)sprintf(out + o, "%s", nl);
  }
  memcpy(out + o, ms, (size_t)(head_end - ms)); o += (size_t)(head_end - ms);
  if (note) o += (size_t)sprintf(out + o, "%s%s%s%s", OWNED_NOTE_1, nl, OWNED_NOTE_2, nl);  /* the file's own line endings */
  for (const char *p = head_end; p < end; ) {
    const char *e = memchr(p, '\n', (size_t)(end - p)); const char *le = e ? e : end;
    const char *next = e ? e + 1 : end;
    memcpy(out + o, p, (size_t)(next - p)); o += (size_t)(next - p);
    if (parse_char_line(p, le, &idx, field, sizeof field) && idx >= 0 && !strcasecmp(field, "cards") && !have[idx]) {
      if (!e) o += (size_t)sprintf(out + o, "%s", nl);  /* last line had no line ending */
      o += (size_t)sprintf(out + o, "#char.%s.owned = true%s", SD_CHARACTER_IDS[idx], nl);
      have[idx] = 1; inserted++;
    }
    p = next;
  }
  if (inserted || need_all || need_power) {
    char tmp[512]; snprintf(tmp, sizeof tmp, "%s.tmp", edit_path);
    int ok = write_file(tmp, out, o);
    if (ok) { remove(edit_path); ok = rename(tmp, edit_path) == 0 || write_file(edit_path, out, o); }
    if (ok && inserted) SE_LOG("[saveedit] added an owned = true line under %d character(s) in save_edit.txt\n", inserted);
    if (ok && need_all) SE_LOG("[saveedit] added the char.all lines (every character at once) to save_edit.txt\n");
    if (ok && need_power) SE_LOG("[saveedit] added the power-ups lines (free_revive, double_rings) to save_edit.txt\n");
    if (!ok) SE_LOG("[saveedit] could not update the character list in save_edit.txt\n");
    if (!ok) inserted = 0;
  }
  free(out);
  return inserted;
}

/* Save keys are case-sensitive (CharID_Shadow); accept any capitalisation by
 * mapping to the game's exact ID. A name not on the roster passes through as
 * typed, so a character added by a later game version still works. */
static const char *canon_char(const char *name) {
  for (int i = 0; i < SD_CHARACTER_COUNT; i++)
    if (!strcasecmp(name, SD_CHARACTER_IDS[i])) return SD_CHARACTER_IDS[i];
  return name;
}

static void write_template(const char *path) {
  FILE *f = fopen(path, "w");
  if (!f) { SE_LOG("[saveedit] could not write %s\n", path); return; }
  fputs(
"# save_edit.txt -- Sonic Dash save editing (sonicdash_nx).\n"
"#\n"
"# Every line is commented out. Remove the '#' from one and give it a value; it\n"
"# is applied to save.txt at EVERY launch, for as long as the line stays\n"
"# uncommented -- \"rings = 50000\" puts you back at 50000 rings each launch.\n"
"# Comment it out again once the change has taken.\n"
"#\n"
"# save.txt is protected by a checksum: editing it directly makes the game reset\n"
"# your progress. Editing here is safe -- the port re-signs the save exactly as\n"
"# the game does, and keeps the untouched original once as save.txt.orig.\n"
"#\n"
"# Only values your save already holds are changed -- except that a char. line\n"
"# for a character you haven't met yet creates their entry first, as the game would.\n"
"# Anything that could not be applied is reported in debug.log ([saveedit]).\n"
"# Quit the game fully before editing: it saves on the way out.\n"
"\n"
"# --- currencies ----------------------------------------------------------\n"
"#rings = 50000\n"
"#red_star_rings = 500\n"
"#gems = 1000\n"
"#flickies = 100\n"
"\n"
"# --- player --------------------------------------------------------------\n"
"#xp = 5000\n"
"\n", f);
  for (int i = 0; i < POWER_BLOCK_N; i++) { fputs(POWER_BLOCK[i], f); fputs("\n", f); }
  fputs("\n"
"# --- characters ----------------------------------------------------------\n"
"# char.<Name>.level and .cards set a character's level and card count. Every\n"
"# character's name is listed at the end of this file; any capitalisation works.\n"
"# char.all.level / .cards / .owned do every character at once (above the list).\n"
"# A character you haven't met yet gets their entry created first, exactly as\n"
"# the game would (locked, level 1, 50 cards), and then your values are applied.\n"
"# Crossover characters (LEGO, Movie, Sanrio, Pac-Man, Angry Birds...) are\n"
"# limited edition: the menu only shows them once owned (owned = true) or once\n"
"# you hold enough cards to unlock them.\n"
"#char.Sonic.level = 10\n"
"#char.Tails.cards = 50\n"
"# owned = true unlocks a character (the owned bit of m_flags). The game forces\n"
"# only two IDs at every load -- Sonic unlocked, Android locked -- so it\n"
"# holds for every other character. Every character has an owned line in the\n"
"# list at the end.\n"
"#char.Tails.owned = true\n"
"\n"
"# --- anything else -------------------------------------------------------\n"
"# prop.<Exact Name> = value sets any property already in save.txt, verbatim\n"
"# (the line under its name). One line, plain ASCII.\n"
"#prop.MusicVolume = 0.5\n"
"\n"
"# --- repair --------------------------------------------------------------\n"
"# If you edited save.txt by hand, the game would reset it. Uncomment this to\n"
"# have the port re-sign a hand-edited save.txt instead -- only when its layout\n"
"# is intact; the copy before repair is kept as save.txt.orig.\n"
"#fix_hand_edited_save = true\n", f);
  write_roster(f);
  fclose(f);
  SE_LOG("[saveedit] wrote %s (every setting commented out)\n", path);
}

/* ------------------------------------------------------------ entry */
int sd_saveedit_run_paths(const char *save_path, const char *edit_path) {
  size_t elen = 0;
  char *edit = read_file(edit_path, &elen);
  if (!edit) { write_template(edit_path); return 0; }
  if (!strstr(edit, ROSTER_MARKER)) {                  /* written by an older build */
    FILE *af = fopen(edit_path, "a");
    if (af) {
      write_roster(af);
      fclose(af);
      SE_LOG("[saveedit] added the full character list (%d) to %s\n", SD_CHARACTER_COUNT, edit_path);
      size_t l2 = 0; char *again = read_file(edit_path, &l2);   /* and the blocks it still lacks */
      if (again) { add_owned_lines(edit_path, again, l2); free(again); }
    }
  } else {
    add_owned_lines(edit_path, edit, elen);             /* a roster from before owned lines */
  }

  /* Collect uncommented settings first: no work at all if there are none. */
  static struct { char *k, *v; } set[1024];          /* 142 characters x 2 lines fit easily */
  int ns = 0, fix = 0, dropped = 0;
  for (char *ln = strtok(edit, "\n"); ln; ln = strtok(NULL, "\n")) {
    char *t = trim(ln);
    if (!*t || *t == '#') continue;
    char *eq = strchr(t, '=');
    if (!eq) { SE_LOG("[saveedit] ignoring '%s' (no '=')\n", t); continue; }
    *eq = 0;
    char *k = trim(t), *v = trim(eq + 1);
    if (!strcmp(k, "fix_hand_edited_save")) { fix = !strcmp(v, "true") || !strcmp(v, "1"); continue; }
    if (!is_ascii_line(v)) { SE_LOG("[saveedit] %s: value must be one line of plain ASCII -- skipped\n", k); continue; }
    if (ns < (int)(sizeof set / sizeof set[0])) { set[ns].k = k; set[ns].v = v; ns++; }
    else dropped++;
  }
  if (dropped) SE_LOG("[saveedit] more than %d settings -- the last %d ignored\n", (int)(sizeof set / sizeof set[0]), dropped);
  if (!ns && !fix) { free(edit); return 0; }

  size_t slen = 0;
  char *raw = read_file(save_path, &slen);
  if (!raw) { SE_LOG("[saveedit] no save.txt yet -- play once, quit, then edit\n"); free(edit); return 0; }
  Save s; char err[160];
  if (save_parse(raw, slen, &s, err, sizeof err)) {
    SE_LOG("[saveedit] save.txt not edited: %s\n", err);
    free(raw); free(edit); return -1;
  }
  const int was_valid = s.valid;
  if (!s.valid && !fix) {
    SE_LOG("[saveedit] save.txt's checksum does not match (edited by hand?) -- not touched. The game "
           "will reset it; set fix_hand_edited_save = true in save_edit.txt to re-sign it instead.\n");
    save_free(&s); free(raw); free(edit); return -1;
  }

  int changed = !was_valid, applied = 0;    /* a repair is itself a change */
  /* char.all lines first, then everything else: a line for one character
   * overrides char.all wherever it sits in the file */
  for (int pass = 0; pass < 2; pass++)
    for (int i = 0; i < ns; i++) {
      if ((strncasecmp(set[i].k, "char.all.", 9) == 0) != (pass == 0)) continue;
      const int r = apply_setting(&s, set[i].k, set[i].v);
      if (r >= 0) applied++;
      if (r > 0) changed = 1;
    }
  if (!changed) {
    SE_LOG(applied ? "[saveedit] settings already match save.txt -- nothing written\n"
                   : "[saveedit] no setting could be applied (see above) -- nothing written\n");
    save_free(&s); free(raw); free(edit); return 0;
  }

  size_t olen = 0;
  char *out = save_serialize(&s, &olen);
  int ok = out != NULL;
  if (ok) {                                  /* 3. read it back before trusting it */
    Save chk; char e2[160];
    ok = !save_parse(out, olen, &chk, e2, sizeof e2) && chk.valid && chk.n == s.n;
    for (int i = 0; ok && i < s.n; i++)
      ok = !strcmp(chk.p[i].key, s.p[i].key) && !strcmp(chk.p[i].value, s.p[i].value);
    if (!ok) SE_LOG("[saveedit] verification of the new save FAILED -- nothing written\n");
    save_free(&chk);
  }
  if (ok) {
    char orig[512], tmp[512];
    snprintf(orig, sizeof orig, "%s.orig", save_path);
    snprintf(tmp, sizeof tmp, "%s.tmp", save_path);
    FILE *o = fopen(orig, "rb");             /* 4. keep the untouched original once */
    if (o) fclose(o); else if (!write_file(orig, raw, slen)) SE_LOG("[saveedit] could not keep %s\n", orig);
    ok = write_file(tmp, out, olen);         /* 5. temp + rename */
    if (ok) { remove(save_path); ok = rename(tmp, save_path) == 0 || write_file(save_path, out, olen); }
#ifdef __SWITCH__
    fsdevCommitDevice("sdmc");
#endif
    SE_LOG(ok ? "[saveedit] save.txt %s and re-signed\n" : "[saveedit] could not write save.txt\n",
           was_valid ? "edited" : "repaired");
  }
  free(out); save_free(&s); free(raw); free(edit);
  return ok ? 1 : -1;
}

#ifdef __SWITCH__
void sd_saveedit_run(void) {
#if SD_SAVE_EDIT
  char sp[320], ep[320];
  snprintf(sp, sizeof sp, "%s/save.txt", GAME_HOME);
  snprintf(ep, sizeof ep, "%s/save_edit.txt", GAME_HOME);
  sd_saveedit_run_paths(sp, ep);
#endif
}
#endif
