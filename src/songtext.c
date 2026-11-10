// See songtext.h. The text entries of a song archive, in the form the game's own DLC has them (the model here is
// "Dancing Queen", abbadanc_m.psarc, a 2019 DLC), with what custom songs' tools write named where it differs:
//
//   manifests/songs_dlc_<key>/<key>_<arr>.json   one arrangement's attributes; read when the song is opened
//   manifests/songs_dlc_<key>/songs_dlc_<key>.hsan  every arrangement's header attributes (a subset of the above, plus
//                                                Representative, RouteMask and a bass's BassPick); read at startup
//   gamexblocks/nsongs/<key>.xblock              one entity per arrangement, naming its manifests and assets by URN
//   <key>_aggregategraph.nt                      N-Triples: what each URN's file is (path, tags, platform)
//   songs/arr/<key>_showlights.xml               stage lights: a fog and a beam colour at each section's start
//   flatmodels/rs/rsenumerable_{root,song}.flat  the entity models, the same in every archive
//   appid.appid                                  248750
//
// Decisions:
// - JSON is written without whitespace and with keys in the DLC's (ordinal) order. The game parses either; less text
//   is less to parse when it lists songs (the rs-fastscan project serves the game its manifests this way).
// - Numbers are written by this file's own code, never printf: the mod runs inside the game, whose locale is not
//   ours to know. Times have 3 decimals at most (the game's files have milliseconds), other fractions 6.
// - Nothing random: UUIDs, link IDs, persistent IDs, MasterID_RDV and the .wem names come from SHA-256 of the names
//   they belong to. The same facts give the same bytes, so sizes kept on disk stay true.
// - The xblock, the flat models and the showlights have a UTF-8 byte order mark and CRLF where the DLC's have them;
//   the aggregate graph has neither and its statements go predicate by predicate, as the DLC's.
// - The platforms differ in the paths (audio/mac, songs/bin/macos; audio/windows, songs/bin/generic) and in the
//   graph: a sound bank is tagged "macos" or "dx9", and a song file is tagged "macos" on the Mac and not at all on
//   Windows (psarc.c turns one platform's archive into the other's by these same rules).
// - CapoFret is written only for a capo: the DLC and the Toolkit do not write it without one (DLC Builder always
//   does). Section UI names come from the table the custom song tools use ("$[34298] Riff [1]"); a name the table
//   lacks is shown as it is.
// - Tones are the game's own tone objects, passed through as JSON text; only their syntax is checked (one that is
//   not an object would break the manifest, and is left out). The manifest names tones by their "Key": Tone_Base
//   and Tone_A to Tone_D hold keys of objects in its "Tones". The .hsan has no tones. Without tones an arrangement
//   gets one of an amp and a cabinet alone, taken from the DLC's tones (abbadanc_clean: Amp_TW22 +
//   Cab_TW112C_57_Edge; abbadanc_bass: Bass_Amp_LT25B + Bass_Cab_AT1150BC_Tube_Cone), without their pedals.
// - Nothing is refused but a song without arrangements (songtext.h): names are cut down to what an entry's name may
//   hold and numbered apart, numbers no song has are clamped, list items that point nowhere fall back or are left
//   out. All of it by rules that depend on the facts alone, so the texts stay the same.
#include "songtext.h"

#include <CommonCrypto/CommonCrypto.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psarc.h"

#define MAX_LIST 4096u     // phrases, phrase iterations, sections, chord templates, ids in one list
#define MAX_LEVELS 65536u  // (difficulty, iteration) lists; showlights
#define MAX_TONE (1u << 20)   // one tone's text
#define BOM "\xef\xbb\xbf"
#define DEFAULT_CONVERTED "01-01-25 00:00"

// --- output ------------------------------------------------------------------------------------------------------
// Counts everything, keeps what fits: one generator gives both a text and its size
typedef struct {
  uint8_t *p;
  size_t cap, n;
} out;

static void put(out *o, const char *d, size_t n) {
  if (n && o->n < o->cap) memcpy(o->p + o->n, d, o->cap - o->n < n ? o->cap - o->n : n);
  o->n += n;
}
static void ch(out *o, char c) { put(o, &c, 1); }
static void str(out *o, const char *s) { put(o, s, strlen(s)); }

static void dec(out *o, int64_t v) {
  char b[24];
  size_t i = sizeof b;
  uint64_t u = v < 0 ? 0 - (uint64_t)v : (uint64_t)v;
  do b[--i] = (char)('0' + u % 10);
  while (u /= 10);
  if (v < 0) b[--i] = '-';
  put(o, b + i, sizeof b - i);
}

static void hex(out *o, const uint8_t *d, size_t n, int upper) {
  const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
  for (size_t i = 0; i < n; i++) ch(o, digits[d[i] >> 4]), ch(o, digits[d[i] & 15]);
}

// v with `places` decimals (at most 6); trim: without trailing zeros, but one decimal always ("10.0"). Not a number
// is 0; a size no song has is clamped, so the text is always a JSON number.
static void num(out *o, double v, int places, int trim) {
  static const int64_t scale[] = {1, 10, 100, 1000, 10000, 100000, 1000000};
  if (v != v) v = 0;
  if (v > 1e12) v = 1e12;
  if (v < -1e12) v = -1e12;
  int64_t q = llround(v * (double)scale[places]);
  if (q < 0) ch(o, '-'), q = -q;
  dec(o, q / scale[places]);
  char d[8];
  int64_t f = q % scale[places];
  int n = places;
  for (int i = places - 1; i >= 0; i--) d[i] = (char)('0' + f % 10), f /= 10;
  while (trim && n > 1 && d[n - 1] == '0') n--;
  if (n) ch(o, '.'), put(o, d, (size_t)n);
}

// %s (a string), %d (an int) and %u (a uint32_t) only
static void fmt(out *o, const char *f, ...) {
  va_list ap;
  va_start(ap, f);
  for (; *f; f++) {
    if (*f != '%') ch(o, *f);
    else if (*++f == 's') str(o, va_arg(ap, const char *));
    else if (*f == 'd') dec(o, va_arg(ap, int));
    else dec(o, va_arg(ap, uint32_t));
  }
  va_end(ap);
}

static int fail(char *err, size_t errlen, const char *f, ...) {
  if (err && errlen) {
    va_list ap;
    va_start(ap, f);
    vsnprintf(err, errlen, f, ap);
    va_end(ap);
  }
  return 0;
}

// ASCII only, whatever the locale
static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static int is_alnum(char c) { return is_digit(c) || is_alpha(c); }
static int is_hex(char c) { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }
static char upper(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 32) : c; }

// --- JSON --------------------------------------------------------------------------------------------------------
// The length of the well-formed UTF-8 sequence at p (not ASCII), 0 when there is none
static size_t utf8_len(const unsigned char *p) {
#define CONT(i) ((p[i] & 0xc0) == 0x80)
  if (p[0] >= 0xc2 && p[0] <= 0xdf) return CONT(1) ? 2 : 0;
  if (p[0] == 0xe0) return p[1] >= 0xa0 && p[1] <= 0xbf && CONT(2) ? 3 : 0;
  if (p[0] == 0xed) return p[1] >= 0x80 && p[1] <= 0x9f && CONT(2) ? 3 : 0;  // no surrogates
  if (p[0] >= 0xe1 && p[0] <= 0xef) return CONT(1) && CONT(2) ? 3 : 0;
  if (p[0] == 0xf0) return p[1] >= 0x90 && p[1] <= 0xbf && CONT(2) && CONT(3) ? 4 : 0;
  if (p[0] >= 0xf1 && p[0] <= 0xf3) return CONT(1) && CONT(2) && CONT(3) ? 4 : 0;
  if (p[0] == 0xf4) return p[1] >= 0x80 && p[1] <= 0x8f && CONT(2) && CONT(3) ? 4 : 0;
  return 0;
#undef CONT
}

// s as a JSON string. UTF-8 goes in as it is; a byte that is not UTF-8 becomes U+FFFD, so the text is always valid.
static void jstr(out *o, const char *s) {
  ch(o, '"');
  for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p;) {
    unsigned c = *p;
    size_t k;
    if (c == '"' || c == '\\') ch(o, '\\'), ch(o, (char)c), p++;
    else if (c == '\n') str(o, "\\n"), p++;
    else if (c == '\r') str(o, "\\r"), p++;
    else if (c == '\t') str(o, "\\t"), p++;
    else if (c < 0x20 || c == 0x7f) {
      uint8_t b = (uint8_t)c;
      str(o, "\\u00"), hex(o, &b, 1, 0), p++;
    } else if (c < 0x80) ch(o, (char)c), p++;
    else if ((k = utf8_len(p))) put(o, (const char *)p, k), p += k;
    else str(o, "\xef\xbf\xbd"), p++;
  }
  ch(o, '"');
}

// A JSON text being written: the bit of each open object or array says whether it has a member yet
typedef struct {
  out *o;
  uint64_t has;
} js;

static void el(js *j) {  // before an array's element
  if (j->has & 1) ch(j->o, ',');
  j->has |= 1;
}
static void key(js *j, const char *k) {  // before an object's value
  el(j);
  ch(j->o, '"'), str(j->o, k), str(j->o, "\":");
}
static void jopen(js *j, char c) { ch(j->o, c), j->has <<= 1; }
static void jclose(js *j, char c) { ch(j->o, c), j->has >>= 1; }
static void kstr(js *j, const char *k, const char *v) { key(j, k), jstr(j->o, v); }
static void kint(js *j, const char *k, int64_t v) { key(j, k), dec(j->o, v); }
static void ktime(js *j, const char *k, double v) { key(j, k), num(j->o, v, 3, 1); }
static void knum(js *j, const char *k, double v) { key(j, k), num(j->o, v, 6, 1); }
static void kbool(js *j, const char *k, int v) { key(j, k), str(j->o, v ? "true" : "false"); }
// A string value made of parts: the key and the lower-case names have nothing to escape
static void kfmt(js *j, const char *k, const char *f, const char *a, const char *b) {
  key(j, k), ch(j->o, '"'), fmt(j->o, f, a, b), ch(j->o, '"');
}

// JSON text someone else wrote (a tone list): only its syntax is checked
static const char *jws(const char *p) {
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
  return p;
}

// p at a string's opening quote: after its closing one, or 0
static const char *jskip_str(const char *p) {
  for (p++; *p != '"'; p++) {
    if ((unsigned char)*p < 0x20) return 0;  // the text's end too
    if (*p != '\\') continue;
    p++;
    if (*p == 'u') {
      for (int i = 0; i < 4; i++)
        if (!is_hex(*++p)) return 0;
    } else if (!*p || !strchr("\"\\/bfnrt", *p)) return 0;
  }
  return p + 1;
}

// After the value at p, or 0 when it is not one
static const char *jskip(const char *p, int depth) {
  p = jws(p);
  if (*p == '"') return jskip_str(p);
  if (*p == '{' || *p == '[') {
    int obj = *p == '{';
    char end = obj ? '}' : ']';
    if (depth >= 32) return 0;
    p = jws(p + 1);
    if (*p == end) return p + 1;
    for (;;) {
      if (obj) {
        p = jws(p);
        if (*p != '"' || !(p = jskip_str(p))) return 0;
        p = jws(p);
        if (*p++ != ':') return 0;
      }
      if (!(p = jskip(p, depth + 1))) return 0;
      p = jws(p);
      if (*p == end) return p + 1;
      if (*p++ != ',') return 0;
    }
  }
  if (!strncmp(p, "true", 4) || !strncmp(p, "null", 4)) return p + 4;
  if (!strncmp(p, "false", 5)) return p + 5;
  if (*p == '-') p++;
  if (!is_digit(*p)) return 0;
  while (is_digit(*p)) p++;
  if (*p == '.') {
    if (!is_digit(*++p)) return 0;
    while (is_digit(*p)) p++;
  }
  if (*p == 'e' || *p == 'E') {
    p++;
    if (*p == '+' || *p == '-') p++;
    if (!is_digit(*p)) return 0;
    while (is_digit(*p)) p++;
  }
  return p;
}

// A tone: where the object is in its text, and its "Key" (still escaped; none: klen 0). 0 when it is no object.
typedef struct {
  const char *p, *key;
  size_t n, klen;
} tone;

static int tone_parse(const char *text, tone *t) {
  memset(t, 0, sizeof *t);
  if (!text) return 0;
  const char *p = jws(text), *e;
  if (*p != '{' || !(e = jskip(p, 0)) || *jws(e) || (size_t)(e - p) > MAX_TONE) return 0;
  t->p = p, t->n = (size_t)(e - p);
  for (const char *q = jws(p + 1); *q == '"';) {
    const char *ke = jskip_str(q), *v = jws(jws(ke) + 1), *ve = jskip(v, 1);
    if (ke - q == 5 && !memcmp(q, "\"Key\"", 5) && *v == '"') {
      t->key = v + 1, t->klen = (size_t)(ve - v) - 2;
      break;
    }
    q = jws(ve);
    if (*q != ',') break;
    q = jws(q + 1);
  }
  return 1;
}

// --- hashes ------------------------------------------------------------------------------------------------------
// SHA-256 of tag, a and b (b may be 0), each with its 0: what one name is derived from never looks like another's
static void sha(uint8_t h[32], const char *tag, const char *a, const char *b) {
  CC_SHA256_CTX c;
  CC_SHA256_Init(&c);
  CC_SHA256_Update(&c, tag, (CC_LONG)strlen(tag) + 1);
  CC_SHA256_Update(&c, a, (CC_LONG)strlen(a) + 1);
  if (b) CC_SHA256_Update(&c, b, (CC_LONG)strlen(b) + 1);
  CC_SHA256_Final(h, &c);
}

static uint32_t h32(const uint8_t *h) { return (uint32_t)h[0] << 24 | (uint32_t)h[1] << 16 | (uint32_t)h[2] << 8 | h[3]; }

// s in lower case, cut to fit (the names hashed here are short; a cut one still hashes the same every time)
static void lower_copy(char *d, size_t cap, const char *s) {
  size_t i = 0;
  for (; s && s[i] && i + 1 < cap; i++) d[i] = lower(s[i]);
  d[i] = 0;
}

// --- names -------------------------------------------------------------------------------------------------------
int sb_song_key_ok(const char *key) {
  size_t n = 0;
  if (!key) return 0;
  for (; key[n]; n++)
    if (n >= SB_KEY_MAX || !is_alnum(key[n])) return 0;
  return n > 0;
}

void sb_song_key(const char *artist, const char *title, char key[SB_KEY_MAX + 1]) {
  enum { TEXT = 22 };
  const char *src[2] = {artist ? artist : "", title ? title : ""};
  char t[TEXT + 2];
  size_t n = 0;
  // the letters and digits, each word starting with a capital: "Dancing Queen" by "ABBA" is ABBADancingQueen
  for (int k = 0; k < 2; k++) {
    int start = 1;
    for (const char *p = src[k]; *p; p++) {
      if (!is_alnum(*p)) {
        start = 1;
        continue;
      }
      if (n < TEXT) t[n++] = start ? upper(*p) : *p;
      start = 0;
    }
  }
  t[n] = 0;
  uint8_t h[32];
  sha(h, "sb-key", src[0], src[1]);
  out o = {(uint8_t *)key, SB_KEY_MAX, 0};
  if (!n || is_digit(t[0])) ch(&o, 'S'), t[TEXT - 1] = 0;  // a key starts with a letter, as the game's own do
  str(&o, t);
  hex(&o, h, 4, 1);
  key[o.n] = 0;
}

void sb_song_pid(const char *key, const char *arr_name, char pid[33]) {
  char k[SB_KEY_MAX + 1], a[64];
  uint8_t h[32];
  lower_copy(k, sizeof k, key);
  lower_copy(a, sizeof a, arr_name);
  sha(h, "sb-pid", k, a);
  out o = {(uint8_t *)pid, 32, 0};
  hex(&o, h, 16, 1);
  pid[32] = 0;
}

void sb_song_audio_ids(const char *key, uint32_t *song, uint32_t *preview) {
  char k[SB_KEY_MAX + 1];
  uint8_t h[32];
  lower_copy(k, sizeof k, key);
  sha(h, "sb-audio", k, 0);
  uint32_t a = h32(h) >> 1, b = h32(h + 4) >> 1;  // 31 bits, as the names of the game's own
  if (!a) a = 1;
  if (!b || b == a) b = a + 1;
  if (song) *song = a;
  if (preview) *preview = b;
}

int sb_song_event(const char *key, int preview, char *buf, size_t len) {
  out o = {(uint8_t *)buf, len, 0};
  fmt(&o, preview ? "Play_%s_Preview" : "Play_%s", key ? key : "");
  if (o.n >= len) return -1;
  buf[o.n] = 0;
  return (int)o.n;
}

uint32_t sb_wwise_id(const char *name) {
  uint32_t h = 2166136261u;
  for (; name && *name; name++) h = (h * 16777619u) ^ (uint8_t)lower(*name);
  return h;
}

// --- the song, made sense of -------------------------------------------------------------------------------------
// What the texts call things: found once per text
typedef struct {
  char key[SB_KEY_MAX + 1], lkey[SB_KEY_MAX + 1];  // the key; in lower case, as paths and URNs have it
  uint32_t n;                                      // arrangements used
  double length;
  char (*name)[SB_NAME_MAX + 1];
  char (*pid)[33];
} names;

static void names_free(names *r) { free(r->name), free(r->pid); }

static int arr_type(const sb_arr_info *a) { return a->type < SB_ARR_LEAD || a->type > SB_ARR_VOCALS ? SB_ARR_LEAD : a->type; }

static int arr_path(const sb_arr_info *a) {
  int type = arr_type(a);
  if (a->path == SB_PATH_LEAD || a->path == SB_PATH_RHYTHM || a->path == SB_PATH_BASS) return a->path;
  return type == SB_ARR_LEAD ? SB_PATH_LEAD : type == SB_ARR_BASS ? SB_PATH_BASS : SB_PATH_RHYTHM;
}

// The one arrangement that stands for its path in the song list: the first on it that is neither bonus nor alternate
static int represents(const sb_song_info *s, uint32_t i) {
  const sb_arr_info *a = &s->arr[i];
  if (a->bonus || a->alternate) return 0;
  for (uint32_t k = 0; k < i; k++) {
    const sb_arr_info *b = &s->arr[k];
    if (arr_type(b) != SB_ARR_VOCALS && !b->bonus && !b->alternate && arr_path(b) == arr_path(a)) return 0;
  }
  return 1;
}

static int name_taken(const names *r, uint32_t i, const char *name) {
  if (!strcmp(name, "showlights")) return 1;  // <key>_showlights is the lights' own
  for (uint32_t k = 0; k < i; k++)
    if (!strcmp(r->name[k], name)) return 1;
  return 0;
}

static int resolve(const sb_song_info *s, names *r, char *err, size_t errlen) {
  static const char *const by_type[] = {"lead", "rhythm", "combo", "bass", "vocals"};
  enum { BASE = SB_NAME_MAX - 5 };  // room for a number
  memset(r, 0, sizeof *r);
  if (!s) return fail(err, errlen, "no song");
  if (!s->narr || !s->arr) return fail(err, errlen, "a song needs an arrangement");
  r->n = s->narr > SB_MAX_ARR ? SB_MAX_ARR : s->narr;
  r->length = s->length > 0 ? s->length < 1e6 ? s->length : 1e6 : 0;  // not a number is not more than 0
  r->name = malloc(r->n * sizeof *r->name), r->pid = malloc(r->n * sizeof *r->pid);
  if (!r->name || !r->pid) {
    names_free(r);
    return fail(err, errlen, "out of memory");
  }
  size_t kn = 0;
  for (const char *p = s->key; p && *p && kn < SB_KEY_MAX; p++)
    if (is_alnum(*p)) r->key[kn++] = *p;
  r->key[kn] = 0;
  if (!kn) sb_song_key(s->artist, s->title, r->key);
  lower_copy(r->lkey, sizeof r->lkey, r->key);
  for (uint32_t i = 0; i < r->n; i++) {
    const sb_arr_info *a = &s->arr[i];
    char base[BASE + 1];
    size_t n = 0;
    for (const char *p = a->name; p && *p && n < BASE; p++)
      if (is_alnum(*p)) base[n++] = lower(*p);
    base[n] = 0;
    if (!n) strcpy(base, by_type[arr_type(a)]);
    strcpy(r->name[i], base);
    for (uint32_t nth = 2; name_taken(r, i, r->name[i]); nth++) {  // at most i + 1 numbers are taken
      out o = {(uint8_t *)r->name[i], SB_NAME_MAX, 0};
      str(&o, base), dec(&o, nth);
      r->name[i][o.n] = 0;
    }
    size_t h = 0;
    while (a->persistent_id && h < 33 && is_hex(a->persistent_id[h])) h++;
    int given = h == 32 && !a->persistent_id[32];
    for (h = 0; given && h < 32; h++) r->pid[i][h] = upper(a->persistent_id[h]);
    r->pid[i][32] = 0;
    for (uint32_t k = 0; given && k < i; k++) given = strcmp(r->pid[k], r->pid[i]) != 0;
    if (!given) sb_song_pid(r->lkey, r->name[i], r->pid[i]);
  }
  return 1;
}

int sb_song_check(const sb_song_info *s, char *err, size_t errlen) {
  names r;
  if (!resolve(s, &r, err, errlen)) return 0;
  names_free(&r);
  return 1;
}

int sb_song_key_of(const sb_song_info *s, char key[SB_KEY_MAX + 1]) {
  names r;
  if (!resolve(s, &r, 0, 0)) return 0;
  memcpy(key, r.key, sizeof r.key);
  names_free(&r);
  return 1;
}

int sb_song_arr_name(const sb_song_info *s, uint32_t i, char name[SB_NAME_MAX + 1]) {
  names r;
  if (!resolve(s, &r, 0, 0)) return 0;
  int ok = i < r.n;
  if (ok) memcpy(name, r.name[i], sizeof r.name[i]);
  names_free(&r);
  return ok;
}

int sb_song_arr_pid(const sb_song_info *s, uint32_t i, char pid[33]) {
  names r;
  if (!resolve(s, &r, 0, 0)) return 0;
  int ok = i < r.n;
  if (ok) memcpy(pid, r.pid[i], sizeof r.pid[i]);
  names_free(&r);
  return ok;
}

// --- entries -----------------------------------------------------------------------------------------------------
static void path_out(out *o, const names *r, const sb_song_info *s, int platform, int kind, uint32_t arr) {
  const char *audio = platform == SB_MAC ? "audio/mac" : "audio/windows", *bin = platform == SB_MAC ? "macos" : "generic";
  const char *k = r->lkey;
  uint32_t song = s->audio_id, preview = s->preview_id, d_song, d_preview;
  sb_song_audio_ids(k, &d_song, &d_preview);
  if (!song) song = d_song;
  if (!preview) preview = d_preview;
  switch (kind) {
  case SB_E_APPID: str(o, "appid.appid"); break;
  case SB_E_FLAT_ROOT: str(o, "flatmodels/rs/rsenumerable_root.flat"); break;
  case SB_E_FLAT_SONG: str(o, "flatmodels/rs/rsenumerable_song.flat"); break;
  case SB_E_GRAPH: fmt(o, "%s_aggregategraph.nt", k); break;
  case SB_E_XBLOCK: fmt(o, "gamexblocks/nsongs/%s.xblock", k); break;
  case SB_E_HSAN: fmt(o, "manifests/songs_dlc_%s/songs_dlc_%s.hsan", k, k); break;
  case SB_E_SHOWLIGHTS: fmt(o, "songs/arr/%s_showlights.xml", k); break;
  case SB_E_MANIFEST: fmt(o, "manifests/songs_dlc_%s/%s_%s.json", k, k, r->name[arr]); break;
  case SB_E_SNG: fmt(o, "songs/bin/%s/%s_%s.sng", bin, k, r->name[arr]); break;
  case SB_E_ART64: fmt(o, "gfxassets/album_art/album_%s_64.dds", k); break;
  case SB_E_ART128: fmt(o, "gfxassets/album_art/album_%s_128.dds", k); break;
  case SB_E_ART256: fmt(o, "gfxassets/album_art/album_%s_256.dds", k); break;
  case SB_E_BNK: fmt(o, "%s/song_%s.bnk", audio, k); break;
  case SB_E_BNK_PREVIEW: fmt(o, "%s/song_%s_preview.bnk", audio, k); break;
  case SB_E_WEM: fmt(o, "%s/%u.wem", audio, song); break;
  default: fmt(o, "%s/%u.wem", audio, preview); break;
  }
}

static int per_arr(int kind) { return kind == SB_E_MANIFEST || kind == SB_E_SNG; }

int sb_song_entry_path(const sb_song_info *s, int platform, int kind, uint32_t arr, char *buf, size_t len) {
  names r;
  if ((platform != SB_MAC && platform != SB_PC) || kind < 0 || kind >= SB_E_KINDS || !resolve(s, &r, 0, 0)) return -1;
  out o = {(uint8_t *)buf, len, 0};
  int ok = per_arr(kind) ? arr < r.n : arr == 0;
  if (ok) path_out(&o, &r, s, platform, kind, arr);
  names_free(&r);
  if (!ok || o.n >= len) return -1;
  buf[o.n] = 0;
  return (int)o.n;
}

uint32_t sb_song_entries(const sb_song_info *s, sb_entry *e, uint32_t cap) {
  // the small ones first, the audio last; the AppID at the end, where the tools put it
  static const int after[] = {SB_E_ART64,  SB_E_ART128,      SB_E_ART256,      SB_E_XBLOCK, SB_E_FLAT_SONG, SB_E_FLAT_ROOT,
                              SB_E_GRAPH,  SB_E_BNK_PREVIEW, SB_E_WEM_PREVIEW, SB_E_BNK,    SB_E_WEM,       SB_E_APPID};
  uint32_t n = 0, narr = s && s->arr ? s->narr > SB_MAX_ARR ? SB_MAX_ARR : s->narr : 0;
  if (!narr) return 0;
#define ADD(k, a)                             \
  do {                                        \
    if (n < cap) e[n] = (sb_entry){(k), (a)}; \
    n++;                                      \
  } while (0)
  for (uint32_t i = 0; i < narr; i++) ADD(SB_E_SNG, i);
  ADD(SB_E_SHOWLIGHTS, 0);
  ADD(SB_E_HSAN, 0);
  for (uint32_t i = 0; i < narr; i++) ADD(SB_E_MANIFEST, i);
  for (size_t i = 0; i < sizeof after / sizeof *after; i++) ADD(after[i], 0);
#undef ADD
  return n;
}

// --- the manifests -----------------------------------------------------------------------------------------------
// ArrangementName: what the game shows. A combo arrangement goes by its path.
static const char *arr_title(const sb_arr_info *a) {
  switch (arr_type(a)) {
  case SB_ARR_LEAD: return "Lead";
  case SB_ARR_RHYTHM: return "Rhythm";
  case SB_ARR_BASS: return "Bass";
  case SB_ARR_VOCALS: return "Vocals";
  default: return arr_path(a) == SB_PATH_LEAD ? "Lead" : "Rhythm";
  }
}

static const char *or_empty(const char *s) { return s ? s : ""; }
static const char *or_unknown(const char *s, const char *unknown) { return s && *s ? s : unknown; }
// A list's length as it is used: nothing of a list that is not there, and no more than the texts are made for
static uint32_t count(const void *list, uint32_t n, uint32_t max) { return !list ? 0 : n > max ? max : n; }

// The name a list is sorted by: without its article
static const char *sort_name(const char *given, const char *name) {
  static const char *const article[] = {"the ", "a ", "an "};
  if (given) return given;
  for (size_t i = 0; i < 3; i++) {
    size_t n = 0;
    while (article[i][n] && lower(name[n]) == article[i][n]) n++;
    if (!article[i][n] && name[n]) return name + n;
  }
  return name;
}

// What a section is called on screen: the game's text number for the name, as the custom song tools have them.
// A name is looked up by its letters alone ("Verse 2" is verse).
static void section_ui(out *o, const char *name) {
  static const struct {
    const char *name;
    int id;
    const char *text;
  } ui[] = {{"fadein", 34276, "Fade In"},       {"fadeout", 34277, "Fade Out"},     {"buildup", 34278, "Buildup"},
            {"chorus", 34279, "Chorus"},        {"hook", 34280, "Hook"},            {"head", 34281, "Head"},
            {"bridge", 34282, "Bridge"},        {"ambient", 34283, "Ambient"},      {"breakdown", 34284, "Breakdown"},
            {"interlude", 34285, "Interlude"},  {"intro", 34286, "Intro"},          {"melody", 34287, "Melody"},
            {"modbridge", 34288, "Modulated Bridge"}, {"modchorus", 34289, "Modulated Chorus"}, {"modverse", 34290, "Modulated Verse"},
            {"outro", 34291, "Outro"},          {"postbrdg", 34292, "Post Bridge"}, {"postchorus", 34293, "Post Chorus"},
            {"postvs", 34294, "Post Verse"},    {"prebrdg", 34295, "Pre Bridge"},   {"prechorus", 34296, "Pre Chorus"},
            {"preverse", 34297, "Pre Verse"},   {"riff", 34298, "Riff"},            {"silence", 34299, "Silence"},
            {"solo", 34300, "Solo"},            {"transition", 34301, "Transition"}, {"vamp", 34302, "Vamp"},
            {"variation", 34303, "Variation"},  {"verse", 34304, "Verse"},          {"tapping", 34305, "Tapping"},
            {"noguitar", 6091, "No Guitar"}};
  char l[16];
  size_t n = 0;
  for (const char *p = name; *p && n + 1 < sizeof l; p++)
    if (is_alpha(*p)) l[n++] = lower(*p);
  l[n] = 0;
  for (size_t i = 0; i < sizeof ui / sizeof *ui; i++)
    if (!strcmp(l, ui[i].name)) {
      ch(o, '"'), fmt(o, "$[%d] %s [1]", ui[i].id, ui[i].text), ch(o, '"');
      return;
    }
  jstr(o, name);
}

// Chords or Techniques: {"<difficulty>": {"<phrase iteration>": [ids]}}. An item that is not after the last one
// written would repeat a key: it is left out.
static void levels(js *j, const char *k, const sb_level_ids *l, uint32_t n) {
  const sb_level_ids *last = 0;
  n = count(l, n, MAX_LEVELS);
  key(j, k), jopen(j, '{');
  for (uint32_t i = 0; i < n; i++) {
    if (last && (l[i].difficulty < last->difficulty || (l[i].difficulty == last->difficulty && l[i].iteration <= last->iteration)))
      continue;
    if (!last || l[i].difficulty != last->difficulty) {
      if (last) jclose(j, '}');
      el(j), ch(j->o, '"'), dec(j->o, l[i].difficulty), str(j->o, "\":"), jopen(j, '{');
    }
    el(j), ch(j->o, '"'), dec(j->o, l[i].iteration), str(j->o, "\":"), jopen(j, '[');
    for (uint32_t m = 0, ids = count(l[i].id, l[i].n, MAX_LIST); m < ids; m++) el(j), dec(j->o, l[i].id[m]);
    jclose(j, ']');
    last = &l[i];
  }
  if (last) jclose(j, '}');
  jclose(j, '}');
}

static void props(js *j, const sb_song_info *s, uint32_t i) {
  const sb_arr_info *a = &s->arr[i];
  const sb_arr_props *p = &a->props;
  int path = arr_path(a), standard = 1;
  for (int n = 0; n < 6; n++) standard &= !a->tuning[n];
  key(j, "ArrangementProperties"), jopen(j, '{');
  kint(j, "barreChords", !!p->barre_chords), kint(j, "bassPick", !!p->bass_pick), kint(j, "bends", !!p->bends);
  kint(j, "bonusArr", !!a->bonus), kint(j, "doubleStops", !!p->double_stops), kint(j, "dropDPower", !!p->drop_d_power);
  kint(j, "fifthsAndOctaves", !!p->fifths_and_octaves), kint(j, "fingerPicking", !!p->finger_picking);
  kint(j, "fretHandMutes", !!p->fret_hand_mutes), kint(j, "harmonics", !!p->harmonics), kint(j, "hopo", !!p->hopo);
  kint(j, "nonStandardChords", !!p->non_standard_chords), kint(j, "openChords", !!p->open_chords);
  kint(j, "palmMutes", !!p->palm_mutes), kint(j, "pathBass", path == SB_PATH_BASS), kint(j, "pathLead", path == SB_PATH_LEAD);
  kint(j, "pathRhythm", path == SB_PATH_RHYTHM), kint(j, "pickDirection", !!p->pick_direction);
  kint(j, "pinchHarmonics", !!p->pinch_harmonics), kint(j, "powerChords", !!p->power_chords);
  kint(j, "represent", represents(s, i)), kint(j, "routeMask", path), kint(j, "slapPop", !!p->slap_pop);
  kint(j, "slides", !!p->slides), kint(j, "standardTuning", standard), kint(j, "sustain", !!p->sustain);
  kint(j, "syncopation", !!p->syncopation), kint(j, "tapping", !!p->tapping), kint(j, "tremolo", !!p->tremolo);
  kint(j, "twoFingerPicking", !!p->two_finger_picking), kint(j, "unpitchedSlides", !!p->unpitched_slides);
  kint(j, "vibrato", !!p->vibrato);
  jclose(j, '}');
}

static void chord_templates(js *j, const sb_arr_info *a) {
  key(j, "ChordTemplates"), jopen(j, '[');
  for (uint32_t i = 0, n = count(a->chord, a->nchords, MAX_LIST); i < n; i++) {
    const sb_chord_template *c = &a->chord[i];
    el(j), jopen(j, '{');
    kint(j, "ChordId", c->id), kstr(j, "ChordName", c->name);
    key(j, "Fingers"), jopen(j, '[');
    for (int k = 0; k < 6; k++) el(j), dec(j->o, c->fingers[k]);
    jclose(j, ']');
    key(j, "Frets"), jopen(j, '[');
    for (int k = 0; k < 6; k++) el(j), dec(j->o, c->frets[k]);
    jclose(j, ']');
    jclose(j, '}');
  }
  jclose(j, ']');
}

// The phrases, iterations and sections of an arrangement as they are used
typedef struct {
  uint32_t nphrases, niters, nsections;
} lists;

static lists lists_of(const sb_arr_info *a) {
  return (lists){count(a->phrase, a->nphrases, MAX_LIST), count(a->iter, a->niters, MAX_LIST),
                 count(a->section, a->nsections, MAX_LIST)};
}

// An iteration's phrase: the first when it names one that is not there
static uint32_t iter_phrase(const sb_arr_info *a, const lists *l, uint32_t i) {
  return a->iter[i].phrase < l->nphrases ? a->iter[i].phrase : 0;
}

static void phrases(js *j, const names *r, const sb_arr_info *a) {
  static const sb_man_phrase none = {0, 0};
  lists l = lists_of(a);
  key(j, "PhraseIterations"), jopen(j, '[');
  for (uint32_t i = 0; i < l.niters; i++) {
    uint32_t pi = iter_phrase(a, &l, i);
    const sb_man_phrase *p = l.nphrases ? &a->phrase[pi] : &none;
    el(j), jopen(j, '{');
    // it ends where the next starts, the last with the song
    ktime(j, "EndTime", i + 1 < l.niters ? a->iter[i + 1].time : r->length), kint(j, "MaxDifficulty", p->max_difficulty);
    kstr(j, "Name", p->name), kint(j, "PhraseIndex", pi), ktime(j, "StartTime", a->iter[i].time);
    jclose(j, '}');
  }
  jclose(j, ']');
  key(j, "Phrases"), jopen(j, '[');
  for (uint32_t i = 0; i < l.nphrases; i++) {
    uint32_t n = 0;
    for (uint32_t k = 0; k < l.niters; k++) n += iter_phrase(a, &l, k) == i;
    el(j), jopen(j, '{');
    kint(j, "IterationCount", n), kint(j, "MaxDifficulty", a->phrase[i].max_difficulty), kstr(j, "Name", a->phrase[i].name);
    jclose(j, '}');
  }
  jclose(j, ']');
}

// A section spans the phrase iterations from the one it starts in to the last that starts before it ends; its
// number counts the sections of its name. Times are compared as they are written, in milliseconds.
static void sections(js *j, const names *r, const sb_arr_info *a) {
  const double half = 0.0005;
  lists l = lists_of(a);
  key(j, "Sections"), jopen(j, '[');
  for (uint32_t i = 0; i < l.nsections; i++) {
    const char *name = or_empty(a->section[i].name);
    double start = a->section[i].time, end = i + 1 < l.nsections ? a->section[i + 1].time : r->length;
    uint32_t number = 1, first = 0, last = 0;
    for (uint32_t k = 0; k < i; k++) number += !strcmp(or_empty(a->section[k].name), name);
    for (uint32_t k = 0; k < l.niters; k++) {
      if (a->iter[k].time < start + half) first = k;
      if (a->iter[k].time < end - half) last = k;
    }
    el(j), jopen(j, '{');
    kint(j, "EndPhraseIterationIndex", last), ktime(j, "EndTime", end), kbool(j, "IsSolo", !strcmp(name, "solo"));
    kstr(j, "Name", name), kint(j, "Number", number), kint(j, "StartPhraseIterationIndex", first);
    ktime(j, "StartTime", start), key(j, "UIName"), section_ui(j->o, name);
    jclose(j, '}');
  }
  jclose(j, ']');
}

// The tone of an arrangement that brings none: the amp and the cabinet of the DLC's own clean tone (bass tone)
#define TONE_GUITAR "SbClean"
#define TONE_BASS "SbBass"
static const char tone_guitar[] =
  "{\"GearList\":{\"Amp\":{\"Key\":\"Amp_TW22\",\"KnobValues\":{\"Amp_TW22_Bass\":45.0,\"Amp_TW22_Gain\":14.0,"
  "\"Amp_TW22_Mid\":36.0,\"Amp_TW22_Treble\":88.0},\"Type\":\"Amps\"},\"Cabinet\":{\"Key\":\"Cab_TW112C_57_Edge\","
  "\"KnobValues\":{},\"Type\":\"Cabinets\"}},\"IsCustom\":true,\"Key\":\"" TONE_GUITAR "\",\"Name\":\"" TONE_GUITAR "\","
  "\"NameSeparator\":\" - \",\"SortOrder\":0.0,\"ToneDescriptors\":[\"$[35720]CLEAN\"],\"Volume\":\"-20.000\"}";
static const char tone_bass[] =
  "{\"GearList\":{\"Amp\":{\"Key\":\"Bass_Amp_LT25B\",\"KnobValues\":{\"Bass_Amp_LT25B_Bass\":49.0,\"Bass_Amp_LT25B_Gain\":6.0,"
  "\"Bass_Amp_LT25B_HiShift\":1.0,\"Bass_Amp_LT25B_LoShift\":1.0,\"Bass_Amp_LT25B_Mid\":84.0,\"Bass_Amp_LT25B_MidFreq\":0.0,"
  "\"Bass_Amp_LT25B_Treble\":55.0},\"Type\":\"Amps\"},\"Cabinet\":{\"Key\":\"Bass_Cab_AT1150BC_Tube_Cone\",\"KnobValues\":{},"
  "\"Type\":\"Cabinets\"}},\"IsCustom\":true,\"Key\":\"" TONE_BASS "\",\"Name\":\"" TONE_BASS "\",\"NameSeparator\":\" - \","
  "\"SortOrder\":0.0,\"ToneDescriptors\":[\"$[35715]BASS\"],\"Volume\":\"-12.000\"}";

// Tone_A to Tone_Multiplayer, and the tones themselves. The base tone, when the arrangement names none, is the
// first tone written.
static void tones(js *j, const sb_arr_info *a) {
  int bass = arr_type(a) == SB_ARR_BASS;
  uint32_t n = count(a->tone, a->ntones, MAX_LIST), first = n;
  tone t, t0 = {0};
  for (uint32_t i = 0; i < n && first == n; i++)
    if (tone_parse(a->tone[i], &t0)) first = i;
  kstr(j, "Tone_A", a->tone_a), kstr(j, "Tone_B", a->tone_b);
  key(j, "Tone_Base");
  if (a->tone_base) jstr(j->o, a->tone_base);
  else if (first == n) jstr(j->o, bass ? TONE_BASS : TONE_GUITAR);
  else ch(j->o, '"'), put(j->o, t0.key, t0.klen), ch(j->o, '"');  // as its own text has it
  kstr(j, "Tone_C", a->tone_c), kstr(j, "Tone_D", a->tone_d), kstr(j, "Tone_Multiplayer", a->tone_multiplayer);
  key(j, "Tones"), jopen(j, '[');
  for (uint32_t i = first; i < n; i++)
    if (tone_parse(a->tone[i], &t)) el(j), put(j->o, t.p, t.n);
  if (first == n) el(j), str(j->o, bass ? tone_bass : tone_guitar);
  jclose(j, ']');
}

// The density of the notes on screen by difficulty level: the DLC's (a scroll speed of 1.3), and the vocals'
static const float density_dlc[20] = {5.0f, 4.7f, 4.4f, 4.1f, 3.8f, 3.5f, 3.3f, 3.1f, 2.8f, 2.6f,
                                      2.4f, 2.1f, 1.9f, 1.7f, 1.6f, 1.5f, 1.4f, 1.3f, 1.3f, 1.3f};

// MasterID_RDV: any number of the arrangement's own, positive
static int32_t master_id(const sb_arr_info *a, const char *pid) {
  uint8_t h[32];
  if (a->master_id) return a->master_id;
  sha(h, "sb-master", pid, 0);
  uint32_t v = h32(h) >> 1;
  return (int32_t)(v ? v : 1);
}

// Arrangement i's attributes: all of them (the .json), or the header's (the .hsan). In the DLC's key order.
static void attributes(js *j, const sb_song_info *s, const names *r, uint32_t i, int header) {
  const sb_arr_info *a = &s->arr[i];
  const char *k = r->lkey, *name = r->name[i];
  int type = arr_type(a), vocals = type == SB_ARR_VOCALS, inst = !vocals, full = !header;
  const char *title = or_unknown(s->title, "Unknown Title"), *artist = or_unknown(s->artist, "Unknown Artist");
  const char *album = or_unknown(s->album, "Unknown Album");
  double hard = a->notes_hard;

  jopen(j, '{');
  kfmt(j, "AlbumArt", "urn:image:dds:album_%s", k, 0);
  if (inst) kstr(j, "AlbumName", album), kstr(j, "AlbumNameSort", sort_name(s->album_sort, album));
  kstr(j, "ArrangementName", arr_title(a));
  if (inst && full) props(j, s, i);
  if (full) kint(j, "ArrangementSort", 0);
  if (inst && full) kint(j, "ArrangementType", type);
  if (inst) kstr(j, "ArtistName", artist), kstr(j, "ArtistNameSort", sort_name(s->artist_sort, artist));
  if (header && type == SB_ARR_BASS) kint(j, "BassPick", !!a->props.bass_pick);
  if (full) kfmt(j, "BlockAsset", "urn:emergent-world:%s", k, 0);
  if (inst && a->capo > 0) knum(j, "CapoFret", a->capo);
  if (inst) knum(j, "CentOffset", a->cents);
  if (inst && full) chord_templates(j, a), levels(j, "Chords", a->chords, a->nchord_levels);
  kbool(j, "DLC", 1), kstr(j, "DLCKey", r->key);
  if (inst) ktime(j, "DNA_Chords", a->dna_chords), ktime(j, "DNA_Riffs", a->dna_riffs), ktime(j, "DNA_Solo", a->dna_solo);
  if (full) {
    key(j, "DynamicVisualDensity"), jopen(j, '[');
    for (int d = 0; d < 20; d++) el(j), num(j->o, vocals ? 2.0 : a->density ? a->density[d] : density_dlc[d], 3, 1);
    jclose(j, ']');
  }
  if (inst) knum(j, "EasyMastery", hard > 0 ? a->notes_easy / hard : 0);
  if (full) {
    char cap[SB_NAME_MAX + 1];
    memcpy(cap, name, sizeof cap);
    cap[0] = upper(cap[0]);
    kfmt(j, "FullName", "%s_%s", r->key, cap);
  }
  if (vocals && full) kstr(j, "InputEvent", "Play_Tone_Standard_Mic");
  if (inst && full) kstr(j, "LastConversionDateTime", s->converted ? s->converted : DEFAULT_CONVERTED);
  kint(j, "LeaderboardChallengeRating", 0);
  kfmt(j, "ManifestUrn", "urn:database:json-db:%s_%s", k, name);
  if (full) kint(j, "MasterID_PS3", -1);
  kint(j, "MasterID_RDV", master_id(a, r->pid[i]));
  if (full) kint(j, "MasterID_XBox360", -1);
  if (inst && full) {
    int max = a->max_difficulty;
    for (uint32_t p = 0, n = count(a->phrase, a->nphrases, MAX_LIST); p < n; p++)
      if (a->phrase[p].max_difficulty > max) max = a->phrase[p].max_difficulty;
    kint(j, "MaxPhraseDifficulty", max);
  }
  if (inst) {
    knum(j, "MediumMastery", hard > 0 ? a->notes_medium / hard : 0);
    knum(j, "NotesEasy", a->notes_easy), knum(j, "NotesHard", hard), knum(j, "NotesMedium", a->notes_medium);
  }
  kstr(j, "PersistentID", r->pid[i]);
  if (inst && full) phrases(j, r, a);
  if (full) kfmt(j, "PreviewBankPath", "song_%s_preview.bnk", k, 0), kint(j, "RelativeDifficulty", 0);
  if (inst && header) kint(j, "Representative", represents(s, i)), kint(j, "RouteMask", arr_path(a));
  kstr(j, "SKU", "RS2");
  if (inst && full) knum(j, "Score_MaxNotes", hard), knum(j, "Score_PNV", hard > 0 ? 100000 / hard : 0), sections(j, r, a);
  kbool(j, "Shipping", 1);
  if (full) {
    kfmt(j, "ShowlightsXML", "urn:application:xml:%s_showlights", k, 0);
    kfmt(j, "SongAsset", "urn:application:musicgame-song:%s_%s", k, name);
    if (inst) knum(j, "SongAverageTempo", a->tempo > 0 ? a->tempo : 120);
    kfmt(j, "SongBank", "song_%s.bnk", k, 0);
  }
  if (inst) {
    knum(j, "SongDiffEasy", a->diff_easy), knum(j, "SongDiffHard", a->diff_hard), knum(j, "SongDiffMed", a->diff_medium);
    knum(j, "SongDifficulty", a->diff_hard);
  }
  if (full) kfmt(j, "SongEvent", "Play_%s", r->key, 0);
  kstr(j, "SongKey", r->key);
  if (inst) {
    ktime(j, "SongLength", r->length), kstr(j, "SongName", title), kstr(j, "SongNameSort", sort_name(s->title_sort, title));
    if (full) ktime(j, "SongOffset", a->start_time == 0 ? 0 : -a->start_time), kint(j, "SongPartition", 1);
  }
  if (full) kfmt(j, "SongXml", "urn:application:xml:%s_%s", k, name);
  if (inst) kint(j, "SongYear", s->year);
  if (inst && full) kint(j, "TargetScore", 100000), levels(j, "Techniques", a->techniques, a->ntechnique_levels), tones(j, a);
  if (inst) {
    key(j, "Tuning"), jopen(j, '{');
    for (int n = 0; n < 6; n++) {
      char sk[] = "string0";
      sk[6] = (char)('0' + n);
      kint(j, sk, a->tuning[n]);
    }
    jclose(j, '}');
  }
  jclose(j, '}');
}

static void entry(js *j, const sb_song_info *s, const names *r, uint32_t i, int header) {
  key(j, r->pid[i]), jopen(j, '{'), key(j, "Attributes");
  attributes(j, s, r, i, header);
  jclose(j, '}');
}

static void manifest(out *o, const sb_song_info *s, const names *r, uint32_t i) {
  js j = {o, 0};
  jopen(&j, '{'), key(&j, "Entries"), jopen(&j, '{');
  entry(&j, s, r, i, 0);
  jclose(&j, '}');
  kstr(&j, "InsertRoot", "Static.Songs.Entries"), kint(&j, "IterationVersion", 3), kstr(&j, "ModelName", "RSEnumerable_Song");
  jclose(&j, '}');
}

static void hsan(out *o, const sb_song_info *s, const names *r) {
  js j = {o, 0};
  jopen(&j, '{'), key(&j, "Entries"), jopen(&j, '{');
  for (uint32_t i = 0; i < r->n; i++) entry(&j, s, r, i, 1);
  jclose(&j, '}');
  kstr(&j, "InsertRoot", "Static.Songs.Headers");
  jclose(&j, '}');
}

// --- the entity block --------------------------------------------------------------------------------------------
static void xprop(out *o, const char *name, const char *f, const char *a, const char *b) {
  fmt(o, "        <property name=\"%s\">\r\n          <set value=\"", name);
  fmt(o, f, a, b);
  str(o, "\" />\r\n        </property>\r\n");
}

static void xblock(out *o, const names *r) {
  const char *k = r->lkey;
  str(o, BOM "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n<game>\r\n  <entitySet>\r\n");
  for (uint32_t i = 0; i < r->n; i++) {
    char id[33], cap[SB_NAME_MAX + 1];
    lower_copy(id, sizeof id, r->pid[i]);
    memcpy(cap, r->name[i], sizeof cap);
    cap[0] = upper(cap[0]);
    fmt(o, "    <entity id=\"%s\" modelName=\"RSEnumerable_Song\" name=\"%s_%s\" iterations=\"0\">\r\n      <properties>\r\n", id,
        r->key, cap);
    xprop(o, "Header", "urn:database:hsan-db:songs_dlc_%s", k, 0);
    xprop(o, "Manifest", "urn:database:json-db:%s_%s", k, r->name[i]);
    xprop(o, "SngAsset", "urn:application:musicgame-song:%s_%s", k, r->name[i]);
    xprop(o, "AlbumArtSmall", "urn:image:dds:album_%s_64", k, 0);
    xprop(o, "AlbumArtMedium", "urn:image:dds:album_%s_128", k, 0);
    xprop(o, "AlbumArtLarge", "urn:image:dds:album_%s_256", k, 0);
    xprop(o, "LyricArt", "", 0, 0);
    xprop(o, "ShowLightsXMLAsset", "urn:application:xml:%s_showlights", k, 0);
    xprop(o, "SoundBank", "urn:audio:wwise-sound-bank:song_%s", k, 0);
    xprop(o, "PreviewSoundBank", "urn:audio:wwise-sound-bank:song_%s_preview", k, 0);
    str(o, "      </properties>\r\n    </entity>\r\n");
  }
  str(o, "  </entitySet>\r\n</game>");
}

// --- the aggregate graph -----------------------------------------------------------------------------------------
// One file of the archive as the graph tells of it
typedef struct {
  const char *tag[3];
  char rel[192], log[192];  // its path in the archive; the path without the platform's folder ("": it has none)
  uint8_t h[32];            // its UUID and link ID come from here
} node;

static uint32_t node_count(const names *r) { return 2 * r->n + 8; }

static void node_at(const sb_song_info *s, const names *r, int platform, uint32_t i, node *n) {
  static const int rest[] = {SB_E_HSAN, SB_E_SHOWLIGHTS, SB_E_ART64,       SB_E_ART128,
                             SB_E_ART256, SB_E_BNK,      SB_E_BNK_PREVIEW, SB_E_XBLOCK};
  int kind = i < 2 * r->n ? (i & 1 ? SB_E_SNG : SB_E_MANIFEST) : rest[i - 2 * r->n];
  uint32_t arr = i < 2 * r->n ? i / 2 : 0;
  const char *plat = platform == SB_MAC ? "macos" : "dx9";
  char same[192];  // the Mac's path whatever the platform: both platforms' archives give a file the same IDs
  out o = {(uint8_t *)n->rel, sizeof n->rel - 1, 0}, l = {(uint8_t *)n->log, sizeof n->log - 1, 0};
  out m = {(uint8_t *)same, sizeof same - 1, 0};
  memset(n, 0, sizeof *n);
  ch(&o, '/'), path_out(&o, r, s, platform, kind, arr);
  path_out(&m, r, s, SB_MAC, kind, arr);
  same[m.n] = 0;
  sha(n->h, "sb-node", same, 0);
  switch (kind) {
  case SB_E_MANIFEST: n->tag[0] = "database", n->tag[1] = "json-db"; break;
  case SB_E_HSAN: n->tag[0] = "database", n->tag[1] = "hsan-db"; break;
  case SB_E_XBLOCK: n->tag[0] = "emergent-world", n->tag[1] = "x-world"; break;
  case SB_E_SNG:
    n->tag[0] = "application", n->tag[1] = "musicgame-song", n->tag[2] = platform == SB_MAC ? plat : 0;
    fmt(&l, "/songs/bin/%s_%s.sng", r->lkey, r->name[arr]);
    break;
  case SB_E_SHOWLIGHTS:
    n->tag[0] = "application", n->tag[1] = "xml";
    put(&l, n->rel, o.n);
    break;
  case SB_E_BNK:
  case SB_E_BNK_PREVIEW:
    n->tag[0] = "audio", n->tag[1] = "wwise-sound-bank", n->tag[2] = plat;
    fmt(&l, kind == SB_E_BNK ? "/audio/song_%s.bnk" : "/audio/song_%s_preview.bnk", r->lkey);
    break;
  default:  // the album art
    n->tag[0] = "dds", n->tag[1] = "image";
    put(&l, n->rel, o.n);
    break;
  }
}

static void uuid(out *o, const uint8_t *h) {
  hex(o, h, 4, 0), ch(o, '-'), hex(o, h + 4, 2, 0), ch(o, '-'), hex(o, h + 6, 2, 0), ch(o, '-'), hex(o, h + 8, 2, 0), ch(o, '-');
  hex(o, h + 10, 6, 0);
}

// `<urn:uuid:...> <http://emergent.net/aweb/1.0/pred> "`; the caller writes the value and `".`
static void statement(out *o, const node *n, const char *pred) {
  if (o->n) ch(o, '\n');
  str(o, "<urn:uuid:"), uuid(o, n->h), fmt(o, "> <http://emergent.net/aweb/1.0/%s> \"", pred);
}

static void graph(out *o, const sb_song_info *s, const names *r, int platform) {
  static const char *const tags[] = {"database", "json-db", "hsan-db", "application", "musicgame-song", "macos", "dx9",
                                     "dds",      "image",   "xml",     "audio",       "wwise-sound-bank", "emergent-world", "x-world"};
  uint32_t nodes = node_count(r);
  node n;
  for (size_t t = 0; t < sizeof tags / sizeof *tags; t++)
    for (uint32_t i = 0; i < nodes; i++) {
      node_at(s, r, platform, i, &n);
      for (int k = 0; k < 3; k++)
        if (n.tag[k] && !strcmp(n.tag[k], tags[t])) statement(o, &n, "tag"), str(o, tags[t]), str(o, "\".");
    }
  for (int pred = 0; pred < 5; pred++)
    for (uint32_t i = 0; i < nodes; i++) {
      node_at(s, r, platform, i, &n);
      const char *slash = strrchr(n.rel, '/'), *dot = strrchr(n.rel, '.');
      if ((pred == 0 || pred == 4) && !n.log[0]) continue;  // only what the game loads by path has these
      switch (pred) {
      case 0: statement(o, &n, "llid"), hex(o, n.h + 16, 4, 0), str(o, "-0000-0000-0000-000000000000"); break;
      case 1: statement(o, &n, "canonical"), put(o, n.rel, (size_t)(slash - n.rel)); break;
      case 2: statement(o, &n, "name"), put(o, slash + 1, (size_t)(dot - slash - 1)); break;
      case 3: statement(o, &n, "relpath"), str(o, n.rel); break;
      default: statement(o, &n, "logpath"), str(o, n.log); break;
      }
      str(o, "\".");
    }
}

// --- the showlights ----------------------------------------------------------------------------------------------
// The lights' times: the song's own list, else the sections of the first arrangement that has any
static uint32_t light_times(const sb_song_info *s, const names *r, const double **t, const sb_man_section **sec) {
  *t = 0, *sec = 0;
  if (count(s->light_time, s->nlights, MAX_LEVELS)) return *t = s->light_time, count(s->light_time, s->nlights, MAX_LEVELS);
  for (uint32_t i = 0; i < r->n; i++) {
    uint32_t n = count(s->arr[i].section, s->arr[i].nsections, MAX_LIST);
    if (arr_type(&s->arr[i]) != SB_ARR_VOCALS && n) return *sec = s->arr[i].section, n;
  }
  return 0;
}

// Notes 24 to 35 are the fog's colours, 48 to 59 the beams', 66 is the lasers off. A time that is not in the song
// (when its length is known) or is before the one before it is left out; with none left the lights are set once,
// at 0. o 0: only counts.
static uint32_t lights(out *o, const sb_song_info *s, const names *r) {
  const double *t;
  const sb_man_section *sec;
  uint32_t n = light_times(s, r, &t, &sec), kept = 0;
  double last = 0, end = r->length > 0 ? r->length : 1e6;
  for (uint32_t i = 0; i <= n; i++) {
    double at = i == n ? 0 : t ? t[i] : sec[i].time;
    if (i == n ? kept != 0 : !(at >= last && at < end)) continue;
    if (o) {
      const int note[3] = {24 + (int)(kept % 12), 48 + (int)(kept % 12), 66};
      for (int k = 0; k < (kept ? 2 : 3); k++)
        str(o, "  <showlight time=\""), num(o, at, 3, 0), fmt(o, "\" note=\"%d\" />\r\n", note[k]);
    }
    last = at, kept++;
  }
  return 2 * kept + 1;
}

static void showlights(out *o, const sb_song_info *s, const names *r) {
  fmt(o, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n<showlights count=\"%u\">\r\n", lights(0, s, r));
  lights(o, s, r);
  str(o, "</showlights>");
}

// --- the flat models ---------------------------------------------------------------------------------------------
static void flat(out *o, int song) {
  static const struct {
    const char *name, *id;
  } p[] = {{"XmlAsset", "194332538"},          {"SngAsset", "2132105560"},      {"AlbumArtSmall", "2295103720"},
           {"AlbumArtMedium", "3067535336"},   {"AlbumArtLarge", "223887511"},  {"LyricArt", "3842719743"},
           {"ShowLightsXMLAsset", "2024527203"}, {"SoundBank", "4177482269"},   {"PreviewSoundBank", "1820138184"},
           {"Manifest", "3402662178"},         {"Header", "3529813182"}};
  str(o, BOM "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n<model\r\n");
  str(o, song ? "  name=\"RSEnumerable_Song\"\r\n  id=\"3871465268\">\r\n"
              : "  name=\"RSEnumerable_Root\"\r\n  id=\"1917312803\">\r\n");
  str(o, "  <trait\r\n    value=\"Active\" />\r\n");
  if (song) str(o, "  <mixin\r\n    name=\"RSEnumerable_Root\"\r\n    id=\"1917312803\"\r\n    type=\"Active\" />\r\n");
  for (size_t i = song ? 0 : 9; i < sizeof p / sizeof *p; i++)  // the root model has the last two alone
    fmt(o, "  <property\r\n    type=\"AssetID\"\r\n    name=\"%s\"\r\n    id=\"%s\">\r\n"
           "    <set\r\n      value=\"\" />\r\n  </property>\r\n",
        p[i].name, p[i].id);
  str(o, "</model>");
}

// --- the texts ---------------------------------------------------------------------------------------------------
int64_t sb_song_text(const sb_song_info *s, int platform, int kind, uint32_t arr, uint8_t *buf, size_t cap) {
  names r;
  if ((platform != SB_MAC && platform != SB_PC) || kind < 0 || kind > SB_E_MANIFEST || !resolve(s, &r, 0, 0)) return -1;
  out o = {buf, buf ? cap : 0, 0};
  int ok = kind == SB_E_MANIFEST ? arr < r.n : arr == 0;
  if (ok) switch (kind) {
    case SB_E_APPID: str(&o, SB_APPID); break;
    case SB_E_FLAT_ROOT: flat(&o, 0); break;
    case SB_E_FLAT_SONG: flat(&o, 1); break;
    case SB_E_GRAPH: graph(&o, s, &r, platform); break;
    case SB_E_XBLOCK: xblock(&o, &r); break;
    case SB_E_HSAN: hsan(&o, s, &r); break;
    case SB_E_SHOWLIGHTS: showlights(&o, s, &r); break;
    default: manifest(&o, s, &r, arr); break;
    }
  names_free(&r);
  return ok ? (int64_t)o.n : -1;
}

uint8_t *sb_song_text_alloc(const sb_song_info *s, int platform, int kind, uint32_t arr, size_t *n) {
  int64_t size = sb_song_text(s, platform, kind, arr, 0, 0);
  uint8_t *p = size < 0 ? 0 : malloc((size_t)size + 1);
  if (!p) return 0;
  if (sb_song_text(s, platform, kind, arr, p, (size_t)size) != size) {  // the facts changed under us
    free(p);
    return 0;
  }
  p[size] = 0;
  if (n) *n = (size_t)size;
  return p;
}
