// The size cache (src/sizecache.c) without a game: cachetest DIR keeps made-up entries in DIR (made new), reads them
// back, checks that every way a kept file can be stale or damaged gives "none" and never other entries, and prints
// "ok". Run by build.sh --test-cache.
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#include "sizecache.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

static uint64_t rnd(uint64_t *s) {
  *s ^= *s << 13, *s ^= *s >> 7, *s ^= *s << 17;
  return *s;
}

static int same(const sb_cache_set *a, const sb_cache_set *b) {
  if (a->n != b->n) return 0;
  for (uint32_t i = 0; i < a->n; i++) {
    const sb_cached *x = &a->e[i], *y = &b->e[i];
    if (strcmp(x->name, y->name) || x->len != y->len || x->tail != y->tail || x->tag != y->tag || !x->data != !y->data) return 0;
    if (x->data && memcmp(x->data, y->data, (size_t)x->len)) return 0;
  }
  return 1;
}

int main(int argc, char **argv) {
  if (argc != 2) return 2;
  char dir[1024];
  snprintf(dir, sizeof dir, "%s/a/b/cache", argv[1]);  // folders that are not there yet
  uint8_t hsan[5000];
  for (size_t i = 0; i < sizeof hsan; i++) hsan[i] = (uint8_t)(i * 7);
  sb_cached e[] = {{"manifests/songs_dlc_x/songs_dlc_x.hsan", sizeof hsan, 0, 5, hsan},
                   {"audio/mac/123.wem", 3666706, 0, 14, 0},
                   {"songs/bin/macos/x_lead.sng", 65636, 91, 8 | 2 << 8, 0},
                   {"appid.appid", 6, 0, 0, (uint8_t *)"248750"},
                   {"empty.flat", 0, 0, 0xffffffffu, (uint8_t *)""}};
  sb_cache_set set = {e, sizeof e / sizeof *e}, got;
  sb_cache_key k = {"/some where/dlc/Song Näme.feedpak", 21290748, 1794325500, 0, 3};

  CHECK(!sb_cache_load(dir, &k, &got));  // nothing kept yet, no folder
  CHECK(sb_cache_store(dir, &k, &set));
  CHECK(sb_cache_load(dir, &k, &got) && same(&set, &got));
  sb_cache_free(&got);
  CHECK(sb_cache_store(dir, &k, &set));  // again: replaced
  CHECK(sb_cache_load(dir, &k, &got) && same(&set, &got));
  sb_cache_free(&got);

  // another size, date, version, platform or path: none
  sb_cache_key o = k;
  o.size++;
  CHECK(!sb_cache_load(dir, &o, &got));
  o = k, o.mtime--;
  CHECK(!sb_cache_load(dir, &o, &got));
  o = k, o.version++;
  CHECK(!sb_cache_load(dir, &o, &got));
  o = k, o.platform = 1;
  CHECK(!sb_cache_load(dir, &o, &got));
  o = k, o.path = "/some where/dlc/Other.feedpak";
  CHECK(!sb_cache_load(dir, &o, &got));
  // the other platform has a file of its own beside it
  sb_cache_set one = {e + 3, 1};
  o = k, o.platform = 1;
  CHECK(sb_cache_store(dir, &o, &one) && sb_cache_load(dir, &o, &got) && same(&one, &got));
  sb_cache_free(&got);
  CHECK(sb_cache_load(dir, &k, &got) && same(&set, &got));
  sb_cache_free(&got);
  // what cannot be kept
  sb_cache_set none = {e, 0};
  CHECK(!sb_cache_store(dir, &k, &none));
  sb_cached noname = {"", 1, 0, 0, 0};
  sb_cache_set bad = {&noname, 1};
  CHECK(!sb_cache_store(dir, &k, &bad));

  // the kept file damaged in every way: none, or (the damage undone by chance: never here) the same entries
  DIR *d = opendir(dir);
  CHECK(d);
  char file[2048] = "";
  for (struct dirent *de; (de = readdir(d));) {
    char p[2048];
    snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
    struct stat st;
    if (de->d_name[0] != '.' && !stat(p, &st) && st.st_size > 5000) strcpy(file, p);
    CHECK(strncmp(de->d_name, "tmp.", 4));  // no half-written file is left
  }
  closedir(d);
  CHECK(*file);
  int fd = open(file, O_RDWR);
  struct stat st;
  CHECK(fd >= 0 && !fstat(fd, &st));
  size_t n = (size_t)st.st_size;
  uint8_t *orig = malloc(n), *b = malloc(n);
  CHECK(orig && b && pread(fd, orig, n, 0) == (ssize_t)n);
  uint64_t s = 0x9e3779b97f4a7c15ull;
  int hits = 0;
  for (int i = 0; i < 3000; i++) {
    memcpy(b, orig, n);
    size_t len = n;
    if (i % 3 == 0) len = (size_t)(rnd(&s) % n);
    else if (i % 3 == 1) b[rnd(&s) % (n < 200 ? n : 200)] ^= (uint8_t)(1u << (rnd(&s) % 8));
    else b[rnd(&s) % n] ^= (uint8_t)(1 + rnd(&s) % 255);
    CHECK(!ftruncate(fd, 0) && pwrite(fd, b, len, 0) == (ssize_t)len);
    if (sb_cache_load(dir, &k, &got)) {
      CHECK(same(&set, &got));
      hits++;
      sb_cache_free(&got);
    }
  }
  CHECK(!hits);
  CHECK(!ftruncate(fd, 0) && pwrite(fd, orig, n, 0) == (ssize_t)n && !close(fd));
  CHECK(sb_cache_load(dir, &k, &got) && same(&set, &got));
  sb_cache_free(&got);
  sb_cache_forget(dir, &k);
  CHECK(!sb_cache_load(dir, &k, &got));
  o = k, o.platform = 1;
  CHECK(sb_cache_load(dir, &o, &got));  // the other platform's is still there
  sb_cache_free(&got);
  printf("ok: kept, read back, 5 kinds of stale and 3000 damaged files refused\n");
  return 0;
}
