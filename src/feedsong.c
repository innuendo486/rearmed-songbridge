// See feedsong.h. What each entry is made from:
//   the texts (manifests, song list manifest, entity block, graph, showlights, flat models, AppID): songtext.c, from
//     the package's manifest and what the song files' making counted (sng.h's facts)
//   a song file for each arrangement that has a fretted chart of up to six strings: chart.c and sng.c
//   the audio: the package's full mix, repacked (wem.c); when that cannot be, the first other stem that can
//   the preview: the package's own, repacked; without one, thirty seconds cut from the audio (wemcut.c)
//   the two sound banks: bank.c, with the first bytes of each audio file
//   the album art in three sizes: art.c, from the package's cover or a plain one
// Tones: the game's own tone objects when the package carries them (an arrangement's tones.definitions, or a rig's
// ext.source.definition in rigs.json), put in the manifest as they are; else songtext.c's default tone.
//
// Three depths of work, each done when an entry first needs it: the package opened (album art, preview, audio), the
// charts read and the song files made (every text and song file), an entry's own bytes. A song's vocals are left
// out for now: the game shows no lyrics for it.
#include "feedsong.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <yyjson.h>

#include "art.h"
#include "bank.h"
#include "chart.h"
#include "feedpak.h"
#include "sizecache.h"
#include "sng.h"
#include "songtext.h"
#include "varc.h"
#include "wemcut.h"

#define VERSION 1u  // of what makes the entries: a change that can change a size raises it
#define BLOCK 0x10000u
#define MAX_TONES 4

typedef struct {
  char *name;
  int64_t len;
  uint32_t tail, tag;  // tag: the entry's kind (SB_E_*), and from bit 8 the manifest's arrangement it is of
  uint8_t *data;       // its bytes once made
  int kept;            // data is there for good (not dropped)
} ent;

// An arrangement that is in the archive
typedef struct {
  uint32_t mi;  // its place in the package's manifest
  uint8_t *sng;
  size_t sng_n;
  sb_sng_facts facts;
  sb_man_phrase *phrase;
  sb_phrase_iter *iter;
  sb_man_section *section;
  sb_chord_template *chord;
  sb_level_ids *level;
  int *level_id;
  char *tone[MAX_TONES];  // tone objects as JSON text
  char tone_key[MAX_TONES][64];
  const char *tones[MAX_TONES];
  uint32_t ntones;
} arr;

struct sb_feedsong {
  char *path, *cache_dir;
  int platform;
  sb_books_fn books;
  sb_cache_key ck;
  char key[SB_KEY_MAX + 1], lkey[SB_KEY_MAX + 1];
  ent *e;
  uint32_t n;
  sb_varc *arc;
  pthread_mutex_t mu;  // everything below
  sb_pak *pak;
  int charted;  // the charts were read (narr may still be 0: then there is nothing)
  arr *a;
  uint32_t narr;
  sb_arr_info *ai;
  sb_song_info info;
  uint8_t *wem, *prev;
  size_t wem_n, prev_n;
  int wem_tried, prev_tried;
  char note[600];
};

int sb_feedsong_log;

static int fail(char *err, size_t errlen, const char *fmt, ...) {
  if (errlen) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
  }
  return 0;
}

// A line for the note, while there is room
static void note(sb_feedsong *s, const char *fmt, ...) {
  size_t at = strlen(s->note);
  if (at + 4 >= sizeof s->note) return;
  if (at) at += (size_t)snprintf(s->note + at, sizeof s->note - at, "; ");
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(s->note + at, sizeof s->note - at, fmt, ap);
  va_end(ap);
}

// --- the package and its charts ----------------------------------------------------------------------------------
// The package opened, and the song's key as the texts and the banks spell it (from the artist and the title)
static int pak_open(sb_feedsong *s, char *err, size_t errlen) {
  if (s->pak) return 1;
  if (!(s->pak = sb_pak_open(s->path, err, errlen))) return 0;
  const sb_manifest *m = sb_pak_manifest(s->pak);
  sb_song_key(m->artist, m->title, s->key);
  return 1;
}

// The tone object named `name` (by its Key or Name) among the candidates, as JSON text (malloc'd), its Key in key
static char *tone_find(yyjson_val **cand, size_t ncand, const char *name, char key[64]) {
  for (int pass = 0; pass < 2; pass++)
    for (size_t i = 0; i < ncand; i++) {
      const char *k = yyjson_get_str(yyjson_obj_get(cand[i], "Key")), *nm = yyjson_get_str(yyjson_obj_get(cand[i], "Name"));
      if (!k || !*k || strlen(k) >= 64) continue;
      // first by name; then, for the tone a chart starts with and does not name, the first there is
      if (pass ? name != 0 : !name || (strcasecmp(k, name) && (!nm || strcasecmp(nm, name)))) continue;
      char *text = yyjson_val_write(cand[i], 0, 0);
      if (text) snprintf(key, 64, "%s", k);
      return text;
    }
  return 0;
}

// The tones of an arrangement: the game's tone objects the package has for the names the song file's tones have
static void tones_of(sb_feedsong *s, arr *a, const char *chart_json, size_t chart_n, yyjson_doc *rigs) {
  yyjson_doc *doc = yyjson_read(chart_json, chart_n, YYJSON_READ_ALLOW_COMMENTS | YYJSON_READ_ALLOW_TRAILING_COMMAS);
  yyjson_val *cand[64];
  size_t ncand = 0, idx, max;
  yyjson_val *v, *tones = doc ? yyjson_obj_get(yyjson_doc_get_root(doc), "tones") : 0;
  yyjson_val *defs = yyjson_obj_get(tones, "definitions");
  const char *base_rig = yyjson_get_str(yyjson_obj_get(tones, "base_rig"));
  // a rig the arrangement starts with comes first: it is the one taken when no name matches
  yyjson_val *rl = rigs ? yyjson_obj_get(yyjson_doc_get_root(rigs), "rigs") : 0;
  for (int pass = 0; pass < 2; pass++)
    yyjson_arr_foreach(rl, idx, max, v) {
      const char *id = yyjson_get_str(yyjson_obj_get(v, "id"));
      int first = base_rig && id && !strcmp(base_rig, id);
      yyjson_val *src = yyjson_obj_get(yyjson_obj_get(v, "ext"), "source"), *def = yyjson_obj_get(src, "definition");
      if (first == !pass && yyjson_is_obj(def) && yyjson_obj_get(def, "GearList") && ncand < 64) cand[ncand++] = def;
    }
  yyjson_arr_foreach(defs, idx, max, v)
    if (yyjson_is_obj(v) && yyjson_obj_get(v, "GearList") && ncand < 64) cand[ncand++] = v;
  const sb_sng_facts *f = &a->facts;
  if (!f->ntones) {  // no tone named anywhere: the first the package has
    if ((a->tone[0] = tone_find(cand, ncand, 0, a->tone_key[0]))) a->ntones = 1;
  } else {
    for (uint32_t i = 0; i < f->ntones && i < MAX_TONES; i++) {
      char *t = tone_find(cand, ncand, f->tones[i], a->tone_key[a->ntones]);
      if (!t && !i) t = tone_find(cand, ncand, 0, a->tone_key[a->ntones]);
      if (!t) {
        if (ncand) note(s, "no tone %.40s in the package", f->tones[i]);
        // the song file numbers its tones: one that is missing takes the first's place
        if (a->ntones && (t = strdup(a->tone[0]))) snprintf(a->tone_key[a->ntones], 64, "%s", a->tone_key[0]);
      }
      if (!t) break;
      a->tone[a->ntones++] = t;
    }
  }
  for (uint32_t i = 0; i < a->ntones; i++) a->tones[i] = a->tone[i];
  yyjson_doc_free(doc);
}

static void arr_free(arr *a) {
  free(a->sng), free(a->phrase), free(a->iter), free(a->section), free(a->chord), free(a->level), free(a->level_id);
  for (int i = 0; i < MAX_TONES; i++) free(a->tone[i]);
  sb_sng_facts_free(&a->facts);
  memset(a, 0, sizeof *a);
}

// What the manifest says of arrangement a, from the song file's facts
static int info_fill(sb_feedsong *s, arr *a, sb_arr_info *o) {
  const sb_pak_arr *m = &sb_pak_manifest(s->pak)->arr[a->mi];
  const sb_sng_facts *f = &a->facts;
  const char *nm = m->name ? m->name : m->id;
  memset(o, 0, sizeof *o);
  o->type = f->bass ? SB_ARR_BASS : strcasestr(nm, "rhythm") || strcasestr(m->id, "rhythm") ? SB_ARR_RHYTHM
          : strcasestr(nm, "combo") || strcasestr(m->id, "combo") ? SB_ARR_COMBO : SB_ARR_LEAD;
  o->name = m->id;
  o->bonus = strcasestr(nm, "bonus") != 0;
  o->alternate = strcasestr(nm, "alt") != 0;
  memcpy(o->tuning, f->tuning, sizeof o->tuning);
  o->capo = f->capo, o->cents = f->cents, o->start_time = f->first_beat;
  o->notes_easy = f->notes_easy, o->notes_medium = f->notes_medium, o->notes_hard = f->notes_hard;
  o->max_difficulty = f->max_difficulty;
  uint32_t t = f->techniques;
  o->props = (sb_arr_props){.bends = !!(t & SB_T_BEND), .slides = !!(t & SB_T_SLIDE), .unpitched_slides = !!(t & SB_T_UNPITCHED_SLIDE),
                            .harmonics = !!(t & SB_T_HARMONIC), .pinch_harmonics = !!(t & SB_T_PINCH_HARMONIC),
                            .hopo = !!(t & (SB_T_HAMMER_ON | SB_T_PULL_OFF)), .palm_mutes = !!(t & SB_T_PALM_MUTE),
                            .fret_hand_mutes = !!(t & SB_T_FRET_HAND_MUTE), .tremolo = !!(t & SB_T_TREMOLO),
                            .vibrato = !!(t & SB_T_VIBRATO), .sustain = !!(t & SB_T_SUSTAIN), .tapping = !!(t & SB_T_TAP),
                            .slap_pop = !!(t & (SB_T_SLAP | SB_T_PLUCK)), .double_stops = !!(t & SB_T_DOUBLE_STOP),
                            .power_chords = !!(t & SB_T_POWER_CHORD), .barre_chords = !!(t & SB_T_BARRE_CHORD),
                            .open_chords = !!(t & SB_T_OPEN_CHORD)};
  a->phrase = calloc(f->nphrases + 1, sizeof *a->phrase), a->iter = calloc(f->niters + 1, sizeof *a->iter);
  a->section = calloc(f->nsections + 1, sizeof *a->section), a->chord = calloc(f->ntemplates + 1, sizeof *a->chord);
  a->level = calloc(f->nuses + 1, sizeof *a->level), a->level_id = calloc(f->nuses + 1, sizeof *a->level_id);
  if (!a->phrase || !a->iter || !a->section || !a->chord || !a->level || !a->level_id) return 0;
  for (uint32_t i = 0; i < f->nphrases; i++) a->phrase[i] = (sb_man_phrase){f->phrases[i].name, f->phrases[i].max_difficulty};
  for (uint32_t i = 0; i < f->niters; i++) a->iter[i] = (sb_phrase_iter){f->iters[i].phrase, f->iters[i].t};
  for (uint32_t i = 0; i < f->nsections; i++) a->section[i] = (sb_man_section){f->sections[i].name, f->sections[i].t};
  for (uint32_t i = 0; i < f->ntemplates; i++) {
    const sb_sng_template *c = &f->templates[i];
    a->chord[i] = (sb_chord_template){.id = (int)i, .name = c->display[0] ? c->display : c->name};
    memcpy(a->chord[i].fingers, c->fingers, 6), memcpy(a->chord[i].frets, c->frets, 6);
  }
  // the chords used, by difficulty and section: the facts have one line for each use, in that order
  uint32_t nl = 0;
  for (uint32_t i = 0; i < f->nuses; i++) {
    const sb_sng_use *u = &f->uses[i];
    a->level_id[i] = (int)u->chord;
    if (nl && a->level[nl - 1].difficulty == u->level && a->level[nl - 1].iteration == u->section) a->level[nl - 1].n++;
    else a->level[nl++] = (sb_level_ids){u->level, u->section, &a->level_id[i], 1};
  }
  o->phrase = a->phrase, o->nphrases = f->nphrases, o->iter = a->iter, o->niters = f->niters;
  o->section = a->section, o->nsections = f->nsections, o->chord = a->chord, o->nchords = f->ntemplates;
  o->chords = a->level, o->nchord_levels = nl;
  o->tone = a->tones, o->ntones = a->ntones;
  if (a->ntones) {
    o->tone_base = a->tone_key[0];
    if (f->nchanges) {  // tones that change: the song file's ids 0 to 3 are the manifest's Tone_A to Tone_D
      o->tone_a = a->tone_key[0];
      o->tone_b = a->ntones > 1 ? a->tone_key[1] : 0;
      o->tone_c = a->ntones > 2 ? a->tone_key[2] : 0;
      o->tone_d = a->ntones > 3 ? a->tone_key[3] : 0;
    }
  }
  return 1;
}

// The charts read, the song files made and the song's facts put together. An arrangement that cannot be a song file
// is left out, with a note; 0 when none can.
static int chart_all(sb_feedsong *s, char *err, size_t errlen) {
  if (s->charted) return s->narr ? 1 : fail(err, errlen, "no arrangement that can be played");
  if (!pak_open(s, err, errlen)) return 0;
  const sb_manifest *m = sb_pak_manifest(s->pak);
  s->charted = 1;
  for (uint32_t i = 0; sb_pak_warning(s->pak, i); i++) note(s, "%s", sb_pak_warning(s->pak, i));
  size_t tn = 0, rn = 0;
  char *timeline = m->timeline ? (char *)sb_pak_read(s->pak, m->timeline, &tn) : 0;
  char *rigs_text = (char *)sb_pak_read(s->pak, "rigs.json", &rn);
  yyjson_doc *rigs = rigs_text ? yyjson_read(rigs_text, rn, YYJSON_READ_ALLOW_COMMENTS | YYJSON_READ_ALLOW_TRAILING_COMMAS) : 0;
  s->a = calloc(m->narr, sizeof *s->a), s->ai = calloc(m->narr, sizeof *s->ai);
  for (uint32_t i = 0; s->a && s->ai && i < m->narr && s->narr < SB_MAX_ARR; i++) {
    const sb_pak_arr *pa = &m->arr[i];
    if (pa->drums || !pa->file) {
      if (!pa->drums && !pa->vocals) note(s, "%s has no chart", pa->id);
      continue;
    }
    size_t n = 0;
    char *json = (char *)sb_pak_read(s->pak, pa->file, &n), why[160] = "";
    if (!json) {
      note(s, "%s: %s is not in the package", pa->id, pa->file);
      continue;
    }
    sb_chart *c = sb_chart_parse(json, n, timeline, tn, why, sizeof why);
    arr *a = &s->a[s->narr];
    if (c) {
      // the manifest's tuning, capo and offset are the ones that count; the chart's own stand in for them
      sb_sng_opts o = {.song_length = m->duration, .bass = pa->type ? !strcasecmp(pa->type, "bass") : -1};
      if (pa->strings <= SB_STRINGS) {
        for (int k = 0; k < SB_STRINGS; k++) c->tuning[k] = k < pa->strings ? pa->tuning[k] : 0;
        if (pa->strings > c->strings) c->strings = pa->strings;
      }
      if (pa->capo) c->capo = pa->capo;
      if (pa->cents != 0) c->cents = pa->cents;
      a->sng = sb_sng_build(c, s->platform, &o, &a->facts, &a->sng_n, why, sizeof why);
      if (a->sng && c->repairs.total) note(s, "%s: %s", pa->id, c->repairs.text);
    }
    if (!a->sng) {
      note(s, "%s left out: %s", pa->id, why);
    } else {
      a->mi = i;
      tones_of(s, a, json, n, rigs);
      if (info_fill(s, a, &s->ai[s->narr])) s->narr++;
      else arr_free(a);
    }
    sb_chart_free(c);
    free(json);
  }
  yyjson_doc_free(rigs);
  free(rigs_text), free(timeline);
  if (!s->narr) return fail(err, errlen, "no arrangement that can be played%s%s", s->note[0] ? ": " : "", s->note);
  s->info = (sb_song_info){.title = m->title, .artist = m->artist, .album = m->album, .year = m->year, .length = m->duration,
                           .arr = s->ai, .narr = s->narr};
  s->info.key = s->key;
  s->info.audio_id = sb_bank_wem_id(s->key, 0), s->info.preview_id = sb_bank_wem_id(s->key, 1);
  return 1;
}

// --- the audio ---------------------------------------------------------------------------------------------------
// The package's audio as the game's: the full mix, else the first stem that can be repacked
static int wem_make(sb_feedsong *s, char *err, size_t errlen) {
  if (s->wem) return 1;
  if (s->wem_tried) return fail(err, errlen, "no audio the game can play");
  if (!pak_open(s, err, errlen)) return 0;
  s->wem_tried = 1;
  const sb_books *lib = s->books ? s->books() : 0;
  if (!lib) return fail(err, errlen, "the game's codebook library is not there");
  const sb_manifest *m = sb_pak_manifest(s->pak);
  char why[160] = "no stems";
  for (int pass = 0; pass < 3 && !s->wem; pass++)
    for (uint32_t i = 0; i < m->nstems && !s->wem; i++) {
      const sb_pak_stem *st = &m->stem[i];
      int full = !strcasecmp(st->id, "full"), want = pass == 0 ? full : pass == 1 ? !full && st->on : !full && !st->on;
      if (!want) continue;
      size_t n = 0;
      uint8_t *ogg = sb_pak_read(s->pak, st->file, &n);
      if (!ogg) {
        snprintf(why, sizeof why, "%s is not in the package", st->file);
        continue;
      }
      s->wem = sb_wem_from_ogg(ogg, n, lib, &s->wem_n, why, sizeof why);
      free(ogg);
      if (s->wem && !full) note(s, "the audio is the stem %s", st->id);
    }
  return s->wem ? 1 : fail(err, errlen, "no audio the game can play (%s)", why);
}

static int prev_make(sb_feedsong *s, char *err, size_t errlen) {
  if (s->prev) return 1;
  if (s->prev_tried) return fail(err, errlen, "no preview");
  if (!pak_open(s, err, errlen)) return 0;
  s->prev_tried = 1;
  const sb_manifest *m = sb_pak_manifest(s->pak);
  const sb_books *lib = s->books ? s->books() : 0;
  size_t n = 0;
  uint8_t *ogg = m->preview && lib ? sb_pak_read(s->pak, m->preview, &n) : 0;
  if (ogg) s->prev = sb_wem_from_ogg(ogg, n, lib, &s->prev_n, 0, 0);
  free(ogg);
  if (!s->prev && wem_make(s, err, errlen))  // none of its own that can be repacked: a piece of the song
    s->prev = sb_wem_preview(s->wem, s->wem_n, -1, 0, &s->prev_n, 0, err, errlen);
  return s->prev != 0;
}

// --- an entry's bytes --------------------------------------------------------------------------------------------
// Entry i made (malloc'd, its size in *n), or 0. With the lock held.
static uint8_t *make(sb_feedsong *s, uint32_t kind, uint32_t mi, size_t *n, char *err, size_t errlen) {
  if (kind <= SB_E_SNG) {
    if (!chart_all(s, err, errlen)) return 0;
    uint32_t k = 0;
    while (k < s->narr && s->a[k].mi != mi) k++;
    if (kind >= SB_E_MANIFEST && k == s->narr) return fail(err, errlen, "an arrangement that is not there now"), (uint8_t *)0;
    if (kind == SB_E_SNG) {
      uint8_t *d = malloc(s->a[k].sng_n);
      if (d) memcpy(d, s->a[k].sng, *n = s->a[k].sng_n);
      return d;
    }
    return sb_song_text_alloc(&s->info, s->platform, (int)kind, kind == SB_E_MANIFEST ? k : 0, n);
  }
  if (kind == SB_E_ART64 || kind == SB_E_ART128 || kind == SB_E_ART256) {
    int px = kind == SB_E_ART64 ? 64 : kind == SB_E_ART128 ? 128 : 256;
    if (!pak_open(s, err, errlen)) return 0;
    const sb_manifest *m = sb_pak_manifest(s->pak);
    size_t in = 0;
    uint8_t *img = m->cover ? sb_pak_read(s->pak, m->cover, &in) : 0, *d = malloc(sb_art_size(px));
    if (d && sb_art_dds(img, in, px, d) < 0) free(d), d = 0;
    free(img);
    *n = sb_art_size(px);
    return d;
  }
  int preview = kind == SB_E_BNK_PREVIEW || kind == SB_E_WEM_PREVIEW;
  if (!(preview ? prev_make(s, err, errlen) : wem_make(s, err, errlen))) return 0;
  const uint8_t *w = preview ? s->prev : s->wem;
  size_t wn = preview ? s->prev_n : s->wem_n;
  if (kind == SB_E_WEM || kind == SB_E_WEM_PREVIEW) {
    uint8_t *d = malloc(wn);
    if (d) memcpy(d, w, *n = wn);
    return d;
  }
  return sb_bank_build(s->key, preview, sb_bank_wem_id(s->key, preview), w, sb_bank_prefetch(wn), SB_BANK_VOLUME, n);
}

// The archive's reader of entries (varc.h)
static int entry_read(void *ctx, uint32_t i, void *buf, uint64_t n, int64_t off) {
  sb_feedsong *s = ctx;
  ent *e = &s->e[i];
  pthread_mutex_lock(&s->mu);
  if (!e->data) {
    size_t got = 0;
    char why[200] = "";
    uint8_t *d = make(s, e->tag & 0xff, e->tag >> 8, &got, why, sizeof why);
    if (d && (int64_t)got != e->len) {  // not what was kept of it: the next start works it out again
      snprintf(why, sizeof why, "%zu bytes, %lld were kept", got, (long long)e->len);
      if (s->cache_dir) sb_cache_forget(s->cache_dir, &s->ck);
      free(d);
      d = 0;
    }
    if (!d) fprintf(stderr, "[songbridge] %s: %s cannot be made: %s\n", s->path, e->name, why);
    else if (sb_feedsong_log) fprintf(stderr, "[songbridge] made %s (%zu bytes)\n", e->name, got);
    e->data = d;
  }
  int ok = e->data != 0;
  if (ok) memcpy(buf, e->data + off, n);
  pthread_mutex_unlock(&s->mu);
  return ok;
}

// --- the song ----------------------------------------------------------------------------------------------------
const char *sb_feedsong_key(const sb_feedsong *s) { return s->lkey; }
const char *sb_feedsong_note(const sb_feedsong *s) { return s->note; }
int64_t sb_feedsong_size(const sb_feedsong *s) { return sb_varc_size(s->arc); }
int64_t sb_feedsong_read(sb_feedsong *s, void *buf, uint64_t n, int64_t off) { return sb_varc_read(s->arc, buf, n, off); }

void sb_feedsong_drop(sb_feedsong *s) {
  pthread_mutex_lock(&s->mu);
  for (uint32_t i = 0; i < s->n; i++)
    if (!s->e[i].kept) free(s->e[i].data), s->e[i].data = 0;
  for (uint32_t i = 0; i < s->narr; i++) arr_free(&s->a[i]);
  free(s->a), free(s->ai), free(s->wem), free(s->prev);
  s->a = 0, s->ai = 0, s->narr = 0, s->charted = 0, s->wem = s->prev = 0, s->wem_tried = s->prev_tried = 0;
  memset(&s->info, 0, sizeof s->info);
  sb_pak_close(s->pak);
  s->pak = 0;
  if (s->arc) sb_varc_drop(s->arc);
  pthread_mutex_unlock(&s->mu);
}

void sb_feedsong_free(sb_feedsong *s) {
  if (!s) return;
  sb_feedsong_drop(s);
  sb_varc_free(s->arc);
  for (uint32_t i = 0; i < s->n; i++) free(s->e[i].name), free(s->e[i].data);
  free(s->e), free(s->path), free(s->cache_dir);
  pthread_mutex_destroy(&s->mu);
  free(s);
}

// Where a kind of entry goes in the archive: the small texts first. The game reads an archive's first 64 KiB when
// it starts, so what lies there is kept with the sizes and nothing has to be made for a song that is only listed.
static int rank(int kind) {
  static const int order[SB_E_KINDS] = {[SB_E_APPID] = 0, [SB_E_HSAN] = 1, [SB_E_FLAT_ROOT] = 2, [SB_E_FLAT_SONG] = 3,
                                        [SB_E_XBLOCK] = 4, [SB_E_SHOWLIGHTS] = 5, [SB_E_GRAPH] = 6, [SB_E_MANIFEST] = 7,
                                        [SB_E_ART64] = 8, [SB_E_ART128] = 9, [SB_E_ART256] = 10, [SB_E_BNK_PREVIEW] = 11,
                                        [SB_E_BNK] = 12, [SB_E_SNG] = 13, [SB_E_WEM_PREVIEW] = 14, [SB_E_WEM] = 15};
  return kind >= 0 && kind < SB_E_KINDS ? order[kind] : SB_E_KINDS;
}

// Every entry made once, for its size and its last block's packed size; the ones in the archive's first 64 KiB are
// kept, the rest forgotten again. With the lock not held (nobody else has s yet).
static int first_build(sb_feedsong *s, char *err, size_t errlen) {
  sb_entry list[14 + 2 * SB_MAX_ARR];
  if (!chart_all(s, err, errlen)) return 0;
  uint32_t n = sb_song_entries(&s->info, list, sizeof list / sizeof *list);
  if (!n || !(s->e = calloc(n, sizeof *s->e))) return fail(err, errlen, "out of memory");
  for (uint32_t i = 1; i < n; i++) {  // by rank, the order within one kept
    sb_entry x = list[i];
    uint32_t k = i;
    for (; k && rank(list[k - 1].kind) > rank(x.kind); k--) list[k] = list[k - 1];
    list[k] = x;
  }
  int64_t at = 0;
  for (uint32_t i = 0; i < n; i++) {
    ent *e = &s->e[s->n];
    char path[512], why[200] = "";
    size_t len = 0;
    uint32_t mi = list[i].kind == SB_E_MANIFEST || list[i].kind == SB_E_SNG ? s->a[list[i].arr].mi : 0;
    if (sb_song_entry_path(&s->info, s->platform, list[i].kind, list[i].arr, path, sizeof path) < 0) continue;
    uint8_t *d = make(s, (uint32_t)list[i].kind, mi, &len, why, sizeof why);
    if (!d) return fail(err, errlen, "%s", why[0] ? why : "an entry cannot be made");  // the game expects every one
    size_t last = len % BLOCK;
    int32_t tail = sb_varc_tail(d + len - last, last);
    e->name = strdup(path), e->len = (int64_t)len, e->tail = tail > 0 ? (uint32_t)tail : 0, e->tag = (uint32_t)list[i].kind | mi << 8;
    e->kept = at < (int64_t)BLOCK;
    if (!e->name || tail < 0) {
      free(d), free(e->name);
      e->name = 0;
      return fail(err, errlen, "an entry cannot be stored");
    }
    if (e->kept) e->data = d;
    else free(d);
    at += e->len;
    s->n++;
  }
  return 1;
}

sb_feedsong *sb_feedsong_open(const char *path, int platform, const char *cache_dir, sb_books_fn books, char *err, size_t errlen) {
  struct stat st;
  if (errlen) err[0] = 0;
  sb_feedsong *s = calloc(1, sizeof *s);
  if (!s) return 0;
  pthread_mutex_init(&s->mu, 0);
  s->platform = platform, s->books = books;
  if (!(s->path = strdup(path)) || (cache_dir && !(s->cache_dir = strdup(cache_dir))) || stat(path, &st)) {
    fail(err, errlen, "cannot be read");
    goto bad;
  }
  if (S_ISDIR(st.st_mode)) {  // a folder is known by its manifest's size and date
    char mf[4096];
    if (snprintf(mf, sizeof mf, "%s/manifest.yaml", path) >= (int)sizeof mf || stat(mf, &st)) {
      fail(err, errlen, "no manifest.yaml");
      goto bad;
    }
  }
  s->ck = (sb_cache_key){s->path, st.st_size, st.st_mtimespec.tv_sec, platform, VERSION};
  sb_cache_set set = {0};
  if (s->cache_dir && sb_cache_load(s->cache_dir, &s->ck, &set)) {
    if ((s->e = calloc(set.n, sizeof *s->e))) {
      for (uint32_t i = 0; i < set.n; i++) {
        s->e[i] = (ent){set.e[i].name, set.e[i].len, set.e[i].tail, set.e[i].tag, set.e[i].data, set.e[i].data != 0};
        set.e[i].name = 0, set.e[i].data = 0;
        const char *x = strstr(s->e[i].name, "gamexblocks/nsongs/");  // the key: the entity block's name
        size_t kl = x ? strcspn(x + 19, ".") : 0;
        if ((s->e[i].tag & 0xff) == SB_E_XBLOCK && kl && kl <= SB_KEY_MAX) memcpy(s->lkey, x + 19, kl), s->lkey[kl] = 0;
      }
      s->n = set.n;
    }
    sb_cache_free(&set);
    if (!s->n || !s->lkey[0]) {
      fail(err, errlen, "out of memory");
      goto bad;
    }
    // the key as the texts spell it comes with the charts; the banks need it, and get it then
  } else {
    if (!first_build(s, err, errlen)) goto bad;
    for (size_t i = 0; i <= strlen(s->key); i++) s->lkey[i] = (char)(s->key[i] >= 'A' && s->key[i] <= 'Z' ? s->key[i] + 32 : s->key[i]);
    if (s->cache_dir) {
      sb_cached *ce = calloc(s->n, sizeof *ce);
      for (uint32_t i = 0; ce && i < s->n; i++) ce[i] = (sb_cached){s->e[i].name, s->e[i].len, s->e[i].tail, s->e[i].tag, s->e[i].kept ? s->e[i].data : 0};
      if (ce) sb_cache_store(s->cache_dir, &s->ck, &(sb_cache_set){ce, s->n});
      free(ce);
    }
    // what was made for the sizes is not kept in memory: a song the player never opens costs nothing
    char keep[sizeof s->note];
    memcpy(keep, s->note, sizeof keep);
    sb_feedsong_drop(s);
    memcpy(s->note, keep, sizeof keep);
  }
  sb_ventry *ve = calloc(s->n, sizeof *ve);
  for (uint32_t i = 0; ve && i < s->n; i++) ve[i] = (sb_ventry){s->e[i].name, s->e[i].len, s->e[i].tail};
  s->arc = ve ? sb_varc_build(ve, s->n, entry_read, s) : 0;
  free(ve);
  if (!s->arc) {
    fail(err, errlen, "the archive cannot be put together");
    goto bad;
  }
  return s;
bad:
  s->arc = 0;
  sb_feedsong_free(s);
  return 0;
}
