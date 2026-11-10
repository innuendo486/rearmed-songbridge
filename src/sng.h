// A chart (chart.h) as the game's own song file (.sng) for one arrangement, and what a manifest for that
// arrangement says about it, counted from the same data so that the two agree.
#ifndef SB_SNG_H
#define SB_SNG_H

#include <stddef.h>
#include <stdint.h>

#include "chart.h"
#include "psarc.h"  // SB_MAC, SB_PC

// sb_sng_facts.techniques: what the arrangement asks of the player, at any difficulty
enum {
  SB_T_BEND = 1 << 0,
  SB_T_SLIDE = 1 << 1,
  SB_T_UNPITCHED_SLIDE = 1 << 2,
  SB_T_HARMONIC = 1 << 3,
  SB_T_PINCH_HARMONIC = 1 << 4,
  SB_T_HAMMER_ON = 1 << 5,
  SB_T_PULL_OFF = 1 << 6,
  SB_T_PALM_MUTE = 1 << 7,
  SB_T_FRET_HAND_MUTE = 1 << 8,
  SB_T_TREMOLO = 1 << 9,
  SB_T_VIBRATO = 1 << 10,
  SB_T_SUSTAIN = 1 << 11,
  SB_T_TAP = 1 << 12,
  SB_T_SLAP = 1 << 13,
  SB_T_PLUCK = 1 << 14,
  SB_T_ACCENT = 1 << 15,
  SB_T_CHORD = 1 << 16,        // any chord of three notes or more
  SB_T_DOUBLE_STOP = 1 << 17,  // a chord of two notes
  SB_T_POWER_CHORD = 1 << 18,  // root and fifth, with or without the octave
  SB_T_BARRE_CHORD = 1 << 19,  // one finger on two strings or more
  SB_T_OPEN_CHORD = 1 << 20,   // a chord of three notes or more with an open string
  SB_T_ARPEGGIO = 1 << 21,
  SB_T_OPEN_NOTE = 1 << 22,    // a single note on an open string
};

// sb_sng_facts.made: what the chart did not have and the song file needs, made up (see the head of sng.c)
enum {
  SB_MADE_BEATS = 1 << 0,       // no beats: four to a measure at 120 a minute
  SB_MADE_SECTION = 1 << 1,     // no sections (in the song): one over the notes
  SB_MADE_ITERATIONS = 1 << 2,  // no phrases: one difficulty, cut at the sections
  SB_MADE_ANCHOR = 1 << 3,      // a difficulty whose first note had no anchor yet: one at that note
};

typedef struct sb_sng_phrase {
  char name[32];
  int max_difficulty;
  uint32_t iterations;
} sb_sng_phrase;

typedef struct sb_sng_iter {
  uint32_t phrase;
  double t, end;
  int max_difficulty;
  int easy, medium, hard;  // the levels the game's three fixed difficulties play it at
  uint32_t notes;          // notes and chords at max_difficulty
} sb_sng_iter;

typedef struct sb_sng_section {
  char name[32];
  int number;
  double t, end;
  uint32_t first_iter, last_iter;
} sb_sng_section;

typedef struct sb_sng_template {
  char name[32], display[32];
  int8_t frets[SB_STRINGS], fingers[SB_STRINGS];
  int32_t midi[SB_STRINGS];  // the pitch of each string played, -1: not played
  uint8_t arp;
} sb_sng_template;

// Template `chord` is played in section `section` at difficulty `level`: by level, section, chord, each once
typedef struct sb_sng_use {
  uint8_t level;
  uint16_t section;
  uint32_t chord;
} sb_sng_use;

typedef struct sb_sng_tone {
  double t;
  uint32_t id;  // into sb_sng_facts.tones
} sb_sng_tone;

typedef struct sb_sng_facts {
  int bass, strings, capo;
  int tuning[SB_STRINGS];
  double cents;
  int max_difficulty;  // the song file has max_difficulty + 1 levels
  double song_length, first_beat;
  double first_note, last_note;  // the first note's time and the end of the last (with its sustain), at any level
  // Counted over the iterations, each at its hardest: single notes; chords; the ones marked ignore among both
  uint32_t notes, chords, ignored;
  uint32_t notes_easy, notes_medium, notes_hard;  // notes and chords at each iteration's easy, medium, hard level
  double points_per_note;                         // 100000 / notes_hard
  uint32_t techniques;                            // SB_T_*
  uint32_t made;                                  // SB_MADE_*
  sb_sng_phrase *phrases;
  sb_sng_iter *iters;
  sb_sng_section *sections;
  sb_sng_template *templates;  // the chart's, in its order: a chord's id is its place here
  sb_sng_use *uses;
  sb_sng_tone *changes;
  uint32_t nphrases, niters, nsections, ntemplates, nuses, nchanges;
  // The tones the changes name, at most four (the song file's ids 0 to 3: the manifest's Tone_A to Tone_D): the one
  // the arrangement starts with first
  char tones[4][64];
  uint32_t ntones;
} sb_sng_facts;

typedef struct sb_sng_opts {
  double song_length;  // the audio's length in seconds; 0: to the last beat or note
  int bass;            // 1 bass, 0 guitar, -1: by the chart's name and tuning ("bass" in the name, or four strings)
} sb_sng_opts;

// The song file of chart c for `platform` (SB_MAC or SB_PC): malloc'd, its size in *out_n. The size is the same on
// both platforms and the bytes are the same every time. o may be 0 (song length from the chart, instrument by its
// name). When facts is not 0 it is filled (free its lists with sb_sng_facts_free). 0 with the reason in err (when
// errlen > 0) when the chart cannot be a song file. From any thread.
uint8_t *sb_sng_build(const sb_chart *c, int platform, const sb_sng_opts *o, sb_sng_facts *facts, size_t *out_n, char *err,
                      size_t errlen);
void sb_sng_facts_free(sb_sng_facts *f);

// The same before it is packed: the song file's plain payload
uint8_t *sb_sng_payload(const sb_chart *c, const sb_sng_opts *o, sb_sng_facts *facts, size_t *out_n, char *err, size_t errlen);
// A payload as a song file: compressed, encrypted with the platform's key, the signature zero. 0: out of memory.
uint8_t *sb_sng_pack(const uint8_t *payload, size_t n, int platform, size_t *out_n);
// A song file's payload (malloc'd, its size in *out_n), 0 when it is not a song file of that platform
uint8_t *sb_sng_unpack(const uint8_t *file, size_t n, int platform, size_t *out_n);

#endif
