// See wem.h. What differs between the two forms of a Vorbis stream:
//
//   Ogg Vorbis                                          the game's .wem
//   pages with lacing and a granule position            a RIFF file: "fmt " (66 bytes) and "data"
//   identification and comment headers                  fields of "fmt "
//   setup header: every codebook written out,           setup packet: each codebook as its 10-bit number in the
//     16-bit types and placeholders                       game's library, the types cut to what varies
//   audio packet: type bit, mode number, for a long     audio packet: mode number, the rest (the window flags follow
//     block the two window flags, the rest                from the neighbours' block sizes)
//   a packet's length in the page's lacing              a 16-bit length before each packet
//   seeking by the pages' granule positions             a seek table before the setup packet
//
// "data" is: seek table | setup packet | audio packets. "fmt " after the 24 bytes of a wave format (tag 0xffff):
//   +0x18 u32 frames            the stream's length as played
//   +0x1c u32 loop start        where the audio packets start, from the seek table's end
//   +0x20 u32 loop end          where they end, from the seek table's end
//   +0x24 u16 0, +0x26 u16      frames decoded past the length (the same at +0x32)
//   +0x28 u32 seek table size   +0x2c u32 the first audio packet's place in "data"
//   +0x30 u16 the largest packet
//   +0x34 u32, +0x38 u32        room for the decoder's set-up (32- and 64-bit game): an upper bound
//   +0x3c u32                   a key for the set-up: streams with one key share one, so it is the setup's checksum
//   +0x40 u8, u8                the two block sizes' exponents
// A seek table entry is two 16-bit steps, frames and bytes, to a packet the decoder can start at: the frames decoded
// through that packet, and the packet's place from the seek table's end.
#include "wem.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define SEEK_FRAMES 16384u  // an entry about this often, as the game's own files have
#define SETUP_ROOM 0x10000u  // the game's own files ask for 16 KiB

// --- bits, least significant first, as Vorbis packs them ---------------------------------------------------------
typedef struct {
  const uint8_t *d;
  uint64_t n, p;  // bits there, bits read
  int bad;        // read past the end
} rd;

static uint32_t get(rd *r, unsigned n) {
  if (n > r->n - r->p) {
    r->bad = 1, r->p = r->n;
    return 0;
  }
  uint32_t v = 0;
  for (unsigned i = 0; i < n; i++, r->p++) v |= (uint32_t)((r->d[r->p >> 3] >> (r->p & 7)) & 1) << i;
  return v;
}

typedef struct {
  uint8_t *d;
  uint64_t n;  // bits written
  size_t cap;
  int bad;  // out of memory
} wr;

static void put(wr *w, uint32_t v, unsigned n) {
  if (w->bad) return;
  if ((w->n + n + 7) / 8 > w->cap) {
    size_t cap = w->cap ? w->cap * 2 : 256;
    uint8_t *d = realloc(w->d, cap);
    if (!d) {
      w->bad = 1;
      return;
    }
    memset(d + w->cap, 0, cap - w->cap);
    w->d = d, w->cap = cap;
  }
  for (unsigned i = 0; i < n; i++, w->n++)
    if ((v >> i) & 1) w->d[w->n >> 3] |= (uint8_t)(1u << (w->n & 7));
}
// n bits from r to w, and their value (n up to 32)
static uint32_t pass(rd *r, wr *w, unsigned n) {
  uint32_t v = get(r, n);
  put(w, v, n);
  return v;
}

static unsigned ilog(uint32_t v) {
  unsigned n = 0;
  for (; v; v >>= 1) n++;
  return n;
}

static int fail(char *err, size_t errlen, const char *fmt, ...) {
  if (errlen) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
  }
  return 0;
}

// --- the setup header --------------------------------------------------------------------------------------------
// The values a lookup of type 1 has: the largest v with v^dim <= entries
static uint32_t quantvals(uint32_t entries, uint32_t dim) {
  uint32_t v = 0;
  if (!dim) return 0;
  for (;; v++) {
    uint64_t acc = 1;
    for (uint32_t i = 0; i < dim && acc <= entries; i++) acc *= (uint64_t)v + 1;
    if (acc > entries) return v;
  }
}

// The codebook at r (as an Ogg setup header has it) in the library's form, to w: 1, or 0 when it has no such form
// or r ends. width: the bits of an entry's length in that form, 0 for a codebook that has none (its lengths are
// ordered); the library's writer chose it, so the caller tries each that fits. *min_width gets the least that fits.
static int book_packed(rd *r, wr *w, unsigned width, unsigned *min_width) {
  if (get(r, 24) != 0x564342) return 0;
  uint32_t dim = get(r, 16), entries = get(r, 24);
  int fits = dim < 16 && entries < 16384;
  put(w, dim, 4), put(w, entries, 14);
  *min_width = 0;
  if (pass(r, w, 1)) {  // ordered
    pass(r, w, 5);
    for (uint32_t cur = 0; cur < entries && !r->bad;) cur += pass(r, w, ilog(entries - cur));
  } else {
    uint32_t sparse = get(r, 1), longest = 0;
    rd at = *r;  // the lengths are read twice: for the longest, then to write them
    for (uint32_t i = 0; i < entries && !r->bad; i++)
      if (!sparse || get(r, 1)) {
        uint32_t l = get(r, 5);
        if (l > longest) longest = l;
      }
    *min_width = ilog(longest) ? ilog(longest) : 1;
    if (width < *min_width || width > 7) fits = 0;
    put(w, width, 3), put(w, sparse, 1);
    *r = at;
    for (uint32_t i = 0; i < entries && !r->bad; i++)
      if (!sparse || pass(r, w, 1)) put(w, get(r, 5), width);
  }
  uint32_t lookup = get(r, 4);
  put(w, lookup, 1);
  if (lookup == 1) {
    pass(r, w, 32), pass(r, w, 32);
    unsigned bits = pass(r, w, 4) + 1;
    pass(r, w, 1);
    for (uint32_t i = quantvals(entries, dim); i && !r->bad; i--) pass(r, w, bits);
  } else if (lookup == 2) {
    get(r, 32), get(r, 32);
    unsigned bits = get(r, 4) + 1;
    get(r, 1);
    for (uint64_t i = (uint64_t)entries * dim; i && !r->bad; i--) get(r, bits);
    fits = 0;
  } else if (lookup) {
    return 0;
  }
  return fits && !r->bad && !w->bad;
}

// The library's number of the codebook whose first nbits bits are d's, or -1
static int book_find(const sb_books *lib, const uint8_t *d, uint64_t nbits) {
  size_t whole = (size_t)(nbits / 8);
  unsigned rest = (unsigned)(nbits % 8);
  for (uint32_t i = 0; i < lib->n; i++) {
    if (lib->avail[i] < whole + (rest != 0) || memcmp(lib->book[i], d, whole)) continue;
    if (!rest || !((lib->book[i][whole] ^ d[whole]) & ((1u << rest) - 1))) return (int)i;
  }
  return -1;
}

// The three kinds of part after the codebooks, the same in both forms but for what the caller takes out before
static void floor1(rd *r, wr *w) {
  uint32_t parts = pass(r, w, 5), cls[32], dims[16] = {0}, top = 0;
  for (uint32_t i = 0; i < parts; i++) {
    cls[i] = pass(r, w, 4);
    if (cls[i] + 1 > top) top = cls[i] + 1;
  }
  for (uint32_t c = 0; c < top; c++) {
    dims[c] = pass(r, w, 3) + 1;
    uint32_t sub = pass(r, w, 2);
    if (sub) pass(r, w, 8);
    for (uint32_t k = 0; k < 1u << sub; k++) pass(r, w, 8);
  }
  pass(r, w, 2);
  unsigned range = pass(r, w, 4);
  for (uint32_t i = 0; i < parts; i++)
    for (uint32_t k = 0; k < dims[cls[i]]; k++) pass(r, w, range);
}

static void residue(rd *r, wr *w) {
  pass(r, w, 24), pass(r, w, 24), pass(r, w, 24);
  uint32_t classes = pass(r, w, 6) + 1, books = 0;
  pass(r, w, 8);
  for (uint32_t i = 0; i < classes; i++) {
    uint32_t c = pass(r, w, 3);
    if (pass(r, w, 1)) c |= pass(r, w, 5) << 3;
    for (; c; c >>= 1) books += c & 1;
  }
  for (uint32_t i = 0; i < books; i++) pass(r, w, 8);
}

static void mapping(rd *r, wr *w, unsigned channels) {
  uint32_t submaps = pass(r, w, 1) ? pass(r, w, 4) + 1 : 1;
  if (pass(r, w, 1)) {
    unsigned bits = ilog(channels - 1);
    for (uint32_t steps = pass(r, w, 8) + 1; steps; steps--) pass(r, w, bits), pass(r, w, bits);
  }
  pass(r, w, 2);
  if (submaps > 1)
    for (unsigned c = 0; c < channels; c++) pass(r, w, 4);
  for (uint32_t i = 0; i < submaps; i++) pass(r, w, 8), pass(r, w, 8), pass(r, w, 8);
}

// The setup header p[0..n) as the game's setup packet in w; which modes have long blocks in long[], their count in
// *nmodes: 1, or 0 with err
static int setup(const uint8_t *p, size_t n, unsigned channels, const sb_books *lib, wr *w, uint8_t long_[64],
                 uint32_t *nmodes, char *err, size_t errlen) {
  if (n < 8 || memcmp(p, "\5vorbis", 7)) return fail(err, errlen, "no Vorbis setup header");
  rd r = {p + 7, (uint64_t)(n - 7) * 8, 0, 0};
  uint32_t books = pass(&r, w, 8) + 1;
  for (uint32_t b = 0; b < books; b++) {
    // the codebook in each form it can have in the library (one when its lengths are ordered, else one for each
    // width of the length field from the least that holds them), until the library has one
    int id = -1;
    rd at = r;
    for (unsigned width = 0, least = 0; width <= 7 && id < 0; width++) {
      wr bw = {0};
      at = r;
      if (book_packed(&at, &bw, width, &least)) id = book_find(lib, bw.d, bw.n);
      free(bw.d);
      if (at.bad) return fail(err, errlen, "the setup header ends in codebook %u", b);
      if (!least) break;
      if (width + 1 < least) width = least - 1;
    }
    r = at;
    if (id < 0) return fail(err, errlen, "codebook %u of %u is not one the game has", b, books);
    put(w, (uint32_t)id, 10);
  }
  for (uint32_t i = get(&r, 6) + 1; i; i--)
    if (get(&r, 16)) return fail(err, errlen, "an unknown time domain transform");
  for (uint32_t i = pass(&r, w, 6) + 1; i && !r.bad; i--) {
    if (get(&r, 16) != 1) return fail(err, errlen, "a floor of type 0");
    floor1(&r, w);
  }
  for (uint32_t i = pass(&r, w, 6) + 1; i && !r.bad; i--) {
    uint32_t type = get(&r, 16);
    if (type > 2) return fail(err, errlen, "an unknown residue type");
    put(w, type, 2);
    residue(&r, w);
  }
  for (uint32_t i = pass(&r, w, 6) + 1; i && !r.bad; i--) {
    if (get(&r, 16)) return fail(err, errlen, "an unknown mapping type");
    mapping(&r, w, channels);
  }
  *nmodes = pass(&r, w, 6) + 1;
  for (uint32_t i = 0; i < *nmodes; i++) {
    long_[i] = (uint8_t)pass(&r, w, 1);
    if (get(&r, 16) | get(&r, 16)) return fail(err, errlen, "an unknown window or transform");
    pass(&r, w, 8);
  }
  if (!get(&r, 1) || r.bad) return fail(err, errlen, "the setup header is cut short");
  if (w->bad) return fail(err, errlen, "out of memory");
  return 1;
}

// --- the file ----------------------------------------------------------------------------------------------------
typedef struct {
  uint8_t *d;
  size_t n, cap;
  int bad;
} buf;

static void add(buf *b, const void *p, size_t n) {
  if (b->bad) return;
  if (n > b->cap - b->n) {
    size_t cap = b->cap ? b->cap * 2 : 1 << 16;
    while (cap - b->n < n) cap *= 2;
    uint8_t *d = realloc(b->d, cap);
    if (!d) {
      b->bad = 1;
      return;
    }
    b->d = d, b->cap = cap;
  }
  if (n) memcpy(b->d + b->n, p, n);
  b->n += n;
}
static void add16(buf *b, uint32_t v) { add(b, (uint8_t[]){(uint8_t)v, (uint8_t)(v >> 8)}, 2); }
static void add32(buf *b, uint32_t v) { add(b, (uint8_t[]){(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)}, 4); }

typedef struct {
  const sb_books *lib;
  char *err;
  size_t errlen;
  int failed;
  uint32_t seen;  // packets so far
  unsigned channels, bs[2], exp[2], mode_bits;
  uint32_t rate, nmodes;
  uint8_t long_[64];
  buf body, seek;  // setup packet and audio packets; the seek table
  size_t first;    // where the audio packets start in body
  uint32_t audio, largest, prev;
  uint64_t frames, granule;  // decoded by the block sizes; the last page's
  uint64_t seek_frames, seek_off;
} conv;

// One packet of the stream, in order; granule: of the page it ends on when it is the last to end there, else -1
static int packet(conv *c, const uint8_t *p, size_t n, int64_t granule) {
  uint32_t k = c->seen++;
  if (k == 0) {
    if (n < 30 || memcmp(p, "\1vorbis", 7) || p[7] | p[8] | p[9] | p[10])
      return fail(c->err, c->errlen, "not a Vorbis stream");
    c->channels = p[11];
    c->rate = (uint32_t)p[12] | (uint32_t)p[13] << 8 | (uint32_t)p[14] << 16 | (uint32_t)p[15] << 24;
    c->exp[0] = p[28] & 15, c->exp[1] = p[28] >> 4;
    c->bs[0] = 1u << c->exp[0], c->bs[1] = 1u << c->exp[1];
    if (c->channels < 1 || c->channels > 2) return fail(c->err, c->errlen, "%u channels: one or two are known", c->channels);
    if (!c->rate || c->exp[0] < 6 || c->exp[1] > 13 || c->exp[0] > c->exp[1]) return fail(c->err, c->errlen, "a damaged Vorbis header");
    return 1;
  }
  if (k == 1) return 1;  // the comments
  if (k == 2) {
    wr w = {0};
    int ok = setup(p, n, c->channels, c->lib, &w, c->long_, &c->nmodes, c->err, c->errlen);
    size_t len = (size_t)((w.n + 7) / 8);
    if (ok && len > 0xffff) ok = fail(c->err, c->errlen, "the setup is too large");
    if (ok) add16(&c->body, (uint32_t)len), add(&c->body, w.d, len);
    free(w.d);
    c->mode_bits = ilog(c->nmodes - 1);
    c->first = c->body.n;
    return ok;
  }
  // audio: the type bit goes, and after the mode number a long block's two window flags
  rd r = {p, (uint64_t)n * 8, 0, 0};
  if (!n || get(&r, 1)) return 1;  // not audio: nothing to play
  wr w = {0};
  uint32_t mode = pass(&r, &w, c->mode_bits);
  if (mode >= c->nmodes) {
    free(w.d);
    return fail(c->err, c->errlen, "a packet of a mode the stream lacks");
  }
  if (c->long_[mode]) get(&r, 2);
  for (uint64_t left = r.n - r.p; left && !r.bad;) {
    unsigned take = left < 32 ? (unsigned)left : 32;
    pass(&r, &w, take);
    left -= take;
  }
  size_t len = (size_t)((w.n + 7) / 8);
  int ok = !r.bad && !w.bad && len <= 0xffff;
  if (ok) {
    uint32_t cur = c->bs[c->long_[mode]];
    if (c->audio) c->frames += (c->prev + cur) / 4;
    c->prev = cur;
    // a seek entry for this packet when the last is far enough back, in frames or (a dense stream) in bytes
    uint64_t off = c->body.n, f = c->frames;  // frames decoded through this packet
    if (c->audio && (f - c->seek_frames >= SEEK_FRAMES || off - c->seek_off >= 0xc000)) {
      if (f - c->seek_frames > 0xffff || off - c->seek_off > 0xffff) ok = 0;
      else add16(&c->seek, (uint32_t)(f - c->seek_frames)), add16(&c->seek, (uint32_t)(off - c->seek_off));
      c->seek_frames = f, c->seek_off = off;
    }
    add16(&c->body, (uint32_t)len), add(&c->body, w.d, len);
    if (len > c->largest) c->largest = (uint32_t)len;
    c->audio++;
    if (granule > 0) c->granule = (uint64_t)granule;
  }
  free(w.d);
  return ok ? 1 : fail(c->err, c->errlen, "a packet that cannot be written (packet %u)", k);
}

uint8_t *sb_wem_from_ogg(const uint8_t *ogg, size_t n, const sb_books *lib, size_t *out_n, char *err, size_t errlen) {
  conv c = {.lib = lib, .err = err, .errlen = errlen};
  buf cur = {0}, out = {0};
  uint8_t *res = 0;
  if (errlen) err[0] = 0;
  // the pages of the first stream: a packet is its segments up to one shorter than 255
  uint32_t serial = 0;
  int have_serial = 0, ok = 1;
  size_t o = 0;
  while (ok && o + 27 <= n) {
    if (memcmp(ogg + o, "OggS", 4) || ogg[o + 4]) {
      ok = fail(err, errlen, o ? "damaged Ogg pages" : "not an Ogg file");
      break;
    }
    unsigned nseg = ogg[o + 26];
    if (o + 27 + nseg > n) break;  // cut short: what was whole is played
    uint32_t ser = (uint32_t)ogg[o + 14] | (uint32_t)ogg[o + 15] << 8 | (uint32_t)ogg[o + 16] << 16 | (uint32_t)ogg[o + 17] << 24;
    int64_t granule = 0;
    for (int i = 7; i >= 0; i--) granule = (int64_t)((uint64_t)granule << 8 | ogg[o + 6 + i]);
    const uint8_t *lace = ogg + o + 27;
    size_t q = o + 27 + nseg, total = 0;
    unsigned ends = 0;
    for (unsigned i = 0; i < nseg; i++) total += lace[i], ends += lace[i] < 255;
    if (q + total > n) break;
    if (!have_serial) serial = ser, have_serial = 1;
    if (ser == serial) {
      if (!(ogg[o + 5] & 1) && cur.n) cur.n = 0;  // a page that does not continue a packet: the open one is lost
      for (unsigned i = 0; i < nseg && ok; i++) {
        add(&cur, ogg + q, lace[i]);
        q += lace[i];
        if (lace[i] < 255) {
          ok = !cur.bad && packet(&c, cur.d, cur.n, --ends ? -1 : granule);
          cur.n = 0;
        }
      }
    }
    o = o + 27 + nseg + total;
  }
  if (ok && !o) ok = fail(err, errlen, "not an Ogg file");
  if (ok && (c.seen < 4 || !c.audio)) ok = fail(err, errlen, "no audio in the stream");
  if (ok && (cur.bad || c.body.bad || c.seek.bad)) ok = fail(err, errlen, "out of memory");
  if (ok) {
    // the length as played: the last page's granule position when it cuts the last block short
    uint64_t frames = c.granule && c.granule <= c.frames ? c.granule : c.frames, extra = c.frames - frames;
    uint64_t data = c.seek.n + c.body.n;
    if (frames > 0xffffffffu || extra > 0xffff || data > 0xfffffff0u - 0x100) ok = fail(err, errlen, "the stream is too long");
    if (ok) {
      uint32_t mask = c.channels == 1 ? 4 : 3;
      add(&out, "RIFF", 4), add32(&out, (uint32_t)(4 + 8 + 0x42 + 8 + data));
      add(&out, "WAVEfmt ", 8), add32(&out, 0x42);
      add16(&out, 0xffff), add16(&out, c.channels), add32(&out, c.rate);
      add32(&out, frames ? (uint32_t)(data * c.rate / frames) : 0), add16(&out, 0), add16(&out, 0);
      add16(&out, 0x30), add16(&out, 0), add32(&out, mask);
      add32(&out, (uint32_t)frames), add32(&out, (uint32_t)c.first), add32(&out, (uint32_t)c.body.n);
      add16(&out, 0), add16(&out, (uint32_t)extra);
      add32(&out, (uint32_t)c.seek.n), add32(&out, (uint32_t)(c.seek.n + c.first));
      add16(&out, c.largest), add16(&out, (uint32_t)extra);
      add32(&out, SETUP_ROOM), add32(&out, SETUP_ROOM);
      add32(&out, (uint32_t)crc32(0, c.body.d + 2, (uInt)(c.first - 2)));
      add(&out, (uint8_t[]){(uint8_t)c.exp[0], (uint8_t)c.exp[1]}, 2);
      add(&out, "data", 4), add32(&out, (uint32_t)data);
      add(&out, c.seek.d, c.seek.n), add(&out, c.body.d, c.body.n);
      if (out.bad) ok = fail(err, errlen, "out of memory");
    }
  }
  if (ok) res = out.d, *out_n = out.n;
  else free(out.d);
  free(cur.d), free(c.body.d), free(c.seek.d);
  return res;
}
