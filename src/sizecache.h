// What is kept on disk of a feedpak between runs: the names and sizes of the entries its archive has (the archive's
// table of contents needs them before any entry is made), and the bytes of the few entries the game reads when it
// starts. One small file for each package and platform, in a folder that can be deleted at any time: a package
// whose file is missing, damaged or of another package (path, size, date) is simply worked out again.
#ifndef SB_SIZECACHE_H
#define SB_SIZECACHE_H

#include <stddef.h>
#include <stdint.h>

typedef struct sb_cached {
  char *name;     // the entry's path in the archive
  int64_t len;    // its size
  uint32_t tail;  // its last block's packed size (varc.h)
  uint32_t tag;   // what the entry is, for whoever makes it
  uint8_t *data;  // its len bytes when they are kept, else 0
} sb_cached;

typedef struct sb_cache_set {
  sb_cached *e;
  uint32_t n;
} sb_cache_set;

// What a package is known by: its path, its size and date, the platform its archive is for, and the version of what
// makes the entries (a change there makes every kept file stale)
typedef struct sb_cache_key {
  const char *path;
  int64_t size, mtime;
  int platform;
  uint32_t version;
} sb_cache_key;

// The entries kept for the package in folder dir: 1 with *out filled (sb_cache_free it), 0 when there are none
int sb_cache_load(const char *dir, const sb_cache_key *k, sb_cache_set *out);
// Keeps them (the folder is made when it is not there): 1, or 0 when that cannot be done, which costs only time
int sb_cache_store(const char *dir, const sb_cache_key *k, const sb_cache_set *set);
// Forgets what is kept for the package (its entries turned out not to be what was kept)
void sb_cache_forget(const char *dir, const sb_cache_key *k);
void sb_cache_free(sb_cache_set *set);

#endif
