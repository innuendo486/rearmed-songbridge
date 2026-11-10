// A song's sound banks (song_<key>.bnk and song_<key>_preview.bnk): what tells the game's audio engine (Wwise) which
// .wem file the song's "play" event plays. A bank also carries the first bytes of that .wem (the prefetch), so that
// playing can start before the file is opened.
#ifndef SB_BANK_H
#define SB_BANK_H

#include <stddef.h>
#include <stdint.h>

#define SB_BANK_PREFETCH 51200u  // the most a bank carries of its .wem (see sb_bank_prefetch)
#define SB_BANK_KEY_MAX 200u     // a song key's length at most: the bank's name must fit a byte's count
#define SB_BANK_VOLUME (-7.0f)   // the volume most custom songs have, dB

// Wwise's id of a name: the 32-bit FNV-1 hash of the name in lower case
uint32_t sb_bank_fnv(const char *name);

// The ids of a song's bank, the same on every run:
//   bank    FNV-1 of "song_<key>" or "song_<key>_preview"     as the game's own banks have it
//   event   FNV-1 of "play_<key>" or "play_<key>_preview"     the name the game posts: it must be this
//   wem, sound, action    made from the key, 1 to 2^31-1; a song's main and preview .wem never share one
typedef struct sb_bank_ids {
  uint32_t bank, wem, sound, action, event;
} sb_bank_ids;
// 1, or 0 for a key that is empty or too long
int sb_bank_ids_of(const char *key, int preview, sb_bank_ids *ids);
// The id of the song's .wem (the file is audio/<platform>/<id>.wem), 0 for such a key
uint32_t sb_bank_wem_id(const char *key, int preview);

// How much of a .wem of wem_size bytes its bank carries: min(wem_size, SB_BANK_PREFETCH)
size_t sb_bank_prefetch(size_t wem_size);
// The size of the bank sb_bank_build makes for a prefetch of that many bytes; 0 for a key it would refuse
size_t sb_bank_size(const char *key, int preview, size_t prefetch);

// The bank of song `key` (as the song's files name it, e.g. "abbadanc"; its case shows only in the bank's name):
// malloc'd, sb_bank_size(key, preview, n) bytes, in *out_n too. wem_id: the .wem's file id; prefetch[0..n): the first
// sb_bank_prefetch(its size) bytes of that .wem; volume in dB. 0 for a bad key, no prefetch or no memory.
uint8_t *sb_bank_build(const char *key, int preview, uint32_t wem_id, const uint8_t *prefetch, size_t n, float volume,
                       size_t *out_n);

// Everything that differs between the banks seen (the game's own and the custom-song tools'), for sb_bank_write
typedef struct sb_bank_spec {
  const char *name;  // the bank's name as written in it: "Song_<Key>" or "Song_<Key>_Preview"
  int preview;       // the sound goes to the previews' bus and plays one at a time
  sb_bank_ids ids;
  float volume;
  uint32_t media_off, media_size;  // the prefetch's place and size in the bank's data: 0 and n (tools leave junk here)
  int from_elapsed;                // a main bank's virtual voice setting (the game's own: 1); a preview's is always 1
  int has_prop36;                  // a fourth property (0x36) the game's newer banks have, and its value
  uint32_t prop36;
} sb_bank_spec;
// The bank as s says, malloc'd, its size in *out_n; 0 for a name that is empty or too long, no prefetch or no memory
uint8_t *sb_bank_write(const sb_bank_spec *s, const uint8_t *prefetch, size_t n, size_t *out_n);

#endif
