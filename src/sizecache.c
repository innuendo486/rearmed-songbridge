// See sizecache.h. A package's file is <dir>/<hash of path and platform>.sbsz:
//   "SBSZ" | u32 format (2) | u32 version | u32 platform | i64 size | i64 mtime | u32 path length | path
//   u32 entries | each: u16 name length | name | i64 len | u32 tail | u32 tag | u8 kept | len bytes when kept
//   u32 CRC-32 of all before it
// Numbers are little-endian. A file is written beside its place and renamed there, so a reader sees a whole one.
#include "sizecache.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#define FORMAT 2u
#define MAX_ENTRIES 4096u
#define MAX_NAME 1024u
#define MAX_FILE (32u << 20)  // names, sizes and the kept bytes of one package: far less

typedef struct {
  uint8_t *d;
  size_t n, cap;
  int bad;
} buf;

static void add(buf *b, const void *p, size_t n) {
  if (b->bad) return;
  if (n > b->cap - b->n) {
    size_t cap = b->cap ? b->cap : 4096;
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
static void add32(buf *b, uint32_t v) { add(b, (uint8_t[]){(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)}, 4); }
static void add64(buf *b, uint64_t v) { add32(b, (uint32_t)v), add32(b, (uint32_t)(v >> 32)); }

typedef struct {
  const uint8_t *d;
  size_t n, p;
  int bad;
} rd;

static const uint8_t *take(rd *r, size_t n) {
  if (r->bad || n > r->n - r->p) {
    r->bad = 1;
    return 0;
  }
  r->p += n;
  return r->d + r->p - n;
}
static uint32_t get32(rd *r) {
  const uint8_t *p = take(r, 4);
  return p ? (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24 : 0;
}
static uint64_t get64(rd *r) {
  uint64_t lo = get32(r);
  return lo | (uint64_t)get32(r) << 32;
}

// The package's file in dir, into out: 1, or 0 when the path is too long
static int file_of(const char *dir, const sb_cache_key *k, char *out, size_t outlen) {
  uint64_t h = 0xcbf29ce484222325ull;  // FNV-1a of the path and the platform
  for (const char *p = k->path; *p; p++) h = (h ^ (uint8_t)*p) * 0x100000001b3ull;
  h = (h ^ (uint8_t)k->platform) * 0x100000001b3ull;
  return snprintf(out, outlen, "%s/%016llx.sbsz", dir, (unsigned long long)h) < (int)outlen;
}

void sb_cache_forget(const char *dir, const sb_cache_key *k) {
  char path[PATH_MAX];
  if (file_of(dir, k, path, sizeof path)) unlink(path);
}

void sb_cache_free(sb_cache_set *set) {
  for (uint32_t i = 0; set->e && i < set->n; i++) free(set->e[i].name), free(set->e[i].data);
  free(set->e);
  set->e = 0, set->n = 0;
}

int sb_cache_load(const char *dir, const sb_cache_key *k, sb_cache_set *out) {
  char path[PATH_MAX];
  struct stat st;
  out->e = 0, out->n = 0;
  if (!file_of(dir, k, path, sizeof path)) return 0;
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return 0;
  uint8_t *d = 0;
  int ok = 0;
  if (fstat(fd, &st) || st.st_size < 48 || (uint64_t)st.st_size > MAX_FILE || !(d = malloc((size_t)st.st_size))) goto out;
  size_t n = (size_t)st.st_size;
  if (read(fd, d, n) != (ssize_t)n) goto out;
  rd r = {d, n - 4, 0, 0};
  rd tail = {d + n - 4, 4, 0, 0};
  if (get32(&tail) != (uint32_t)crc32(0, d, (uInt)(n - 4))) goto out;
  const uint8_t *magic = take(&r, 4);
  if (!magic || memcmp(magic, "SBSZ", 4) || get32(&r) != FORMAT || get32(&r) != k->version || get32(&r) != (uint32_t)k->platform) goto out;
  if ((int64_t)get64(&r) != k->size || (int64_t)get64(&r) != k->mtime) goto out;
  uint32_t pl = get32(&r);
  const uint8_t *p = pl == strlen(k->path) ? take(&r, pl) : 0;
  if (!p || memcmp(p, k->path, pl)) goto out;  // another package with the same hash
  uint32_t count = get32(&r);
  if (r.bad || !count || count > MAX_ENTRIES || !(out->e = calloc(count, sizeof *out->e))) goto out;
  for (uint32_t i = 0; i < count; i++) {
    const uint8_t *l = take(&r, 2);
    uint32_t nl = l ? (uint32_t)l[0] | (uint32_t)l[1] << 8 : 0;
    const uint8_t *name = nl && nl <= MAX_NAME ? take(&r, nl) : 0;
    sb_cached *e = &out->e[out->n];
    if (!name || memchr(name, 0, nl) || !(e->name = malloc(nl + 1))) goto out;
    out->n++;
    memcpy(e->name, name, nl);
    e->name[nl] = 0;
    e->len = (int64_t)get64(&r), e->tail = get32(&r), e->tag = get32(&r);
    const uint8_t *kept = take(&r, 1);
    if (!kept || e->len < 0 || *kept > 1) goto out;
    if (*kept) {
      const uint8_t *bytes = (uint64_t)e->len <= r.n - r.p ? take(&r, (size_t)e->len) : 0;
      if (!bytes || !(e->data = malloc((size_t)e->len + 1))) goto out;
      memcpy(e->data, bytes, (size_t)e->len);
    }
  }
  ok = !r.bad && r.p == r.n;
out:
  close(fd);
  free(d);
  if (!ok) sb_cache_free(out);
  return ok;
}

// dir and the folders above it, made when they are not there
static int make_dirs(const char *dir) {
  char p[PATH_MAX];
  size_t n = strlen(dir);
  if (!n || n >= sizeof p) return 0;
  memcpy(p, dir, n + 1);
  for (size_t i = 1; i <= n; i++)
    if (p[i] == '/' || !p[i]) {
      char c = p[i];
      p[i] = 0;
      if (mkdir(p, 0700) && errno != EEXIST) return 0;
      p[i] = c;
    }
  return 1;
}

int sb_cache_store(const char *dir, const sb_cache_key *k, const sb_cache_set *set) {
  char path[PATH_MAX], tmp[PATH_MAX];
  if (!set->n || set->n > MAX_ENTRIES || !file_of(dir, k, path, sizeof path) || !make_dirs(dir)) return 0;
  if (snprintf(tmp, sizeof tmp, "%s/tmp.XXXXXX", dir) >= (int)sizeof tmp) return 0;
  buf b = {0};
  add(&b, "SBSZ", 4), add32(&b, FORMAT), add32(&b, k->version), add32(&b, (uint32_t)k->platform);
  add64(&b, (uint64_t)k->size), add64(&b, (uint64_t)k->mtime);
  add32(&b, (uint32_t)strlen(k->path)), add(&b, k->path, strlen(k->path));
  add32(&b, set->n);
  for (uint32_t i = 0; i < set->n; i++) {
    const sb_cached *e = &set->e[i];
    size_t nl = e->name ? strlen(e->name) : 0;
    if (!nl || nl > MAX_NAME || e->len < 0) b.bad = 1;
    add(&b, (uint8_t[]){(uint8_t)nl, (uint8_t)(nl >> 8)}, 2), add(&b, e->name, nl);
    add64(&b, (uint64_t)e->len), add32(&b, e->tail), add32(&b, e->tag);
    add(&b, (uint8_t[]){e->data != 0}, 1);
    if (e->data) add(&b, e->data, (size_t)e->len);
    if (b.n > MAX_FILE) b.bad = 1;
  }
  if (!b.bad) add32(&b, (uint32_t)crc32(0, b.d, (uInt)b.n));
  int ok = 0, fd = b.bad ? -1 : mkstemp(tmp);
  if (fd >= 0) {
    ok = write(fd, b.d, b.n) == (ssize_t)b.n;
    ok = !close(fd) && ok && !rename(tmp, path);
    if (!ok) unlink(tmp);
  }
  free(b.d);
  return ok;
}
