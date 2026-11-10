// See vocals.h. A lyrics song file is a song file (sng.c) with nothing but its vocals: every other list is empty,
// and after the vocals come the symbol tables of the font they are drawn with (vocals_font.h: the game's own).
//   4 x u32 0                      beats, phrases, chord templates, chord notes
//   u32 count, count x 60 bytes    time f32, note i32, length f32, text (48 bytes, UTF-8, 0-filled)
//   the symbol tables              (only when there are vocals)
//   9 x u32 0                      phrase iterations ... difficulty levels
//   95 bytes of metadata           all 0 but the capo, -1
// The note is the pitch sung; 254, as custom songs have it, is "none".
#include "vocals.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <yyjson.h>
#include <zlib.h>

#include "sng.h"
#include "vocals_font.h"

#define MAX_VOCALS 20000u  // a song has a few hundred
#define TEXT 48u
#define NO_NOTE 254

typedef struct {
  float t, len;
  char text[TEXT];
} vocal;

static int by_time(const void *a, const void *b) {
  float x = ((const vocal *)a)->t, y = ((const vocal *)b)->t;
  return x < y ? -1 : x > y;
}

// A number that is one, or a string that reads as one
static int number(yyjson_val *v, double *out) {
  if (yyjson_is_num(v)) {
    *out = yyjson_get_num(v);
  } else if (yyjson_is_str(v)) {
    char *end = 0;
    *out = strtod(yyjson_get_str(v), &end);
    if (end == yyjson_get_str(v) || *end) return 0;
  } else {
    return 0;
  }
  return isfinite(*out);
}

uint8_t *sb_vocals_payload(const char *json, size_t n, size_t *out_n, uint32_t *count, char *err, size_t errlen) {
  yyjson_doc *doc = yyjson_read(json, n, YYJSON_READ_ALLOW_COMMENTS | YYJSON_READ_ALLOW_TRAILING_COMMAS);
  yyjson_val *root = doc ? yyjson_doc_get_root(doc) : 0, *v;
  uint8_t *out = 0;
  vocal *list = 0;
  uint32_t nv = 0;
  if (errlen) err[0] = 0;
  if (!yyjson_is_arr(root)) {
    if (errlen) snprintf(err, errlen, "the lyrics are not a list");
    goto done;
  }
  size_t idx, max, cap = yyjson_arr_size(root);
  if (cap > MAX_VOCALS) cap = MAX_VOCALS;
  if (!(list = calloc(cap + 1, sizeof *list))) goto done;
  yyjson_arr_foreach(root, idx, max, v) {
    double t, d = 0;
    const char *w = yyjson_get_str(yyjson_obj_get(v, "w"));
    if (nv == cap || !w || !*w || !number(yyjson_obj_get(v, "t"), &t) || t < 0 || t > 86400) continue;
    if (!number(yyjson_obj_get(v, "d"), &d) || d < 0 || d > 600) d = 0;
    size_t len = strlen(w);
    if (len >= TEXT) {  // cut at a character's start, and keep what its last character said of the line
      char last = w[len - 1];
      len = TEXT - 2;
      while (len && ((unsigned char)w[len] & 0xc0) == 0x80) len--;
      memcpy(list[nv].text, w, len);
      if (last == '+' || last == '-') list[nv].text[len] = last;
    } else {
      memcpy(list[nv].text, w, len);
    }
    list[nv].t = (float)t, list[nv].len = (float)d;
    nv++;
  }
  if (!nv) {
    if (errlen) snprintf(err, errlen, "no lyrics in the file");
    goto done;
  }
  qsort(list, nv, sizeof *list, by_time);
  size_t size = 16 + 4 + (size_t)nv * 60 + SB_VOCALS_SYMBOLS + 36 + 95;
  if (!(out = calloc(1, size))) goto done;
  uint8_t *p = out + 16;
  memcpy(p, &nv, 4), p += 4;
  for (uint32_t i = 0; i < nv; i++, p += 60) {
    int32_t note = NO_NOTE;
    memcpy(p, &list[i].t, 4), memcpy(p + 4, &note, 4), memcpy(p + 8, &list[i].len, 4), memcpy(p + 12, list[i].text, TEXT);
  }
  uLongf got = SB_VOCALS_SYMBOLS;
  if (uncompress(p, &got, sb_vocals_symbols_z, sizeof sb_vocals_symbols_z) != Z_OK || got != SB_VOCALS_SYMBOLS) {
    free(out);
    out = 0;
    goto done;
  }
  p += SB_VOCALS_SYMBOLS + 36;
  p[40] = 0xff;  // the metadata's capo: none
  *out_n = size;
  if (count) *count = nv;
done:
  if (!out && errlen && !err[0]) snprintf(err, errlen, "out of memory");
  free(list);
  yyjson_doc_free(doc);
  return out;
}

uint8_t *sb_vocals_sng(const char *json, size_t n, int platform, size_t *out_n, uint32_t *count, char *err, size_t errlen) {
  size_t pn = 0;
  if (errlen) err[0] = 0;
  uint8_t *p = sb_vocals_payload(json, n, &pn, count, err, errlen);
  if (!p) return 0;
  uint8_t *f = sb_sng_pack(p, pn, platform, out_n);
  free(p);
  if (!f && errlen) snprintf(err, errlen, "out of memory");
  return f;
}
