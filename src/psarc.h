// A Rocksmith 2014 song archive (psarc) of one platform, and the same archive as the other platform's game reads
// it: built in memory where it differs (table of contents, name list, aggregate graph, song files), the rest read
// from the file.
#ifndef SB_PSARC_H
#define SB_PSARC_H

#include <stdint.h>

enum { SB_MAC = 0, SB_PC = 1 };

typedef struct sb_arc sb_arc;    // an archive's table of contents and names
typedef struct sb_virt sb_virt;  // the archive as platform `to` reads it

// The archive in fd (size bytes), or 0 when it is not a whole Rocksmith 2014 archive: every entry must lie in the
// file. Reads with pread; fd is not kept.
sb_arc *sb_parse(int fd, int64_t size);
void sb_arc_free(sb_arc *a);
// The archive's song keys (the names of its gamexblocks/*.xblock, lower case): calls f for each
void sb_arc_keys(const sb_arc *a, void (*f)(void *ctx, const char *key), void *ctx);

// The archive for platform `to`, or 0 (an entry that cannot be read). a must outlive it; fd is not kept.
sb_virt *sb_virt_build(const sb_arc *a, int fd, int to);
int64_t sb_virt_size(const sb_virt *v);
// n bytes at off, from any thread; fd is the archive's file. The bytes read, 0 at or past the end, -1 for an error.
int64_t sb_virt_read(sb_virt *v, int fd, void *buf, uint64_t n, int64_t off);
// Forgets the song files made so far (they are made again when read)
void sb_virt_drop(sb_virt *v);
void sb_virt_free(sb_virt *v);

#endif
