// See art.h. The game's own album art, in every archive looked at (official and custom, Mac and PC), is a DDS file
// with one level and no more: "DDS ", a 124-byte header, then px * px / 2 bytes of DXT1 blocks. No mipmaps, no DX10
// header. The header, as the official files have it (all else zero):
//   +0x04 u32 124        the header's size
//   +0x08 u32 0x1007     flags: caps, height, width, pixel format
//   +0x0c u32 px, +0x10 u32 px (height, width)
//   +0x4c u32 32         the pixel format's size
//   +0x50 u32 4          its flags: a FourCC
//   +0x54 "DXT1"
//   +0x6c u32 0x1000     caps: a texture
// Custom songs' files differ in fields that say the same thing another way: the flag 0x80000 with the blocks' size
// at +0x14, and those written by ImageMagick a level count of 1 at +0x1c and its name at +0x20. The official form is
// the one written here.
//
// A DXT1 block is 4 x 4 pixels in 8 bytes: two colours as 16 bits each (5 red, 6 green, 5 blue, red highest; little
// endian), then 2 bits a pixel, the top left pixel lowest: 0 and 1 are the two colours, 2 and 3 the thirds between
// them when the first colour is the larger number. When it is not, 2 is the midpoint and 3 is transparent black:
// that form is never written here.
//
// The picture is read and scaled by ImageIO and CoreGraphics (its best interpolation, into sRGB, over black), then
// each block is fitted: the two colours start at the ends of the pixels' spread along their principal axis, and are
// then moved, a few times, to the least-squares best for the pixels each index got, and last by single steps while
// the block's error falls. A block of one colour takes the pair of colours whose third lies nearest to it, which two
// equal colours seldom reach. All channels weigh the same.
#include "art.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAX_EDGE 16384  // a picture larger than this on a side is not a cover; it would take a gigabyte to open
#define DARK 24         // the plain cover's grey: a value both 5 and 6 bits hold exactly
#define REFINE 4        // least-squares rounds a block at most; they stop when one does not help
#define CLIMB 16        // passes of single steps after them at most; they stop likewise

size_t sb_art_size(int px) {
  return px == 64 || px == 128 || px == 256 ? SB_ART_HEADER + (size_t)px * (size_t)px / 2 : 0;
}

static void le32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24);
}

static void header(uint8_t *h, int px) {
  memset(h, 0, SB_ART_HEADER);
  memcpy(h, "DDS ", 4);
  le32(h + 0x04, 124);
  le32(h + 0x08, 0x1007);
  le32(h + 0x0c, (uint32_t)px);
  le32(h + 0x10, (uint32_t)px);
  le32(h + 0x4c, 32);
  le32(h + 0x50, 4);
  memcpy(h + 0x54, "DXT1", 4);
  le32(h + 0x6c, 0x1000);
}

// --- the compressor ----------------------------------------------------------------------------------------------
static int wide5(int v) { return v << 3 | v >> 2; }
static int wide6(int v) { return v << 2 | v >> 4; }

static uint16_t to565(const double *c) {
  int q[3];
  for (int k = 0; k < 3; k++) {
    double top = k == 1 ? 63.0 : 31.0, v = c[k] < 0 ? 0 : c[k] > 255 ? 255 : c[k];
    q[k] = (int)(v * top / 255.0 + 0.5);
  }
  return (uint16_t)(q[0] << 11 | q[1] << 5 | q[2]);
}

// the four colours of a block whose first colour is the larger number
static void palette(uint16_t c0, uint16_t c1, int pal[4][3]) {
  uint16_t c[2] = {c0, c1};
  for (int i = 0; i < 2; i++)
    pal[i][0] = wide5(c[i] >> 11), pal[i][1] = wide6(c[i] >> 5 & 63), pal[i][2] = wide5(c[i] & 31);
  for (int k = 0; k < 3; k++) {
    pal[2][k] = (2 * pal[0][k] + pal[1][k] + 1) / 3;
    pal[3][k] = (pal[0][k] + 2 * pal[1][k] + 1) / 3;
  }
}

// each pixel's nearest of the four colours into idx; the squared error of the block
static int assign(const int p[16][3], uint16_t c0, uint16_t c1, uint8_t idx[16]) {
  int pal[4][3], sum = 0;
  palette(c0, c1, pal);
  for (int i = 0; i < 16; i++) {
    int best = 1 << 30;
    for (int j = 0; j < 4; j++) {
      int dr = p[i][0] - pal[j][0], dg = p[i][1] - pal[j][1], db = p[i][2] - pal[j][2], d = dr * dr + dg * dg + db * db;
      if (d < best) best = d, idx[i] = (uint8_t)j;
    }
    sum += best;
  }
  return sum;
}

// One colour: every pixel takes index 2, the third nearer the first colour, and the two colours are the pair whose
// third comes nearest; of equally near pairs the closest together, as decoders differ in how they round a third.
// The search is long for a block's worth of work and flat covers repeat one colour, so the last answer is kept.
typedef struct {
  int set, c[3];
  uint16_t c0, c1;
} memo;

static void solid(memo *m, const int *c, uint16_t *c0, uint16_t *c1) {
  int lo[3], hi[3];
  if (m->set && m->c[0] == c[0] && m->c[1] == c[1] && m->c[2] == c[2]) {
    *c0 = m->c0, *c1 = m->c1;
    return;
  }
  for (int k = 0; k < 3; k++) {
    int top = k == 1 ? 64 : 32, best = 1 << 30;
    for (int a = 0; a < top; a++)
      for (int b = 0; b < top; b++) {
        int wa = k == 1 ? wide6(a) : wide5(a), wb = k == 1 ? wide6(b) : wide5(b);
        int score = abs((2 * wa + wb + 1) / 3 - c[k]) * 256 + abs(wa - wb);
        if (score < best) best = score, hi[k] = a, lo[k] = b;
      }
  }
  *c0 = (uint16_t)(hi[0] << 11 | hi[1] << 5 | hi[2]);
  *c1 = (uint16_t)(lo[0] << 11 | lo[1] << 5 | lo[2]);
  m->set = 1, m->c[0] = c[0], m->c[1] = c[1], m->c[2] = c[2], m->c0 = *c0, m->c1 = *c1;
}

static void put_block(uint8_t *out, uint16_t c0, uint16_t c1, const uint8_t idx[16]) {
  static const uint8_t swapped[4] = {1, 0, 3, 2};
  uint32_t bits = 0;
  int swap = c0 < c1;
  if (swap) {
    uint16_t t = c0;
    c0 = c1, c1 = t;
  }
  // two equal colours make the three-colour form, where 0 is still that colour
  for (int i = 0; i < 16; i++) bits |= (uint32_t)(c0 == c1 ? 0 : swap ? swapped[idx[i]] : idx[i]) << (2 * i);
  out[0] = (uint8_t)c0, out[1] = (uint8_t)(c0 >> 8), out[2] = (uint8_t)c1, out[3] = (uint8_t)(c1 >> 8);
  le32(out + 4, bits);
}

static void block(const int p[16][3], memo *m, uint8_t *out) {
  uint8_t idx[16], tidx[16];
  uint16_t c0, c1;
  int lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0};
  double mean[3] = {0, 0, 0};
  for (int i = 0; i < 16; i++)
    for (int k = 0; k < 3; k++) {
      if (p[i][k] < lo[k]) lo[k] = p[i][k];
      if (p[i][k] > hi[k]) hi[k] = p[i][k];
      mean[k] += p[i][k] / 16.0;
    }
  if (lo[0] == hi[0] && lo[1] == hi[1] && lo[2] == hi[2]) {
    memset(idx, 2, 16);
    solid(m, lo, &c0, &c1);
    put_block(out, c0, c1, idx);
    return;
  }
  // the principal axis: the covariance matrix applied a few times to the box's diagonal
  double cov[3][3] = {{0}}, axis[3] = {hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]};
  for (int i = 0; i < 16; i++)
    for (int a = 0; a < 3; a++)
      for (int b = 0; b < 3; b++) cov[a][b] += (p[i][a] - mean[a]) * (p[i][b] - mean[b]);
  for (int it = 0; it < 8; it++) {
    double v[3], big = 0;
    for (int a = 0; a < 3; a++) {
      v[a] = cov[a][0] * axis[0] + cov[a][1] * axis[1] + cov[a][2] * axis[2];
      if (fabs(v[a]) > big) big = fabs(v[a]);
    }
    if (big < 1e-9) break;
    for (int a = 0; a < 3; a++) axis[a] = v[a] / big;
  }
  double len2 = axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2], tmin = 1e30, tmax = -1e30, e0[3], e1[3];
  for (int i = 0; i < 16; i++) {
    double t = 0;
    for (int k = 0; k < 3; k++) t += (p[i][k] - mean[k]) * axis[k];
    if (t < tmin) tmin = t;
    if (t > tmax) tmax = t;
  }
  for (int k = 0; k < 3; k++) e0[k] = mean[k] + axis[k] * tmax / len2, e1[k] = mean[k] + axis[k] * tmin / len2;
  c0 = to565(e0), c1 = to565(e1);
  int err = assign(p, c0, c1, idx);
  for (int round = 0; round < REFINE && err; round++) {
    // index j stands for w[j] of the first colour and the rest of the second
    static const double w[4] = {1.0, 0.0, 2.0 / 3.0, 1.0 / 3.0};
    double aa = 0, bb = 0, ab = 0, at[3] = {0, 0, 0}, bt[3] = {0, 0, 0};
    for (int i = 0; i < 16; i++) {
      double a = w[idx[i]], b = 1.0 - a;
      aa += a * a, bb += b * b, ab += a * b;
      for (int k = 0; k < 3; k++) at[k] += a * p[i][k], bt[k] += b * p[i][k];
    }
    double det = aa * bb - ab * ab;
    if (fabs(det) < 1e-6) break;  // every pixel on one index
    for (int k = 0; k < 3; k++) e0[k] = (at[k] * bb - bt[k] * ab) / det, e1[k] = (bt[k] * aa - at[k] * ab) / det;
    uint16_t t0 = to565(e0), t1 = to565(e1);
    int terr = assign(p, t0, t1, tidx);
    if (terr >= err) break;
    err = terr, c0 = t0, c1 = t1;
    memcpy(idx, tidx, 16);
  }
  // then each colour a step up or down in each channel, for as long as a step helps: worth some tenths of a dB
  for (int moved = 1, pass = 0; moved && pass < CLIMB && err; pass++) {
    static const int top[3] = {31, 63, 31}, shift[3] = {11, 5, 0};
    moved = 0;
    for (int e = 0; e < 2; e++)
      for (int k = 0; k < 3; k++)
        for (int d = -1; d <= 1; d += 2) {
          uint16_t t[2] = {c0, c1};
          int v = (t[e] >> shift[k] & top[k]) + d;
          if (v < 0 || v > top[k]) continue;
          t[e] = (uint16_t)((t[e] & ~(top[k] << shift[k])) | v << shift[k]);
          int terr = assign(p, t[0], t[1], tidx);
          if (terr >= err) continue;
          err = terr, c0 = t[0], c1 = t[1], moved = 1;
          memcpy(idx, tidx, 16);
        }
  }
  if (c0 == c1) {  // too close for two colours: the thirds of a pair around the mean may still come nearer
    int mid[3] = {(int)(mean[0] + 0.5), (int)(mean[1] + 0.5), (int)(mean[2] + 0.5)}, pal[4][3], serr = 0;
    uint16_t s0, s1;
    solid(m, mid, &s0, &s1);
    palette(s0 > s1 ? s0 : s1, s0 > s1 ? s1 : s0, pal);
    int at = s0 > s1 ? 2 : 3;  // where put_block's swap leaves index 2
    for (int i = 0; i < 16; i++)
      for (int k = 0; k < 3; k++) serr += (p[i][k] - pal[at][k]) * (p[i][k] - pal[at][k]);
    if (serr < err) {
      c0 = s0, c1 = s1;
      memset(idx, 2, 16);
    }
  }
  put_block(out, c0, c1, idx);
}

void sb_art_dxt1(const uint8_t *rgbx, int px, uint8_t *out) {
  memo m = {0};
  for (int by = 0; by < px; by += 4)
    for (int bx = 0; bx < px; bx += 4, out += 8) {
      int p[16][3];
      for (int i = 0; i < 16; i++) {
        const uint8_t *s = rgbx + 4 * ((size_t)(by + i / 4) * (size_t)px + (size_t)(bx + i % 4));
        p[i][0] = s[0], p[i][1] = s[1], p[i][2] = s[2];
      }
      block(p, &m, out);
    }
}

// --- the picture -------------------------------------------------------------------------------------------------
// The picture scaled to cover px x px, as red, green, blue and a spare byte a pixel into rgbx: 1, or 0
static int scaled(const uint8_t *image, size_t n, int px, uint8_t *rgbx) {
  int done = 0;
  CFDataRef data = CFDataCreateWithBytesNoCopy(kCFAllocatorDefault, image, (CFIndex)n, kCFAllocatorNull);
  CGImageSourceRef src = data ? CGImageSourceCreateWithData(data, 0) : 0;
  CGImageRef img = src && CGImageSourceGetCount(src) > 0 ? CGImageSourceCreateImageAtIndex(src, 0, 0) : 0;
  size_t w = img ? CGImageGetWidth(img) : 0, h = img ? CGImageGetHeight(img) : 0;
  CGColorSpaceRef rgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  if (w && h && w <= MAX_EDGE && h <= MAX_EDGE && rgb) {
    memset(rgbx, 0, (size_t)px * (size_t)px * 4);
    CGContextRef cg = CGBitmapContextCreate(rgbx, (size_t)px, (size_t)px, 8, (size_t)px * 4, rgb,
                                            (CGBitmapInfo)kCGImageAlphaNoneSkipLast | kCGBitmapByteOrderDefault);
    if (cg) {
      // the shorter side fills the square, the longer one hangs over both edges alike
      double s = (double)px / (double)(w < h ? w : h), dw = (double)w * s, dh = (double)h * s;
      CGContextSetInterpolationQuality(cg, kCGInterpolationHigh);
      CGContextDrawImage(cg, CGRectMake((px - dw) / 2, (px - dh) / 2, dw, dh), img);
      CGContextRelease(cg);
      done = 1;
    }
  }
  if (rgb) CGColorSpaceRelease(rgb);
  if (img) CGImageRelease(img);
  if (src) CFRelease(src);
  if (data) CFRelease(data);
  return done;
}

int sb_art_dds(const uint8_t *image, size_t n, int px, uint8_t *out) {
  if (!sb_art_size(px) || !out) return -1;
  size_t count = (size_t)px * (size_t)px;
  int made = 0;
  header(out, px);
  uint8_t *rgbx = image && n && n <= (size_t)INT32_MAX ? malloc(count * 4) : 0;
  if (rgbx && scaled(image, n, px, rgbx)) {
    sb_art_dxt1(rgbx, px, out + SB_ART_HEADER);
    made = 1;
  }
  free(rgbx);
  if (!made) {
    // the plain cover: both colours the same grey, every pixel the first
    uint16_t c = (uint16_t)((DARK >> 3) << 11 | (DARK >> 2) << 5 | DARK >> 3);
    for (uint8_t *b = out + SB_ART_HEADER; b < out + SB_ART_HEADER + count / 2; b += 8) {
      memset(b, 0, 8);
      b[0] = b[2] = (uint8_t)c, b[1] = b[3] = (uint8_t)(c >> 8);
    }
  }
  return made;
}
