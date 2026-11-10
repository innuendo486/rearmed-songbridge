// A song's cover as the three textures a song archive carries (gfxassets/album_art/album_<key>_64.dds, _128.dds and
// _256.dds), made from the JPEG or PNG a feedpak carries, or a plain dark cover when there is none. A texture's size
// follows from its edge alone, so an archive's table of contents can be written before any cover is made.
#ifndef SB_ART_H
#define SB_ART_H

#include <stddef.h>
#include <stdint.h>

#define SB_ART_HEADER 128  // "DDS " and the 124-byte header; the DXT1 blocks follow

// The size of the texture of edge px (64, 128 or 256): 2176, 8320, 32896. 0 for any other edge.
size_t sb_art_size(int px);

// The texture of edge px of the picture image[0..n) (JPEG, PNG, or anything else ImageIO reads), sb_art_size(px)
// bytes into out. The picture is scaled to cover the square: one that is not square loses the ends of its longer
// side, half at each end. Transparent parts come out black.
// 1: the picture's texture. 0: the picture is missing (image is 0 or n is 0) or cannot be read, and out is the plain
// dark cover. -1: px is no texture's edge or out is 0, and nothing is written.
// The same picture gives the same bytes every time. Any thread, also several at once; needs no run loop.
int sb_art_dds(const uint8_t *image, size_t n, int px, uint8_t *out);

// The compressor alone: px x px pixels (px a multiple of 4), four bytes each (red, green, blue, one ignored), the top
// row first, as px * px / 2 bytes of DXT1 blocks into out.
void sb_art_dxt1(const uint8_t *rgbx, int px, uint8_t *out);

#endif
