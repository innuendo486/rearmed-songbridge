// A feedpak (feedpak.h) as the song archive the game reads: the entries a custom song's archive has, their bytes
// made from the package when the game reads them (varc.h). Only the table of contents, the song list manifest and
// the AppID are there from the start, which is all the game reads of an archive until the song is looked at.
//
// The table of contents needs every entry's size beforehand. The first time a package is seen its entries are all
// made once for their sizes, which are kept (sizecache.h); after that opening one reads only the kept file.
#ifndef SB_FEEDSONG_H
#define SB_FEEDSONG_H

#include <stddef.h>
#include <stdint.h>

#include "wem.h"

typedef struct sb_feedsong sb_feedsong;

// Not 0: each entry made is said on stderr (off unless whoever serves the songs turns it on)
extern int sb_feedsong_log;

// The game's codebook library when it can be had, else 0 (then a package's audio cannot be made). Asked for when
// audio is first made, from any thread.
typedef const sb_books *(*sb_books_fn)(void);

// The package at path as an archive for platform (SB_MAC or SB_PC). cache_dir: where sizes are kept, 0 for nowhere
// (every open then makes the entries). 0 when there is nothing to play in it, with the reason in err: no package,
// no arrangement that can be a song file, no audio the game can play. What was left out or repaired on the way is
// in sb_feedsong_note.
sb_feedsong *sb_feedsong_open(const char *path, int platform, const char *cache_dir, sb_books_fn books, char *err, size_t errlen);
// The song's key in lower case: the name its entity block has, by which a song is one song
const char *sb_feedsong_key(const sb_feedsong *s);
// What there is to say of the package for a log (what was left out, repaired or made up), "" when nothing
const char *sb_feedsong_note(const sb_feedsong *s);
int64_t sb_feedsong_size(const sb_feedsong *s);
// n bytes of the archive at off, from any thread: the bytes read, 0 at or past the end, -1 for an error
int64_t sb_feedsong_read(sb_feedsong *s, void *buf, uint64_t n, int64_t off);
// Forgets the entries made so far and closes the package (both come back when read)
void sb_feedsong_drop(sb_feedsong *s);
void sb_feedsong_free(sb_feedsong *s);

#endif
