// See varc.h. The archive as served (psarc.c has the format):
//   header, table of contents | name list (packed) | the entries, one after the other
// An entry is cut in 64 KiB blocks. A whole block is stored as it is (size 0 in the table). A short last block is
// stored as it is too (its own size in the table) unless it starts as a zlib stream: then it is packed, and its
// packed size is the one the entry was given (sb_ventry.tail).
#include "varc.h"

#include <CommonCrypto/CommonCrypto.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "keys.h"

#define BLOCK 0x10000u
#define ENTRY 30u
#define MAX_ENTRIES 4096u

typedef struct {
  int64_t len, off;         // the entry's size; where it is in the archive
  int64_t stored;           // the bytes it takes there
  uint32_t tail;            // its last block's packed size, 0: not packed
  _Atomic(uint8_t *) packed;  // that block, once made
} vent;

struct sb_varc {
  uint8_t *head;  // header, table of contents, name list
  int64_t head_len, size;
  vent *e;
  uint32_t n;
  sb_vread read;
  void *ctx;
  pthread_mutex_t mu;  // packing a last block
};

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v; }
static void put40(uint8_t *p, uint64_t v) { p[0] = (uint8_t)(v >> 32), put32(p + 1, (uint32_t)v); }

// AES-256-CFB (128-bit feedback) encryption, as openssl's aes-256-cfb
static void cfb_encrypt(const uint8_t key[32], const uint8_t iv[16], uint8_t *d, size_t n) {
  uint8_t prev[16], ks[16];
  memcpy(prev, iv, 16);
  for (size_t i = 0; i < n; i += 16) {
    size_t k = n - i < 16 ? n - i : 16, moved = 0;
    CCCrypt(kCCEncrypt, kCCAlgorithmAES, kCCOptionECBMode, key, 32, 0, prev, 16, ks, 16, &moved);
    for (size_t j = 0; j < k; j++) d[i + j] ^= ks[j];
    memcpy(prev, d + i, k);
  }
}

static int looks_zlib(const uint8_t *p, size_t n) {
  return n >= 2 && (p[0] & 0x0f) == 8 && (p[0] >> 4) <= 7 && ((p[0] << 8 | p[1]) % 31) == 0;
}

// block packed: the bytes (to free) and their count in *zn, or 0
static uint8_t *pack_block(const uint8_t *block, size_t n, uLongf *zn) {
  *zn = compressBound((uLong)n);
  uint8_t *z = malloc(*zn);
  if (z && compress2(z, zn, block, (uLong)n, 9) != Z_OK) {
    free(z);
    z = 0;
  }
  return z;
}

int32_t sb_varc_tail(const uint8_t *block, size_t n) {
  if (!n || n >= BLOCK || !looks_zlib(block, n)) return 0;
  uLongf zn;
  uint8_t *z = pack_block(block, n, &zn);
  free(z);
  // packed to the block's own size it would be taken for the block itself; to a whole block's, for a stored one
  return z && zn != n && zn < BLOCK ? (int32_t)zn : -1;
}

int64_t sb_varc_size(const sb_varc *v) { return v->size; }

void sb_varc_drop(sb_varc *v) {
  pthread_mutex_lock(&v->mu);
  for (uint32_t i = 0; i < v->n; i++) free(atomic_exchange(&v->e[i].packed, 0));
  pthread_mutex_unlock(&v->mu);
}

void sb_varc_free(sb_varc *v) {
  if (!v) return;
  sb_varc_drop(v);
  pthread_mutex_destroy(&v->mu);
  free(v->e);
  free(v->head);
  free(v);
}

sb_varc *sb_varc_build(const sb_ventry *e, uint32_t n, sb_vread read, void *ctx) {
  if (!n || n > MAX_ENTRIES || !read) return 0;
  sb_varc *v = calloc(1, sizeof *v);
  char *names = 0;
  uint8_t *zn = 0;
  uint16_t *nzl = 0;
  if (!v) return 0;
  pthread_mutex_init(&v->mu, 0);
  v->n = n, v->read = read, v->ctx = ctx;
  if (!(v->e = calloc(n, sizeof *v->e))) goto fail;

  // the name list, packed block by block
  size_t names_len = 0;
  for (uint32_t i = 0; i < n; i++) {
    if (!e[i].name || !*e[i].name || strchr(e[i].name, '\n') || e[i].len < 0) goto fail;
    names_len += strlen(e[i].name) + 1;
  }
  names_len--;  // no newline after the last
  if (!(names = malloc(names_len + 1))) goto fail;
  for (size_t i = 0, at = 0; i < n; i++) {
    size_t l = strlen(e[i].name);
    memcpy(names + at, e[i].name, l);
    names[at + l] = '\n';
    at += l + 1;
  }
  uint32_t nnb = (uint32_t)((names_len + BLOCK - 1) / BLOCK);
  size_t zcap = (size_t)nnb * compressBound(BLOCK), zlen = 0;
  zn = malloc(zcap ? zcap : 1), nzl = calloc(nnb ? nnb : 1, sizeof *nzl);
  if (!zn || !nzl) goto fail;
  for (uint32_t b = 0; b < nnb; b++) {
    size_t raw = names_len - (size_t)b * BLOCK < BLOCK ? names_len - (size_t)b * BLOCK : BLOCK;
    const uint8_t *src = (const uint8_t *)names + (size_t)b * BLOCK;
    uLongf got = (uLongf)(zcap - zlen);
    int packed = compress2(zn + zlen, &got, src, (uLong)raw, 9) == Z_OK && got < raw;
    if (!packed) {
      if (looks_zlib(src, raw) && raw < BLOCK) goto fail;  // names starting as a zlib stream that do not pack: none do
      memcpy(zn + zlen, src, raw);
      got = (uLongf)raw;
    }
    nzl[b] = (uint16_t)(packed ? got : raw % BLOCK);
    zlen += got;
  }

  // sizes and places
  uint64_t nz = nnb;
  for (uint32_t i = 0; i < n; i++) {
    uint64_t nb = ((uint64_t)e[i].len + BLOCK - 1) / BLOCK, last = (uint64_t)e[i].len % BLOCK;
    if (e[i].tail && (!last || e[i].tail >= BLOCK || e[i].tail == last)) goto fail;
    v->e[i].len = e[i].len, v->e[i].tail = e[i].tail;
    v->e[i].stored = e[i].tail ? e[i].len - (int64_t)last + e[i].tail : e[i].len;
    nz += nb;
  }
  uint64_t toc_len = 32 + (uint64_t)ENTRY * (n + 1) + 2 * nz;
  if (toc_len > (64u << 20)) goto fail;
  v->head_len = (int64_t)(toc_len + zlen);
  int64_t at = v->head_len;
  for (uint32_t i = 0; i < n; i++) {
    v->e[i].off = at;
    at += v->e[i].stored;
  }
  v->size = at;

  // header and table of contents: entry 0 is the name list (no hash), entry i + 1 is e[i]
  if (!(v->head = calloc(1, (size_t)v->head_len + 1))) goto fail;
  uint8_t *toc = v->head + 32, *zt = toc + ENTRY * (n + 1);
  uint32_t zi = 0;
  put32(toc + 16, 0), put40(toc + 20, names_len), put40(toc + 25, toc_len);
  for (uint32_t b = 0; b < nnb; b++, zi++) zt[2 * zi] = (uint8_t)(nzl[b] >> 8), zt[2 * zi + 1] = (uint8_t)nzl[b];
  memcpy(v->head + toc_len, zn, zlen);
  for (uint32_t i = 0; i < n; i++) {
    uint8_t *t = toc + ENTRY * (i + 1);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CC_MD5(e[i].name, (CC_LONG)strlen(e[i].name), t);
#pragma clang diagnostic pop
    put32(t + 16, zi), put40(t + 20, (uint64_t)e[i].len), put40(t + 25, (uint64_t)v->e[i].off);
    uint64_t nb = ((uint64_t)e[i].len + BLOCK - 1) / BLOCK, last = (uint64_t)e[i].len % BLOCK;
    for (uint64_t b = 0; b < nb; b++, zi++) {
      uint32_t z = b + 1 < nb || !last ? 0 : e[i].tail ? e[i].tail : (uint32_t)last;
      zt[2 * zi] = (uint8_t)(z >> 8), zt[2 * zi + 1] = (uint8_t)z;
    }
  }
  cfb_encrypt(sb_toc_key, sb_toc_iv, toc, (size_t)toc_len - 32);
  memcpy(v->head, "PSAR", 4);
  put32(v->head + 4, 0x10004);
  memcpy(v->head + 8, "zlib", 4);
  put32(v->head + 12, (uint32_t)toc_len);
  put32(v->head + 16, ENTRY);
  put32(v->head + 20, n + 1);
  put32(v->head + 24, BLOCK);
  put32(v->head + 28, 4);
  free(names), free(zn), free(nzl);
  return v;
fail:
  free(names), free(zn), free(nzl);
  sb_varc_free(v);
  return 0;
}

// Entry i's packed last block, or 0 (it cannot be made, or does not pack to the size it was given)
static const uint8_t *packed_tail(sb_varc *v, uint32_t i) {
  vent *e = &v->e[i];
  uint8_t *z = atomic_load(&e->packed);
  if (z) return z;
  pthread_mutex_lock(&v->mu);
  if (!(z = atomic_load(&e->packed))) {
    size_t last = (size_t)(e->len % BLOCK);
    uint8_t *raw = malloc(last);
    uLongf zn = 0;
    if (raw && v->read(v->ctx, i, raw, last, e->len - (int64_t)last)) z = pack_block(raw, last, &zn);
    free(raw);
    if (z && zn != e->tail) {
      free(z);
      z = 0;
    }
    if (z) atomic_store(&e->packed, z);
  }
  pthread_mutex_unlock(&v->mu);
  return z;
}

int64_t sb_varc_read(sb_varc *v, void *buf, uint64_t n, int64_t off) {
  if (off < 0) return -1;
  if (off >= v->size) return 0;
  if (n > (uint64_t)(v->size - off)) n = (uint64_t)(v->size - off);
  uint8_t *out = buf;
  uint64_t done = 0;
  if (off < v->head_len) {
    done = n < (uint64_t)(v->head_len - off) ? n : (uint64_t)(v->head_len - off);
    memcpy(out, v->head + off, done);
  }
  if (done == n) return (int64_t)done;
  // the entry the next byte is in: the last that starts at or before it
  int64_t pos = off + (int64_t)done;
  uint32_t lo = 0, hi = v->n - 1;
  while (lo < hi) {
    uint32_t mid = (lo + hi + 1) / 2;
    if (v->e[mid].off <= pos) lo = mid;
    else hi = mid - 1;
  }
  for (uint32_t i = lo; done < n && i < v->n; i++) {
    const vent *e = &v->e[i];
    int64_t in = pos - e->off;  // within what the entry takes in the archive
    if (in >= e->stored) continue;  // an empty entry
    int64_t plain = e->tail ? e->len - e->len % BLOCK : e->len;  // the part that is the entry's own bytes
    if (in < plain) {
      uint64_t k = n - done < (uint64_t)(plain - in) ? n - done : (uint64_t)(plain - in);
      if (!v->read(v->ctx, i, out + done, k, in)) return -1;
      done += k, pos += (int64_t)k, in += (int64_t)k;
    }
    if (done < n && in < e->stored) {
      const uint8_t *z = packed_tail(v, i);
      if (!z) return -1;
      uint64_t k = n - done < (uint64_t)(e->stored - in) ? n - done : (uint64_t)(e->stored - in);
      memcpy(out + done, z + (in - plain), k);
      done += k, pos += (int64_t)k;
    }
  }
  return (int64_t)done;
}
