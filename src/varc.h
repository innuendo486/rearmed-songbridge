// A Rocksmith 2014 song archive that is in no file: a list of entries whose sizes are known and whose bytes are made
// when they are read. The header, the table of contents and the name list are built at once; an entry is stored, not
// compressed, so where each of its bytes lies follows from the sizes alone.
#ifndef SB_VARC_H
#define SB_VARC_H

#include <stddef.h>
#include <stdint.h>

typedef struct sb_varc sb_varc;

typedef struct sb_ventry {
  const char *name;  // the entry's path in the archive
  int64_t len;       // its size
  // The packed size of its last block when that is short and must be packed (sb_varc_tail), else 0
  uint32_t tail;
} sb_ventry;

// n bytes of entry `index` at off, into buf: 1, or 0 when they cannot be made. Called from any thread, also for one
// entry at once, and only for bytes inside the entry.
typedef int (*sb_vread)(void *ctx, uint32_t index, void *buf, uint64_t n, int64_t off);

// An entry's last block is short when its size is no multiple of 64 KiB. The game takes a short block for a zlib
// stream when it starts as one, so such a block must really be one: its packed size, 0 when the block goes in as it
// is (n is 0 or a whole block, or the block does not start as a zlib stream), -1 when it cannot be packed. A size
// that was kept must come out again when the entry is read: the block is packed the same way then.
int32_t sb_varc_tail(const uint8_t *block, size_t n);

// The archive of entries e[0..n) in that order (names and sizes are copied; read and ctx must outlive it), or 0
sb_varc *sb_varc_build(const sb_ventry *e, uint32_t n, sb_vread read, void *ctx);
int64_t sb_varc_size(const sb_varc *v);
// n bytes at off, from any thread: the bytes read, 0 at or past the end, -1 for an error
int64_t sb_varc_read(sb_varc *v, void *buf, uint64_t n, int64_t off);
// Forgets the packed last blocks made so far (they are made again when read)
void sb_varc_drop(sb_varc *v);
void sb_varc_free(sb_varc *v);

#endif
