// See psarc.h. The archive: a 32-byte header, the table of contents (30-byte entries, then the block sizes, both
// AES-256-CFB encrypted when the flags have 4), then the entries' blocks. An entry is cut in 64 KiB blocks, each
// zlib-compressed or stored as it is when that is not smaller (block size 0 in the table: a full stored block).
// Entry 0 is the list of the other entries' names.
//
// The archive for the other platform, as served:
//   header, new table of contents | name list, aggregate graph (rebuilt, in memory) | song files (stored, made when
//   read) | the file from its first entry to its end, unchanged
// Entries that are not rebuilt keep their blocks and point into the last part. The old name list, aggregate graph
// and song files are still in that part; nothing points at them.
#include "psarc.h"

#include <CommonCrypto/CommonCrypto.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

#include "keys.h"

#define BLOCK 0x10000u
#define ENTRY 30u
#define MAX_TOC (64u << 20)
#define MAX_REBUILT (256u << 20)  // a name list, aggregate graph or song file larger than this is not one
#define SNG_MAGIC 0x4au
#define SNG_HEAD 24u  // magic, flags, IV
#define SIG_LEN 56u

typedef struct {
  uint8_t md5[16];
  uint32_t zi, nb;  // first block, blocks
  uint64_t len, off, stored;
} ent;

struct sb_arc {
  uint32_t n, nz, flags, toc_len;
  int64_t size;
  ent *e;
  uint16_t *zl;
  char *names;  // entry 0, with a 0 after it
  size_t names_len;
};

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t be40(const uint8_t *p) { return (uint64_t)p[0] << 32 | be32(p + 1); }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v; }
static void put40(uint8_t *p, uint64_t v) { p[0] = (uint8_t)(v >> 32), put32(p + 1, (uint32_t)v); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]; }

static int pread_all(int fd, void *buf, size_t n, int64_t off) {
  uint8_t *b = buf;
  while (n) {
    ssize_t r = pread(fd, b, n, off);
    if (r <= 0) return 0;
    b += r, n -= (size_t)r, off += r;
  }
  return 1;
}

// --- AES-256: CFB (128-bit feedback) and CTR (big-endian counter), as openssl's aes-256-cfb and aes-256-ctr -------
static void ecb(const uint8_t key[32], const uint8_t *in, uint8_t *out, size_t n) {
  size_t moved = 0;
  CCCrypt(kCCEncrypt, kCCAlgorithmAES, kCCOptionECBMode, key, 32, 0, in, n, out, n, &moved);
}
static void cfb(const uint8_t key[32], const uint8_t iv[16], uint8_t *d, size_t n, int decrypt) {
  uint8_t prev[16], ks[16];
  memcpy(prev, iv, 16);
  for (size_t i = 0; i < n; i += 16) {
    size_t k = n - i < 16 ? n - i : 16;
    ecb(key, prev, ks, 16);
    if (decrypt) memcpy(prev, d + i, k);
    for (size_t j = 0; j < k; j++) d[i + j] ^= ks[j];
    if (!decrypt) memcpy(prev, d + i, k);
  }
}
static void ctr(const uint8_t key[32], const uint8_t iv[16], uint8_t *d, size_t n) {
  uint8_t c[16], in[4096], ks[4096];
  memcpy(c, iv, 16);
  for (size_t i = 0; i < n;) {
    size_t m = n - i < sizeof in ? n - i : sizeof in, blocks = (m + 15) / 16;
    for (size_t b = 0; b < blocks; b++) {
      memcpy(in + 16 * b, c, 16);
      for (int k = 15; k >= 0 && ++c[k] == 0; k--) {}
    }
    ecb(key, in, ks, blocks * 16);
    for (size_t j = 0; j < m; j++) d[i + j] ^= ks[j];
    i += m;
  }
}

// --- reading -----------------------------------------------------------------------------------------------------
void sb_arc_free(sb_arc *a) {
  if (!a) return;
  free(a->e);
  free(a->zl);
  free(a->names);
  free(a);
}

// Entry i's content (len bytes and a 0), or 0
static uint8_t *read_entry(const sb_arc *a, int fd, uint32_t i) {
  const ent *e = &a->e[i];
  if (e->len > MAX_REBUILT) return 0;
  uint8_t *out = malloc(e->len + 1), *z = malloc(BLOCK);
  uint64_t off = e->off, done = 0;
  for (uint32_t b = 0; out && z && b < e->nb; b++) {
    uint32_t zs = a->zl[e->zi + b] ? a->zl[e->zi + b] : BLOCK;
    uint64_t want = e->len - done < BLOCK ? e->len - done : BLOCK;
    if (!pread_all(fd, z, zs, (int64_t)off)) goto fail;
    int ok = 0;
    if (zs < BLOCK) {
      uLongf got = (uLongf)want;
      ok = uncompress(out + done, &got, z, zs) == Z_OK && got == want;
    }
    if (!ok) {
      if (zs != want) goto fail;  // neither a zlib stream of the block nor the block itself
      memcpy(out + done, z, zs);
    }
    off += zs, done += want;
  }
  if (!out || !z) goto fail;
  free(z);
  out[e->len] = 0;
  return out;
fail:
  free(out);
  free(z);
  return 0;
}

static uint32_t count_lines(const char *s, size_t n) {
  if (!n) return 0;
  uint32_t k = 1;
  for (size_t i = 0; i + 1 < n; i++) k += s[i] == '\n';  // a newline at the very end ends the last name
  return k;
}

sb_arc *sb_parse(int fd, int64_t size) {
  uint8_t h[32], *toc = 0;
  if (size < 32 || !pread_all(fd, h, 32, 0)) return 0;
  if (memcmp(h, "PSAR", 4) || memcmp(h + 8, "zlib", 4)) return 0;
  uint32_t toc_len = be32(h + 12), esize = be32(h + 16), n = be32(h + 20), bsize = be32(h + 24), flags = be32(h + 28);
  if (esize != ENTRY || bsize != BLOCK || n < 1 || n > (1u << 20)) return 0;
  if (toc_len > MAX_TOC || (uint64_t)toc_len < 32 + (uint64_t)ENTRY * n || (int64_t)toc_len > size) return 0;
  sb_arc *a = calloc(1, sizeof *a);
  size_t tn = toc_len - 32;
  toc = malloc(tn + 1);
  if (!a || !toc || !pread_all(fd, toc, tn, 32)) goto fail;
  if (flags & 4) cfb(sb_toc_key, sb_toc_iv, toc, tn, 1);
  a->n = n, a->flags = flags, a->toc_len = toc_len, a->size = size;
  a->nz = (uint32_t)((tn - (size_t)ENTRY * n) / 2);
  a->e = calloc(n, sizeof *a->e);
  a->zl = calloc(a->nz + 1, sizeof *a->zl);
  if (!a->e || !a->zl) goto fail;
  for (uint32_t i = 0; i < a->nz; i++) a->zl[i] = (uint16_t)(toc[ENTRY * n + 2 * i] << 8 | toc[ENTRY * n + 2 * i + 1]);
  for (uint32_t i = 0; i < n; i++) {
    const uint8_t *p = toc + ENTRY * i;
    ent *e = &a->e[i];
    memcpy(e->md5, p, 16);
    e->zi = be32(p + 16), e->len = be40(p + 20), e->off = be40(p + 25);
    uint64_t nb = (e->len + BLOCK - 1) / BLOCK;
    if (nb > a->nz || e->zi > a->nz - nb) goto fail;
    e->nb = (uint32_t)nb;
    for (uint32_t b = 0; b < e->nb; b++) e->stored += a->zl[e->zi + b] ? a->zl[e->zi + b] : BLOCK;
    if (e->nb && (e->off < toc_len || e->off > (uint64_t)size || e->stored > (uint64_t)size - e->off)) goto fail;
  }
  free(toc);
  toc = 0;
  a->names = (char *)read_entry(a, fd, 0);
  a->names_len = (size_t)a->e[0].len;
  if (!a->names || memchr(a->names, 0, a->names_len) || count_lines(a->names, a->names_len) != n - 1) goto fail;
  return a;
fail:
  free(toc);
  sb_arc_free(a);
  return 0;
}

// Name k (from 0: entry k + 1) of a name list: where it starts, its length in *len
static const char *name_at(const char *s, size_t n, const char *prev, size_t prev_len, size_t *len) {
  const char *p = prev ? prev + prev_len + 1 : s, *end = s + n;
  if (p > end) p = end;
  const char *nl = memchr(p, '\n', (size_t)(end - p));
  *len = (size_t)((nl ? nl : end) - p);
  return p;
}

static int ends_with(const char *s, size_t n, const char *sfx) {
  size_t k = strlen(sfx);
  return n >= k && !memcmp(s + n - k, sfx, k);
}
static int contains(const char *s, size_t n, const char *sub) {
  size_t k = strlen(sub);
  for (size_t i = 0; i + k <= n; i++)
    if (!memcmp(s + i, sub, k)) return 1;
  return 0;
}

void sb_arc_keys(const sb_arc *a, void (*f)(void *ctx, const char *key), void *ctx) {
  const char *p = 0;
  size_t len = 0;
  for (uint32_t k = 0; k + 1 < a->n; k++) {
    p = name_at(a->names, a->names_len, p, len, &len);
    if (len < 13 || memcmp(p, "gamexblocks/", 12) || !ends_with(p, len, ".xblock")) continue;
    const char *b = p + len - 7, *s = b;
    while (s > p && s[-1] != '/') s--;
    char key[256];
    size_t kn = (size_t)(b - s);
    if (!kn || kn >= sizeof key) continue;
    for (size_t i = 0; i < kn; i++) key[i] = (char)(s[i] >= 'A' && s[i] <= 'Z' ? s[i] + 32 : s[i]);
    key[kn] = 0;
    f(ctx, key);
  }
}

// --- the other platform's texts ----------------------------------------------------------------------------------
// *s (len *n, malloc'd, a 0 after it) with every `from` replaced by `to`; 0 when out of memory
static int replace(char **s, size_t *n, const char *from, const char *to) {
  size_t fl = strlen(from), tl = strlen(to), hits = 0;
  for (size_t i = 0; i + fl <= *n;)
    if (!memcmp(*s + i, from, fl)) hits++, i += fl;
    else i++;
  if (!hits) return 1;
  char *out = malloc(*n + hits * tl - hits * fl + 1);
  if (!out) return 0;
  size_t o = 0;
  for (size_t i = 0; i < *n;)
    if (i + fl <= *n && !memcmp(*s + i, from, fl)) memcpy(out + o, to, tl), o += tl, i += fl;
    else out[o++] = (*s)[i++];
  out[o] = 0;
  free(*s);
  *s = out, *n = o;
  return 1;
}

// A path or the name list for platform `to`
static int paths_to(char **s, size_t *n, int to) {
  if (to == SB_MAC) return replace(s, n, "audio/windows", "audio/mac") && replace(s, n, "bin/generic", "bin/macos");
  return replace(s, n, "audio/mac", "audio/windows") && replace(s, n, "bin/macos", "bin/generic");
}

// The Mac's aggregate graph tags its song files "macos" as well; Windows' gives them no platform tag (its "dx9" is
// on the audio alone). Those lines are taken out before macos becomes dx9: of each node whose canonical path is
// /songs/bin/macos, the line `<node> <.../tag> "macos".`
static void drop_mac_sng_tags(char *s, size_t *n) {
  static const char canon[] = "canonical> \"/songs/bin/macos\"", tag[] = "/tag> \"macos\".";
  size_t o = 0, len = *n, nid = 0, cap = 0;
  char *copy = malloc(len + 1);
  struct id { const char *p; size_t n; } *ids = 0;  // "<urn:uuid:...>" of each song file's node
  if (!copy) return;
  memcpy(copy, s, len + 1);
  for (size_t i = 0; i < len;) {
    const char *line = copy + i, *nl = memchr(line, '\n', len - i);
    size_t ll = nl ? (size_t)(nl - line) : len - i;
    const char *gt = memchr(line, '>', ll);
    if (gt && line[0] == '<' && contains(line, ll, canon)) {
      if (nid == cap) {
        struct id *g = realloc(ids, (cap = cap ? cap * 2 : 16) * sizeof *ids);
        if (!g) break;
        ids = g;
      }
      ids[nid++] = (struct id){line, (size_t)(gt - line) + 1};
    }
    i += ll + (nl ? 1 : 0);
  }
  for (size_t i = 0; i < len;) {
    const char *line = copy + i, *nl = memchr(line, '\n', len - i);
    size_t ll = nl ? (size_t)(nl - line) : len - i, step = ll + (nl ? 1 : 0);
    int drop = 0;
    if (line[0] == '<' && ends_with(line, ll, tag))
      for (size_t k = 0; k < nid && !drop; k++) drop = ll > ids[k].n && !memcmp(line, ids[k].p, ids[k].n);
    if (!drop) memcpy(s + o, line, step), o += step;
    i += step;
  }
  if (o && o < len && s[o - 1] == '\n' && copy[len - 1] != '\n') o--;  // the last line went: so does the newline before it
  s[o] = 0;
  *n = o;
  free(ids);
  free(copy);
}

static int graph_to(char **s, size_t *n, int to) {
  if (to == SB_MAC) return paths_to(s, n, to) && replace(s, n, "dx9", "macos");
  drop_mac_sng_tags(*s, n);
  return paths_to(s, n, to) && replace(s, n, "macos", "dx9");
}

// --- the archive for the other platform --------------------------------------------------------------------------
typedef struct {
  uint32_t ent;
  int src;  // the platform whose key it has
  int64_t voff, len;
  _Atomic(uint8_t *) data;
} vsng;

struct sb_virt {
  const sb_arc *a;
  int to;
  int64_t size;
  uint8_t *head;  // header, table of contents, name list, aggregate graph
  int64_t head_len, tail_voff, tail_roff, tail_len;
  vsng *sng;
  uint32_t nsng;
  pthread_mutex_t mu;  // making a song file
};

typedef struct {
  uint8_t *d;
  size_t n;
  uint16_t *zl;
  uint32_t nz;
} blob;

// src cut in blocks, each compressed when that is smaller
static int pack(const uint8_t *src, size_t len, blob *b) {
  uint32_t nb = (uint32_t)((len + BLOCK - 1) / BLOCK);
  uLong bound = compressBound(BLOCK);
  uint8_t *tmp = malloc(bound);
  b->d = malloc(len + 1), b->zl = calloc(nb + 1, sizeof *b->zl), b->n = 0, b->nz = nb;
  if (!tmp || !b->d || !b->zl) {
    free(tmp);
    return 0;
  }
  for (uint32_t i = 0; i < nb; i++) {
    size_t raw = len - (size_t)i * BLOCK < BLOCK ? len - (size_t)i * BLOCK : BLOCK;
    uLongf zn = bound;
    if (compress2(tmp, &zn, src + (size_t)i * BLOCK, raw, 9) == Z_OK && zn < raw) {
      memcpy(b->d + b->n, tmp, zn), b->n += zn, b->zl[i] = (uint16_t)zn;
    } else {
      memcpy(b->d + b->n, src + (size_t)i * BLOCK, raw), b->n += raw, b->zl[i] = (uint16_t)(raw % BLOCK);
    }
  }
  free(tmp);
  return 1;
}

static int looks_zlib(const uint8_t *p) { return (p[0] & 0x0f) == 8 && (p[0] >> 4) <= 7 && ((p[0] << 8 | p[1]) % 31) == 0; }

// The song file d (len bytes) of platform `from`, for platform `to`, in place: decrypted with the one's key and
// encrypted with the other's (a plain one, flags 1: encrypted), the signature zero. The length stays. The file is
// served as stored blocks; a reader tells a stored block shorter than 64 KiB from a compressed one by its first two
// bytes, so the IV is changed until the last block's are not a zlib header.
static void sng_rekey(uint8_t *d, size_t len, int from, int to) {
  if (len < SNG_HEAD + 4 + SIG_LEN || le32(d) != SNG_MAGIC) return;  // not a song file: as it is
  uint32_t flags = le32(d + 4);
  uint8_t *body = d + SNG_HEAD;
  size_t bn = len - SNG_HEAD - SIG_LEN;
  if (flags == 3) {
    ctr(sb_sng_key[from], d + 8, body, bn);
  } else if (flags == 1) {
    // plain: its zlib stream must end before the signature's place, or there is no such place to zero
    z_stream z = {0};
    uint8_t sink[4096];
    if (inflateInit(&z) != Z_OK) return;
    z.next_in = body + 4, z.avail_in = (uInt)(len - SNG_HEAD - 4);
    int r;
    do {
      z.next_out = sink, z.avail_out = sizeof sink;
      r = inflate(&z, Z_NO_FLUSH);
    } while (r == Z_OK);
    size_t used = z.total_in;
    inflateEnd(&z);
    if (r != Z_STREAM_END || 4 + used > bn) return;
    memset(body + 4 + used, 0, bn - 4 - used);
    memset(d + 8, 0, 16);
    d[4] = 3;
  } else {
    return;
  }
  memset(d + len - SIG_LEN, 0, SIG_LEN);
  size_t last = len % BLOCK ? len - len % BLOCK : 0;  // a last block that is short and not the first
  for (;;) {
    ctr(sb_sng_key[to], d + 8, body, bn);
    if (!last || len - last < 2 || !looks_zlib(d + last)) return;
    ctr(sb_sng_key[to], d + 8, body, bn);
    for (int k = 0; k < 16 && ++d[8 + k] == 0; k++) {}
  }
}

static uint8_t *sng_get(sb_virt *v, int fd, vsng *s) {
  uint8_t *d = atomic_load(&s->data);
  if (d) return d;
  pthread_mutex_lock(&v->mu);
  d = atomic_load(&s->data);
  if (!d && (d = read_entry(v->a, fd, s->ent))) {
    sng_rekey(d, (size_t)s->len, s->src, v->to);
    atomic_store(&s->data, d);
  }
  pthread_mutex_unlock(&v->mu);
  return d;
}

void sb_virt_drop(sb_virt *v) {
  pthread_mutex_lock(&v->mu);
  for (uint32_t i = 0; i < v->nsng; i++) free(atomic_exchange(&v->sng[i].data, 0));
  pthread_mutex_unlock(&v->mu);
}

void sb_virt_free(sb_virt *v) {
  if (!v) return;
  sb_virt_drop(v);
  pthread_mutex_destroy(&v->mu);
  free(v->sng);
  free(v->head);
  free(v);
}

int64_t sb_virt_size(const sb_virt *v) { return v->size; }

sb_virt *sb_virt_build(const sb_arc *a, int fd, int to) {
  enum { KEEP, NAMES, GRAPH, SNG };
  uint32_t n = a->n;
  sb_virt *v = calloc(1, sizeof *v);
  uint8_t *kind = calloc(n, 1), *toc = 0;
  char *names = malloc(a->names_len + 1), *graph = 0;
  size_t names_len = a->names_len, graph_len = 0;
  blob bn = {0}, bg = {0};
  if (!v || !kind || !names) goto fail;
  pthread_mutex_init(&v->mu, 0);
  v->a = a, v->to = to;
  memcpy(names, a->names, names_len + 1);
  if (!paths_to(&names, &names_len, to) || !pack((const uint8_t *)names, names_len, &bn)) goto fail;

  // what each entry becomes
  kind[0] = NAMES;
  const char *p = 0;
  size_t len = 0;
  for (uint32_t i = 1; i < n; i++) {
    p = name_at(a->names, a->names_len, p, len, &len);
    int src = contains(p, len, "songs/bin/generic/") ? SB_PC : contains(p, len, "songs/bin/macos/") ? SB_MAC : -1;
    if (src >= 0 && src != to && a->e[i].len <= MAX_REBUILT) {
      kind[i] = SNG;
      v->nsng++;
    } else if (!graph && ends_with(p, len, "aggregategraph.nt")) {
      if (!(graph = (char *)read_entry(a, fd, i))) goto fail;
      graph_len = (size_t)a->e[i].len;
      if (!graph_to(&graph, &graph_len, to) || !pack((const uint8_t *)graph, graph_len, &bg)) goto fail;
      kind[i] = GRAPH;
    }
  }
  v->sng = calloc(v->nsng + 1, sizeof *v->sng);
  if (!v->sng) goto fail;

  uint64_t nz = bn.nz;
  for (uint32_t i = 1; i < n; i++) nz += kind[i] == GRAPH ? bg.nz : a->e[i].nb;
  uint64_t toc_len = 32 + (uint64_t)ENTRY * n + 2 * nz;
  if (toc_len > MAX_TOC) goto fail;
  v->head_len = (int64_t)(toc_len + bn.n + bg.n);
  int64_t sng_len = 0;
  for (uint32_t i = 1, k = 0; i < n; i++)
    if (kind[i] == SNG) {
      v->sng[k] = (vsng){.ent = i, .voff = v->head_len + sng_len, .len = (int64_t)a->e[i].len};
      sng_len += v->sng[k++].len;
    }
  v->tail_voff = v->head_len + sng_len;
  v->tail_roff = a->toc_len;
  v->tail_len = a->size - a->toc_len;
  v->size = v->tail_voff + v->tail_len;

  // the table of contents
  if (!(v->head = calloc(1, (size_t)v->head_len + 1))) goto fail;
  toc = v->head + 32;
  uint8_t *zt = toc + ENTRY * n;
  uint32_t zi = 0, ks = 0;
  const char *np = 0;
  size_t nl = 0;
  p = 0, len = 0;
  for (uint32_t i = 0; i < n; i++) {
    const ent *e = &a->e[i];
    uint8_t *t = toc + ENTRY * i;
    uint64_t off, elen = e->len;
    if (i) {
      memcpy(t, e->md5, 16);  // the name list's own stays zero
      p = name_at(a->names, a->names_len, p, len, &len);
      np = name_at(names, names_len, np, nl, &nl);
      if (nl != len || memcmp(p, np, len)) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        CC_MD5(np, (CC_LONG)nl, t);
#pragma clang diagnostic pop
      }
    }
    put32(t + 16, zi);
    if (kind[i] == NAMES || kind[i] == GRAPH) {
      const blob *b = kind[i] == NAMES ? &bn : &bg;
      off = kind[i] == NAMES ? toc_len : toc_len + bn.n;
      elen = kind[i] == NAMES ? names_len : graph_len;
      memcpy(v->head + off, b->d, b->n);
      for (uint32_t k = 0; k < b->nz; k++, zi++) zt[2 * zi] = (uint8_t)(b->zl[k] >> 8), zt[2 * zi + 1] = (uint8_t)b->zl[k];
    } else if (kind[i] == SNG) {
      vsng *s = &v->sng[ks++];
      s->src = to == SB_MAC ? SB_PC : SB_MAC;
      off = (uint64_t)s->voff;
      for (uint32_t k = 0; k < e->nb; k++, zi++) {
        uint64_t raw = e->len - (uint64_t)k * BLOCK < BLOCK ? e->len - (uint64_t)k * BLOCK : BLOCK;
        zt[2 * zi] = (uint8_t)((raw % BLOCK) >> 8), zt[2 * zi + 1] = (uint8_t)(raw % BLOCK);
      }
    } else {
      off = e->nb ? e->off - a->toc_len + (uint64_t)v->tail_voff : (uint64_t)v->tail_voff;
      for (uint32_t k = 0; k < e->nb; k++, zi++) zt[2 * zi] = (uint8_t)(a->zl[e->zi + k] >> 8), zt[2 * zi + 1] = (uint8_t)a->zl[e->zi + k];
    }
    put40(t + 20, elen);
    put40(t + 25, off);
  }
  cfb(sb_toc_key, sb_toc_iv, toc, (size_t)toc_len - 32, 0);
  memcpy(v->head, "PSAR", 4);
  put32(v->head + 4, 0x10004);
  memcpy(v->head + 8, "zlib", 4);
  put32(v->head + 12, (uint32_t)toc_len);
  put32(v->head + 16, ENTRY);
  put32(v->head + 20, n);
  put32(v->head + 24, BLOCK);
  put32(v->head + 28, a->flags | 4);
  free(kind), free(names), free(graph), free(bn.d), free(bn.zl), free(bg.d), free(bg.zl);
  return v;
fail:
  free(kind), free(names), free(graph), free(bn.d), free(bn.zl), free(bg.d), free(bg.zl);
  if (v) {
    pthread_mutex_destroy(&v->mu);
    free(v->sng);
    free(v->head);
    free(v);
  }
  return 0;
}

int64_t sb_virt_read(sb_virt *v, int fd, void *buf, uint64_t n, int64_t off) {
  if (off < 0) return -1;
  if (off >= v->size) return 0;
  if (n > (uint64_t)(v->size - off)) n = (uint64_t)(v->size - off);
  uint8_t *out = buf;
  uint64_t done = 0;
  while (done < n) {
    int64_t at = off + (int64_t)done;
    uint64_t k = n - done;
    if (at < v->head_len) {
      if (k > (uint64_t)(v->head_len - at)) k = (uint64_t)(v->head_len - at);
      memcpy(out + done, v->head + at, k);
    } else if (at < v->tail_voff) {
      uint32_t lo = 0, hi = v->nsng - 1;  // the song file at `at`
      while (lo < hi) {
        uint32_t mid = (lo + hi + 1) / 2;
        if (v->sng[mid].voff <= at) lo = mid;
        else hi = mid - 1;
      }
      vsng *s = &v->sng[lo];
      while (at >= s->voff + s->len) s++;  // past ones of no length
      uint8_t *d = sng_get(v, fd, s);
      if (!d) return -1;
      if (k > (uint64_t)(s->voff + s->len - at)) k = (uint64_t)(s->voff + s->len - at);
      memcpy(out + done, d + (at - s->voff), k);
    } else {
      if (!pread_all(fd, out + done, k, v->tail_roff + (at - v->tail_voff))) return -1;
    }
    done += k;
  }
  return (int64_t)done;
}
