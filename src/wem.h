// An Ogg Vorbis file as the game's own audio file (.wem, Wwise Vorbis), without decoding it: the same packets, as the
// game's decoder reads them. That decoder takes its codebooks from a library the game carries, so only a file whose
// codebooks are all in the library can be made (the ones libvorbis writes are).
#ifndef SB_WEM_H
#define SB_WEM_H

#include <stddef.h>
#include <stdint.h>

// The game's codebook library: n codebooks, book[i] the first byte of codebook i and avail[i] how many bytes may be
// read there (a codebook is read by its bits, not by a size; avail only keeps the reading inside the library).
typedef struct sb_books {
  uint32_t n;
  const uint8_t *const *book;
  const size_t *avail;
} sb_books;

// The .wem of the Ogg Vorbis file ogg[0..n): malloc'd, its size in *out_n. 0 when it cannot be made, with the reason
// in err (when errlen > 0): not Ogg Vorbis, more than two channels, a codebook the game lacks, a damaged file.
uint8_t *sb_wem_from_ogg(const uint8_t *ogg, size_t n, const sb_books *lib, size_t *out_n, char *err, size_t errlen);

#endif
