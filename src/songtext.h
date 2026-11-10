// The text entries of a Rocksmith 2014 song archive, made from plain facts about the song: the arrangements'
// manifests (.json), the song list manifest (.hsan), the entity block (.xblock), the aggregate graph (.nt), the
// showlights, the two flat models and the AppID. Also the names that tie an archive together: the song key, the
// arrangements' persistent IDs, the Wwise event names and the path of every entry on each platform.
//
// Everything here is a pure function of its input: the same facts give the same bytes (and so the same sizes) in any
// process, on any thread, under any locale. Nothing is kept between calls.
//
// Song packages come in many forms, so whatever the facts are, texts are made of them: a missing or impossible value
// is replaced by the default its field names, never refused. Only a song without arrangements is no song.
#ifndef SB_SONGTEXT_H
#define SB_SONGTEXT_H

#include <stddef.h>
#include <stdint.h>

#define SB_KEY_MAX 32   // a song key's longest length
#define SB_NAME_MAX 24  // an arrangement name's
#define SB_MAX_ARR 256  // arrangements in one song: more are left out
#define SB_APPID "248750"  // the free DLC's: the game lists a song only with an AppID of its DLC list

// What an arrangement is: the game's own numbers (the manifest's ArrangementType)
enum { SB_ARR_LEAD = 0, SB_ARR_RHYTHM = 1, SB_ARR_COMBO = 2, SB_ARR_BASS = 3, SB_ARR_VOCALS = 4 };
// The path it is listed under (the manifest's routeMask)
enum { SB_PATH_LEAD = 1, SB_PATH_RHYTHM = 2, SB_PATH_BASS = 4 };

typedef struct sb_man_phrase {
  const char *name;    // 0: ""
  int max_difficulty;  // its highest difficulty level
} sb_man_phrase;

typedef struct sb_phrase_iter {
  uint32_t phrase;  // index into the phrases (one that is not there: the first)
  double time;      // where it starts, seconds; it ends where the next starts, the last at the song's end
} sb_phrase_iter;

typedef struct sb_man_section {
  const char *name;  // the game's section name ("intro", "verse", "chorus", "solo", "noguitar", ...); 0: ""
  double time;       // where it starts; it ends where the next starts, the last at the song's end
} sb_man_section;

typedef struct sb_chord_template {
  int id;            // its index among the song file's chord templates
  const char *name;  // "Bmin"; 0: ""
  int8_t fingers[6], frets[6];  // per string, low E first; -1: not played
} sb_chord_template;

// "At this difficulty level, this phrase iteration uses these": chord template ids (Chords) or technique ids
// (Techniques). A list of these is in ascending order of (difficulty, iteration), each pair once: one that is not
// after the one before it is left out.
typedef struct sb_level_ids {
  uint32_t difficulty, iteration;
  const int *id;
  uint32_t n;
} sb_level_ids;

// The techniques an arrangement uses (the manifest's ArrangementProperties): 1 when it has any. All default to 0.
typedef struct sb_arr_props {
  uint8_t barre_chords, bass_pick, bends, double_stops, drop_d_power, fifths_and_octaves, finger_picking,
    fret_hand_mutes, harmonics, hopo, non_standard_chords, open_chords, palm_mutes, pick_direction, pinch_harmonics,
    power_chords, slap_pop, slides, sustain, syncopation, tapping, tremolo, two_finger_picking, unpitched_slides,
    vibrato;
} sb_arr_props;

// Facts about one arrangement. A struct that is all zeros is a valid, empty lead arrangement, and each field says
// what its zero means. Vocals use type, name, persistent_id and master_id only.
typedef struct sb_arr_info {
  int type;          // SB_ARR_* (anything else: a lead)
  // What its files are called after the key ("lead" in <key>_lead.json), from any text, a package's arrangement id
  // for one: its ASCII letters and digits in lower case ("Lead-2" is lead2). 0 or nothing left: its type ("lead",
  // "rhythm", "combo", "bass", "vocals"). A name an earlier arrangement has gets a number: lead, lead2, lead3.
  const char *name;
  // 32 hex digits: the player's scores are kept under it. 0, not that, or an earlier arrangement's: sb_song_pid of
  // the key and the name above.
  const char *persistent_id;
  int32_t master_id;          // MasterID_RDV. 0: from the persistent ID
  int path;                   // SB_PATH_*. Anything else: by type (lead: lead; rhythm, combo: rhythm; bass: bass)
  // A bonus or an alternate arrangement. One arrangement represents its path: the first on it that is neither;
  // a later one counts as an alternate whether it says so or not.
  uint8_t bonus, alternate;
  int tuning[6];              // semitones from standard, lowest string first (a bass: the last two 0)
  int capo;                   // fret; 0: none
  double cents;               // cent offset from A440
  double tempo;               // average, beats per minute. 0: 120
  double start_time;          // the first beat, seconds (the manifest's SongOffset is its negative)
  uint32_t notes_easy, notes_medium, notes_hard;  // note counts at the three mastery levels; hard: every note
  int max_difficulty;         // the highest difficulty level; the phrases' highest is used when that is more
  double diff_easy, diff_medium, diff_hard;  // SongDiffEasy/Med/Hard, 0 to 1. 0: not rated
  double dna_chords, dna_riffs, dna_solo;    // seconds of each DNA kind
  const float *density;       // DynamicVisualDensity, 20 values. 0: the game's own DLC's (see songtext.c)
  sb_arr_props props;
  const sb_man_phrase *phrase;
  uint32_t nphrases;
  const sb_phrase_iter *iter;  // in order of time
  uint32_t niters;
  const sb_man_section *section;  // in order of time
  uint32_t nsections;
  const sb_chord_template *chord;
  uint32_t nchords;
  const sb_level_ids *chords;  // Chords
  uint32_t nchord_levels;
  const sb_level_ids *techniques;  // Techniques
  uint32_t ntechnique_levels;
  // Tones: the game's own tone objects, each as JSON text ({"GearList": ..., "Key": ..., ...}), put in the manifest's
  // "Tones" as they are; what is in them is not looked at. One that is not a JSON object is left out. With none
  // (left): one clean tone that needs only gear of the base game (a bass tone for a bass).
  const char *const *tone;
  uint32_t ntones;
  const char *tone_base;  // Tone_Base: the "Key" of the tone the arrangement starts with. 0: the first tone's
  const char *tone_a, *tone_b, *tone_c, *tone_d;  // Tone_A to Tone_D: the keys of the tones it changes between. 0: ""
  const char *tone_multiplayer;                   // Tone_Multiplayer. 0: ""
} sb_arr_info;

typedef struct sb_song_info {
  // The song key: its ASCII letters and digits, the first SB_KEY_MAX of them. 0 or none of those:
  // sb_song_key(artist, title). sb_song_key_of tells which it came to.
  const char *key;
  const char *title, *artist, *album;  // UTF-8. 0 or "": "Unknown Title", "Unknown Artist", "Unknown Album"
  const char *title_sort, *artist_sort, *album_sort;  // 0: the name without a leading "The ", "A " or "An "
  int year;       // 0 when not known (written as 0)
  double length;  // seconds. Not known: 0 (so is one that is no length), and what ends with the song ends at 0
  const sb_arr_info *arr;
  uint32_t narr;  // at least 1; of more than SB_MAX_ARR the first SB_MAX_ARR are used
  uint32_t audio_id, preview_id;  // the names of the two .wem entries. 0: sb_song_audio_ids(key)
  const char *converted;  // LastConversionDateTime, "MM-dd-yy HH:mm". 0: a fixed date (the text must not change)
  // Where the showlights change, seconds, in order. 0: the sections of the first arrangement that has any; with
  // none of those either, the lights are set once at the start.
  const double *light_time;
  uint32_t nlights;
} sb_song_info;

// --- names -------------------------------------------------------------------------------------------------------
// A song key for artist and title (UTF-8, either may be 0): their ASCII letters and digits, at most 22, then 8 hex
// digits of a hash of both, so two songs get the same key only by a hash collision. Always the same for the same
// artist and title; never empty; starts with a letter. Songs without artist and title all get one key: give them
// something of their own (a file name) as the title.
void sb_song_key(const char *artist, const char *title, char key[SB_KEY_MAX + 1]);
// 1 when key can be one: 1 to SB_KEY_MAX ASCII letters and digits
int sb_song_key_ok(const char *key);
// The persistent ID of the arrangement called arr_name in the song `key`: 32 upper-case hex digits of a hash of both
// (neither's case matters), so a song packed again keeps its scores.
void sb_song_pid(const char *key, const char *arr_name, char pid[33]);
// The .wem ids of a song when it gives none: from a hash of the key, never 0, never the same two
void sb_song_audio_ids(const char *key, uint32_t *song, uint32_t *preview);
// The Wwise event that plays the song ("Play_<key>") or its preview ("Play_<key>_Preview"): its length, or -1 when
// it does not fit in len
int sb_song_event(const char *key, int preview, char *buf, size_t len);
// The Wwise ID of a name (an event's ID in a sound bank): FNV-1, 32 bits, of the name in lower case
uint32_t sb_wwise_id(const char *name);

// 1 when s can be made into an archive's texts, else 0 with the reason in err (when errlen > 0): there is no song,
// or it has no arrangements. Nothing else is refused.
int sb_song_check(const sb_song_info *s, char *err, size_t errlen);
// The song key, arrangement i's name and its persistent ID as the texts use them (given or derived): 1, or 0 when
// s is not valid or has no arrangement i
int sb_song_key_of(const sb_song_info *s, char key[SB_KEY_MAX + 1]);
int sb_song_arr_name(const sb_song_info *s, uint32_t i, char name[SB_NAME_MAX + 1]);
int sb_song_arr_pid(const sb_song_info *s, uint32_t i, char pid[33]);

// --- entries -----------------------------------------------------------------------------------------------------
// The kinds of entry in a song archive. The first eight are the texts made here.
enum {
  SB_E_APPID,       // appid.appid
  SB_E_FLAT_ROOT,   // flatmodels/rs/rsenumerable_root.flat
  SB_E_FLAT_SONG,   // flatmodels/rs/rsenumerable_song.flat
  SB_E_GRAPH,       // <key>_aggregategraph.nt
  SB_E_XBLOCK,      // gamexblocks/nsongs/<key>.xblock
  SB_E_HSAN,        // manifests/songs_dlc_<key>/songs_dlc_<key>.hsan
  SB_E_SHOWLIGHTS,  // songs/arr/<key>_showlights.xml
  SB_E_MANIFEST,    // manifests/songs_dlc_<key>/<key>_<arrangement>.json, one per arrangement
  SB_E_SNG,         // songs/bin/macos|generic/<key>_<arrangement>.sng, one per arrangement
  SB_E_ART64,       // gfxassets/album_art/album_<key>_64.dds
  SB_E_ART128,
  SB_E_ART256,
  SB_E_BNK,          // audio/mac|windows/song_<key>.bnk
  SB_E_BNK_PREVIEW,  // audio/mac|windows/song_<key>_preview.bnk
  SB_E_WEM,          // audio/mac|windows/<audio_id>.wem
  SB_E_WEM_PREVIEW,  // audio/mac|windows/<preview_id>.wem
  SB_E_KINDS
};

typedef struct sb_entry {
  int kind;      // SB_E_*
  uint32_t arr;  // the arrangement, for SB_E_MANIFEST and SB_E_SNG; else 0
} sb_entry;

// Every entry of the song's archive, in the order they go in: how many there are (14, and 2 per
// arrangement), the first cap of them in e; 0 when s is not valid. The same on both platforms.
uint32_t sb_song_entries(const sb_song_info *s, sb_entry *e, uint32_t cap);
// An entry's path in the archive for platform (SB_MAC or SB_PC, psarc.h): its length, or -1 (s is not valid, there
// is no such entry, or it does not fit in len). All in lower case, as the game asks for them.
int sb_song_entry_path(const sb_song_info *s, int platform, int kind, uint32_t arr, char *buf, size_t len);

// --- texts -------------------------------------------------------------------------------------------------------
// A text entry (the kinds up to SB_E_MANIFEST) for platform: its whole size, with the first cap bytes of it in buf
// (buf may be 0 when cap is 0: that asks for the size alone). -1 when s is not valid or kind is not a text.
int64_t sb_song_text(const sb_song_info *s, int platform, int kind, uint32_t arr, uint8_t *buf, size_t cap);
// The same, malloc'd (with a 0 after it), its size in *n; 0 when it cannot be made
uint8_t *sb_song_text_alloc(const sb_song_info *s, int platform, int kind, uint32_t arr, size_t *n);

#endif
