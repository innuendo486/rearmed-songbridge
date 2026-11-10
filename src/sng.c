// See sng.h. A song file is
//   u32 0x4a | u32 flags (3: compressed and encrypted) | 16-byte IV | payload | 56-byte signature
// all little-endian on both platforms. The payload is u32 plain length + a zlib stream, AES-256-CTR encrypted with
// the platform's key (big-endian counter from the IV). RS Re-ARMed does not check the signature: it is zero here.
// The archive assembler serves the file as stored 64 KiB blocks, and a reader takes a short last block that starts
// as a zlib stream for a compressed one, so the IV is counted up until the last block does not start so (as
// psarc.c's sng_rekey does).
//
// The plain payload, every list a u32 count and its records:
//   beats             f32 time, u16 measure, u16 beat in the measure, u32 phrase iteration, u32 mask (1 first beat
//                     of a measure, 2 an even measure)
//   phrases           i8 solo, i8 disparity, i8 ignore, pad, u32 max difficulty, u32 iterations, char name[32]
//   chord templates   u32 mask (1 arpeggio, 2 "nop"), i8 frets[6], i8 fingers[6], i32 MIDI notes[6], char name[32]
//   chord notes       u32 mask[6], {bend[32], u32 count}[6], i8 slide to[6], i8 unpitched slide to[6], i16 vibrato[6]
//   vocals            none here (a lyrics file has them, and the symbol tables that follow them)
//   phrase iterations u32 phrase, f32 time, f32 end, u32 difficulty[3] (easy, medium, hard)
//   phrase extra info, linked difficulties, actions, events (f32 time, char name[256]): none here
//   tones             f32 time, u32 id (0-3: the manifest's Tone_A to Tone_D)
//   DNAs              none here
//   sections          char name[32], u32 number, f32 start, f32 end, u32 first and last phrase iteration,
//                     u8 string mask[36]: by difficulty, the strings its notes are on
//   levels            u32 difficulty, anchors, anchor extensions, hand shapes, arpeggio hand shapes, notes,
//                     f32 average notes of each phrase, u32 notes of each iteration without the ignored, u32 with
//     anchor          f32 time, f32 end, f32 first note, f32 last note, i8 fret, pad[3], i32 width, u32 iteration
//     anchor extension f32 time, i8 fret, pad[7]: where a slide ends
//     hand shape      u32 template, f32 start, f32 end, f32 first note, f32 last note
//     note            u32 mask, u32 flags (1: draw the fret number), u32 hash, f32 time, i8 string, i8 fret,
//                     i8 anchor fret, i8 anchor width, i32 template, i32 chord notes, i32 phrase, i32 iteration,
//                     i16 hand shape, i16 arpeggio, i16 next and previous note in the iteration, i16 parent note,
//                     i8 slide to, i8 unpitched slide to, i8 left hand, i8 tap, i8 pick direction, i8 slap,
//                     i8 pluck, i16 vibrato, f32 sustain, f32 max bend, bends (f32 time, f32 step, pad[3], i8)
//   metadata          f64 max score, f64 max notes, f64 max notes without the ignored, f64 points per note,
//                     f32 first beat length, f32 start time, i8 capo (-1 none), char conversion date[32], i16 part,
//                     f32 song length, tuning (i16 each), f32 first note time (twice), i32 max difficulty
// A chord is a note record with string and fret -1; the notes it is made of are its template's, and what they do
// (bends, slides, mutes) is in the chord notes record it names, when they do anything.
//
// From a feedpak arrangement (the "wire format"), compared with real song files turned into wire charts and built
// again (tests/run_sng_test.py: 12 files, 46089 notes over all their levels):
//
// Exact, in every file: beats; chord templates (mask, frets, fingers, pitches, name); iteration times and each one's
//   max difficulty; sections (name, number, times, first and last iteration); tone changes; in every level the
//   anchors (time, end, fret, width, iteration, first note time), anchor extensions, hand shapes and arpeggios
//   (template, times, first note time), and every note and chord: time, string, fret, anchor, template, iteration,
//   hand shape, next and previous note, parent note, slides, left hand, sustain, bends (curve from "bnv"), vibrato,
//   slap, pluck, pick, tap, and the mask but for the bits below; notes counted per iteration; the metadata's score,
//   counts, times, capo, tuning, max difficulty.
// Derived by rule, and not always what the file had:
//   "draw the fret number" (a fretted note, or a chord with its box, where the hand has moved since the note
//     before): as the custom song tools have it (all of their notes match); the game's own files number more notes
//     by a rule this does not know (86% match over all files)
//   a chord's "show the chord box" bit (a chord in a hand shape that does not repeat the chord before it in that
//     shape): 46049 of 46089
//   an anchor's and a hand shape's last note time (the last note's end when that is 5 ms or more inside, else its
//     start): 99%
//   a chord's own mute, palm mute, accent, link and ignore bits, from its notes: all muted ("fhm" or "mt"), all
//     palm-muted, any accented, any linked, all ignored; double stop: two notes
//   chord notes records: made when a note of the chord does something (a technique, a slide, a bend, a sustain),
//     an open string alone not counting. The game's own files keep records for chords whose notes do nothing; the
//     custom song tools do not, nor does this.
//   the iterations' easy and medium levels (a quarter and a half of the hardest, halves to even): what the tools
//     write when the chart has no "hero levels"; charts that have them differ (the format does not carry them)
//   a section's string masks, by difficulty the strings its notes are on: as the game's own files (all match); the
//     custom song tools write other values, by a rule this does not know
//   the first beat's length (the second beat's time less the first's): differs in one of the game's own songs
// Not in the wire format, written as defaults:
//   phrase names and which iterations repeat one phrase: every iteration gets a phrase of its own ("COUNT", "p1",
//     ..., "END"), so the game's per-phrase difficulty does not carry from one repeat to the next, and the average
//     notes of a phrase are its one iteration's
//   phrase solo, disparity and ignore marks: 0
//   linked difficulties, phrase extra info, actions, events (crowd and tuning cues), DNA marks: empty lists
//   a simple bend given only as its peak ("bn" without "bnv"): a curve made up from the peak and the intent ("bt"):
//     up at a third of the sustain, and so on (bend_shape)
//   tap finger 0, slap 1, pluck 1 when set (-1 when not); pick direction 0 unless "pkd" is 1; vibrato strength 80
//   the unpitched slide of the game's own files also sets the pitched slide bit: here, as the tools, it does not
//   note hashes: the CRC-32 of the record (the game's own are of unknown make)
//   conversion date: a fixed one, so that the same chart always gives the same bytes
//   the part number: 1
//   right-hand fingering ("rh") is only a bit of the mask; "fg" is the left hand finger; "ch", "sd" and a chord's
//     "fn" have no place in a song file
// Made up when the chart has none (sb_sng_facts.made says which): without "phrases" the chart has one level, cut
// into iterations at its sections: a "COUNT" iteration before the first note, an "END" after the last when the song
// goes on. Without sections one section spans the notes. Without beats a 120 bpm grid is made. A level without an
// anchor at its first note gets one there. With "phrases", a phrase that lacks a difficulty below its hardest plays
// the nearest one below it there.
#include "sng.h"

#include <CommonCrypto/CommonCrypto.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zlib.h>

#include "keys.h"

#define BLOCK 0x10000u
#define SNG_MAGIC 0x4au
#define SNG_HEAD 24u  // magic, flags, IV
#define SIG_LEN 56u
#define MAX_PAYLOAD (512u << 20)
#define CN_SIZE 2376u  // a chord notes record
#define CN_BENDS 24u
#define CN_BEND_SIZE 388u  // 32 bends of 12 bytes, their count
#define CN_SLIDE 2352u
#define CN_UNPITCHED 2358u
#define CN_VIBRATO 2364u
#define MAX_CHORD_NOTES 0x10000u
#define NO_FIRST 3.4028235e38f   // an anchor without notes: what the game's own files have
#define NO_LAST 1.17549435e-38f

enum {  // note mask
  M_CHORD = 0x2, M_OPEN = 0x4, M_FRET_MUTE = 0x8, M_TREMOLO = 0x10, M_HARMONIC = 0x20, M_PALM_MUTE = 0x40, M_SLAP = 0x80,
  M_PLUCK = 0x100, M_HAMMER = 0x200, M_PULL = 0x400, M_SLIDE = 0x800, M_BEND = 0x1000, M_SUSTAIN = 0x2000, M_TAP = 0x4000,
  M_PINCH = 0x8000, M_VIBRATO = 0x10000, M_MUTE = 0x20000, M_IGNORE = 0x40000, M_LEFT_HAND = 0x80000,
  M_RIGHT_HAND = 0x100000, M_HIGH_DENSITY = 0x200000, M_UNPITCHED = 0x400000, M_SINGLE = 0x800000,
  M_CHORD_NOTES = 0x1000000, M_DOUBLE_STOP = 0x2000000, M_ACCENT = 0x4000000, M_PARENT = 0x8000000,
  M_CHILD = 0x10000000, M_ARPEGGIO = 0x20000000,
};
#define M_PANEL 0x80000000u

// --- AES-256-CTR (big-endian counter), as openssl's aes-256-ctr -----------------------------------------------------
static void ctr(const uint8_t key[32], const uint8_t iv[16], uint8_t *d, size_t n) {
  uint8_t c[16], in[4096], ks[4096];
  memcpy(c, iv, 16);
  for (size_t i = 0; i < n;) {
    size_t m = n - i < sizeof in ? n - i : sizeof in, blocks = (m + 15) / 16, moved = 0;
    for (size_t b = 0; b < blocks; b++) {
      memcpy(in + 16 * b, c, 16);
      for (int k = 15; k >= 0 && ++c[k] == 0; k--) {}
    }
    CCCrypt(kCCEncrypt, kCCAlgorithmAES, kCCOptionECBMode, key, 32, 0, in, blocks * 16, ks, blocks * 16, &moved);
    for (size_t j = 0; j < m; j++) d[i + j] ^= ks[j];
    i += m;
  }
}

static int looks_zlib(const uint8_t *p) { return (p[0] & 0x0f) == 8 && (p[0] >> 4) <= 7 && ((p[0] << 8 | p[1]) % 31) == 0; }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]; }
static void put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24);
}

uint8_t *sb_sng_pack(const uint8_t *payload, size_t n, int platform, size_t *out_n) {
  if (n > MAX_PAYLOAD || (platform != SB_MAC && platform != SB_PC)) return 0;
  uLongf zn = compressBound((uLong)n);
  uint8_t *d = calloc(1, SNG_HEAD + 4 + zn + SIG_LEN);
  if (!d) return 0;
  if (compress2(d + SNG_HEAD + 4, &zn, payload, (uLong)n, 9) != Z_OK) {
    free(d);
    return 0;
  }
  size_t len = SNG_HEAD + 4 + zn + SIG_LEN, bn = 4 + zn;
  put32(d, SNG_MAGIC), put32(d + 4, 3), put32(d + SNG_HEAD, (uint32_t)n);
  memset(d + len - SIG_LEN, 0, SIG_LEN);
  size_t last = len % BLOCK ? len - len % BLOCK : 0;  // a last block that is short and not the first
  for (;;) {
    ctr(sb_sng_key[platform], d + 8, d + SNG_HEAD, bn);
    if (!last || len - last < 2 || !looks_zlib(d + last)) break;
    ctr(sb_sng_key[platform], d + 8, d + SNG_HEAD, bn);
    for (int k = 0; k < 16 && ++d[8 + k] == 0; k++) {}
  }
  *out_n = len;
  return d;
}

uint8_t *sb_sng_unpack(const uint8_t *file, size_t n, int platform, size_t *out_n) {
  if (n < SNG_HEAD + 4 + SIG_LEN || le32(file) != SNG_MAGIC || (platform != SB_MAC && platform != SB_PC)) return 0;
  uint32_t flags = le32(file + 4);
  if (flags != 3 && flags != 1) return 0;
  size_t bn = n - SNG_HEAD - (flags == 3 ? SIG_LEN : 0);
  uint8_t *body = malloc(bn), *out = 0;
  if (!body) return 0;
  memcpy(body, file + SNG_HEAD, bn);
  if (flags == 3) ctr(sb_sng_key[platform], file + 8, body, bn);
  uint32_t plain = le32(body);
  uLongf got = plain;
  if (plain <= MAX_PAYLOAD && bn - 4 <= 0xffffffffu && (out = malloc(plain ? plain : 1))) {
    // a plain file may have bytes after its stream, so the stream's own end decides
    z_stream z = {0};
    int r = Z_DATA_ERROR;
    if (inflateInit(&z) == Z_OK) {
      z.next_in = body + 4, z.avail_in = (uInt)(bn - 4), z.next_out = out, z.avail_out = plain;
      r = inflate(&z, Z_FINISH);
      got = z.total_out;
      inflateEnd(&z);
    }
    if (r != Z_STREAM_END || got != plain) free(out), out = 0;
  }
  free(body);
  if (out) *out_n = plain;
  return out;
}

// --- writing --------------------------------------------------------------------------------------------------------
typedef struct {
  uint8_t *d;
  size_t n, cap;
  int bad;  // out of memory, or past MAX_PAYLOAD
} buf;

static uint8_t *room(buf *b, size_t n) {
  if (b->bad) return 0;
  if (b->n + n > b->cap) {
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < b->n + n) cap *= 2;
    uint8_t *d = cap > MAX_PAYLOAD ? 0 : realloc(b->d, cap);
    if (!d) {
      b->bad = 1;
      return 0;
    }
    b->d = d, b->cap = cap;
  }
  b->n += n;
  return b->d + b->n - n;
}
static void w(buf *b, const void *p, size_t n) {
  uint8_t *d = room(b, n);
  if (d && n) memcpy(d, p, n);
}
static void wz(buf *b, size_t n) {
  uint8_t *d = room(b, n);
  if (d) memset(d, 0, n);
}
static void w8(buf *b, int v) {
  uint8_t x = (uint8_t)v;
  w(b, &x, 1);
}
static void w16(buf *b, int v) {
  uint8_t x[2] = {(uint8_t)v, (uint8_t)((unsigned)v >> 8)};
  w(b, x, 2);
}
static void w32(buf *b, uint32_t v) {
  uint8_t x[4];
  put32(x, v);
  w(b, x, 4);
}
static void wf(buf *b, float f) {
  uint32_t v;
  memcpy(&v, &f, 4);
  w32(b, v);
}
static void wd(buf *b, double f) {
  uint64_t v;
  memcpy(&v, &f, 8);
  w32(b, (uint32_t)v), w32(b, (uint32_t)(v >> 32));
}
static void wstr(buf *b, const char *s, size_t width) {
  uint8_t *d = room(b, width);
  if (!d) return;
  size_t n = strlen(s);
  memset(d, 0, width);
  memcpy(d, s, n < width ? n : width - 1);
}

// --- the song being built -------------------------------------------------------------------------------------------
typedef struct {
  float t, end;
  int max;                 // the hardest level it has notes at
  const sb_phrase *src;    // 0: the whole chart, cut by time
  char name[32];
  uint32_t top;            // its notes at `max`
  uint32_t at[3];          // and at its easy, medium, hard levels
} iter;

typedef struct {
  float t, end;
  char name[32];
  int number;
  uint32_t first, last;
  uint8_t strings[SB_MAX_DIFFICULTY + 1];
} sect;

typedef struct {
  float t;
  int measure, beat;
} beat;

typedef struct {
  const sb_chart *c;
  char *err;
  size_t errlen;
  int bass;
  float length;
  iter *it;
  sect *sec;
  beat *beats;
  uint32_t nit, nsec, nbeats, nlevels;
  // the chord notes records, each once: the records, and a table of their numbers + 1 by hash
  uint8_t *cn;
  uint32_t *cn_slot;
  uint32_t ncn, cn_cap, cn_slots;
  sb_sng_use *uses;
  uint32_t nuses, uses_cap;
  // facts
  uint32_t techniques, notes, chords, ignored, made;
  float first_note, last_note;
} song;

static int fail(song *s, const char *fmt, ...) {
  if (s->errlen) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->err, s->errlen, fmt, ap);
    va_end(ap);
  }
  return 0;
}

// The last of n times (in records `size` apart, the time first) that is not after t, -1 when all are
static int32_t at_or_before(const void *times, uint32_t n, size_t size, float t) {
  uint32_t lo = 0, hi = n;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    float x;
    memcpy(&x, (const uint8_t *)times + mid * size, 4);
    if (x <= t) lo = mid + 1;
    else hi = mid;
  }
  return (int32_t)lo - 1;
}

static uint32_t iter_of(const song *s, float t) {
  int32_t i = at_or_before(s->it, s->nit, sizeof *s->it, t);
  return i < 0 ? 0 : (uint32_t)i;
}

// The part of an iteration played at `level`: the one of that difficulty, else the nearest below, else the lowest.
// 0 above the iteration's hardest.
static const sb_part *part_at(const iter *it, uint32_t level) {
  const sb_phrase *p = it->src;
  if ((int)level > it->max || !p->nlevels) return 0;
  const sb_part *best = &p->levels[0];
  for (uint32_t i = 0; i < p->nlevels && p->levels[i].difficulty <= (int)level; i++) best = &p->levels[i];
  return best;
}

static uint32_t note_mask(const sb_note *n) {
  uint32_t m = 0, f = n->flags;
  if (n->f == 0) m |= M_OPEN;
  if (n->sus > 0) m |= M_SUSTAIN;
  if (n->sl >= 0) m |= M_SLIDE;
  if (n->slu >= 0) m |= M_UNPITCHED;
  if (n->nbends || n->bn > 0) m |= M_BEND;
  if (n->rh >= 0) m |= M_RIGHT_HAND;
  if (f & SB_N_HAMMER) m |= M_HAMMER;
  if (f & SB_N_PULL) m |= M_PULL;
  if (f & SB_N_HARMONIC) m |= M_HARMONIC;
  if (f & SB_N_PINCH) m |= M_PINCH;
  if (f & SB_N_PALM_MUTE) m |= M_PALM_MUTE;
  if (f & SB_N_MUTE) m |= M_MUTE;
  if (f & SB_N_VIBRATO) m |= M_VIBRATO;
  if (f & SB_N_TREMOLO) m |= M_TREMOLO;
  if (f & SB_N_ACCENT) m |= M_ACCENT;
  if (f & SB_N_TAP) m |= M_TAP;
  if (f & SB_N_LINK) m |= M_PARENT;
  if (f & SB_N_FRET_MUTE) m |= M_FRET_MUTE;
  if (f & SB_N_PLUCK) m |= M_PLUCK;
  if (f & SB_N_SLAP) m |= M_SLAP;
  if (f & SB_N_IGNORE) m |= M_IGNORE;
  return m;
}

static uint32_t techniques_of(uint32_t m) {
  uint32_t t = 0;
  if (m & M_BEND) t |= SB_T_BEND;
  if (m & M_SLIDE) t |= SB_T_SLIDE;
  if (m & M_UNPITCHED) t |= SB_T_UNPITCHED_SLIDE;
  if (m & M_HARMONIC) t |= SB_T_HARMONIC;
  if (m & M_PINCH) t |= SB_T_PINCH_HARMONIC;
  if (m & M_HAMMER) t |= SB_T_HAMMER_ON;
  if (m & M_PULL) t |= SB_T_PULL_OFF;
  if (m & M_PALM_MUTE) t |= SB_T_PALM_MUTE;
  if (m & (M_FRET_MUTE | M_MUTE)) t |= SB_T_FRET_HAND_MUTE;
  if (m & M_TREMOLO) t |= SB_T_TREMOLO;
  if (m & M_VIBRATO) t |= SB_T_VIBRATO;
  if (m & M_SUSTAIN) t |= SB_T_SUSTAIN;
  if (m & M_TAP) t |= SB_T_TAP;
  if (m & M_SLAP) t |= SB_T_SLAP;
  if (m & M_PLUCK) t |= SB_T_PLUCK;
  if (m & M_ACCENT) t |= SB_T_ACCENT;
  if (m & M_ARPEGGIO) t |= SB_T_ARPEGGIO;
  return t;
}

// A note's bend curve: up to 32 points of time (absolute) and step. The file's own curve, else one made up from the
// peak and the intent.
static uint32_t bend_shape(const sb_part *p, const sb_note *n, float t, float pt[32], float pv[32]) {
  if (n->nbends) {
    for (uint32_t i = 0; i < n->nbends && i < 32; i++) pt[i] = t + p->bends[n->bend0 + i].t, pv[i] = p->bends[n->bend0 + i].v;
    return n->nbends < 32 ? n->nbends : 32;
  }
  if (!(n->bn > 0)) return 0;
  float third = n->sus / 3;
  switch (n->bt) {
    case 1:  // release
    case 3:  // pre-bend and release
      pt[0] = t, pv[0] = n->bn, pt[1] = t + 2 * third, pv[1] = 0;
      return 2;
    case 2:  // pre-bend
      pt[0] = t, pv[0] = n->bn;
      return 1;
    case 4:  // up and back
      pt[0] = t + third, pv[0] = n->bn, pt[1] = t + 2 * third, pv[1] = 0;
      return 2;
    default:
      pt[0] = t + third, pv[0] = n->bn;
      return 1;
  }
}

// The number of the chord notes record rec (CN_SIZE bytes), added when it is new: -1 out of memory, -2 too many
static int32_t chord_notes(song *s, const uint8_t *rec) {
  if ((s->ncn + 1) * 2 > s->cn_slots) {
    uint32_t slots = s->cn_slots ? s->cn_slots * 2 : 256, *t = calloc(slots, sizeof *t);
    if (!t) return -1;
    for (uint32_t i = 0; i < s->ncn; i++) {
      uint32_t h = (uint32_t)crc32(0, s->cn + (size_t)i * CN_SIZE, CN_SIZE) & (slots - 1);
      while (t[h]) h = (h + 1) & (slots - 1);
      t[h] = i + 1;
    }
    free(s->cn_slot), s->cn_slot = t, s->cn_slots = slots;
  }
  uint32_t h = (uint32_t)crc32(0, rec, CN_SIZE) & (s->cn_slots - 1);
  for (; s->cn_slot[h]; h = (h + 1) & (s->cn_slots - 1))
    if (!memcmp(s->cn + (size_t)(s->cn_slot[h] - 1) * CN_SIZE, rec, CN_SIZE)) return (int32_t)s->cn_slot[h] - 1;
  if (s->ncn == MAX_CHORD_NOTES) return -2;
  if (s->ncn == s->cn_cap) {
    uint32_t cap = s->cn_cap ? s->cn_cap * 2 : 64;
    uint8_t *d = realloc(s->cn, (size_t)cap * CN_SIZE);
    if (!d) return -1;
    s->cn = d, s->cn_cap = cap;
  }
  memcpy(s->cn + (size_t)s->ncn * CN_SIZE, rec, CN_SIZE);
  s->cn_slot[h] = ++s->ncn;
  return (int32_t)s->ncn - 1;
}

static int use_add(song *s, uint32_t level, uint32_t section, uint32_t chord) {
  if (s->nuses == s->uses_cap) {
    uint32_t cap = s->uses_cap ? s->uses_cap * 2 : 256;
    sb_sng_use *u = realloc(s->uses, cap * sizeof *u);
    if (!u) return 0;
    s->uses = u, s->uses_cap = cap;
  }
  s->uses[s->nuses++] = (sb_sng_use){(uint8_t)level, (uint16_t)section, chord};
  return 1;
}

// --- one level ------------------------------------------------------------------------------------------------------
typedef struct {
  float t, sus;  // a chord's sustain is its longest note's
  const sb_part *part;
  const sb_note *n;   // a single note, or
  const sb_chord *c;  // a chord
  uint32_t seq;
  uint32_t pi;
  int32_t anchor, shape[2];
} ev;

typedef struct {
  float t;
  int8_t fret, width;
} anc;

typedef struct {
  float t0, t1;
  uint32_t id;
} shp;

static int by_ev(const void *a, const void *b) {
  const ev *x = a, *y = b;
  if (x->t != y->t) return x->t < y->t ? -1 : 1;
  if (!x->c != !y->c) return x->c ? -1 : 1;  // a chord before the single notes at its time
  if (x->n && x->n->s != y->n->s) return x->n->s - y->n->s;
  return x->seq < y->seq ? -1 : x->seq > y->seq;
}
static int by_anc(const void *a, const void *b) {
  const anc *x = a, *y = b;
  return x->t < y->t ? -1 : x->t > y->t;
}
static int by_shp(const void *a, const void *b) {
  const shp *x = a, *y = b;
  return x->t0 < y->t0 ? -1 : x->t0 > y->t0 ? 1 : x->t1 < y->t1 ? -1 : x->t1 > y->t1;
}

// The first and last note time of the stretch [t0, t1), as the song file has them: *from is where the search goes on
// for the next stretch. 0 when the stretch has no note.
static int stretch(const ev *e, uint32_t n, uint32_t *from, float t0, float t1, float *first, float *last) {
  uint32_t i = *from;
  while (i < n && e[i].t < t0) i++;
  *from = i;
  if (i == n || e[i].t >= t1) return 0;
  *first = e[i].t;
  while (i + 1 < n && e[i + 1].t < t1) i++;
  float end = e[i].t + e[i].sus;
  *last = end < t1 - 0.005f ? end : e[i].t;
  return 1;
}

static int level_build(song *s, uint32_t level, buf *b) {
  const sb_chart *c = s->c;
  uint32_t nparts = c->nphrases ? s->nit : 1, nev = 0, nanc = 0, nshp[2] = {0, 0};
  size_t tev = 0, tanc = 0, tshp = 0;
  int ok = 0;
  const sb_part **parts = calloc(nparts, sizeof *parts);
  ev *e = 0;
  anc *a = 0;
  shp *h[2] = {0, 0};
  uint32_t *count = 0, *count_played = 0;
  if (!parts) return fail(s, "out of memory");
  for (uint32_t i = 0; i < nparts; i++) {
    parts[i] = c->nphrases ? part_at(&s->it[i], level) : &c->flat;
    if (!parts[i]) continue;
    tev += parts[i]->nnotes + parts[i]->nchords, tanc += parts[i]->nanchors, tshp += parts[i]->nshapes;
  }
  if (tev > SB_MAX_EVENTS) {
    fail(s, "%zu notes and chords at difficulty %u: more than the %u a song file holds", tev, level, SB_MAX_EVENTS);
    goto out;
  }
  if (tshp > 32767) {
    fail(s, "%zu hand shapes at difficulty %u: more than a song file holds", tshp, level);
    goto out;
  }
  e = calloc(tev + 1, sizeof *e), a = calloc(tanc + 1, sizeof *a);
  h[0] = calloc(tshp + 1, sizeof **h), h[1] = calloc(tshp + 1, sizeof **h);
  count = calloc(s->nit, sizeof *count), count_played = calloc(s->nit, sizeof *count_played);
  if (!e || !a || !h[0] || !h[1] || !count || !count_played) {
    fail(s, "out of memory");
    goto out;
  }
  for (uint32_t i = 0; i < nparts; i++) {
    const sb_part *p = parts[i];
    if (!p) continue;
    for (uint32_t k = 0; k < p->nnotes; k++, nev++)
      e[nev] = (ev){.t = (float)p->notes[k].t, .sus = p->notes[k].sus, .part = p, .n = &p->notes[k], .seq = nev};
    for (uint32_t k = 0; k < p->nchords; k++, nev++) {
      const sb_chord *ch = &p->chords[k];
      float sus = 0;
      for (uint32_t j = 0; j < ch->nnotes; j++)
        if (p->cnotes[ch->note0 + j].sus > sus) sus = p->cnotes[ch->note0 + j].sus;
      e[nev] = (ev){.t = (float)ch->t, .sus = sus, .part = p, .c = ch, .seq = nev};
    }
    for (uint32_t k = 0; k < p->nanchors; k++) a[nanc++] = (anc){(float)p->anchors[k].t, p->anchors[k].fret, p->anchors[k].width};
    for (uint32_t k = 0; k < p->nshapes; k++) {
      int arp = p->shapes[k].arp != 0;
      h[arp][nshp[arp]++] = (shp){(float)p->shapes[k].t0, (float)p->shapes[k].t1, p->shapes[k].id};
    }
  }
  qsort(e, nev, sizeof *e, by_ev);
  qsort(a, nanc, sizeof *a, by_anc);
  qsort(h[0], nshp[0], sizeof **h, by_shp), qsort(h[1], nshp[1], sizeof **h, by_shp);
  uint32_t keep = 0;
  for (uint32_t i = 0; i < nanc; i++)  // one anchor a time
    if (!keep || a[i].t > a[keep - 1].t) a[keep++] = a[i];
  nanc = keep;
  if (nev && (!nanc || a[0].t > e[0].t)) {  // a note needs an anchor: one at the first note
    int fret = 1;
    if (e[0].n && e[0].n->f > 0) fret = e[0].n->f;
    if (e[0].c)
      for (uint32_t j = e[0].c->nnotes; j--;)
        if (e[0].part->cnotes[e[0].c->note0 + j].f > 0) fret = e[0].part->cnotes[e[0].c->note0 + j].f;
    memmove(a + 1, a, nanc * sizeof *a);
    a[0] = (anc){e[0].t, (int8_t)(fret > 21 ? 21 : fret), 4}, nanc++;
    s->made |= SB_MADE_ANCHOR;
  }
  for (uint32_t i = 0; i < nev; i++) {
    e[i].pi = iter_of(s, e[i].t);
    e[i].anchor = at_or_before(a, nanc, sizeof *a, e[i].t);
    for (int k = 0; k < 2; k++) {
      int32_t x = at_or_before(h[k], nshp[k], sizeof **h, e[i].t);
      e[i].shape[k] = x >= 0 && e[i].t < h[k][x].t1 ? x : -1;
    }
    count[e[i].pi]++;
  }

  w32(b, level);
  // anchors
  w32(b, nanc);
  float last_start = s->it[s->nit - 1].t;
  for (uint32_t i = 0, from = 0; i < nanc; i++) {
    float end = i + 1 < nanc ? a[i + 1].t : last_start > a[i].t ? last_start : s->length, first = NO_FIRST, last = NO_LAST;
    stretch(e, nev, &from, a[i].t, end, &first, &last);
    wf(b, a[i].t), wf(b, end), wf(b, first), wf(b, last);
    w8(b, a[i].fret), wz(b, 3), w32(b, (uint32_t)a[i].width), w32(b, iter_of(s, a[i].t));
  }
  // anchor extensions: where the slides end
  uint32_t nslides = 0;
  for (uint32_t i = 0; i < nev; i++) nslides += e[i].n && e[i].n->sl >= 0;
  w32(b, nslides);
  for (uint32_t i = 0; i < nev; i++) {
    if (!e[i].n || e[i].n->sl < 0) continue;
    wf(b, e[i].t + e[i].sus), w8(b, e[i].n->sl), wz(b, 7);
  }
  // hand shapes, arpeggios
  for (int k = 0; k < 2; k++) {
    w32(b, nshp[k]);
    for (uint32_t i = 0, from = 0; i < nshp[k]; i++) {
      float first = -1, last = -1;
      uint32_t at = from;  // shapes may overlap: each searches from where the one before started
      stretch(e, nev, &at, h[k][i].t0, h[k][i].t1, &first, &last);
      from = at;
      w32(b, h[k][i].id), wf(b, h[k][i].t0), wf(b, h[k][i].t1), wf(b, first), wf(b, last);
    }
  }
  // notes
  w32(b, nev);
  int32_t linked[SB_STRINGS] = {-1, -1, -1, -1, -1, -1};  // the note each string's last note links on from, -1
  int32_t prev_chord = -1;
  for (uint32_t i = 0; i < nev; i++) {
    const ev *x = &e[i];
    const anc *an = &a[x->anchor];
    uint32_t mask, flags = 0;
    int32_t chord_id = -1, cn_id = -1, parent = -1;
    int8_t string = -1, fret = -1, slide = -1, unpitched = -1, left = -1, tap = -1, pick = -1, slap = -1, pluck = -1;
    int vibrato = 0;
    float pt[32], pv[32], max_bend = 0;
    uint32_t nb = 0;
    int moved = !i || a[e[i - 1].anchor].fret != an->fret;  // the hand is somewhere else than at the note before
    int32_t sec = at_or_before(s->sec, s->nsec, sizeof *s->sec, x->t);  // the section it is in, -1 before the first
    if (x->n) {
      const sb_note *n = x->n;
      mask = M_SINGLE | note_mask(n);
      string = n->s, fret = n->f, slide = n->sl, unpitched = n->slu, pick = n->pkd == 1;
      if (n->fg >= 0) left = n->fg, mask |= M_LEFT_HAND;
      if (n->flags & SB_N_TAP) tap = 0;
      if (n->flags & SB_N_SLAP) slap = 1;
      if (n->flags & SB_N_PLUCK) pluck = 1;
      if (n->flags & SB_N_VIBRATO) vibrato = 80;
      if (x->shape[1] >= 0) mask |= M_ARPEGGIO;
      if (linked[n->s] >= 0) mask |= M_CHILD, parent = linked[n->s];
      linked[n->s] = n->flags & SB_N_LINK ? (int32_t)i : -1;
      nb = bend_shape(x->part, n, x->t, pt, pv);
      for (uint32_t k = 0; k < nb; k++)
        if (pv[k] > max_bend) max_bend = pv[k];
      if (n->f > 0 && moved) flags = 1;
      if (n->f == 0) s->techniques |= SB_T_OPEN_NOTE;
    } else {
      const sb_chord *ch = x->c;
      const sb_note *cnote = &x->part->cnotes[ch->note0];
      const sb_template *tp = &c->templates[ch->id];
      uint8_t rec[CN_SIZE];
      uint32_t any = 0, all = ~0u, does = 0;
      int open = 0, barre = 0, lo = -1, fifth = 1;
      memset(rec, 0, sizeof rec);
      memset(rec + CN_SLIDE, 0xff, 12);
      for (uint32_t j = 0; j < ch->nnotes; j++) {
        const sb_note *n = &cnote[j];
        uint32_t m = note_mask(n);
        any |= m, all &= m;
        uint8_t *bd = rec + CN_BENDS + (size_t)n->s * CN_BEND_SIZE;
        uint32_t k, nbn = bend_shape(x->part, n, x->t, pt, pv);
        for (k = 0; k < nbn; k++) memcpy(bd + 12 * k, &pt[k], 4), memcpy(bd + 12 * k + 4, &pv[k], 4);
        put32(bd + 384, nbn);
        // what the chord itself says (a fret-hand mute of all its strings) is not a thing its notes do
        m &= ~(uint32_t)M_FRET_MUTE;
        put32(rec + 4 * n->s, m);
        rec[CN_SLIDE + n->s] = (uint8_t)n->sl, rec[CN_UNPITCHED + n->s] = (uint8_t)n->slu;
        if (n->flags & SB_N_VIBRATO) rec[CN_VIBRATO + 2 * n->s] = 80;
        does |= m & ~(uint32_t)M_OPEN;  // an open string is the template's to say
        linked[n->s] = n->flags & SB_N_LINK ? (int32_t)i : -1;
        open |= n->f == 0;
        // a power chord: every note the root, its fifth or its octave, by the strings' pitches
        int pitch = (n->s < 4 || s->bass ? 5 * n->s : 5 * n->s - 1) + c->tuning[n->s] + n->f;
        if (lo < 0) lo = pitch;
        else if ((pitch - lo) % 12 != 0 && (pitch - lo) % 12 != 7) fifth = 0;
      }
      for (int p = 0; p < SB_STRINGS; p++)
        for (int q = p + 1; q < SB_STRINGS; q++)
          barre |= tp->fingers[p] > 0 && tp->fingers[p] == tp->fingers[q] && tp->frets[p] > 0 && tp->frets[q] > 0;
      mask = M_CHORD;
      if (does) {
        cn_id = chord_notes(s, rec);
        if (cn_id < 0) {
          fail(s, cn_id == -1 ? "out of memory" : "more chords with techniques than a song file holds");
          goto out;
        }
        mask |= M_CHORD_NOTES;
      }
      if (ch->nnotes == 2) mask |= M_DOUBLE_STOP;
      if (x->sus > 0) mask |= M_SUSTAIN;
      if (ch->hd) mask |= M_HIGH_DENSITY;
      if (all & (M_FRET_MUTE | M_MUTE)) mask |= M_FRET_MUTE;
      if (all & M_PALM_MUTE) mask |= M_PALM_MUTE;
      if (all & M_IGNORE) mask |= M_IGNORE;
      if (any & M_ACCENT) mask |= M_ACCENT;
      if (any & M_PARENT) mask |= M_PARENT;
      if (x->shape[1] >= 0) mask |= M_ARPEGGIO;
      // the chord box: not for a chord that repeats the one before it in the same hand shape
      int32_t sh = x->shape[0] >= 0 ? x->shape[0] : x->shape[1];
      int same = prev_chord >= 0 && e[prev_chord].c->id == ch->id && e[prev_chord].shape[0] == x->shape[0] &&
                 e[prev_chord].shape[1] == x->shape[1];
      if (sh >= 0 && !same) mask |= M_PANEL;
      if (moved && (mask & M_PANEL)) flags = 1;
      prev_chord = (int32_t)i;
      chord_id = (int32_t)ch->id;
      s->techniques |= techniques_of(any) | (ch->nnotes == 2 ? SB_T_DOUBLE_STOP : SB_T_CHORD);
      if (fifth && ch->nnotes <= 3) s->techniques |= SB_T_POWER_CHORD;
      if (barre) s->techniques |= SB_T_BARRE_CHORD;
      if (open && ch->nnotes > 2) s->techniques |= SB_T_OPEN_CHORD;
      if (sec >= 0 && !use_add(s, level, (uint32_t)sec, ch->id)) {
        fail(s, "out of memory");
        goto out;
      }
    }
    s->techniques |= techniques_of(mask);
    if (!(mask & M_IGNORE)) count_played[x->pi]++;
    if (sec >= 0) {
      if (x->n) s->sec[sec].strings[level] |= (uint8_t)(1u << x->n->s);
      else
        for (uint32_t j = 0; j < x->c->nnotes; j++) s->sec[sec].strings[level] |= (uint8_t)(1u << x->part->cnotes[x->c->note0 + j].s);
    }
    if ((int)level == s->it[x->pi].max) {
      if (x->n) s->notes++;
      else s->chords++;
      if (mask & M_IGNORE) s->ignored++;
    }
    if (x->t < s->first_note) s->first_note = x->t;
    if (x->t + x->sus > s->last_note) s->last_note = x->t + x->sus;

    size_t start = b->n;
    w32(b, mask), w32(b, flags), w32(b, 0), wf(b, x->t);
    w8(b, string), w8(b, fret), w8(b, an->fret), w8(b, an->width);
    w32(b, (uint32_t)chord_id), w32(b, (uint32_t)cn_id), w32(b, x->pi), w32(b, x->pi);  // a phrase an iteration
    w16(b, x->shape[0]), w16(b, x->shape[1]);
    w16(b, i + 1 < nev && e[i + 1].pi == x->pi ? (int)i + 1 : -1);
    w16(b, i && e[i - 1].pi == x->pi ? (int)i - 1 : -1);
    w16(b, parent);
    w8(b, slide), w8(b, unpitched), w8(b, left), w8(b, tap), w8(b, pick), w8(b, slap), w8(b, pluck);
    w16(b, vibrato), wf(b, x->sus), wf(b, max_bend);
    w32(b, x->n ? nb : 0);
    for (uint32_t k = 0; x->n && k < nb; k++) wf(b, pt[k]), wf(b, pv[k]), w32(b, 0);
    if (!b->bad) put32(b->d + start + 8, (uint32_t)crc32(0, b->d + start, (uInt)(b->n - start)));
  }
  // the phrases' average notes (a phrase an iteration: its own), the iterations' notes
  w32(b, s->nit);
  for (uint32_t i = 0; i < s->nit; i++) wf(b, (float)count[i]);
  w32(b, s->nit);
  for (uint32_t i = 0; i < s->nit; i++) w32(b, count_played[i]);
  w32(b, s->nit);
  for (uint32_t i = 0; i < s->nit; i++) w32(b, count[i]);
  for (uint32_t i = 0; i < s->nit; i++) {
    iter *it = &s->it[i];
    int hard = it->max, easy = (int)rint(hard / 4.0), medium = (int)rint(hard / 2.0);  // halves to even
    if ((int)level == hard) it->top = it->at[2] = count[i];
    if ((int)level == easy) it->at[0] = count[i];
    if ((int)level == medium) it->at[1] = count[i];
  }
  ok = 1;
out:
  free(parts), free(e), free(a), free(h[0]), free(h[1]), free(count), free(count_played);
  return ok;
}

// --- the song's frame: beats, iterations, sections ------------------------------------------------------------------
static int frame_build(song *s, const sb_sng_opts *o) {
  const sb_chart *c = s->c;
  // where the notes are
  float first = 3.4e38f, end = 0;
  for (uint32_t i = 0; i <= c->nphrases; i++) {
    for (uint32_t l = 0; l < (i ? c->phrases[i - 1].nlevels : 1); l++) {
      const sb_part *p = i ? &c->phrases[i - 1].levels[l] : &c->flat;
      if (!i && c->nphrases) break;  // with phrases the plain lists are not played
      for (uint32_t k = 0; k < p->nnotes; k++) {
        float t = (float)p->notes[k].t;
        if (t < first) first = t;
        if (t + p->notes[k].sus > end) end = t + p->notes[k].sus;
      }
      for (uint32_t k = 0; k < p->nchords; k++) {
        float t = (float)p->chords[k].t;
        if (t < first) first = t;
        if (t > end) end = t;
        for (uint32_t j = 0; j < p->chords[k].nnotes; j++)
          if (t + p->cnotes[p->chords[k].note0 + j].sus > end) end = t + p->cnotes[p->chords[k].note0 + j].sus;
      }
    }
  }
  if (first > end) return fail(s, "the arrangement has no notes and no chords");
  float length = o && o->song_length > 0 && o->song_length < 86400 ? (float)o->song_length : 0;
  if (c->nbeats && (float)c->beats[c->nbeats - 1].t > length && !(o && o->song_length > 0)) length = (float)c->beats[c->nbeats - 1].t;
  if (end > length) length = end;
  if (c->nphrases && (float)c->phrases[c->nphrases - 1].t0 >= length) length = (float)c->phrases[c->nphrases - 1].t0 + 1;
  if (!(length > 0)) length = 1;
  s->length = length;

  // beats: the chart's, else four to a measure at 120 a minute
  s->nbeats = c->nbeats ? c->nbeats : (uint32_t)(length * 2) + 1;
  if (!c->nbeats) s->made |= SB_MADE_BEATS;
  if (!(s->beats = calloc(s->nbeats, sizeof *s->beats))) return fail(s, "out of memory");
  int measure = 0, in = 0, seen = -1;
  for (uint32_t i = 0; i < s->nbeats; i++) {
    int m = c->nbeats ? c->beats[i].measure : i % 4 ? -1 : (int)(i / 4) + 1;
    // a measure starts where its number first shows (some writers repeat it on the measure's other beats)
    if (m >= 0 && m != seen) measure = seen = m, in = 0;
    else if (i) in++;
    s->beats[i] = (beat){c->nbeats ? (float)c->beats[i].t : (float)i * 0.5f, measure, in};
  }
  float start = s->beats[0].t < first ? s->beats[0].t : first;

  // iterations
  if (c->nphrases) {
    s->nit = c->nphrases;
    if (!(s->it = calloc(s->nit, sizeof *s->it))) return fail(s, "out of memory");
    for (uint32_t i = 0; i < s->nit; i++) {
      const sb_phrase *p = &c->phrases[i];
      iter *it = &s->it[i];
      it->t = (float)p->t0, it->src = p, it->max = 0;
      if (i && it->t <= s->it[i - 1].t) return fail(s, "two phrases start at %.3f", p->t0);
      uint32_t events = 0;
      for (uint32_t l = 0; l < p->nlevels; l++) {
        if (p->levels[l].difficulty <= p->max_difficulty) it->max = p->levels[l].difficulty;
        events += p->levels[l].nnotes + p->levels[l].nchords;
      }
      if (!i && !events) snprintf(it->name, sizeof it->name, "COUNT");
      else if (i == s->nit - 1 && !events) snprintf(it->name, sizeof it->name, "END");
      else snprintf(it->name, sizeof it->name, "p%u", i);
    }
  } else {
    // COUNT to the first section or note, an iteration at each section from there, END after the last note
    s->made |= SB_MADE_ITERATIONS;
    float body = c->nsections && (float)c->sections[0].t <= first ? (float)c->sections[0].t : first, stop = 0;
    if (body < start) body = start;
    for (uint32_t i = 0; i < s->nbeats && !stop; i++)
      if (s->beats[i].t > end && s->beats[i].t < length) stop = s->beats[i].t;
    if (!(s->it = calloc(c->nsections + 3, sizeof *s->it))) return fail(s, "out of memory");
    if (body > start) s->it[s->nit].t = start, snprintf(s->it[s->nit++].name, 32, "COUNT");
    s->it[s->nit].t = body, snprintf(s->it[s->nit].name, 32, "p%u", s->nit), s->nit++;
    for (uint32_t i = 0; i < c->nsections; i++) {
      float t = (float)c->sections[i].t;
      if (t <= body || t >= length) continue;
      if (t > end) stop = 0;  // a section after the last note ends the song's body itself
      s->it[s->nit].t = t, snprintf(s->it[s->nit].name, 32, t > end ? "END" : "p%u", s->nit), s->nit++;
    }
    if (stop > s->it[s->nit - 1].t) s->it[s->nit].t = stop, snprintf(s->it[s->nit++].name, 32, "END");
  }
  for (uint32_t i = 0; i < s->nit; i++) {
    s->it[i].end = i + 1 < s->nit ? s->it[i + 1].t : length;
    if ((uint32_t)s->it[i].max + 1 > s->nlevels) s->nlevels = (uint32_t)s->it[i].max + 1;
  }

  // sections: the chart's, else one over the notes
  s->nsec = c->nsections ? c->nsections : 1;
  if (!(s->sec = calloc(s->nsec, sizeof *s->sec))) return fail(s, "out of memory");
  uint32_t n = 0;
  for (uint32_t i = 0; i < c->nsections; i++) {
    if ((float)c->sections[i].t >= length) continue;
    memcpy(s->sec[n].name, c->sections[i].name, 32);
    s->sec[n].number = c->sections[i].number, s->sec[n++].t = (float)c->sections[i].t;
  }
  if (!n) snprintf(s->sec[0].name, 32, "riff"), s->sec[0].number = 1, s->sec[0].t = first, n = 1, s->made |= SB_MADE_SECTION;
  s->nsec = n;
  for (uint32_t i = 0; i < n; i++) {
    sect *x = &s->sec[i];
    x->end = i + 1 < n ? s->sec[i + 1].t : length;
    x->first = iter_of(s, x->t);
    uint32_t last = x->first;
    while (last + 1 < s->nit && s->it[last + 1].t < x->end) last++;
    x->last = last;
  }
  return 1;
}

// The tones named by the chart's changes as ids 0 to 3, the starting tone first; a change to a fifth tone is left out
static uint32_t tones_build(const sb_chart *c, char names[4][64], sb_sng_tone *out) {
  uint32_t n = 0, nout = 0;
  if (c->tone_base[0] && c->ntones) memcpy(names[n++], c->tone_base, 64);
  for (uint32_t i = 0; i < c->ntones; i++) {
    uint32_t id = 0;
    while (id < n && strcmp(names[id], c->tones[i].name)) id++;
    if (id == n) {
      if (n == 4) continue;
      memcpy(names[n++], c->tones[i].name, 64);
    }
    if (out) out[nout] = (sb_sng_tone){c->tones[i].t, id};
    nout++;
  }
  return nout;
}

static int by_use(const void *a, const void *b) {
  const sb_sng_use *x = a, *y = b;
  if (x->level != y->level) return x->level - y->level;
  if (x->section != y->section) return x->section - y->section;
  return x->chord < y->chord ? -1 : x->chord > y->chord;
}

void sb_sng_facts_free(sb_sng_facts *f) {
  if (!f) return;
  free(f->phrases), free(f->iters), free(f->sections), free(f->templates), free(f->uses), free(f->changes);
  memset(f, 0, sizeof *f);
}

static int32_t midi_of(const song *s, const sb_template *t, int string) {
  static const int8_t open[SB_STRINGS] = {40, 45, 50, 55, 59, 64};
  if (t->frets[string] < 0) return -1;
  return open[string] - (s->bass ? 12 : 0) + s->c->tuning[string] + t->frets[string];
}

static int facts_fill(song *s, sb_sng_facts *f, uint32_t nchanges) {
  const sb_chart *c = s->c;
  memset(f, 0, sizeof *f);
  f->bass = s->bass, f->strings = c->strings, f->capo = c->capo, f->cents = c->cents;
  memcpy(f->tuning, c->tuning, sizeof f->tuning);
  f->max_difficulty = (int)s->nlevels - 1;
  f->song_length = s->length, f->first_beat = s->beats[0].t, f->first_note = s->first_note, f->last_note = s->last_note;
  f->notes = s->notes, f->chords = s->chords, f->ignored = s->ignored, f->techniques = s->techniques, f->made = s->made;
  f->phrases = calloc(s->nit, sizeof *f->phrases), f->iters = calloc(s->nit, sizeof *f->iters);
  f->sections = calloc(s->nsec, sizeof *f->sections), f->templates = calloc(c->ntemplates + 1, sizeof *f->templates);
  f->changes = calloc(nchanges + 1, sizeof *f->changes);
  if (!f->phrases || !f->iters || !f->sections || !f->templates || !f->changes) return 0;
  f->nphrases = f->niters = s->nit, f->nsections = s->nsec, f->ntemplates = c->ntemplates;
  for (uint32_t i = 0; i < s->nit; i++) {
    const iter *it = &s->it[i];
    int hard = it->max;
    memcpy(f->phrases[i].name, it->name, 32);
    f->phrases[i].max_difficulty = hard, f->phrases[i].iterations = 1;
    f->iters[i] = (sb_sng_iter){i, it->t, it->end, hard, (int)rint(hard / 4.0), (int)rint(hard / 2.0), hard, it->top};
    f->notes_easy += it->at[0], f->notes_medium += it->at[1], f->notes_hard += it->at[2];
  }
  f->points_per_note = f->notes_hard ? 100000.0 / f->notes_hard : 0;
  for (uint32_t i = 0; i < s->nsec; i++) {
    const sect *x = &s->sec[i];
    memcpy(f->sections[i].name, x->name, 32);
    f->sections[i].number = x->number, f->sections[i].t = x->t, f->sections[i].end = x->end;
    f->sections[i].first_iter = x->first, f->sections[i].last_iter = x->last;
  }
  for (uint32_t i = 0; i < c->ntemplates; i++) {
    const sb_template *t = &c->templates[i];
    sb_sng_template *x = &f->templates[i];
    memcpy(x->name, t->name, 32), memcpy(x->display, t->display, 32);
    memcpy(x->frets, t->frets, SB_STRINGS), memcpy(x->fingers, t->fingers, SB_STRINGS);
    x->arp = t->arp;
    for (int k = 0; k < SB_STRINGS; k++) x->midi[k] = midi_of(s, t, k);
  }
  qsort(s->uses, s->nuses, sizeof *s->uses, by_use);
  uint32_t keep = 0;
  for (uint32_t i = 0; i < s->nuses; i++)
    if (!keep || by_use(&s->uses[i], &s->uses[keep - 1])) s->uses[keep++] = s->uses[i];
  f->uses = s->uses, f->nuses = keep, s->uses = 0;
  f->nchanges = tones_build(c, f->tones, f->changes);
  for (f->ntones = 0; f->ntones < 4 && f->tones[f->ntones][0]; f->ntones++) {}
  return 1;
}

uint8_t *sb_sng_payload(const sb_chart *c, const sb_sng_opts *o, sb_sng_facts *facts, size_t *out_n, char *err, size_t errlen) {
  song s = {.c = c, .err = err, .errlen = errlen, .first_note = 3.4e38f};
  buf b = {0}, lv = {0};
  char tone_names[4][64] = {{0}};
  uint8_t *out = 0;
  if (errlen) err[0] = 0;
  if (facts) memset(facts, 0, sizeof *facts);
  s.bass = o && o->bass >= 0 ? o->bass != 0 : c->strings == 4 || strcasestr(c->name, "bass") != 0;
  if (!frame_build(&s, o)) goto out;
  for (uint32_t l = 0; l < s.nlevels; l++)
    if (!level_build(&s, l, &lv)) goto out;

  // beats
  w32(&b, s.nbeats);
  for (uint32_t i = 0; i < s.nbeats; i++) {
    const beat *x = &s.beats[i];
    // the iteration that started before the beat: a beat on an iteration's first moment still counts to the one before
    uint32_t pi = iter_of(&s, x->t);
    if (pi && s.it[pi].t == x->t) pi--;
    wf(&b, x->t), w16(&b, x->measure), w16(&b, x->beat), w32(&b, pi);
    w32(&b, x->beat ? 0 : x->measure % 2 ? 1 : 3);
  }
  // phrases: one an iteration
  w32(&b, s.nit);
  for (uint32_t i = 0; i < s.nit; i++) wz(&b, 4), w32(&b, (uint32_t)s.it[i].max), w32(&b, 1), wstr(&b, s.it[i].name, 32);
  // chord templates
  w32(&b, c->ntemplates);
  for (uint32_t i = 0; i < c->ntemplates; i++) {
    const sb_template *t = &c->templates[i];
    w32(&b, (t->arp ? 1u : 0) | (strstr(t->display, "-nop") ? 2u : 0));
    w(&b, t->frets, SB_STRINGS), w(&b, t->fingers, SB_STRINGS);
    for (int k = 0; k < SB_STRINGS; k++) w32(&b, (uint32_t)midi_of(&s, t, k));
    wstr(&b, t->name, 32);
  }
  w32(&b, s.ncn), w(&b, s.cn, (size_t)s.ncn * CN_SIZE);
  w32(&b, 0);  // vocals
  w32(&b, s.nit);
  for (uint32_t i = 0; i < s.nit; i++) {
    int hard = s.it[i].max;
    w32(&b, i), wf(&b, s.it[i].t), wf(&b, s.it[i].end);
    w32(&b, (uint32_t)rint(hard / 4.0)), w32(&b, (uint32_t)rint(hard / 2.0)), w32(&b, (uint32_t)hard);
  }
  w32(&b, 0), w32(&b, 0), w32(&b, 0), w32(&b, 0);  // phrase extra info, linked difficulties, actions, events
  uint32_t nchanges = tones_build(c, tone_names, 0);
  sb_sng_tone *changes = calloc(nchanges + 1, sizeof *changes);
  if (!changes) {
    fail(&s, "out of memory");
    goto out;
  }
  memset(tone_names, 0, sizeof tone_names);
  tones_build(c, tone_names, changes);
  w32(&b, nchanges);
  for (uint32_t i = 0; i < nchanges; i++) wf(&b, (float)changes[i].t), w32(&b, changes[i].id);
  free(changes);
  w32(&b, 0);  // DNAs
  w32(&b, s.nsec);
  for (uint32_t i = 0; i < s.nsec; i++) {
    const sect *x = &s.sec[i];
    wstr(&b, x->name, 32), w32(&b, (uint32_t)x->number), wf(&b, x->t), wf(&b, x->end), w32(&b, x->first), w32(&b, x->last);
    w(&b, x->strings, sizeof x->strings);
  }
  w32(&b, s.nlevels), w(&b, lv.d, lv.n);
  // metadata
  uint32_t top = 0;
  for (uint32_t i = 0; i < s.nit; i++) top += s.it[i].top;
  wd(&b, 100000), wd(&b, top), wd(&b, top - s.ignored), wd(&b, top ? 100000.0 / top : 0);
  // the first beat's length from the times as written (the difference of the 32-bit ones is a little off)
  double beat1 = c->nbeats > 1 ? c->beats[1].t - c->beats[0].t : s.nbeats > 1 ? 0.5 : 0;
  wf(&b, (float)beat1), wf(&b, s.beats[0].t);
  w8(&b, c->capo > 0 ? c->capo : -1);
  wstr(&b, "1-1-25 0:00", 32);
  w16(&b, 1), wf(&b, s.length);
  w32(&b, SB_STRINGS);
  for (int k = 0; k < SB_STRINGS; k++) w16(&b, c->tuning[k]);
  wf(&b, s.first_note), wf(&b, s.first_note), w32(&b, s.nlevels - 1);
  if (b.bad || lv.bad) {
    fail(&s, "out of memory, or a song file past %u MiB", MAX_PAYLOAD >> 20);
    goto out;
  }
  if (facts && !facts_fill(&s, facts, nchanges)) {
    sb_sng_facts_free(facts);
    fail(&s, "out of memory");
    goto out;
  }
  out = b.d, b.d = 0, *out_n = b.n;
out:
  free(b.d), free(lv.d), free(s.it), free(s.sec), free(s.beats), free(s.cn), free(s.cn_slot), free(s.uses);
  return out;
}

uint8_t *sb_sng_build(const sb_chart *c, int platform, const sb_sng_opts *o, sb_sng_facts *facts, size_t *out_n, char *err,
                      size_t errlen) {
  size_t n = 0;
  if (platform != SB_MAC && platform != SB_PC) {
    if (errlen) snprintf(err, errlen, "no such platform");
    return 0;
  }
  uint8_t *p = sb_sng_payload(c, o, facts, &n, err, errlen);
  if (!p) return 0;
  uint8_t *f = sb_sng_pack(p, n, platform, out_n);
  free(p);
  if (!f) {
    if (errlen) snprintf(err, errlen, "out of memory");
    if (facts) sb_sng_facts_free(facts);
  }
  return f;
}
