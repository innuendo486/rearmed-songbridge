// The song texts' test program (tests/run_songtext_test.py drives it):
//   songtexttest self            the checks that need no files
//   songtexttest gen SPEC DIR    the song described by SPEC (JSON, see load_song) as text entries under DIR/mac and
//                                DIR/pc, and a line for every entry, arrangement and name on stdout
//   songtexttest key ARTIST TITLE  the song key
// Every text is made more than once and in buffers of other sizes: the bytes and the size must always be the same.
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "psarc.h"
#include "songtext.h"
#include "yyjson.h"

static int checks;
#define CHECK(c, ...)                                 \
  do {                                                \
    checks++;                                         \
    if (!(c)) {                                       \
      fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
      fprintf(stderr, __VA_ARGS__);                   \
      fprintf(stderr, "\n");                          \
      exit(1);                                        \
    }                                                 \
  } while (0)

// --- a text, made the hard way -----------------------------------------------------------------------------------
// The text (malloc'd, a 0 after it): asked for its size, made into a buffer of exactly that size between guard
// bytes, made again into half of it and by sb_song_text_alloc; all must agree. 0 when it cannot be made.
static char *text(const sb_song_info *s, int platform, int kind, uint32_t arr, size_t *n) {
  int64_t size = sb_song_text(s, platform, kind, arr, 0, 0);
  if (size < 0) {
    CHECK(!sb_song_text_alloc(s, platform, kind, arr, n), "alloc gives a text that has no size");
    return 0;
  }
  uint8_t *b = malloc((size_t)size + 9), *half = malloc((size_t)size + 9);
  size_t an = 0, h = (size_t)size / 2;
  memset(b, 0xa5, (size_t)size + 9), memset(half, 0xa5, (size_t)size + 9);
  CHECK(sb_song_text(s, platform, kind, arr, b + 4, (size_t)size) == size, "the size changes between two calls");
  CHECK(!memcmp(b, "\xa5\xa5\xa5\xa5", 4) && !memcmp(b + 4 + size, "\xa5\xa5\xa5\xa5", 4), "writes outside its buffer");
  CHECK(sb_song_text(s, platform, kind, arr, half + 4, h) == size, "the size depends on the buffer");
  CHECK(!memcmp(half + 4, b + 4, h) && half[4 + h] == 0xa5, "a short buffer gets something else than the start");
  CHECK(sb_song_text(s, platform, kind, arr, 0, 99) == size, "no buffer but a size for it");
  uint8_t *a = sb_song_text_alloc(s, platform, kind, arr, &an);
  CHECK(a && an == (size_t)size && !memcmp(a, b + 4, an) && !a[an], "alloc gives another text");
  free(a), free(half);
  memmove(b, b + 4, (size_t)size);
  b[size] = 0;
  if (n) *n = (size_t)size;
  return (char *)b;
}

static const char *const platform_name[] = {"mac", "pc"};

// --- gen ---------------------------------------------------------------------------------------------------------
typedef struct {
  void **p;
  size_t n, cap;
} arena;

static void *zalloc(arena *a, size_t count, size_t size) {
  if (a->n == a->cap) a->p = realloc(a->p, (a->cap = a->cap ? a->cap * 2 : 64) * sizeof *a->p);
  return a->p[a->n++] = calloc(count ? count : 1, size);
}

static void arena_free(arena *a) {
  for (size_t i = 0; i < a->n; i++) free(a->p[i]);
  free(a->p);
}

static const char *gs(yyjson_val *o, const char *k) { return yyjson_get_str(yyjson_obj_get(o, k)); }
static double gd(yyjson_val *o, const char *k) {
  yyjson_val *v = yyjson_obj_get(o, k);
  return yyjson_is_num(v) ? yyjson_get_num(v) : 0;
}
static int64_t gi(yyjson_val *o, const char *k) {
  yyjson_val *v = yyjson_obj_get(o, k);
  return yyjson_is_int(v) ? yyjson_get_sint(v) : (int64_t)gd(o, k);
}
static double at_d(yyjson_val *a, size_t i) { return yyjson_get_num(yyjson_arr_get(a, i)); }
static int64_t at_i(yyjson_val *a, size_t i) { return yyjson_get_sint(yyjson_arr_get(a, i)); }

// [[difficulty, iteration, [ids]], ...]
static const sb_level_ids *load_levels(yyjson_val *l, uint32_t *n, arena *A) {
  *n = (uint32_t)yyjson_arr_size(l);
  sb_level_ids *out = zalloc(A, *n, sizeof *out);
  for (uint32_t i = 0; i < *n; i++) {
    yyjson_val *e = yyjson_arr_get(l, i), *ids = yyjson_arr_get(e, 2);
    int *id = zalloc(A, yyjson_arr_size(ids), sizeof *id);
    for (size_t k = 0; k < yyjson_arr_size(ids); k++) id[k] = (int)at_i(ids, k);
    out[i] = (sb_level_ids){(uint32_t)at_i(e, 0), (uint32_t)at_i(e, 1), id, (uint32_t)yyjson_arr_size(ids)};
  }
  return out;
}

#define PROP(f) {#f, offsetof(sb_arr_props, f)}
static const struct {
  const char *name;
  size_t off;
} prop_names[] = {PROP(barre_chords),    PROP(bass_pick),          PROP(bends),          PROP(double_stops),   PROP(drop_d_power),
                  PROP(fifths_and_octaves), PROP(finger_picking),  PROP(fret_hand_mutes), PROP(harmonics),     PROP(hopo),
                  PROP(non_standard_chords), PROP(open_chords),    PROP(palm_mutes),     PROP(pick_direction), PROP(pinch_harmonics),
                  PROP(power_chords),    PROP(slap_pop),           PROP(slides),         PROP(sustain),        PROP(syncopation),
                  PROP(tapping),         PROP(tremolo),            PROP(two_finger_picking), PROP(unpitched_slides), PROP(vibrato)};

// The spec: the structs' fields by their names; lists of structs as lists of lists in the fields' order
// (phrases [name, max_difficulty], iters [phrase, time], sections [name, time], chord_templates [id, name,
// [fingers], [frets]], chords and techniques [difficulty, iteration, [ids]]); notes, diff, dna as [easy, medium,
// hard] and [chords, riffs, solo]; tones as a list of strings (each a tone's JSON text). A field left out is left 0.
static void load_song(yyjson_val *root, sb_song_info *s, arena *A) {
  memset(s, 0, sizeof *s);
  s->key = gs(root, "key"), s->title = gs(root, "title"), s->artist = gs(root, "artist"), s->album = gs(root, "album");
  s->title_sort = gs(root, "title_sort"), s->artist_sort = gs(root, "artist_sort"), s->album_sort = gs(root, "album_sort");
  s->year = (int)gi(root, "year"), s->length = gd(root, "length"), s->converted = gs(root, "converted");
  s->audio_id = (uint32_t)gi(root, "audio_id"), s->preview_id = (uint32_t)gi(root, "preview_id");
  yyjson_val *lights = yyjson_obj_get(root, "lights"), *arrs = yyjson_obj_get(root, "arr");
  if (lights) {
    double *t = zalloc(A, yyjson_arr_size(lights), sizeof *t);
    for (size_t i = 0; i < yyjson_arr_size(lights); i++) t[i] = at_d(lights, i);
    s->light_time = t, s->nlights = (uint32_t)yyjson_arr_size(lights);
  }
  s->narr = (uint32_t)yyjson_arr_size(arrs);
  sb_arr_info *arr = zalloc(A, s->narr, sizeof *arr);
  s->arr = arr;
  for (uint32_t i = 0; i < s->narr; i++) {
    yyjson_val *o = yyjson_arr_get(arrs, i), *v;
    sb_arr_info *a = &arr[i];
    a->type = (int)gi(o, "type"), a->name = gs(o, "name"), a->persistent_id = gs(o, "pid");
    a->master_id = (int32_t)gi(o, "master_id"), a->path = (int)gi(o, "path");
    a->bonus = (uint8_t)gi(o, "bonus"), a->alternate = (uint8_t)gi(o, "alternate");
    a->capo = (int)gi(o, "capo"), a->cents = gd(o, "cents"), a->tempo = gd(o, "tempo"), a->start_time = gd(o, "start_time");
    a->max_difficulty = (int)gi(o, "max_difficulty");
    if ((v = yyjson_obj_get(o, "tuning")))
      for (size_t k = 0; k < 6 && k < yyjson_arr_size(v); k++) a->tuning[k] = (int)at_i(v, k);
    if ((v = yyjson_obj_get(o, "notes")))
      a->notes_easy = (uint32_t)at_i(v, 0), a->notes_medium = (uint32_t)at_i(v, 1), a->notes_hard = (uint32_t)at_i(v, 2);
    if ((v = yyjson_obj_get(o, "diff"))) a->diff_easy = at_d(v, 0), a->diff_medium = at_d(v, 1), a->diff_hard = at_d(v, 2);
    if ((v = yyjson_obj_get(o, "dna"))) a->dna_chords = at_d(v, 0), a->dna_riffs = at_d(v, 1), a->dna_solo = at_d(v, 2);
    if ((v = yyjson_obj_get(o, "density"))) {
      float *d = zalloc(A, 20, sizeof *d);
      for (size_t k = 0; k < 20 && k < yyjson_arr_size(v); k++) d[k] = (float)at_d(v, k);
      a->density = d;
    }
    if ((v = yyjson_obj_get(o, "props")))
      for (size_t k = 0; k < sizeof prop_names / sizeof *prop_names; k++)
        ((uint8_t *)&a->props)[prop_names[k].off] = (uint8_t)gi(v, prop_names[k].name);
    if ((v = yyjson_obj_get(o, "phrases"))) {
      sb_man_phrase *p = zalloc(A, yyjson_arr_size(v), sizeof *p);
      for (size_t k = 0; k < yyjson_arr_size(v); k++) {
        yyjson_val *e = yyjson_arr_get(v, k);
        p[k] = (sb_man_phrase){yyjson_get_str(yyjson_arr_get(e, 0)), (int)at_i(e, 1)};
      }
      a->phrase = p, a->nphrases = (uint32_t)yyjson_arr_size(v);
    }
    if ((v = yyjson_obj_get(o, "iters"))) {
      sb_phrase_iter *p = zalloc(A, yyjson_arr_size(v), sizeof *p);
      for (size_t k = 0; k < yyjson_arr_size(v); k++) {
        yyjson_val *e = yyjson_arr_get(v, k);
        p[k] = (sb_phrase_iter){(uint32_t)at_i(e, 0), at_d(e, 1)};
      }
      a->iter = p, a->niters = (uint32_t)yyjson_arr_size(v);
    }
    if ((v = yyjson_obj_get(o, "sections"))) {
      sb_man_section *p = zalloc(A, yyjson_arr_size(v), sizeof *p);
      for (size_t k = 0; k < yyjson_arr_size(v); k++) {
        yyjson_val *e = yyjson_arr_get(v, k);
        p[k] = (sb_man_section){yyjson_get_str(yyjson_arr_get(e, 0)), at_d(e, 1)};
      }
      a->section = p, a->nsections = (uint32_t)yyjson_arr_size(v);
    }
    if ((v = yyjson_obj_get(o, "chord_templates"))) {
      sb_chord_template *p = zalloc(A, yyjson_arr_size(v), sizeof *p);
      for (size_t k = 0; k < yyjson_arr_size(v); k++) {
        yyjson_val *e = yyjson_arr_get(v, k);
        p[k].id = (int)at_i(e, 0), p[k].name = yyjson_get_str(yyjson_arr_get(e, 1));
        for (size_t m = 0; m < 6; m++)
          p[k].fingers[m] = (int8_t)at_i(yyjson_arr_get(e, 2), m), p[k].frets[m] = (int8_t)at_i(yyjson_arr_get(e, 3), m);
      }
      a->chord = p, a->nchords = (uint32_t)yyjson_arr_size(v);
    }
    if ((v = yyjson_obj_get(o, "chords"))) a->chords = load_levels(v, &a->nchord_levels, A);
    if ((v = yyjson_obj_get(o, "techniques"))) a->techniques = load_levels(v, &a->ntechnique_levels, A);
    if ((v = yyjson_obj_get(o, "tones"))) {
      const char **p = zalloc(A, yyjson_arr_size(v), sizeof *p);
      for (size_t k = 0; k < yyjson_arr_size(v); k++) p[k] = yyjson_get_str(yyjson_arr_get(v, k));
      a->tone = p, a->ntones = (uint32_t)yyjson_arr_size(v);
    }
    a->tone_base = gs(o, "tone_base"), a->tone_multiplayer = gs(o, "tone_multiplayer");
    a->tone_a = gs(o, "tone_a"), a->tone_b = gs(o, "tone_b"), a->tone_c = gs(o, "tone_c"), a->tone_d = gs(o, "tone_d");
  }
}

static void write_file(const char *dir, const char *platform, const char *path, const char *data, size_t n) {
  char full[1024];
  snprintf(full, sizeof full, "%s/%s/%s", dir, platform, path);
  for (char *p = full + strlen(dir) + 1; *p; p++)
    if (*p == '/') *p = 0, mkdir(full, 0777), *p = '/';
  FILE *f = fopen(full, "wb");
  CHECK(f && fwrite(data, 1, n, f) == n && !fclose(f), "cannot write %s", full);
}

static int gen(const char *spec, const char *dir) {
  yyjson_read_err rerr;
  yyjson_doc *doc = yyjson_read_file(spec, 0, 0, &rerr);
  CHECK(doc, "%s: %s", spec, rerr.msg);
  arena A = {0};
  sb_song_info s;
  char err[256] = "";
  load_song(yyjson_doc_get_root(doc), &s, &A);
  if (!sb_song_check(&s, err, sizeof err)) {
    CHECK(err[0], "refused without a reason");
    CHECK(!sb_song_entries(&s, 0, 0) && sb_song_text(&s, SB_MAC, SB_E_HSAN, 0, 0, 0) == -1, "refused, but has entries or texts");
    printf("error\t%s\n", err);
    arena_free(&A), yyjson_doc_free(doc);
    return 2;
  }
  uint32_t n = sb_song_entries(&s, 0, 0), song_id, preview_id, narr = s.narr > SB_MAX_ARR ? SB_MAX_ARR : s.narr;
  sb_entry *e = calloc(n, sizeof *e);
  char key[SB_KEY_MAX + 1];
  CHECK(n == 14 + 2 * narr && sb_song_entries(&s, e, n) == n, "the entries' count");
  CHECK(sb_song_key_of(&s, key) && sb_song_key_ok(key), "the song has no key");
  printf("key\t%s\n", key);
  mkdir(dir, 0777);
  for (int platform = SB_MAC; platform <= SB_PC; platform++)
    for (uint32_t i = 0; i < n; i++) {
      char path[256];
      size_t size = 0;
      CHECK(sb_song_entry_path(&s, platform, e[i].kind, e[i].arr, path, sizeof path) == (int)strlen(path), "an entry without a path");
      char *t = text(&s, platform, e[i].kind, e[i].arr, &size);
      CHECK(!t == (e[i].kind > SB_E_MANIFEST), "%s: text or not", path);
      if (t) write_file(dir, platform_name[platform], path, t, size);
      printf("entry\t%s\t%d\t%u\t%lld\t%s\n", platform_name[platform], e[i].kind, e[i].arr, t ? (long long)size : -1LL, path);
      free(t);
    }
  for (uint32_t i = 0; i < narr; i++) {
    char name[SB_NAME_MAX + 1], pid[33];
    CHECK(sb_song_arr_name(&s, i, name) && sb_song_arr_pid(&s, i, pid), "an arrangement without a name");
    printf("arr\t%u\t%s\t%s\n", i, name, pid);
  }
  for (int preview = 0; preview < 2; preview++) {
    char ev[64];
    CHECK(sb_song_event(key, preview, ev, sizeof ev) > 0, "no event name");
    printf("event\t%s\t%u\n", ev, sb_wwise_id(ev));
  }
  sb_song_audio_ids(key, &song_id, &preview_id);
  printf("audio\t%u\t%u\n", song_id, preview_id);
  free(e), arena_free(&A), yyjson_doc_free(doc);
  return 0;
}

// --- self --------------------------------------------------------------------------------------------------------
// The text parsed as JSON: it must be
static yyjson_doc *parsed(const sb_song_info *s, int kind, uint32_t arr) {
  size_t n = 0;
  char *t = text(s, SB_MAC, kind, arr, &n);
  CHECK(t, "no text");
  CHECK(strlen(t) == n, "a 0 inside the text");
  yyjson_read_err rerr;
  yyjson_doc *d = yyjson_read_opts(t, n, 0, 0, &rerr);
  CHECK(d, "not JSON: %s at %zu in %.200s", rerr.msg, rerr.pos, t);
  free(t);
  return d;
}

// The attributes of the first entry
static yyjson_val *attrs(yyjson_doc *d) {
  yyjson_val *entries = yyjson_obj_get(yyjson_doc_get_root(d), "Entries");
  yyjson_obj_iter it = yyjson_obj_iter_with(entries);
  yyjson_val *k = yyjson_obj_iter_next(&it);
  return yyjson_obj_get(yyjson_obj_iter_get_val(k), "Attributes");
}

static int all_alnum(const char *s) {
  for (; *s; s++)
    if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z'))) return 0;
  return 1;
}

static void test_keys(void) {
  char k[SB_KEY_MAX + 1], k2[SB_KEY_MAX + 1], longs[301];
  memset(longs, 'x', 300), longs[300] = 0;
  static const char *const songs[][2] = {
    {"ABBA", "Dancing Queen"}, {"AC/DC", "Back In Black"}, {"ACDC", "BackInBlack"},  {"", ""},
    {"3 Doors Down", "Kryptonite"}, {"\xe3\x82\xa2\xe3\x83\x99", "\xe6\xad\x8c"},  {"Motörhead", "Été — \"Live\""},
    {"a", "b"},  {"A", "B"}, {"ab", ""}, {"", "ab"}, {"The \\ Back\nslash", "x"}, {"...", "!!!"}, {"Queen", "Innuendo"}, {"Queen", "Innuendo "}};
  enum { N = sizeof songs / sizeof *songs };
  char all[N + 3][SB_KEY_MAX + 1];
  for (int i = 0; i < N; i++) {
    sb_song_key(songs[i][0], songs[i][1], k);
    sb_song_key(songs[i][0], songs[i][1], k2);
    CHECK(!strcmp(k, k2), "the key of the same song changes");
    CHECK(sb_song_key_ok(k) && all_alnum(k), "%s is no key", k);
    CHECK(strlen(k) >= 9 && strlen(k) <= 30, "%s: its length", k);
    CHECK((k[0] >= 'A' && k[0] <= 'Z') || (k[0] >= 'a' && k[0] <= 'z'), "%s does not start with a letter", k);
    strcpy(all[i], k);
  }
  sb_song_key(0, 0, all[N]);
  CHECK(!strcmp(all[N], all[3]), "no artist and no title is the empty artist and title");
  sb_song_key(longs, longs, all[N]);
  CHECK(sb_song_key_ok(all[N]) && strlen(all[N]) == 30, "a long name's key: %s", all[N]);
  longs[299] = 'y';
  sb_song_key(longs, longs, all[N + 1]);
  CHECK(sb_song_key_ok(all[N + 1]), "a long name's key");
  sb_song_key("x", longs, all[N + 2]);
  for (int i = 0; i < N + 3; i++)
    for (int j = 0; j < i; j++)
      CHECK(strcmp(all[i], all[j]) || (i == N && j == 3), "songs %d and %d have the same key %s", i, j, all[i]);
  sb_song_key("ABBA", "Dancing Queen", k);
  CHECK(!strncmp(k, "ABBADancingQueen", 16) && strlen(k) == 24, "ABBA's key is %s", k);
  sb_song_key("3 Doors Down", "Kryptonite", k);
  CHECK(!strncmp(k, "S3DoorsDownKryptonite", 21), "a key that would start with a digit: %s", k);
  CHECK(!sb_song_key_ok("") && !sb_song_key_ok(0) && !sb_song_key_ok("a-b") && !sb_song_key_ok("a b") && !sb_song_key_ok("\xc3\xa4"),
        "takes a bad key");
  CHECK(sb_song_key_ok("a") && sb_song_key_ok("12345678901234567890123456789012") && !sb_song_key_ok("123456789012345678901234567890123"),
        "the key's length");

  char p1[33], p2[33], p3[33];
  sb_song_pid("AbbaDanc", "lead", p1), sb_song_pid("abbadanc", "LEAD", p2), sb_song_pid("AbbaDanc", "lead2", p3);
  CHECK(strlen(p1) == 32 && !strcmp(p1, p2) && strcmp(p1, p3), "persistent IDs");
  for (int i = 0; i < 32; i++) CHECK(strchr("0123456789ABCDEF", p1[i]), "a persistent ID is upper-case hex");
  sb_song_pid("AbbaDanc2", "lead", p2);
  CHECK(strcmp(p1, p2), "two songs' arrangements share a persistent ID");

  uint32_t a, b, c, d;
  sb_song_audio_ids("AbbaDanc", &a, &b), sb_song_audio_ids("abbadanc", &c, &d);
  CHECK(a && b && a != b && a == c && b == d && a < 0x80000000u && b < 0x80000000u, "audio ids");
  sb_song_audio_ids("AbbaDanc", 0, 0);

  char ev[64], tiny[8];
  CHECK(sb_song_event("AbbaDanc", 0, ev, sizeof ev) == 13 && !strcmp(ev, "Play_AbbaDanc"), "the event");
  CHECK(sb_song_event("AbbaDanc", 1, ev, sizeof ev) == 21 && !strcmp(ev, "Play_AbbaDanc_Preview"), "the preview's event");
  CHECK(sb_song_event("AbbaDanc", 0, tiny, sizeof tiny) == -1 && sb_song_event("AbbaDanc", 0, ev, 13) == -1 && sb_song_event("AbbaDanc", 0, ev, 14) == 13,
        "an event that does not fit");
  // FNV-1 by hand: of "a", (2166136261 * 16777619) ^ 'a'
  CHECK(sb_wwise_id("a") == 0x050c5d7eu && sb_wwise_id("A") == sb_wwise_id("a") && sb_wwise_id("") == 2166136261u && sb_wwise_id(0) == 2166136261u,
        "the Wwise ID");
  CHECK(sb_wwise_id("Play_AbbaDanc") == sb_wwise_id("play_abbadanc") && sb_wwise_id("Play_AbbaDanc") != sb_wwise_id("Play_AbbaDanc_Preview"),
        "the Wwise ID's case");
}

// An empty song: one arrangement of each type, everything else 0
static void test_minimal(void) {
  for (int type = SB_ARR_LEAD; type <= SB_ARR_VOCALS; type++) {
    sb_arr_info a = {.type = type};
    sb_song_info s = {.key = "Min", .arr = &a, .narr = 1};
    char err[64] = "x";
    CHECK(sb_song_check(&s, err, sizeof err) && !strcmp(err, "x"), "an empty arrangement is refused: %s", err);
    CHECK(sb_song_check(&s, 0, 0) && sb_song_check(&s, err, 0), "the reason is not optional");
    for (int platform = SB_MAC; platform <= SB_PC; platform++)
      for (int kind = 0; kind < SB_E_KINDS; kind++) {
        size_t n = 0;
        char *t = text(&s, platform, kind, 0, &n), path[128];
        CHECK(!t == (kind > SB_E_MANIFEST), "kind %d: text or not", kind);
        CHECK(!t || n > 0, "an empty text");
        CHECK(sb_song_entry_path(&s, platform, kind, 0, path, sizeof path) > 0, "kind %d has no path", kind);
        CHECK(sb_song_entry_path(&s, platform, kind, 0, path, strlen(path)) == -1, "a path in a buffer too small");
        CHECK(sb_song_entry_path(&s, platform, kind, 1, path, sizeof path) == -1, "a path of an arrangement that is not");
        CHECK(sb_song_text(&s, platform, kind, 1, 0, 0) == -1, "a text of an arrangement that is not");
        if (kind == SB_E_APPID) CHECK(n == 6 && !memcmp(t, "248750", 6), "the AppID");
        free(t);
      }
    CHECK(sb_song_text(&s, 2, SB_E_HSAN, 0, 0, 0) == -1 && sb_song_text(&s, -1, SB_E_HSAN, 0, 0, 0) == -1, "a third platform");
    CHECK(sb_song_text(&s, SB_MAC, -1, 0, 0, 0) == -1 && sb_song_text(&s, SB_MAC, SB_E_KINDS, 0, 0, 0) == -1, "a kind that is not");
    char path[8];
    CHECK(sb_song_entry_path(&s, 2, SB_E_HSAN, 0, path, sizeof path) == -1 && sb_song_entry_path(&s, SB_MAC, SB_E_KINDS, 0, path, sizeof path) == -1,
          "a path for nothing");
    yyjson_doc *d = parsed(&s, SB_E_MANIFEST, 0), *h = parsed(&s, SB_E_HSAN, 0);
    yyjson_val *at = attrs(d);
    CHECK(!strcmp(yyjson_get_str(yyjson_obj_get(at, "SongKey")), "Min"), "the key in the manifest");
    if (type != SB_ARR_VOCALS) {
      yyjson_val *tones = yyjson_obj_get(at, "Tones"), *gear = yyjson_obj_get(yyjson_arr_get_first(tones), "GearList");
      const char *want = type == SB_ARR_BASS ? "Bass_Amp_LT25B" : "Amp_TW22";
      CHECK(yyjson_arr_size(tones) == 1 && yyjson_obj_size(gear) == 2, "one tone of an amp and a cabinet");
      CHECK(!strcmp(yyjson_get_str(yyjson_obj_get(yyjson_obj_get(gear, "Amp"), "Key")), want), "the default amp");
      CHECK(!strcmp(yyjson_get_str(yyjson_obj_get(at, "Tone_Base")), yyjson_get_str(yyjson_obj_get(yyjson_arr_get_first(tones), "Key"))),
            "the base tone is the tone");
      CHECK(yyjson_get_num(yyjson_obj_get(at, "SongAverageTempo")) == 120 && yyjson_get_num(yyjson_obj_get(at, "Score_PNV")) == 0, "defaults");
      CHECK(yyjson_arr_size(yyjson_obj_get(at, "DynamicVisualDensity")) == 20 && !yyjson_obj_get(at, "CapoFret"), "defaults");
      CHECK(!strcmp(yyjson_get_str(yyjson_obj_get(at, "SongName")), "Unknown Title") && !strcmp(yyjson_get_str(yyjson_obj_get(at, "ArtistName")), "Unknown Artist") &&
              !strcmp(yyjson_get_str(yyjson_obj_get(at, "AlbumName")), "Unknown Album") && yyjson_get_num(yyjson_obj_get(at, "SongLength")) == 0 &&
              yyjson_get_num(yyjson_obj_get(at, "SongYear")) == 0,
            "a song nothing is known of");
      CHECK(yyjson_obj_size(yyjson_obj_get(at, "ArrangementProperties")) == 32, "32 arrangement properties");
    }
    yyjson_doc_free(d), yyjson_doc_free(h);
  }
}

static void refused(const sb_song_info *s, const char *what) {
  char err[128] = "", path[128], key[SB_KEY_MAX + 1];
  sb_entry e[4];
  CHECK(!sb_song_check(s, err, sizeof err) && err[0], "%s is not refused", what);
  CHECK(!sb_song_check(s, 0, 0), "%s is not refused without a place for the reason", what);
  CHECK(!sb_song_entries(s, e, 4), "%s has entries", what);
  for (int kind = 0; kind < SB_E_KINDS; kind++) {
    CHECK(sb_song_text(s, SB_MAC, kind, 0, 0, 0) == -1 && !sb_song_text_alloc(s, SB_PC, kind, 0, 0), "%s has a text", what);
    CHECK(sb_song_entry_path(s, SB_MAC, kind, 0, path, sizeof path) == -1, "%s has a path", what);
  }
  char name[SB_NAME_MAX + 1], pid[33];
  CHECK(!sb_song_arr_name(s, 0, name) && !sb_song_arr_pid(s, 0, pid) && !sb_song_key_of(s, key), "%s has names", what);
}

// Every text of a song that must be taken as it is: the manifests are JSON, the names are names. The key (to free).
static char *taken(const sb_song_info *s, const char *what) {
  char err[128] = "", *key = malloc(SB_KEY_MAX + 1);
  CHECK(sb_song_check(s, err, sizeof err), "%s is refused: %s", what, err);
  CHECK(sb_song_key_of(s, key) && sb_song_key_ok(key), "%s: the key", what);
  uint32_t narr = s->narr > SB_MAX_ARR ? SB_MAX_ARR : s->narr, n = sb_song_entries(s, 0, 0);
  CHECK(n == 14 + 2 * narr, "%s: the entries", what);
  sb_entry *e = calloc(n, sizeof *e);
  sb_song_entries(s, e, n);
  for (int platform = SB_MAC; platform <= SB_PC; platform++)
    for (uint32_t i = 0; i < n; i++) {
      char path[160];
      CHECK(sb_song_entry_path(s, platform, e[i].kind, e[i].arr, path, sizeof path) > 0, "%s: an entry's path", what);
      for (const char *p = path; *p; p++)
        CHECK((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '/' || *p == '_' || *p == '.', "%s: the path %s", what, path);
      if (e[i].kind > SB_E_MANIFEST) continue;
      if (platform == SB_MAC && (e[i].kind == SB_E_MANIFEST || e[i].kind == SB_E_HSAN)) yyjson_doc_free(parsed(s, e[i].kind, e[i].arr));
      else free(text(s, platform, e[i].kind, e[i].arr, 0));
    }
  free(e);
  return key;
}

static void test_refused(void) {
  sb_arr_info a = {.type = SB_ARR_LEAD}, *many = calloc(SB_MAX_ARR + 500, sizeof *many);
  sb_song_info ok = {.key = "Song", .length = 200, .arr = &a, .narr = 1}, s;
  char *k, name[SB_NAME_MAX + 1], pid[33], pid2[33];
  free(taken(&ok, "the good song"));
  // the only songs that are refused
  refused(0, "no song");
  s = ok, s.narr = 0, refused(&s, "no arrangements");
  s = ok, s.arr = 0, refused(&s, "arrangements that are not there");
  s = ok, s.arr = 0, s.narr = 0, refused(&s, "nothing");

  // more arrangements than are used
  s = ok, s.arr = many, s.narr = SB_MAX_ARR + 500;
  CHECK(sb_song_check(&s, 0, 0) && sb_song_entries(&s, 0, 0) == 14 + 2 * SB_MAX_ARR, "too many arrangements are cut");
  CHECK(sb_song_arr_name(&s, SB_MAX_ARR - 1, name) && !strcmp(name, "lead256") && !sb_song_arr_name(&s, SB_MAX_ARR, name), "the last arrangement used");
  CHECK(sb_song_text(&s, SB_MAC, SB_E_MANIFEST, SB_MAX_ARR - 1, 0, 0) > 0 && sb_song_text(&s, SB_MAC, SB_E_MANIFEST, SB_MAX_ARR, 0, 0) == -1, "its manifest");
  free(many);

  // keys: what is left of them, or one made of the artist and the title
  static const char *const keys[][2] = {{"a-b", "ab"}, {"a/b", "ab"}, {"../x", "x"}, {"Sp ace", "Space"}, {"\xc3\x84\xc3\xa4ni", "ni"},
                                        {"123456789012345678901234567890123", "12345678901234567890123456789012"}};
  for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) {
    s = ok, s.key = keys[i][0], k = taken(&s, "a key with more than letters");
    CHECK(!strcmp(k, keys[i][1]), "the key %s came to %s", keys[i][0], k);
    free(k);
  }
  static const char *const nokeys[] = {0, "", "---", "\xe6\xad\x8c"};
  char made[SB_KEY_MAX + 1], made2[SB_KEY_MAX + 1];
  sb_song_key("Band", "Song", made), sb_song_key(0, 0, made2);
  for (size_t i = 0; i < sizeof nokeys / sizeof *nokeys; i++) {
    s = ok, s.key = nokeys[i], s.artist = "Band", s.title = "Song", k = taken(&s, "no key");
    CHECK(!strcmp(k, made), "without a key: %s, not %s", k, made);
    free(k);
    s.artist = 0, s.title = 0, k = taken(&s, "no key, artist or title");
    CHECK(!strcmp(k, made2), "without anything: %s, not %s", k, made2);
    free(k);
  }

  static const double lengths[] = {0, -1, NAN, INFINITY, -INFINITY, 1e300, -1e300};
  for (size_t i = 0; i < sizeof lengths / sizeof *lengths; i++) {
    s = ok, s.length = lengths[i], free(taken(&s, "a length that is none"));
    yyjson_doc *d = parsed(&s, SB_E_MANIFEST, 0);
    double len = yyjson_get_num(yyjson_obj_get(attrs(d), "SongLength"));
    CHECK(len == (lengths[i] > 1 ? 1e6 : 0), "the length %g is written as %g", lengths[i], len);
    yyjson_doc_free(d);
  }
  s = ok, s.nlights = 3, free(taken(&s, "light times that are not there"));
  s = ok, s.title = "", s.artist = "", s.album = "", s.title_sort = "", s.converted = "", free(taken(&s, "empty strings"));
  char *huge = malloc(200001);
  memset(huge, 'h', 200000), huge[200000] = 0;
  s = ok, s.key = huge, s.title = huge, s.artist = huge, s.album = huge, s.converted = huge, a.name = huge, a.persistent_id = huge, a.tone_base = huge;
  free(taken(&s, "very long strings"));
  CHECK(sb_song_arr_name(&s, 0, name) && strlen(name) == SB_NAME_MAX - 5, "a long name is cut: %s", name);
  a = (sb_arr_info){.type = SB_ARR_LEAD};
  free(huge);

  sb_arr_info b;
#define ODD(what, ...)             \
  do {                             \
    b = a;                         \
    __VA_ARGS__;                   \
    s = ok, s.arr = &b;            \
    free(taken(&s, what));         \
  } while (0)
#define NAME(want) CHECK(sb_song_arr_name(&s, 0, name) && !strcmp(name, want), "the name came to %s, not %s", name, want)
  ODD("a type that is not", b.type = 5);
  NAME("lead");
  ODD("a type that is not", b.type = INT_MIN);
  ODD("a path that is not", b.path = 3);
  ODD("a path that is not", b.path = INT_MAX);
  ODD("an empty name", b.name = "");
  NAME("lead");
  ODD("an empty bass's name", b.name = "", b.type = SB_ARR_BASS);
  NAME("bass");
  ODD("a name with a slash", b.name = "le/ad");
  NAME("lead");
  ODD("a name of dots", b.name = "..");
  NAME("lead");
  ODD("a name with a dash", b.name = "Lead-2");
  NAME("lead2");
  ODD("a name with spaces", b.name = " Rhythm Guitar (Alt.) ");
  NAME("rhythmguitaralt");
  ODD("a name that is not ASCII", b.name = "\xe3\x83\xaa\xe3\x83\xbc\xe3\x83\x89", b.type = SB_ARR_RHYTHM);
  NAME("rhythm");
  ODD("a long name", b.name = "abcdefghijklmnopqrstuvwxyz0123456789");
  NAME("abcdefghijklmnopqrs");
  ODD("the lights' name", b.name = "ShowLights");
  NAME("showlights2");
  sb_song_pid("Song", "lead", pid2);
  ODD("a short persistent ID", b.persistent_id = "ABCDEF");
  CHECK(sb_song_arr_pid(&s, 0, pid) && !strcmp(pid, pid2), "a bad persistent ID is not replaced by the derived one");
  ODD("a long persistent ID", b.persistent_id = "0123456789ABCDEF0123456789ABCDEF0");
  CHECK(sb_song_arr_pid(&s, 0, pid) && !strcmp(pid, pid2), "a bad persistent ID is not replaced by the derived one");
  ODD("a persistent ID that is not hex", b.persistent_id = "0123456789ABCDEF0123456789ABCDEG");
  CHECK(sb_song_arr_pid(&s, 0, pid) && !strcmp(pid, pid2), "a bad persistent ID is not replaced by the derived one");
  ODD("an empty persistent ID", b.persistent_id = "");
  CHECK(sb_song_arr_pid(&s, 0, pid) && !strcmp(pid, pid2), "a bad persistent ID is not replaced by the derived one");
  ODD("phrases that are not there", b.nphrases = 1);
  ODD("sections that are not there", b.nsections = 1);
  ODD("iterations that are not there", b.niters = 1);
  ODD("chord templates that are not there", b.nchords = 1);
  ODD("tones that are not there", b.ntones = 4);
  sb_man_phrase ph = {"p", 3};
  sb_phrase_iter it[2] = {{1, 0.0}, {UINT32_MAX, 5.0}};
  ODD("iterations of no phrase", b.phrase = &ph, b.nphrases = 1, b.iter = it, b.niters = 2);
  yyjson_doc *d = parsed(&s, SB_E_MANIFEST, 0);
  yyjson_val *v = yyjson_obj_get(attrs(d), "PhraseIterations");
  CHECK(yyjson_get_int(yyjson_obj_get(yyjson_arr_get(v, 1), "PhraseIndex")) == 0 && !strcmp(yyjson_get_str(yyjson_obj_get(yyjson_arr_get(v, 1), "Name")), "p"),
        "an iteration of no phrase is of the first");
  CHECK(yyjson_get_int(yyjson_obj_get(yyjson_arr_get_first(yyjson_obj_get(attrs(d), "Phrases")), "IterationCount")) == 2, "and counts as one");
  yyjson_doc_free(d);
  ODD("iterations without phrases", b.iter = it, b.niters = 2);
  int ids[2] = {1, 2};
  sb_level_ids mixed[5] = {{2, 0, ids, 2}, {1, 0, ids, 2}, {2, 0, ids, 1}, {2, 1, 0, 2}, {3, 7, ids, 2}};
  ODD("chords and techniques out of order", b.chords = mixed, b.nchord_levels = 5, b.techniques = mixed + 1, b.ntechnique_levels = 4);
  d = parsed(&s, SB_E_MANIFEST, 0), v = yyjson_obj_get(attrs(d), "Chords");
  CHECK(yyjson_obj_size(v) == 2 && yyjson_obj_size(yyjson_obj_get(v, "2")) == 2 && yyjson_arr_size(yyjson_obj_get(yyjson_obj_get(v, "2"), "0")) == 2 &&
          yyjson_arr_size(yyjson_obj_get(yyjson_obj_get(v, "2"), "1")) == 0 && yyjson_arr_size(yyjson_obj_get(yyjson_obj_get(v, "3"), "7")) == 2,
        "chords out of order are left out");
  CHECK(yyjson_obj_size(yyjson_obj_get(attrs(d), "Techniques")) == 3, "techniques in order are all there");
  yyjson_doc_free(d);
  ODD("chords that are not there", b.nchord_levels = 1);
#undef ODD

  // arrangements that would be the same: numbered apart, in the order given
  sb_arr_info same[7] = {{.name = "Lead"}, {.name = "lead"}, {.type = SB_ARR_LEAD}, {.name = "LEAD-2"}, {.name = "lead 3"}, {.name = ""}, {.name = "showlights"}};
  for (int i = 0; i < 7; i++) same[i].persistent_id = "0123456789abcdef0123456789abcdef";
  s = ok, s.arr = same, s.narr = 7, free(taken(&s, "seven arrangements called lead"));
  static const char *const want[7] = {"lead", "lead2", "lead3", "lead22", "lead32", "lead4", "showlights2"};
  char pids[7][33];
  for (uint32_t i = 0; i < 7; i++) {
    CHECK(sb_song_arr_name(&s, i, name) && !strcmp(name, want[i]), "arrangement %u is called %s, not %s", i, name, want[i]);
    CHECK(sb_song_arr_pid(&s, i, pids[i]), "a persistent ID");
    for (uint32_t m = 0; m < i; m++) CHECK(strcmp(pids[i], pids[m]), "arrangements %u and %u share a persistent ID", i, m);
  }
  CHECK(!strcmp(pids[0], "0123456789ABCDEF0123456789ABCDEF"), "the first keeps the persistent ID given");
}

static void test_many(void) {
  static const uint32_t counts[] = {20, SB_MAX_ARR};
  for (int c = 0; c < 2; c++) {
    uint32_t count = counts[c], step = count > 64 ? 37 : 1;
    sb_arr_info *a = calloc(count, sizeof *a);
    for (uint32_t i = 0; i < count; i++) a[i].type = (int)(i % 5);
    sb_song_info s = {.key = "Many", .title = "Many", .length = 100, .arr = a, .narr = count};
    char err[128] = "";
    CHECK(sb_song_check(&s, err, sizeof err), "%u arrangements: %s", count, err);
    uint32_t n = 14 + 2 * count;
    sb_entry *e = calloc(n, sizeof *e);
    CHECK(sb_song_entries(&s, e, n) == n, "%u arrangements' entries", count);
    CHECK(sb_song_entries(&s, e, 3) == n, "the entries' count does not depend on the room");
    char (*name)[SB_NAME_MAX + 1] = calloc(count, sizeof *name), (*pid)[33] = calloc(count, sizeof *pid), (*path)[128] = calloc(n, sizeof *path);
    for (uint32_t i = 0; i < count; i++) {
      CHECK(sb_song_arr_name(&s, i, name[i]) && sb_song_arr_pid(&s, i, pid[i]), "names");
      for (uint32_t k = 0; k < i; k++) CHECK(strcmp(name[i], name[k]) && strcmp(pid[i], pid[k]), "arrangements %u and %u are the same", i, k);
      if (i % step) continue;
      yyjson_doc *d = parsed(&s, SB_E_MANIFEST, i);
      CHECK(!strcmp(yyjson_get_str(yyjson_obj_get(attrs(d), "PersistentID")), pid[i]), "the manifest's persistent ID");
      yyjson_doc_free(d);
    }
    CHECK(!strcmp(name[0], "lead") && !strcmp(name[5], "lead2") && !strcmp(name[10], "lead3") && !strcmp(name[9], "vocals2"), "the names by type");
    for (uint32_t i = 0; i < n; i++) {
      CHECK(sb_song_entry_path(&s, SB_MAC, e[i].kind, e[i].arr, path[i], sizeof path[i]) > 0, "an entry's path");
      for (uint32_t k = 0; k < i; k++) CHECK(strcmp(path[i], path[k]), "two entries at %s", path[i]);
    }
    yyjson_doc *h = parsed(&s, SB_E_HSAN, 0);
    CHECK(yyjson_obj_size(yyjson_obj_get(yyjson_doc_get_root(h), "Entries")) == count, "the header has every arrangement");
    yyjson_doc_free(h);
    for (int platform = SB_MAC; platform <= SB_PC; platform++)
      for (int kind = 0; kind <= SB_E_SHOWLIGHTS; kind++) free(text(&s, platform, kind, 0, 0));
    free(a), free(e), free(name), free(pid), free(path);
  }
}

// Numbers no song has: the texts must still be texts
static void test_extremes(void) {
  static const double bad[] = {NAN, INFINITY, -INFINITY, 1e300, -1e300, 1e-300, -0.0, 0.0004, -0.0004, 4e9, -4e9};
  enum { NB = sizeof bad / sizeof *bad, BIG = 4096 };
  sb_man_phrase *ph = calloc(BIG, sizeof *ph);
  sb_phrase_iter *it = calloc(BIG, sizeof *it);
  sb_man_section *sec = calloc(BIG, sizeof *sec);
  sb_chord_template *ct = calloc(BIG, sizeof *ct);
  sb_level_ids *lv = calloc(BIG, sizeof *lv);
  double *lt = calloc(BIG, sizeof *lt);
  int ids[3] = {INT_MIN, 0, INT_MAX};
  float dens[20];
  for (int i = 0; i < 20; i++) dens[i] = (float)bad[i % NB];
  static const char *const tn[] = {0, "", "[", "{\"Key\":\"k\\u0001\"}", "{}", "nonsense", "{\"Key\":7}"};
  for (uint32_t i = 0; i < BIG; i++) {
    ph[i] = (sb_man_phrase){i % 3 ? "p\"\\\n" : 0, i % 2 ? INT_MAX : INT_MIN};
    it[i] = (sb_phrase_iter){BIG - 1 - i, bad[i % NB]};
    sec[i] = (sb_man_section){i % 4 == 0 ? 0 : i % 4 == 1 ? "solo" : i % 4 == 2 ? "Verse 2 \xff" : "a section whose name is long \t", bad[(i + 3) % NB]};
    ct[i] = (sb_chord_template){i % 2 ? INT_MAX : INT_MIN, i % 2 ? "C\"7" : 0, {-128, 127, 0, -1, 1, 2}, {127, -128, 0, 24, -1, 3}};
    lv[i] = (sb_level_ids){i / 64, i % 64 + (i % 64 == 63 ? 0xfffffff0u : 0), i % 5 ? ids : 0, i % 5 ? 3 : i % 2 ? 9 : 0};
    lt[i] = bad[i % NB];
  }
  for (int v = 0; v < NB; v++) {
    uint32_t big = v ? 96 : BIG;  // the longest lists once: they are slow under the sanitizers
    sb_arr_info a[2] = {{.type = SB_ARR_LEAD, .master_id = INT32_MIN, .path = SB_PATH_BASS, .bonus = 255, .alternate = 255,
                         .tuning = {INT_MIN, INT_MAX, -1, 1, 0, 0}, .capo = INT_MAX, .cents = bad[v], .tempo = bad[v], .start_time = bad[v],
                         .notes_easy = UINT32_MAX, .notes_medium = UINT32_MAX, .notes_hard = v % 2 ? UINT32_MAX : 0, .max_difficulty = INT_MAX,
                         .diff_easy = bad[v], .diff_medium = bad[v], .diff_hard = bad[v], .dna_chords = bad[v], .dna_riffs = bad[v],
                         .dna_solo = bad[v], .density = dens, .phrase = ph, .nphrases = big, .iter = it, .niters = big, .section = sec,
                         .nsections = big, .chord = ct, .nchords = big, .chords = lv, .nchord_levels = big, .techniques = lv,
                         .ntechnique_levels = big, .tone_base = "\x01\x7f", .tone_a = "\xff\xfe", .tone_d = "d", .tone = tn, .ntones = 7},
                        {.type = SB_ARR_BASS, .capo = INT_MIN, .max_difficulty = INT_MIN, .master_id = INT32_MAX}};
    memset(&a[0].props, 0xff, sizeof a[0].props);
    sb_song_info s = {.key = "Z", .title = "\xc0\xaf\xed\xa0\x80\xf4\x90\x80\x80\xe2\x82", .artist = "\x01\x02\x1f", .year = v % 2 ? INT_MIN : INT_MAX,
                      .length = v % 2 ? 1e300 : 1e-9, .arr = a, .narr = 2, .audio_id = UINT32_MAX, .preview_id = UINT32_MAX,
                      .converted = "\"\\", .light_time = lt, .nlights = v % 3 ? big : 0};
    char err[128] = "";
    CHECK(sb_song_check(&s, err, sizeof err), "extremes are refused: %s", err);
    for (uint32_t i = 0; i < 2; i++) yyjson_doc_free(parsed(&s, SB_E_MANIFEST, i));
    yyjson_doc_free(parsed(&s, SB_E_HSAN, 0));
    for (int platform = SB_MAC; platform <= SB_PC; platform++)
      for (int kind = 0; kind <= SB_E_SHOWLIGHTS; kind++) free(text(&s, platform, kind, 0, 0));
    if (v == 0) {
      yyjson_doc *d = parsed(&s, SB_E_MANIFEST, 0);
      // bytes that are not UTF-8 become U+FFFD one by one; control characters are escaped
      CHECK(!strcmp(yyjson_get_str(yyjson_obj_get(attrs(d), "SongName")),
                    "\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd"),
            "bad UTF-8 in a title: %s", yyjson_get_str(yyjson_obj_get(attrs(d), "SongName")));
      CHECK(!strcmp(yyjson_get_str(yyjson_obj_get(attrs(d), "ArtistName")), "\x01\x02\x1f"), "control characters in a name");
      CHECK(!strcmp(yyjson_get_str(yyjson_obj_get(attrs(d), "Tone_Base")), "\x01\x7f"), "control characters in a tone's key");
      CHECK(yyjson_get_num(yyjson_obj_get(attrs(d), "CentOffset")) == 0 && yyjson_get_num(yyjson_obj_get(attrs(d), "SongAverageTempo")) == 120,
            "not a number is written as 0");
      CHECK(yyjson_arr_size(yyjson_obj_get(attrs(d), "Tones")) == 3, "the tones that are objects are kept");
      yyjson_doc_free(d);
    }
  }
  free(ph), free(it), free(sec), free(ct), free(lv), free(lt);
}

static void test_tones(void) {
  static const char *const raw[] = {
    " \n {\"Name\" : \"n\", \"GearList\":{\"Amp\":{\"Key\":\"Amp_X\",\"KnobValues\":{\"a\":-1.5e+2,\"b\":[true,false,null]}}},\n"
    "  \"Key\"\t: \"My \\\"Tone\\\" \\u00e9\" , \"Volume\":\"-12\"}\r\n",
    "{\"Key\":\"Second\",\"Anything\":{\"at\":[\"all\",{}]}}"};
  sb_arr_info a = {.type = SB_ARR_RHYTHM, .tone = raw, .ntones = 2};
  sb_song_info s = {.key = "Tones", .length = 10, .arr = &a, .narr = 1};
#define STR(o, k) yyjson_get_str(yyjson_obj_get(o, k))
  yyjson_doc *d = parsed(&s, SB_E_MANIFEST, 0), *h;
  yyjson_val *at = attrs(d), *tones = yyjson_obj_get(at, "Tones");
  CHECK(yyjson_arr_size(tones) == 2 && !strcmp(STR(yyjson_arr_get(tones, 1), "Key"), "Second"), "the tones as given");
  CHECK(!strcmp(STR(yyjson_arr_get(tones, 0), "Volume"), "-12") && yyjson_obj_get(yyjson_arr_get(tones, 1), "Anything"), "the tones as given");
  CHECK(!strcmp(STR(at, "Tone_Base"), "My \"Tone\" \xc3\xa9"), "the base tone is the first tone's key: %s", STR(at, "Tone_Base"));
  CHECK(!strcmp(STR(at, "Tone_A"), "") && !strcmp(STR(at, "Tone_D"), "") && !strcmp(STR(at, "Tone_Multiplayer"), ""), "no other tones");
  yyjson_doc_free(d);
  // the text of the first is in the manifest byte for byte, without the whitespace around it
  size_t n;
  char *t = text(&s, SB_MAC, SB_E_MANIFEST, 0, &n);
  CHECK(strstr(t, "\"Tones\":[{\"Name\" : \"n\", \"GearList\"") && strstr(t, "\"Volume\":\"-12\"},{\"Key\":\"Second\""), "the tones' text as it was");
  free(t);
  a.tone_base = "Second", a.tone_a = "Second", a.tone_b = "My", a.tone_c = "c", a.tone_d = "d", a.tone_multiplayer = "m";
  d = parsed(&s, SB_E_MANIFEST, 0), at = attrs(d);
  CHECK(!strcmp(STR(at, "Tone_Base"), "Second") && !strcmp(STR(at, "Tone_A"), "Second") && !strcmp(STR(at, "Tone_B"), "My") &&
          !strcmp(STR(at, "Tone_C"), "c") && !strcmp(STR(at, "Tone_D"), "d") && !strcmp(STR(at, "Tone_Multiplayer"), "m"),
        "the tones named");
  yyjson_doc_free(d);
  h = parsed(&s, SB_E_HSAN, 0);
  CHECK(!yyjson_obj_get(attrs(h), "Tones") && !yyjson_obj_get(attrs(h), "Tone_Base"), "the header has no tones");
  yyjson_doc_free(h);
  // what is no tone object is left out; with none left, the default tone
  static const char *const junk[] = {0, "", "{", "[]", "[{\"Key\":\"InAList\"}]", "{\"Key\":}", "{\"a\":1,}", "{\"a\" \"b\"}", "{} x", "nul", "\"str\"",
                                     "{\"a\":tru}", "{\"a\":\"\\x\"}", "{\"a\":\"\\u12g4\"}", "{\"a\":\"a\nb\"}", "{\"a\":01x}", "{\"a\":1.}", "{\"a\":1e}",
                                     "{\"a\":-}", "{\"a\"}", "{\"open", "7",
                                     "{\"a\":[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[1]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]}"};
  enum { NJ = sizeof junk / sizeof *junk };
  a = (sb_arr_info){.type = SB_ARR_BASS, .tone = junk, .ntones = NJ};
  d = parsed(&s, SB_E_MANIFEST, 0), at = attrs(d), tones = yyjson_obj_get(at, "Tones");
  CHECK(yyjson_arr_size(tones) == 1 && !strcmp(STR(yyjson_arr_get_first(tones), "Key"), "SbBass") && !strcmp(STR(at, "Tone_Base"), "SbBass"),
        "no tone among the junk: the default");
  yyjson_doc_free(d);
  const char *mixed[NJ + 2];
  memcpy(mixed, junk, sizeof junk);
  mixed[NJ] = "{\"NoKey\":1}", mixed[NJ + 1] = raw[1];
  a.tone = mixed, a.ntones = NJ + 2;
  d = parsed(&s, SB_E_MANIFEST, 0), at = attrs(d), tones = yyjson_obj_get(at, "Tones");
  CHECK(yyjson_arr_size(tones) == 2 && !strcmp(STR(yyjson_arr_get(tones, 1), "Key"), "Second") && !strcmp(STR(at, "Tone_Base"), ""),
        "the tones among the junk; the first has no key, so the base tone has no name");
  yyjson_doc_free(d);
#undef STR
}

// What is derived from what is given: one arrangement by hand
static void test_derived(void) {
  sb_man_phrase ph[] = {{"COUNT", 0}, {"riff", 7}, {"END", 0}};
  sb_phrase_iter it[] = {{0, 0.0}, {1, 10.0}, {1, 20.5}, {1, 30.0}, {2, 40.0}};
  sb_man_section sec[] = {{"intro", 10.0}, {"solo", 20.5}, {"Guitar Hero", 30.0}, {"noguitar", 40.0}};
  sb_arr_info a = {.type = SB_ARR_COMBO, .path = SB_PATH_LEAD, .capo = 2, .notes_easy = 10, .notes_medium = 20, .notes_hard = 40,
                   .start_time = 10.0, .tuning = {-2, 0, 0, 0, 0, 0}, .phrase = ph, .nphrases = 3, .iter = it, .niters = 5,
                   .section = sec, .nsections = 4, .alternate = 1};
  sb_song_info s = {.key = "Der", .title = "The Song", .artist = "A Band", .album = "An Album", .length = 50.25, .arr = &a, .narr = 1};
  yyjson_doc *d = parsed(&s, SB_E_MANIFEST, 0), *hd = parsed(&s, SB_E_HSAN, 0);
  yyjson_val *at = attrs(d), *h = attrs(hd), *ap = yyjson_obj_get(at, "ArrangementProperties"), *v;
#define NUM(o, k) yyjson_get_num(yyjson_obj_get(o, k))
#define STR(o, k) yyjson_get_str(yyjson_obj_get(o, k))
  CHECK(!strcmp(STR(at, "SongNameSort"), "Song") && !strcmp(STR(at, "ArtistNameSort"), "Band") && !strcmp(STR(at, "AlbumNameSort"), "Album"), "sort names");
  CHECK(!strcmp(STR(at, "ArrangementName"), "Lead") && NUM(at, "ArrangementType") == 2 && !strcmp(STR(at, "FullName"), "Der_Combo"), "a combo on the lead path");
  CHECK(NUM(ap, "pathLead") == 1 && NUM(ap, "pathRhythm") == 0 && NUM(ap, "routeMask") == 1 && NUM(ap, "represent") == 0 && NUM(ap, "standardTuning") == 0,
        "the path and the tuning");
  CHECK(NUM(h, "RouteMask") == 1 && NUM(h, "Representative") == 0 && NUM(h, "CapoFret") == 2 && NUM(at, "CapoFret") == 2, "the header's own");
  CHECK(NUM(at, "EasyMastery") == 0.25 && NUM(at, "MediumMastery") == 0.5 && NUM(at, "Score_PNV") == 2500 && NUM(at, "Score_MaxNotes") == 40, "mastery");
  CHECK(NUM(at, "MaxPhraseDifficulty") == 7 && NUM(at, "SongOffset") == -10 && NUM(at, "SongLength") == 50.25, "derived numbers");
  CHECK(NUM(yyjson_obj_get(at, "Tuning"), "string0") == -2 && yyjson_obj_size(yyjson_obj_get(at, "Tuning")) == 6, "the tuning");
  v = yyjson_obj_get(at, "Phrases");
  CHECK(NUM(yyjson_arr_get(v, 1), "IterationCount") == 3 && NUM(yyjson_arr_get(v, 2), "IterationCount") == 1, "iteration counts");
  v = yyjson_obj_get(at, "PhraseIterations");
  CHECK(NUM(yyjson_arr_get(v, 2), "EndTime") == 30 && NUM(yyjson_arr_get(v, 4), "EndTime") == 50.25 && !strcmp(STR(yyjson_arr_get(v, 4), "Name"), "END") &&
          NUM(yyjson_arr_get(v, 1), "MaxDifficulty") == 7,
        "phrase iterations");
  v = yyjson_obj_get(at, "Sections");
  CHECK(NUM(yyjson_arr_get(v, 0), "StartPhraseIterationIndex") == 1 && NUM(yyjson_arr_get(v, 0), "EndPhraseIterationIndex") == 1 &&
          NUM(yyjson_arr_get(v, 3), "StartPhraseIterationIndex") == 4 && NUM(yyjson_arr_get(v, 3), "EndTime") == 50.25,
        "sections' phrase iterations");
  CHECK(yyjson_get_bool(yyjson_obj_get(yyjson_arr_get(v, 1), "IsSolo")) && !yyjson_get_bool(yyjson_obj_get(yyjson_arr_get(v, 0), "IsSolo")), "the solo");
  CHECK(!strcmp(STR(yyjson_arr_get(v, 0), "UIName"), "$[34286] Intro [1]") && !strcmp(STR(yyjson_arr_get(v, 3), "UIName"), "$[6091] No Guitar [1]") &&
          !strcmp(STR(yyjson_arr_get(v, 2), "UIName"), "Guitar Hero"),
        "sections' names on screen");
  yyjson_doc_free(d), yyjson_doc_free(hd);
  // two leads, two rhythms and a bonus: the first of a path represents it
  sb_arr_info five[5] = {{.type = SB_ARR_LEAD, .name = "lead"}, {.type = SB_ARR_LEAD, .name = "lead-2"}, {.type = SB_ARR_RHYTHM, .bonus = 1},
                         {.type = SB_ARR_RHYTHM}, {.type = SB_ARR_COMBO}};
  sb_song_info s5 = {.key = "Five", .length = 9, .arr = five, .narr = 5};
  static const int rep5[5] = {1, 0, 0, 1, 0}, bonus5[5] = {0, 0, 1, 0, 0};
  static const char *const full5[5] = {"Five_Lead", "Five_Lead2", "Five_Rhythm", "Five_Rhythm2", "Five_Combo"};
  hd = parsed(&s5, SB_E_HSAN, 0);
  for (uint32_t i = 0; i < 5; i++) {
    char pid[33];
    sb_song_arr_pid(&s5, i, pid);
    d = parsed(&s5, SB_E_MANIFEST, i), at = attrs(d), ap = yyjson_obj_get(at, "ArrangementProperties");
    h = yyjson_obj_get(yyjson_obj_get(yyjson_obj_get(yyjson_doc_get_root(hd), "Entries"), pid), "Attributes");
    CHECK(NUM(ap, "represent") == rep5[i] && NUM(ap, "bonusArr") == bonus5[i] && NUM(h, "Representative") == rep5[i], "arrangement %u represents or not", i);
    CHECK(!strcmp(STR(at, "FullName"), full5[i]) && !strcmp(STR(at, "ArrangementName"), i < 2 ? "Lead" : "Rhythm"), "arrangement %u's name: %s", i, STR(at, "FullName"));
    yyjson_doc_free(d);
  }
  yyjson_doc_free(hd);
  // given names win
  s.title_sort = "T", s.artist_sort = "", a.name = "Bonus1", a.persistent_id = "00112233445566778899aabbccddeeff", a.master_id = 77;
  d = parsed(&s, SB_E_MANIFEST, 0), at = attrs(d);
  CHECK(!strcmp(STR(at, "SongNameSort"), "T") && !strcmp(STR(at, "ArtistNameSort"), "") && NUM(at, "MasterID_RDV") == 77, "given names");
  CHECK(!strcmp(STR(at, "PersistentID"), "00112233445566778899AABBCCDDEEFF") && !strcmp(STR(at, "ManifestUrn"), "urn:database:json-db:der_bonus1") &&
          !strcmp(STR(at, "FullName"), "Der_Bonus1"),
        "a given name and persistent ID");
  yyjson_doc_free(d);
  char path[128];
  CHECK(sb_song_entry_path(&s, SB_MAC, SB_E_SNG, 0, path, sizeof path) > 0 && !strcmp(path, "songs/bin/macos/der_bonus1.sng"), "the Mac's song file: %s", path);
  CHECK(sb_song_entry_path(&s, SB_PC, SB_E_SNG, 0, path, sizeof path) > 0 && !strcmp(path, "songs/bin/generic/der_bonus1.sng"), "the PC's song file: %s", path);
  s.audio_id = 123, s.preview_id = 4000000000u;
  CHECK(sb_song_entry_path(&s, SB_MAC, SB_E_WEM, 0, path, sizeof path) > 0 && !strcmp(path, "audio/mac/123.wem"), "the Mac's audio: %s", path);
  CHECK(sb_song_entry_path(&s, SB_PC, SB_E_WEM_PREVIEW, 0, path, sizeof path) > 0 && !strcmp(path, "audio/windows/4000000000.wem"), "the PC's audio: %s", path);
#undef NUM
#undef STR
}

int main(int argc, char **argv) {
  if (argc == 4 && !strcmp(argv[1], "gen")) return gen(argv[2], argv[3]);
  if (argc == 4 && !strcmp(argv[1], "key")) {
    char k[SB_KEY_MAX + 1];
    sb_song_key(argv[2], argv[3], k);
    puts(k);
    return 0;
  }
  if (argc == 2 && !strcmp(argv[1], "self")) {
    test_keys();
    test_minimal();
    test_refused();
    test_many();
    test_extremes();
    test_tones();
    test_derived();
    printf("ok, %d checks\n", checks);
    return 0;
  }
  fprintf(stderr, "usage: songtexttest self | gen SPEC DIR | key ARTIST TITLE\n");
  return 2;
}
