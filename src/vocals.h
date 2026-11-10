// A package's lyrics (lyrics.json: a list of {"t": seconds, "d": seconds, "w": text}) as the game's lyrics song file.
// A syllable's text is kept as it is: a "-" at its end joins it to the next and a "+" ends a line, in the package
// as in the game.
#ifndef SB_VOCALS_H
#define SB_VOCALS_H

#include <stddef.h>
#include <stdint.h>

// The lyrics song file for platform (SB_MAC or SB_PC): malloc'd, its size in *out_n, the syllables it has in *count
// (may be 0). A syllable without a time or a text is left out, and they are put in order of time. 0 with the reason
// in err when the file is not a list of lyrics or has none that can be used.
uint8_t *sb_vocals_sng(const char *json, size_t n, int platform, size_t *out_n, uint32_t *count, char *err, size_t errlen);
// The same before it is packed: the song file's plain payload
uint8_t *sb_vocals_payload(const char *json, size_t n, size_t *out_n, uint32_t *count, char *err, size_t errlen);

#endif
