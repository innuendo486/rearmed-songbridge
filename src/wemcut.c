// See wemcut.h; wem.c has the file's format. What a cut rests on:
//
// A Vorbis packet is one block, short or long (its mode says which), and blocks overlap by half: decoding packet i
// after packet i-1 gives (size[i-1] + size[i]) / 4 frames, and the first packet a decoder is given gives none: it only
// primes the overlap. That is how the format seeks, too: a seek table entry names a packet and the frames decoded
// through it, and decoding from that packet yields the frames after those. So with F(i) the frames decoded through
// packet i of the source, the packets s..e as a stream of their own play the source's frames F(s) to F(e), exactly:
// what packet s lacks (the packet before it, and with that the shape of its window's first half) only touches the
// half of its block that is thrown away. The cut's header says F(e) - F(s) frames, as wem.c counts them for a whole
// stream. A packet does not say which window its long block has; the decoder takes that from the neighbours' sizes,
// and for a stream's first and last packet from nothing, which again only shapes halves that are not played.
//
// The result is the source's setup packet, the packets s..e and a new seek table, under the source's "fmt " with the
// counts and sizes set (the room for the decoder's set-up, the set-up's key and the block sizes stay: the setup is
// the same). When the cut reaches the source's last packet, the frames the source decodes past its length are the
// cut's too. Nothing is faded: a preview starts and stops at full level (the game's own previews fade, in the audio).
//
// The modes' block sizes are at the end of the setup packet, so that is read through: the same walk as wem.c's
// setup(), without the writing.
#include "wemcut.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SEEK_FRAMES 16384u  // as wem.c: an entry about this often
#define SEEK_BYTES 0xc000u
#define FMT 0x42u

// --- bits, least significant first -------------------------------------------------------------------------------
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

static unsigned ilog(uint32_t v) {
  unsigned n = 0;
  for (; v; v >>= 1) n++;
  return n;
}

// The reason in err; 0, for the caller to return
static uint8_t *none(char *err, size_t errlen, const char *fmt, ...) {
  if (errlen) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
  }
  return 0;
}

static uint32_t u16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t u32(const uint8_t *p) { return u16(p) | u16(p + 2) << 16; }
static void p16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
static void p32(uint8_t *p, uint32_t v) { p16(p, v), p16(p + 2, v >> 16); }

// --- the setup packet: which modes have long blocks --------------------------------------------------------------
static void floor1(rd *r) {
  uint32_t parts = get(r, 5), cls[32], dims[16] = {0}, top = 0;
  for (uint32_t i = 0; i < parts; i++) {
    cls[i] = get(r, 4);
    if (cls[i] + 1 > top) top = cls[i] + 1;
  }
  for (uint32_t c = 0; c < top; c++) {
    dims[c] = get(r, 3) + 1;
    uint32_t sub = get(r, 2);
    if (sub) get(r, 8);
    for (uint32_t k = 0; k < 1u << sub; k++) get(r, 8);
  }
  get(r, 2);
  unsigned range = get(r, 4);
  for (uint32_t i = 0; i < parts; i++)
    for (uint32_t k = 0; k < dims[cls[i]]; k++) get(r, range);
}

static void residue(rd *r) {
  get(r, 24), get(r, 24), get(r, 24);
  uint32_t classes = get(r, 6) + 1, books = 0;
  get(r, 8);
  for (uint32_t i = 0; i < classes; i++) {
    uint32_t c = get(r, 3);
    if (get(r, 1)) c |= get(r, 5) << 3;
    for (; c; c >>= 1) books += c & 1;
  }
  for (uint32_t i = 0; i < books; i++) get(r, 8);
}

static void mapping(rd *r, unsigned channels) {
  uint32_t submaps = get(r, 1) ? get(r, 4) + 1 : 1;
  if (get(r, 1)) {
    unsigned bits = ilog(channels - 1);
    for (uint32_t steps = get(r, 8) + 1; steps; steps--) get(r, bits), get(r, bits);
  }
  get(r, 2);
  if (submaps > 1)
    for (unsigned c = 0; c < channels; c++) get(r, 4);
  for (uint32_t i = 0; i < submaps; i++) get(r, 8), get(r, 8), get(r, 8);
}

// The modes of the setup packet p[0..n): their count, 0 when the packet is damaged; long_[i]: mode i's blocks are long
static uint32_t modes(const uint8_t *p, size_t n, unsigned channels, uint8_t long_[64]) {
  rd r = {p, (uint64_t)n * 8, 0, 0};
  for (uint32_t i = get(&r, 8) + 1; i && !r.bad; i--) get(&r, 10);  // the codebooks, by their numbers
  for (uint32_t i = get(&r, 6) + 1; i && !r.bad; i--) floor1(&r);
  for (uint32_t i = get(&r, 6) + 1; i && !r.bad; i--) get(&r, 2), residue(&r);
  for (uint32_t i = get(&r, 6) + 1; i && !r.bad; i--) mapping(&r, channels);
  uint32_t count = get(&r, 6) + 1;
  for (uint32_t i = 0; i < count; i++) long_[i] = (uint8_t)get(&r, 1), get(&r, 8);
  return r.bad ? 0 : count;
}

// --- the packets -------------------------------------------------------------------------------------------------
typedef struct {
  const uint8_t *d;  // "data"
  size_t n;
  uint32_t bs[2], nmodes;
  unsigned mode_bits;
  uint8_t long_[64];
} src;

// A place between packets: o in data, `count` packets before it, `frames` decoded through them, the last one's block
typedef struct {
  size_t o;
  uint64_t count, frames;
  uint32_t prev;
} pos;

// The packet at a, and a moved past it: 1; 0 at the end (also at a packet that is cut short), -1 at a damaged one
static int step(const src *s, pos *a) {
  if (s->n - a->o < 2) return 0;
  size_t len = u16(s->d + a->o);
  if (len > s->n - a->o - 2) return 0;
  if (!len) return -1;
  uint32_t mode = s->d[a->o + 2] & ((1u << s->mode_bits) - 1);
  if (mode >= s->nmodes) return -1;
  uint32_t cur = s->bs[s->long_[mode]];
  if (a->count) a->frames += (a->prev + cur) / 4;
  a->prev = cur, a->count++, a->o += 2 + len;
  return 1;
}

uint8_t *sb_wem_preview(const uint8_t *wem, size_t n, double start, double seconds, size_t *out_n, uint64_t *at,
                        char *err, size_t errlen) {
  if (errlen) err[0] = 0;
  if (n < 12 || memcmp(wem, "RIFF", 4) || memcmp(wem + 8, "WAVE", 4)) return none(err, errlen, "not a .wem file");
  // the chunks; one that runs past the end is taken as far as the file goes
  const uint8_t *f = 0;
  src s = {0};
  int have_data = 0;
  for (size_t o = 12; n - o >= 8;) {
    size_t len = u32(wem + o + 4), room = n - o - 8;
    if (len > room) len = room;
    if (!memcmp(wem + o, "fmt ", 4) && !f && len >= FMT) f = wem + o + 8;
    if (!memcmp(wem + o, "data", 4) && !have_data) s.d = wem + o + 8, s.n = len, have_data = 1;
    o += 8 + len;
    if (len & 1 && o < n) o++;
  }
  if (!f || !have_data || u16(f) != 0xffff) return none(err, errlen, "not a Wwise Vorbis file");
  unsigned channels = u16(f + 2), e0 = f[0x40], e1 = f[0x41];
  uint32_t rate = u32(f + 4), seek = u32(f + 0x28), first = u32(f + 0x2c);
  if (!channels || channels > 255 || !rate || e0 < 6 || e1 > 13 || e0 > e1) return none(err, errlen, "a damaged header");
  // seek table | setup packet | audio packets
  if (seek > s.n || first > s.n || first < seek || first - seek < 3) return none(err, errlen, "a header whose offsets are past the data");
  size_t setup_len = u16(s.d + seek), setup_all = first - seek;  // the packet; with its length (and what else is there)
  if (setup_len > setup_all - 2) return none(err, errlen, "a setup packet past the audio's start");
  s.bs[0] = 1u << e0, s.bs[1] = 1u << e1;
  s.nmodes = modes(s.d + seek + 2, setup_len, channels, s.long_);
  if (!s.nmodes) return none(err, errlen, "a damaged setup packet");
  s.mode_bits = ilog(s.nmodes - 1);

  // the whole stream's length: by its packets, less what the header says is decoded past the end
  pos all = {first, 0, 0, 0};
  int r;
  while ((r = step(&s, &all)) > 0) {}
  if (r < 0) return none(err, errlen, "a damaged packet (packet %llu)", (unsigned long long)all.count);
  if (!all.count) return none(err, errlen, "no audio packets");
  uint64_t played = u32(f + 0x18);
  if (!played || played > all.frames) played = all.frames;
  uint64_t extra = all.frames - played;

  // the stretch asked for, in frames; then in packets
  if (!(seconds > 0)) seconds = SB_PREVIEW_SECONDS;
  double want_d = seconds * rate, from_d = start >= 0 ? start * rate : (double)played / 3;  // a NaN start: the default
  uint64_t want = want_d >= (double)played ? played : (uint64_t)want_d;
  uint64_t from = from_d >= (double)played ? played : (uint64_t)from_d;
  if (from > played - want) from = played - want;
  // packet s: the last with F(s) <= from (the first has F = 0); `before` and `a` are the places before and after it
  pos before = {first, 0, 0, 0}, a = before, c;
  step(&s, &a);
  for (c = a; step(&s, &c) > 0 && c.frames <= from;) before = a, a = c;
  uint64_t f0 = a.frames;
  pos end = a;
  while (end.frames - f0 < want && step(&s, &end) > 0) {}
  int to_end = end.count == all.count;
  uint64_t frames = end.frames - f0, over = 0;
  if (to_end) over = extra < frames ? extra : frames;
  frames -= over;

  // the new seek table, and the largest packet
  size_t body = setup_all + (end.o - before.o), nseek = 0, cap = 64;
  uint8_t *tab = malloc(cap);
  uint32_t largest = 0;
  int ok = tab != 0;
  uint64_t sf = 0, so = 0;
  pos q = {before.o, 0, 0, 0};
  while (ok && q.o < end.o) {
    size_t po = q.o, off = setup_all + (po - before.o);
    uint64_t k = q.count;
    step(&s, &q);  // known to be whole and sound: the walk above passed it
    uint32_t len = (uint32_t)(q.o - po - 2);
    if (len > largest) largest = len;
    if (k && (q.frames - sf >= SEEK_FRAMES || off - so >= SEEK_BYTES)) {
      if (q.frames - sf > 0xffff || off - so > 0xffff) {
        none(err, errlen, "packets too far apart for a seek table");
        ok = 0;
        break;
      }
      if (nseek + 4 > cap) {
        uint8_t *t = realloc(tab, cap *= 2);
        if (!t) {
          ok = 0;
          break;
        }
        tab = t;
      }
      p16(tab + nseek, (uint32_t)(q.frames - sf)), p16(tab + nseek + 2, (uint32_t)(off - so));
      nseek += 4, sf = q.frames, so = off;
    }
  }
  uint64_t data = (uint64_t)nseek + body;
  uint8_t *out = ok && data < 0xffffff00u ? malloc(12 + 8 + FMT + 8 + (size_t)data) : 0;
  if (!out) {
    if (ok) none(err, errlen, "out of memory");
    free(tab);
    return 0;
  }
  uint8_t *p = out;
  memcpy(p, "RIFF", 4), p32(p + 4, (uint32_t)(4 + 8 + FMT + 8 + data)), memcpy(p + 8, "WAVEfmt ", 8), p32(p + 16, FMT);
  p += 20;
  memcpy(p, f, FMT);
  p32(p + 8, frames ? (uint32_t)(data * rate / frames) : 0);
  p16(p + 0x10, 0x30);  // the chunk is cut to the fields known: what a longer one had after them is gone
  p32(p + 0x18, (uint32_t)frames), p32(p + 0x1c, (uint32_t)setup_all), p32(p + 0x20, (uint32_t)body);
  p16(p + 0x24, 0), p16(p + 0x26, (uint32_t)over);
  p32(p + 0x28, (uint32_t)nseek), p32(p + 0x2c, (uint32_t)(nseek + setup_all));
  p16(p + 0x30, largest), p16(p + 0x32, (uint32_t)over);
  p += FMT;
  memcpy(p, "data", 4), p32(p + 4, (uint32_t)data);
  p += 8;
  if (nseek) memcpy(p, tab, nseek);
  memcpy(p + nseek, s.d + seek, setup_all);
  memcpy(p + nseek + setup_all, s.d + before.o, end.o - before.o);
  free(tab);
  *out_n = 12 + 8 + FMT + 8 + (size_t)data;
  if (at) *at = f0;
  return out;
}
