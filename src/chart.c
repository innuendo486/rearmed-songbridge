// See chart.h. The file is JSON, read with yyjson (comments and trailing commas allowed: the format's hand-edited
// .jsonc form). Feedpaks are written by many tools and by hand, so this takes what it can use and counts the rest:
//
//   dropped    a note without a usable time, string or fret (missing, not a number, negative, a time past a day), or
//              on a string the tuning has not; a chord without a usable time or with no usable note left (not even
//              in its template); a chord's second note on one string
//   skipped    an anchor, hand shape, beat, section, phrase or tone change without a usable time; a hand shape
//              naming a template that is not there; a phrase starting where another does; a phrase's level with a
//              difficulty outside 0 to 35 or given twice; a bend point that is not one; bend points past the 32nd
//   clamped    a fret or slide above 24 (24); an anchor's fret and width (1 to 24); a bend (0 to 12 semitones);
//              capo, cent offset, tuning steps; a phrase's max difficulty (35); a hand shape that ends before it
//              starts (ends where it starts)
//   defaulted  a value of the wrong kind where the format has a default (a list that is not a list is empty)
//   accepted   numbers and booleans written as strings ("12.5", "true"), booleans written as numbers
//   made       a template for a chord that names one that is not there (from the chord's notes); a chord's notes
//              from its template when it lists none; anchors for a difficulty that has notes and none (a new one
//              when a note falls outside the four frets of the last); hand shapes for a difficulty that has chords
//              and none (one for each run of the same chord)
//   sorted     notes, chords, anchors, hand shapes, phrases, levels, beats, sections, tone changes
//   ignored    keys this does not know; a song timeline that is not JSON
// Refused, with the reason: a file that is not JSON or not an object, a tuning of more than six strings, more than
// SB_MAX_EVENTS notes and chords at one difficulty (the song file's own limit), a chart without a usable note or
// chord. Beats, sections and phrases that are missing are not made here: the song file writer makes them (sng.c),
// since they are the song file's and not the chart's.
#include "chart.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yyjson.h"

#define MAX_JSON (64u << 20)
#define MAX_TIME 86400.0
#define MAX_PHRASES 4096u
#define MAX_BEATS 200000u
#define MAX_SECTIONS 1024u
#define MAX_TONES 1024u
#define MAX_TEMPLATES 8192u  // the file's and the ones made
#define MAX_BENDS 32u        // points of one note's curve: what a chord's note has room for in the song file
#define MAX_LIST (1u << 20)  // anchors, hand shapes of one difficulty

typedef struct {
  char *err;
  size_t errlen;
  sb_chart *c;
} rd;

static int fail(rd *r, const char *fmt, ...) {
  if (r->errlen) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->err, r->errlen, fmt, ap);
    va_end(ap);
  }
  return 0;
}

static void fixed(rd *r, int which, uint32_t n) { r->c->repairs.n[which] += n, r->c->repairs.total += n; }

static const char *const repair_names[SB_R_COUNT] = {
    "notes dropped",   "chords dropped", "frets clamped",   "templates made", "items skipped",
    "values defaulted", "lists sorted",  "anchor lists made", "hand shape lists made", "timeline ignored",
};
const char *sb_repair_name(int which) { return which >= 0 && which < SB_R_COUNT ? repair_names[which] : ""; }

// --- values --------------------------------------------------------------------------------------------------------
// v as a finite number: a number, or a string that is one ("12.5")
static int number(yyjson_val *v, double *out) {
  if (yyjson_is_num(v)) *out = yyjson_get_num(v);
  else if (yyjson_is_str(v)) {
    const char *s = yyjson_get_str(v);
    char *end;
    if (!*s || strlen(s) != yyjson_get_len(v)) return 0;
    *out = strtod(s, &end);
    if (end == s || *end) return 0;
  } else return 0;
  return isfinite(*out);
}

// obj[key] as a number, def when it is absent; counted and def when it is something else
static double num(rd *r, yyjson_val *obj, const char *key, double def) {
  yyjson_val *v = yyjson_obj_get(obj, key);
  double d;
  if (!v || yyjson_is_null(v)) return def;
  if (number(v, &d)) return d;
  fixed(r, SB_R_VALUES_DEFAULTED, 1);
  return def;
}

// A whole number brought into [lo, hi]
static int integer(rd *r, yyjson_val *obj, const char *key, int def, int lo, int hi) {
  double d = rint(num(r, obj, key, def));
  return d < lo ? lo : d > hi ? hi : (int)d;
}

// obj[key] as seconds from the song's start: 0 when there is no such time (absent, not a number, negative, past a day)
static int seconds(yyjson_val *obj, const char *key, double *out) {
  return number(yyjson_obj_get(obj, key), out) && *out >= 0 && *out <= MAX_TIME;
}

// true, false, a number (not 0: true) or a string of either, as writers in the wild have it
static int flag(rd *r, yyjson_val *obj, const char *key) {
  yyjson_val *v = yyjson_obj_get(obj, key);
  double d;
  if (!v || yyjson_is_null(v)) return 0;
  if (yyjson_is_bool(v)) return yyjson_get_bool(v);
  if (yyjson_is_str(v) && (!strcmp(yyjson_get_str(v), "true") || !strcmp(yyjson_get_str(v), "false"))) return yyjson_get_str(v)[0] == 't';
  if (number(v, &d)) return d != 0;
  fixed(r, SB_R_VALUES_DEFAULTED, 1);
  return 0;
}

// obj[key] into dst (cap bytes), cut at a whole UTF-8 character; "" when absent or not a string
static void text(rd *r, yyjson_val *obj, const char *key, char *dst, size_t cap) {
  yyjson_val *v = yyjson_obj_get(obj, key);
  dst[0] = 0;
  if (!v || yyjson_is_null(v)) return;
  if (!yyjson_is_str(v)) {
    fixed(r, SB_R_VALUES_DEFAULTED, 1);
    return;
  }
  const char *s = yyjson_get_str(v);
  size_t n = strlen(s);  // to the first 0 byte, should the string hold one
  if (n >= cap) {
    n = cap - 1;
    while (n && ((unsigned char)s[n] & 0xc0) == 0x80) n--;
  }
  memcpy(dst, s, n);
  dst[n] = 0;
}

// obj[key] as a list, 0 (an empty one) when it is absent or something else
static yyjson_val *list(rd *r, yyjson_val *obj, const char *key) {
  yyjson_val *v = yyjson_obj_get(obj, key);
  if (!v || yyjson_is_null(v) || yyjson_is_arr(v)) return yyjson_is_arr(v) ? v : 0;
  fixed(r, SB_R_VALUES_DEFAULTED, 1);
  return 0;
}

static void *alloc(rd *r, size_t n, size_t size) {
  void *p = calloc(n ? n : 1, size);
  if (!p) fail(r, "out of memory");
  return p;
}

// --- notes ---------------------------------------------------------------------------------------------------------
typedef struct {
  sb_bend *d;
  uint32_t n, cap;
} bendv;

static int bends_read(rd *r, yyjson_val *o, sb_note *n, bendv *bv) {
  yyjson_val *a = list(r, o, "bnv"), *p;
  size_t i, max;
  float last = 0;
  n->bend0 = bv->n;
  yyjson_arr_foreach(a, i, max, p) {
    double t, v;
    if (n->nbends == MAX_BENDS || !seconds(p, "t", &t) || !number(yyjson_obj_get(p, "v"), &v)) {
      fixed(r, SB_R_ITEMS_SKIPPED, n->nbends == MAX_BENDS ? (uint32_t)(max - i) : 1);
      if (n->nbends == MAX_BENDS) break;  // a longer curve keeps its start
      continue;
    }
    if (bv->n == bv->cap) {
      uint32_t cap = bv->cap ? bv->cap * 2 : 256;
      sb_bend *d = realloc(bv->d, cap * sizeof *d);
      if (!d) return fail(r, "out of memory");
      bv->d = d, bv->cap = cap;
    }
    sb_bend *b = &bv->d[bv->n++];
    b->t = (float)t < last ? last : (float)t;  // in order
    b->v = (float)(v < 0 ? 0 : v > 12 ? 12 : v);
    last = b->t;
    n->nbends++;
  }
  return 1;
}

static const struct {
  const char *key;
  uint32_t bit;
} note_flags[] = {
    {"ho", SB_N_HAMMER},   {"po", SB_N_PULL},    {"hm", SB_N_HARMONIC}, {"hp", SB_N_PINCH}, {"pm", SB_N_PALM_MUTE},
    {"mt", SB_N_MUTE},     {"vb", SB_N_VIBRATO}, {"tr", SB_N_TREMOLO},  {"ac", SB_N_ACCENT}, {"tp", SB_N_TAP},
    {"ln", SB_N_LINK},     {"fhm", SB_N_FRET_MUTE}, {"plk", SB_N_PLUCK}, {"slp", SB_N_SLAP}, {"ig", SB_N_IGNORE},
};

// A note, or a chord's note (chord: its time is the chord's, t). 1 read, 0 not usable, -1 out of memory.
static int note_read(rd *r, yyjson_val *o, sb_note *n, int chord, double t, bendv *bv) {
  double s, f, d;
  memset(n, 0, sizeof *n);
  n->t = t;
  if (!yyjson_is_obj(o) || (!chord && !seconds(o, "t", &n->t))) return 0;
  if (!number(yyjson_obj_get(o, "s"), &s) || !number(yyjson_obj_get(o, "f"), &f)) return 0;
  s = rint(s), f = rint(f);
  if (s < 0 || s >= r->c->strings || f < 0) return 0;
  if (f > SB_MAX_FRET) f = SB_MAX_FRET, fixed(r, SB_R_FRETS_CLAMPED, 1);
  n->s = (int8_t)s, n->f = (int8_t)f;
  d = num(r, o, "sus", 0);
  n->sus = (float)(d < 0 ? 0 : d > MAX_TIME ? MAX_TIME : d);
  for (int k = 0; k < 2; k++) {
    d = rint(num(r, o, k ? "slu" : "sl", -1));
    if (d > SB_MAX_FRET) d = SB_MAX_FRET, fixed(r, SB_R_FRETS_CLAMPED, 1);
    *(k ? &n->slu : &n->sl) = (int8_t)(d < 0 ? -1 : d);
  }
  d = num(r, o, "bn", 0);
  n->bn = (float)(d < 0 ? 0 : d > 12 ? 12 : d);
  int bt = integer(r, o, "bt", 0, -1, 5);
  n->bt = (int8_t)(bt < 0 || bt > 4 ? 0 : bt);
  n->rh = (int8_t)integer(r, o, "rh", -1, -1, 9);
  n->pkd = (int8_t)integer(r, o, "pkd", -1, -1, 1);
  n->fg = (int8_t)integer(r, o, "fg", -1, -1, 4);
  for (size_t i = 0; i < sizeof note_flags / sizeof *note_flags; i++)
    if (flag(r, o, note_flags[i].key)) n->flags |= note_flags[i].bit;
  return bends_read(r, o, n, bv) ? 1 : -1;
}

static int by_note(const void *a, const void *b) {
  const sb_note *x = a, *y = b;
  return x->t < y->t ? -1 : x->t > y->t ? 1 : x->s - y->s;
}
static int by_string(const void *a, const void *b) { return ((const sb_note *)a)->s - ((const sb_note *)b)->s; }
static int by_chord(const void *a, const void *b) {
  const sb_chord *x = a, *y = b;
  return x->t < y->t ? -1 : x->t > y->t ? 1 : x->note0 < y->note0 ? -1 : x->note0 > y->note0;
}
static int by_anchor(const void *a, const void *b) {
  const sb_anchor *x = a, *y = b;
  return x->t < y->t ? -1 : x->t > y->t;
}
static int by_shape(const void *a, const void *b) {
  const sb_shape *x = a, *y = b;
  return x->t0 < y->t0 ? -1 : x->t0 > y->t0 ? 1 : x->t1 < y->t1 ? -1 : x->t1 > y->t1;
}

// Sorts a list by its times (each record starts with its time, a double) and counts it when it was not in order
static void sort(rd *r, void *d, uint32_t n, size_t size, int (*cmp)(const void *, const void *)) {
  for (uint32_t i = 1; i < n; i++) {
    double x, y;
    memcpy(&x, (uint8_t *)d + (i - 1) * size, 8), memcpy(&y, (uint8_t *)d + i * size, 8);
    if (y < x) {
      fixed(r, SB_R_LISTS_SORTED, 1);
      break;
    }
  }
  qsort(d, n, size, cmp);
}

static void part_free(sb_part *p) {
  free(p->notes), free(p->chords), free(p->cnotes), free(p->bends), free(p->anchors), free(p->shapes);
  memset(p, 0, sizeof *p);
}

// The template of a chord that named none the chart has: its notes' frets, no fingers, no name. -1: no room.
static int32_t template_make(rd *r, const sb_note *n, uint32_t count, uint32_t *made_from) {
  sb_chart *c = r->c;
  sb_template t;
  memset(&t, 0, sizeof t);
  memset(t.frets, -1, sizeof t.frets), memset(t.fingers, -1, sizeof t.fingers);
  for (uint32_t i = 0; i < count; i++) t.frets[n[i].s] = n[i].f;
  if (*made_from == UINT32_MAX) *made_from = c->ntemplates;
  for (uint32_t i = *made_from; i < c->ntemplates; i++)
    if (!memcmp(c->templates[i].frets, t.frets, sizeof t.frets) && !c->templates[i].name[0]) return (int32_t)i;
  if (c->ntemplates == MAX_TEMPLATES) return -1;
  c->templates[c->ntemplates] = t;  // room for MAX_TEMPLATES was made when the file's were read
  fixed(r, SB_R_TEMPLATES_MADE, 1);
  return (int32_t)c->ntemplates++;
}

// The lowest and highest fretted note of event (note or chord) number i in time order: 0 when it has none
typedef struct {
  double t;
  int lo, hi;
} span;
static int by_span(const void *a, const void *b) {
  const span *x = a, *y = b;
  return x->t < y->t ? -1 : x->t > y->t;
}

// Anchors for a part that has notes and none: the hand stays while the notes are within its four frets
static int anchors_make(rd *r, sb_part *p) {
  uint32_t n = p->nnotes + p->nchords, k = 0;
  span *e = alloc(r, n, sizeof *e);
  sb_anchor *a = alloc(r, n, sizeof *a);
  if (!e || !a) {
    free(e), free(a);
    return 0;
  }
  free(p->anchors), p->anchors = a;
  for (uint32_t i = 0; i < p->nnotes; i++) e[k++] = (span){p->notes[i].t, p->notes[i].f, p->notes[i].f};
  for (uint32_t i = 0; i < p->nchords; i++) {
    span x = {p->chords[i].t, 0, 0};
    for (uint32_t j = 0; j < p->chords[i].nnotes; j++) {
      int f = p->cnotes[p->chords[i].note0 + j].f;
      if (f > 0 && (!x.lo || f < x.lo)) x.lo = f;
      if (f > x.hi) x.hi = f;
    }
    e[k++] = x;
  }
  qsort(e, n, sizeof *e, by_span);
  int at = 0, width = 4;
  for (uint32_t i = 0; i < n; i++) {
    if (!e[i].hi && at) continue;  // open strings: the hand stays
    int lo = e[i].lo ? e[i].lo : 1, hi = e[i].hi ? e[i].hi : 1;
    if (at && lo >= at && hi < at + width) continue;
    if (p->nanchors && a[p->nanchors - 1].t == e[i].t) continue;
    at = lo > 21 ? 21 : lo, width = hi - at + 1 > 4 ? hi - at + 1 : 4;
    a[p->nanchors++] = (sb_anchor){e[i].t, (int8_t)at, (int8_t)width};
  }
  free(e);
  fixed(r, SB_R_ANCHORS_MADE, 1);
  return 1;
}

// Hand shapes for a part that has chords and none: one from a chord to the last repeat of it that no other note or
// chord comes before, held for the chord's sustain or to the next thing played, a quarter second at least
static int shapes_make(rd *r, sb_part *p) {
  sb_shape *h = alloc(r, p->nchords, sizeof *h);
  if (!h) return 0;
  free(p->shapes), p->shapes = h, p->nshapes = 0;
  uint32_t ni = 0;
  for (uint32_t i = 0; i < p->nchords;) {
    const sb_chord *ch = &p->chords[i];
    uint32_t j = i;
    while (ni < p->nnotes && p->notes[ni].t <= ch->t) ni++;
    double next = ni < p->nnotes ? p->notes[ni].t : MAX_TIME + 1;  // the next single note
    while (j + 1 < p->nchords && p->chords[j + 1].id == ch->id && p->chords[j + 1].t < next) j++;
    double end = p->chords[j].t, sus = 0, stop = j + 1 < p->nchords && p->chords[j + 1].t < next ? p->chords[j + 1].t : next;
    for (uint32_t k = 0; k < p->chords[j].nnotes; k++)
      if (p->cnotes[p->chords[j].note0 + k].sus > sus) sus = p->cnotes[p->chords[j].note0 + k].sus;
    end += sus > 0.25 ? sus : 0.25;
    if (end > stop && stop > p->chords[j].t) end = stop;
    h[p->nshapes++] = (sb_shape){ch->t, end, ch->id, r->c->templates[ch->id].arp};
    i = j + 1;
  }
  fixed(r, SB_R_SHAPES_MADE, 1);
  return 1;
}

// Anchors and hand shapes for the parts of one difficulty (0: the plain lists) when none of them has any: one
// phrase's stretch may well have none, its hand staying where the phrase before left it
static int parts_complete(rd *r, int difficulty, int phrases) {
  sb_chart *c = r->c;
  uint32_t anchors = 0, shapes = 0;
  for (int pass = 0; pass < 2; pass++) {  // count, then make
    for (uint32_t k = 0; k < (phrases ? c->nphrases : 1); k++) {
      for (uint32_t l = 0; l < (phrases ? c->phrases[k].nlevels : 1); l++) {
        sb_part *x = phrases ? &c->phrases[k].levels[l] : &c->flat;
        if (x->difficulty != difficulty) continue;
        if (!pass) {
          anchors += x->nanchors, shapes += x->nshapes;
          continue;
        }
        if (!anchors && x->nnotes + x->nchords && !anchors_make(r, x)) return 0;
        if (!shapes && x->nchords && !shapes_make(r, x)) return 0;
      }
    }
  }
  return 1;
}

// The notes, chords, anchors and hand shapes of obj (an arrangement, or one difficulty of a phrase)
static int part_read(rd *r, yyjson_val *obj, sb_part *p) {
  sb_chart *c = r->c;
  yyjson_val *notes = list(r, obj, "notes"), *chords = list(r, obj, "chords"), *anchors = list(r, obj, "anchors");
  yyjson_val *shapes = list(r, obj, "handshapes"), *o;
  size_t i, max, nn = yyjson_arr_size(notes), nc = yyjson_arr_size(chords);
  uint32_t made_from = UINT32_MAX;
  if (nn + nc > SB_MAX_EVENTS)
    return fail(r, "%zu notes and chords at one difficulty: more than the %u a song file holds", nn + nc, SB_MAX_EVENTS);
  if (yyjson_arr_size(anchors) > MAX_LIST) anchors = 0, fixed(r, SB_R_ITEMS_SKIPPED, 1);  // made anew below
  if (yyjson_arr_size(shapes) > MAX_LIST) shapes = 0, fixed(r, SB_R_ITEMS_SKIPPED, 1);
  bendv bv = {0};
  int ok = 0, got;
  if (!(p->notes = alloc(r, nn, sizeof *p->notes)) || !(p->chords = alloc(r, nc, sizeof *p->chords)) ||
      !(p->cnotes = alloc(r, nc * SB_STRINGS, sizeof *p->cnotes)) ||
      !(p->anchors = alloc(r, yyjson_arr_size(anchors), sizeof *p->anchors)) ||
      !(p->shapes = alloc(r, yyjson_arr_size(shapes), sizeof *p->shapes)))
    goto out;
  yyjson_arr_foreach(notes, i, max, o) {
    if ((got = note_read(r, o, &p->notes[p->nnotes], 0, 0, &bv)) < 0) goto out;
    if (got) p->nnotes++;
    else fixed(r, SB_R_NOTES_DROPPED, 1);
  }
  yyjson_arr_foreach(chords, i, max, o) {
    sb_chord *ch = &p->chords[p->nchords];
    yyjson_val *cn, *n;
    size_t j, jmax;
    memset(ch, 0, sizeof *ch);
    if (!yyjson_is_obj(o) || !seconds(o, "t", &ch->t)) {
      fixed(r, SB_R_CHORDS_DROPPED, 1);
      continue;
    }
    int id = integer(r, o, "id", -1, -1, (int)MAX_TEMPLATES), known = id >= 0 && (uint32_t)id < c->ntemplates;
    ch->hd = (uint8_t)flag(r, o, "hd"), ch->note0 = p->ncnotes;
    unsigned seen = 0;
    cn = list(r, o, "notes");
    yyjson_arr_foreach(cn, j, jmax, n) {
      sb_note x;
      if ((got = note_read(r, n, &x, 1, ch->t, &bv)) < 0) goto out;
      if (!got || seen & 1u << x.s) {  // six strings, so six notes at most get past this
        fixed(r, SB_R_NOTES_DROPPED, 1);
        bv.n = got ? x.bend0 : bv.n;
        continue;
      }
      seen |= 1u << x.s, p->cnotes[p->ncnotes++] = x, ch->nnotes++;
    }
    if (!ch->nnotes && known) {  // the chord as its template has it, as the format's first reader took it
      const sb_template *t = &c->templates[id];
      for (int s = 0; s < c->strings; s++) {
        if (t->frets[s] < 0) continue;
        sb_note *x = &p->cnotes[p->ncnotes++];
        memset(x, 0, sizeof *x);
        x->t = ch->t, x->s = (int8_t)s, x->f = t->frets[s], x->sl = x->slu = x->rh = x->pkd = x->fg = -1;
        ch->nnotes++;
      }
    }
    qsort(&p->cnotes[ch->note0], ch->nnotes, sizeof *p->cnotes, by_string);
    if (ch->nnotes && !known) id = template_make(r, &p->cnotes[ch->note0], ch->nnotes, &made_from);
    if (!ch->nnotes || id < 0) {
      p->ncnotes = ch->note0;
      fixed(r, SB_R_CHORDS_DROPPED, 1);
      continue;
    }
    ch->id = (uint32_t)id;
    p->nchords++;
  }
  yyjson_arr_foreach(anchors, i, max, o) {
    sb_anchor *a = &p->anchors[p->nanchors];
    if (!seconds(o, "time", &a->t)) {
      fixed(r, SB_R_ITEMS_SKIPPED, 1);
      continue;
    }
    a->fret = (int8_t)integer(r, o, "fret", 1, 1, SB_MAX_FRET), a->width = (int8_t)integer(r, o, "width", 4, 1, SB_MAX_FRET);
    p->nanchors++;
  }
  yyjson_arr_foreach(shapes, i, max, o) {
    sb_shape *h = &p->shapes[p->nshapes];
    int id = yyjson_is_obj(o) ? integer(r, o, "chord_id", 0, -1, (int)MAX_TEMPLATES) : -1;
    if (id < 0 || (uint32_t)id >= c->ntemplates || !seconds(o, "start_time", &h->t0)) {
      fixed(r, SB_R_ITEMS_SKIPPED, 1);
      continue;
    }
    if (!seconds(o, "end_time", &h->t1) || h->t1 < h->t0) h->t1 = h->t0;
    h->id = (uint32_t)id, h->arp = (uint8_t)(flag(r, o, "arp") || c->templates[id].arp);
    p->nshapes++;
  }
  sort(r, p->notes, p->nnotes, sizeof *p->notes, by_note);
  sort(r, p->chords, p->nchords, sizeof *p->chords, by_chord);
  sort(r, p->anchors, p->nanchors, sizeof *p->anchors, by_anchor);
  sort(r, p->shapes, p->nshapes, sizeof *p->shapes, by_shape);
  ok = 1;
out:
  p->bends = bv.d, p->nbends = bv.n;
  return ok;
}

// --- the rest of an arrangement ------------------------------------------------------------------------------------
static int templates_read(rd *r, yyjson_val *root) {
  sb_chart *c = r->c;
  yyjson_val *a = list(r, root, "templates"), *o;
  size_t i, max;
  if (!(c->templates = alloc(r, MAX_TEMPLATES, sizeof *c->templates))) return 0;
  yyjson_arr_foreach(a, i, max, o) {
    if (c->ntemplates == MAX_TEMPLATES / 2) {  // the rest is room for the ones made
      fixed(r, SB_R_ITEMS_SKIPPED, (uint32_t)(max - i));
      break;
    }
    // a template that is not one stays as an empty one: the chords count the list's places
    sb_template *t = &c->templates[c->ntemplates++];
    memset(t->frets, -1, sizeof t->frets), memset(t->fingers, -1, sizeof t->fingers);
    if (!yyjson_is_obj(o)) {
      fixed(r, SB_R_VALUES_DEFAULTED, 1);
      continue;
    }
    text(r, o, "name", t->name, sizeof t->name), text(r, o, "displayName", t->display, sizeof t->display);
    if (!t->display[0]) memcpy(t->display, t->name, sizeof t->display);
    t->arp = (uint8_t)(flag(r, o, "arp") || strstr(t->display, "-arp"));  // how the game's own files mark one
    for (int k = 0; k < 2; k++) {
      int8_t *dst = k ? t->fingers : t->frets;
      yyjson_val *l = list(r, o, k ? "fingers" : "frets"), *v;  // (the loop below names its list more than once)
      size_t j, jmax;
      yyjson_arr_foreach(l, j, jmax, v) {
        double d;
        if (j >= (size_t)c->strings) break;  // strings the tuning has not
        if (!number(v, &d)) {
          fixed(r, SB_R_VALUES_DEFAULTED, 1);
          continue;
        }
        d = rint(d);
        if (d < 0) continue;
        if (!k && d > SB_MAX_FRET) d = SB_MAX_FRET, fixed(r, SB_R_FRETS_CLAMPED, 1);
        dst[j] = (int8_t)(d > 4 && k ? -1 : d);
      }
    }
  }
  return 1;
}

static int by_level(const void *a, const void *b) { return ((const sb_part *)a)->difficulty - ((const sb_part *)b)->difficulty; }
static int by_phrase(const void *a, const void *b) {
  const sb_phrase *x = a, *y = b;
  return x->t0 < y->t0 ? -1 : x->t0 > y->t0;
}

static void phrase_free(sb_phrase *p) {
  for (uint32_t l = 0; l < p->nlevels; l++) part_free(&p->levels[l]);
  free(p->levels);
  memset(p, 0, sizeof *p);
}

static int phrases_read(rd *r, yyjson_val *root) {
  sb_chart *c = r->c;
  yyjson_val *a = list(r, root, "phrases"), *o, *levels, *lv;
  size_t i, max, j, jmax;
  if (yyjson_arr_size(a) > MAX_PHRASES) a = 0, fixed(r, SB_R_ITEMS_SKIPPED, 1);  // not a ladder anyone wrote: the plain lists
  if (!(c->phrases = alloc(r, yyjson_arr_size(a), sizeof *c->phrases))) return 0;
  uint64_t events = 0;
  yyjson_arr_foreach(a, i, max, o) {
    sb_phrase *p = &c->phrases[c->nphrases];
    memset(p, 0, sizeof *p);
    if (!seconds(o, "start_time", &p->t0)) {
      fixed(r, SB_R_ITEMS_SKIPPED, 1);
      continue;
    }
    if (!seconds(o, "end_time", &p->t1) || p->t1 < p->t0) p->t1 = p->t0;
    p->max_difficulty = integer(r, o, "max_difficulty", 0, 0, SB_MAX_DIFFICULTY);
    levels = list(r, o, "levels");
    if (!(p->levels = alloc(r, yyjson_arr_size(levels) < SB_MAX_DIFFICULTY + 1 ? yyjson_arr_size(levels) : SB_MAX_DIFFICULTY + 1,
                            sizeof *p->levels)))
      return 0;
    c->nphrases++;  // from here sb_chart_free frees its levels
    uint64_t seen = 0;
    yyjson_arr_foreach(levels, j, jmax, lv) {
      yyjson_val *dv = yyjson_obj_get(lv, "difficulty");
      double d = 0;
      int usable = yyjson_is_obj(lv) && (!dv || number(dv, &d));
      d = rint(d);
      if (!usable || d < 0 || d > SB_MAX_DIFFICULTY || seen >> (int)d & 1) {
        fixed(r, SB_R_ITEMS_SKIPPED, 1);
        continue;
      }
      sb_part *l = &p->levels[p->nlevels++];
      l->difficulty = (int)d, seen |= 1ull << (int)d;
      if (!part_read(r, lv, l)) return 0;
      events += l->nnotes + l->nchords;
    }
    qsort(p->levels, p->nlevels, sizeof *p->levels, by_level);
  }
  sort(r, c->phrases, c->nphrases, sizeof *c->phrases, by_phrase);
  uint32_t keep = 0;
  for (uint32_t k = 0; k < c->nphrases; k++) {
    // as the song file has them (32-bit floats), two phrases may not start together: the first stays
    if (keep && (float)c->phrases[k].t0 <= (float)c->phrases[keep - 1].t0) {
      phrase_free(&c->phrases[k]);
      fixed(r, SB_R_ITEMS_SKIPPED, 1);
    } else {
      sb_phrase moved = c->phrases[k];
      memset(&c->phrases[k], 0, sizeof moved);
      c->phrases[keep++] = moved;
    }
  }
  c->nphrases = keep;
  for (int d = 0; d <= SB_MAX_DIFFICULTY; d++)
    if (!parts_complete(r, d, 1)) return 0;
  if (!events) {  // phrases that hold nothing say nothing: the chart is its plain lists
    for (uint32_t k = 0; k < c->nphrases; k++) phrase_free(&c->phrases[k]);
    c->nphrases = 0;
  }
  return 1;
}

static int by_beat(const void *a, const void *b) {
  const sb_beat *x = a, *y = b;
  return x->t < y->t ? -1 : x->t > y->t;
}
static int by_section(const void *a, const void *b) {
  const sb_section *x = a, *y = b;
  return x->t < y->t ? -1 : x->t > y->t;
}
static int by_tone(const void *a, const void *b) {
  const sb_tone *x = a, *y = b;
  return x->t < y->t ? -1 : x->t > y->t;
}

// obj's beats and sections, when it has them, in place of the chart's
static int timeline_read(rd *r, yyjson_val *obj) {
  sb_chart *c = r->c;
  yyjson_val *a, *o;
  size_t i, max;
  if ((a = list(r, obj, "beats"))) {
    size_t n = yyjson_arr_size(a) < MAX_BEATS ? yyjson_arr_size(a) : MAX_BEATS;
    sb_beat *b = alloc(r, n, sizeof *b);
    if (!b) return 0;
    free(c->beats), c->beats = b, c->nbeats = 0;
    yyjson_arr_foreach(a, i, max, o) {
      if (c->nbeats == n || !seconds(o, "time", &b[c->nbeats].t)) {
        fixed(r, SB_R_ITEMS_SKIPPED, 1);
        continue;
      }
      b[c->nbeats++].measure = integer(r, o, "measure", -1, -1, 65535);
    }
    sort(r, b, c->nbeats, sizeof *b, by_beat);
    uint32_t keep = 0;
    for (uint32_t k = 0; k < c->nbeats; k++)
      if (!keep || (float)b[k].t > (float)b[keep - 1].t) b[keep++] = b[k];
    fixed(r, SB_R_ITEMS_SKIPPED, c->nbeats - keep);
    c->nbeats = keep;
  }
  if ((a = list(r, obj, "sections"))) {
    size_t n = yyjson_arr_size(a) < MAX_SECTIONS ? yyjson_arr_size(a) : MAX_SECTIONS;
    sb_section *s = alloc(r, n, sizeof *s);
    if (!s) return 0;
    free(c->sections), c->sections = s, c->nsections = 0;
    yyjson_arr_foreach(a, i, max, o) {
      if (c->nsections == n || !seconds(o, "time", &s[c->nsections].t)) {
        fixed(r, SB_R_ITEMS_SKIPPED, 1);
        continue;
      }
      text(r, o, "name", s[c->nsections].name, sizeof s->name);
      if (!s[c->nsections].name[0]) snprintf(s[c->nsections].name, sizeof s->name, "riff");
      s[c->nsections++].number = integer(r, o, "number", 1, 0, 9999);
    }
    sort(r, s, c->nsections, sizeof *s, by_section);
    uint32_t keep = 0;
    for (uint32_t k = 0; k < c->nsections; k++)
      if (!keep || (float)s[k].t > (float)s[keep - 1].t) s[keep++] = s[k];
    fixed(r, SB_R_ITEMS_SKIPPED, c->nsections - keep);
    c->nsections = keep;
  }
  return 1;
}

// The tone names and the times they change at. What the tones are (gear, rigs) is another reader's business.
static int tones_read(rd *r, yyjson_val *root) {
  sb_chart *c = r->c;
  yyjson_val *t = yyjson_obj_get(root, "tones"), *a, *o;
  size_t i, max;
  if (!yyjson_is_obj(t)) return 1;
  if (yyjson_is_str(yyjson_obj_get(t, "base"))) text(r, t, "base", c->tone_base, sizeof c->tone_base);
  a = yyjson_obj_get(t, "changes");
  if (!yyjson_is_arr(a)) return 1;
  size_t n = yyjson_arr_size(a) < MAX_TONES ? yyjson_arr_size(a) : MAX_TONES;
  if (!(c->tones = alloc(r, n, sizeof *c->tones))) return 0;
  yyjson_arr_foreach(a, i, max, o) {
    sb_tone *x = &c->tones[c->ntones];
    yyjson_val *name = yyjson_obj_get(o, "name");
    if (c->ntones == n || !seconds(o, "t", &x->t) || !yyjson_is_str(name) || !*yyjson_get_str(name)) {
      fixed(r, SB_R_ITEMS_SKIPPED, 1);
      continue;
    }
    text(r, o, "name", x->name, sizeof x->name);
    c->ntones++;
  }
  sort(r, c->tones, c->ntones, sizeof *c->tones, by_tone);
  return 1;
}

static int chart_read(rd *r, yyjson_val *root, yyjson_val *timeline) {
  sb_chart *c = r->c;
  yyjson_val *a, *v;
  size_t i, max;
  if (!yyjson_is_obj(root)) return fail(r, "not an arrangement: the file is not a JSON object");
  text(r, root, "name", c->name, sizeof c->name);
  a = list(r, root, "tuning");
  if (yyjson_arr_size(a) > SB_STRINGS) return fail(r, "%zu strings: this takes arrangements of up to six", yyjson_arr_size(a));
  c->strings = (int)yyjson_arr_size(a);
  yyjson_arr_foreach(a, i, max, v) {
    double d = 0;
    if (!number(v, &d)) d = 0, fixed(r, SB_R_VALUES_DEFAULTED, 1);
    d = rint(d);
    c->tuning[i] = (int)(d < -24 ? -24 : d > 24 ? 24 : d);
  }
  if (!c->strings) c->strings = SB_STRINGS;
  c->capo = integer(r, root, "capo", 0, 0, SB_MAX_FRET);
  double cents = num(r, root, "centOffset", 0);
  c->cents = cents < -1200 ? -1200 : cents > 1200 ? 1200 : cents;
  if (!templates_read(r, root) || !part_read(r, root, &c->flat) || !parts_complete(r, 0, 0) || !phrases_read(r, root) ||
      !timeline_read(r, root) || !tones_read(r, root))
    return 0;
  if (timeline) {
    if (!yyjson_is_obj(timeline)) fixed(r, SB_R_TIMELINE_IGNORED, 1);
    else if (!timeline_read(r, timeline)) return 0;
  }
  if (!c->nphrases && !c->flat.nnotes && !c->flat.nchords) {
    uint32_t dropped = c->repairs.n[SB_R_NOTES_DROPPED] + c->repairs.n[SB_R_CHORDS_DROPPED];
    return dropped ? fail(r, "the arrangement has no notes and no chords that can be used (%u dropped)", dropped)
                   : fail(r, "the arrangement has no notes and no chords");
  }
  sb_repairs *rp = &c->repairs;
  size_t at = 0;
  for (int k = 0; k < SB_R_COUNT && at < sizeof rp->text; k++) {
    if (!rp->n[k]) continue;
    int n = snprintf(rp->text + at, sizeof rp->text - at, "%s%u %s", at ? ", " : "", rp->n[k], repair_names[k]);
    at += n > 0 ? (size_t)n : 0;
  }
  return 1;
}

void sb_chart_free(sb_chart *c) {
  if (!c) return;
  part_free(&c->flat);
  for (uint32_t i = 0; i < c->nphrases; i++) phrase_free(&c->phrases[i]);
  free(c->phrases), free(c->templates), free(c->beats), free(c->sections), free(c->tones), free(c);
}

static yyjson_doc *json_read(rd *r, const char *d, size_t n, const char *what) {
  yyjson_read_err e;
  if (!d || !n) return fail(r, "%s is empty", what), (yyjson_doc *)0;
  if (n > MAX_JSON) return fail(r, "%s is %zu bytes: too large", what, n), (yyjson_doc *)0;
  // yyjson wants a writable buffer only for its in-place mode, which this does not use
  yyjson_doc *doc = yyjson_read_opts((char *)(uintptr_t)d, n, YYJSON_READ_ALLOW_COMMENTS | YYJSON_READ_ALLOW_TRAILING_COMMAS, 0, &e);
  if (!doc) fail(r, "%s is not JSON: %s at byte %zu", what, e.msg ? e.msg : "error", e.pos);
  return doc;
}

sb_chart *sb_chart_parse(const char *json, size_t n, const char *timeline, size_t tn, char *err, size_t errlen) {
  char none[1];
  rd r = {err, errlen, 0}, quiet = {none, 0, 0};
  if (errlen) err[0] = 0;
  yyjson_doc *doc = json_read(&r, json, n, "the arrangement"), *tdoc = 0;
  if (!doc) return 0;
  sb_chart *c = r.c = calloc(1, sizeof *c);
  if (!c) fail(&r, "out of memory");
  else {
    // a song timeline that is not JSON is as good as none
    if (timeline && !(tdoc = json_read(&quiet, timeline, tn, ""))) fixed(&r, SB_R_TIMELINE_IGNORED, 1);
    if (!chart_read(&r, yyjson_doc_get_root(doc), tdoc ? yyjson_doc_get_root(tdoc) : 0)) sb_chart_free(c), c = 0;
  }
  yyjson_doc_free(doc), yyjson_doc_free(tdoc);
  return c;
}
