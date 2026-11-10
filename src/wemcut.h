// A stretch of a .wem (Wwise Vorbis, as wem.c writes it and as the game's own files are) as a .wem of its own, without
// decoding: the song's preview.
#ifndef SB_WEMCUT_H
#define SB_WEMCUT_H

#include <stddef.h>
#include <stdint.h>

#define SB_PREVIEW_SECONDS 30.0

// About `seconds` (SB_PREVIEW_SECONDS when not above 0) of the .wem at wem[0..n), from `start` seconds on: malloc'd,
// its size in *out_n. start below 0: a third into the song. A start too near the end is moved back so that the whole
// length fits; a file shorter than the length is given whole. The cut is at packets: it starts at the last packet
// that ends at or before the start, and ends with the first that reaches the length. *at (when not 0) gets the frame
// of the source that the result's first frame is. 0 when there is none, with the reason in err (when errlen > 0):
// not such a file, a damaged one, no audio packets.
uint8_t *sb_wem_preview(const uint8_t *wem, size_t n, double start, double seconds, size_t *out_n, uint64_t *at,
                        char *err, size_t errlen);

#endif
