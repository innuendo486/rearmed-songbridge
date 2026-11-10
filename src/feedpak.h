// A feedpak (the feedBack project's open song package: github.com/got-feedBack/feedpak-spec): a zip file or a folder
// with manifest.yaml, the index of everything else in it. This reads the package and its manifest; what the files
// hold is their readers' business.
#ifndef SB_FEEDPAK_H
#define SB_FEEDPAK_H

#include <stddef.h>
#include <stdint.h>

typedef struct sb_pak_arr {
  char *id, *name;  // name is the id when the manifest gives none
  char *file;       // the fretted chart (arrangement JSON), 0 when it has none (notation only, a drum part)
  char *type;       // the manifest's hint ("guitar", "bass", "drums", ...), 0 when it gives none
  int drums;        // a drum part: never a fretted chart
  int vocals;       // the sung part (type "vocals"): its words are the manifest's lyrics, it has no fretted chart
  int strings;      // the tuning's length, 4 to 8
  int tuning[8];    // semitones from standard, lowest string first
  int capo;
  double cents;
} sb_pak_arr;

typedef struct sb_pak_stem {
  char *id, *file;
  char *codec;  // as the manifest says, else by the file's extension: "vorbis", "pcm", "opus", "mp3", "flac"; 0 unknown
  int on;       // plays when the song opens
} sb_pak_stem;

typedef struct sb_manifest {
  char *title, *artist;                 // always there (by default when the manifest has none)
  char *album, *album_artist, *mbid, *isrc;  // 0 when not given
  char *cover, *preview, *lyrics, *timeline;  // paths, 0 when not given
  int year;         // 0 when not given
  double duration;  // seconds, 0 when not given
  sb_pak_arr *arr;
  uint32_t narr;
  sb_pak_stem *stem;
  uint32_t nstems;
} sb_manifest;

typedef struct sb_pak sb_pak;

// The package at path (a zip file or a folder), its manifest read. Packages come in many forms, so what can be read
// is: a part that is missing or makes no sense is left out or given its default (a file whose path could leave the
// package, a stem with no file, a tuning that is none; a song with no title is named by its file), and
// sb_pak_warning says so. 0, with the reason in err, only when there is nothing to play: no package, no manifest
// that is YAML, no arrangement, no audio.
sb_pak *sb_pak_open(const char *path, char *err, size_t errlen);
const sb_manifest *sb_pak_manifest(const sb_pak *p);
// Line i of what was left out or repaired when the package was read, 0 past the last
const char *sb_pak_warning(const sb_pak *p, uint32_t i);
// The size of the package's file `rel` (a path as the manifest has it), -1 when there is none
int64_t sb_pak_size(const sb_pak *p, const char *rel);
// That file: malloc'd (with a 0 after it), its size in *n; 0 when there is none or it cannot be read. From any thread.
uint8_t *sb_pak_read(const sb_pak *p, const char *rel, size_t *n);
void sb_pak_close(sb_pak *p);

#endif
