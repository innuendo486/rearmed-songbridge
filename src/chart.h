// One arrangement of a feedpak (arrangements/<id>.json, the "wire format": feedpak-spec section 6) as plain C: the
// notes and chords of the hardest play, the same per phrase and difficulty when the file has that, and the song's
// beats and sections. Feedpaks come in many forms, so the reader works with what it gets: what it cannot use it
// drops or puts right, and counts (sb_repairs). A chart that sb_chart_parse returns has its times finite and in
// order, its strings, frets and chord numbers in range.
#ifndef SB_CHART_H
#define SB_CHART_H

#include <stddef.h>
#include <stdint.h>

enum { SB_STRINGS = 6, SB_MAX_FRET = 24, SB_MAX_DIFFICULTY = 35 };
// At most this many notes and chords together at one difficulty (the song file counts them in 16 bits)
#define SB_MAX_EVENTS 32767u

// sb_note.flags
enum {
  SB_N_HAMMER = 1 << 0,     // ho
  SB_N_PULL = 1 << 1,       // po
  SB_N_HARMONIC = 1 << 2,   // hm
  SB_N_PINCH = 1 << 3,      // hp
  SB_N_PALM_MUTE = 1 << 4,  // pm
  SB_N_MUTE = 1 << 5,       // mt
  SB_N_VIBRATO = 1 << 6,    // vb
  SB_N_TREMOLO = 1 << 7,    // tr
  SB_N_ACCENT = 1 << 8,     // ac
  SB_N_TAP = 1 << 9,        // tp
  SB_N_LINK = 1 << 10,      // ln
  SB_N_FRET_MUTE = 1 << 11, // fhm
  SB_N_PLUCK = 1 << 12,     // plk
  SB_N_SLAP = 1 << 13,      // slp
  SB_N_IGNORE = 1 << 14,    // ig
};

typedef struct sb_bend {
  float t, v;  // seconds from the note's start; semitones
} sb_bend;

typedef struct sb_note {
  double t;                // seconds; a chord's note has its chord's
  float sus;               // seconds, 0: none
  float bn;                // the bend's peak in semitones, 0: none
  int8_t s, f;             // string (0 the lowest), fret (0 open)
  int8_t sl, slu;          // the fret slid to, with and without pitch; -1: no slide
  int8_t rh, pkd, fg, bt;  // right hand, pick direction, fret-hand finger (-1: not given); the bend's intent (0-4)
  uint32_t flags;          // SB_N_*
  uint32_t bend0, nbends;  // its bend curve (bnv): sb_part.bends[bend0 .. bend0 + nbends), none when nbends is 0
} sb_note;

typedef struct sb_chord {
  double t;
  uint32_t id;             // its template
  uint8_t hd;              // high density: drawn without its fret numbers
  uint32_t note0, nnotes;  // sb_part.cnotes[note0 .. note0 + nnotes): one a string, never none
} sb_chord;

typedef struct sb_anchor {
  double t;
  int8_t fret, width;  // 1 to 24; 1 or more
} sb_anchor;

typedef struct sb_shape {
  double t0, t1;  // t1 >= t0
  uint32_t id;    // its template
  uint8_t arp;
} sb_shape;

// The notes of one stretch of the song at one difficulty: each list in the order of its times
typedef struct sb_part {
  int difficulty;
  sb_note *notes;
  sb_chord *chords;
  sb_note *cnotes;  // the chords' notes
  sb_bend *bends;   // the notes' bend curves
  sb_anchor *anchors;
  sb_shape *shapes;
  uint32_t nnotes, nchords, ncnotes, nbends, nanchors, nshapes;
} sb_part;

typedef struct sb_phrase {
  double t0, t1;
  int max_difficulty;  // 0 to SB_MAX_DIFFICULTY
  sb_part *levels;     // by difficulty, each at most once
  uint32_t nlevels;
} sb_phrase;

typedef struct sb_template {
  char name[32], display[32];  // display is name when the file gives none
  int8_t frets[SB_STRINGS], fingers[SB_STRINGS];  // -1: string not played; no finger
  uint8_t arp;
} sb_template;

typedef struct sb_beat {
  double t;
  int measure;  // the measure's number on its first beat, else -1
} sb_beat;

typedef struct sb_section {
  double t;
  char name[32];
  int number;
} sb_section;

typedef struct sb_tone {
  double t;
  char name[64];
} sb_tone;

// What the reader dropped or put right, as counts
enum {
  SB_R_NOTES_DROPPED,     // no usable time, string or fret; a string the tuning has not
  SB_R_CHORDS_DROPPED,    // no usable time, or no usable note left
  SB_R_FRETS_CLAMPED,     // a fret or slide above 24, taken as 24
  SB_R_TEMPLATES_MADE,    // a chord named a template that is not there: one made from its notes
  SB_R_ITEMS_SKIPPED,     // an anchor, hand shape, beat, section, phrase, level, bend point or tone that cannot be used
  SB_R_VALUES_DEFAULTED,  // a value of the wrong kind where the format has a default
  SB_R_LISTS_SORTED,      // a list that was not in the order of its times
  SB_R_ANCHORS_MADE,      // a difficulty with notes and no anchors: anchors made, one a hand position
  SB_R_SHAPES_MADE,       // a difficulty with chords and no hand shapes: one made for each run of a chord
  SB_R_TIMELINE_IGNORED,  // a song timeline that is not JSON: the arrangement's own beats and sections stay
  SB_R_COUNT
};
typedef struct sb_repairs {
  uint32_t n[SB_R_COUNT];
  uint32_t total;
  char text[200];  // the same for a log line: "3 notes dropped, 1 template made", "" when nothing was
} sb_repairs;
// "notes dropped", ...
const char *sb_repair_name(int which);

typedef struct sb_chart {
  char name[64];
  int strings;             // the tuning's length: 1 to 6 (6 when the file has no tuning)
  int tuning[SB_STRINGS];  // semitones from standard, lowest string first; 0 past `strings`
  int capo;
  double cents;
  sb_part flat;  // the hardest play
  sb_template *templates;
  sb_phrase *phrases;  // by t0, no two at one time; none: one difficulty
  sb_beat *beats;      // times rising
  sb_section *sections;
  char tone_base[64];  // the tone the arrangement starts with, "" when not named
  sb_tone *tones;      // the changes
  uint32_t ntemplates, nphrases, nbeats, nsections, ntones;
  sb_repairs repairs;
} sb_chart;

// The arrangement in json[0..n) (comments and trailing commas allowed) with the beats and sections of the song
// timeline in timeline[0..tn) (song_timeline.json; 0: the ones in the arrangement itself). 0 with the reason in err
// (when errlen > 0) only when there is nothing to play or the song file cannot hold it: not JSON, a tuning of more
// than six strings, no usable note or chord, more than SB_MAX_EVENTS notes and chords at one difficulty. A fret
// above 24 is clamped to 24, not refused. Free with sb_chart_free. From any thread.
sb_chart *sb_chart_parse(const char *json, size_t n, const char *timeline, size_t tn, char *err, size_t errlen);
void sb_chart_free(sb_chart *c);

#endif
