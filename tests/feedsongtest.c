// A feedpak as a song archive (src/feedsong.c) without a game:
//   feedsongtest PAK mac|pc CACHE CODEBOOKS OUT
// opens the package (CACHE: the folder its sizes are kept in, "-" for none), reads the whole archive in several ways,
// each compared with the others, opens it again (now from what was kept) and compares that too, and writes the
// archive to OUT. Prints the song's key, the archive's size and what there is to say of the package; or
// "none: REASON" and exit 3. CODEBOOKS is the game's codebook library as tests/wemtest.c reads it.
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "feedsong.h"
#include "psarc.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

static sb_books lib;
static const sb_books *books(void) { return lib.n ? &lib : 0; }

static void load_books(const char *path) {
  struct stat st;
  int fd = open(path, O_RDONLY);
  if (fd < 0 || fstat(fd, &st)) return;
  size_t ln = (size_t)st.st_size;
  uint8_t *l = malloc(ln + 1);
  CHECK(l && read(fd, l, ln) == (ssize_t)ln && ln > 8 && !memcmp(l, "RSCB", 4));
  close(fd);
  uint32_t count;
  memcpy(&count, l + 4, 4);
  CHECK(ln >= 8 + 4 * (size_t)count);
  const uint8_t *data = l + 8 + 4 * (size_t)count;
  size_t dn = ln - (8 + 4 * (size_t)count);
  const uint8_t **book = calloc(count, sizeof *book);
  size_t *avail = calloc(count, sizeof *avail);
  for (uint32_t i = 0; i < count; i++) {
    uint32_t off;
    memcpy(&off, l + 8 + 4 * (size_t)i, 4);
    CHECK(off <= dn);
    book[i] = data + off, avail[i] = dn - off;
  }
  lib = (sb_books){count, book, avail};
}

static uint64_t rnd(uint64_t *s) {
  *s ^= *s << 13, *s ^= *s >> 7, *s ^= *s << 17;
  return *s;
}

static sb_feedsong *song;
static uint8_t *ref;
static int64_t size;

static void *reader(void *arg) {
  uint64_t s = 0x9e3779b97f4a7c15ull * (uint64_t)((intptr_t)arg + 1);
  uint8_t *buf = malloc(300000);
  CHECK(buf);
  for (int k = 0; k < 1500; k++) {
    int64_t off = (int64_t)(rnd(&s) % (uint64_t)(size + 100));
    size_t n = (size_t)(rnd(&s) % (k % 7 ? 5000 : 300000));
    int64_t want = off >= size ? 0 : (int64_t)n > size - off ? size - off : (int64_t)n;
    CHECK(sb_feedsong_read(song, buf, n, off) == want && !memcmp(buf, ref + off, (size_t)want));
  }
  free(buf);
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 6) return 2;
  int platform = !strcmp(argv[2], "mac") ? SB_MAC : SB_PC;
  const char *cache = strcmp(argv[3], "-") ? argv[3] : 0;
  char err[400];
  load_books(argv[4]);
  song = sb_feedsong_open(argv[1], platform, cache, books, err, sizeof err);
  if (!song) return printf("none: %s\n", err), 3;
  size = sb_feedsong_size(song);
  uint8_t *b = malloc((size_t)size + 1);
  CHECK((ref = malloc((size_t)size + 1)) && b);
  // what the game reads at its start, before anything else is asked for: the first 64 KiB
  CHECK(sb_feedsong_read(song, ref, 65536, 0) == (size < 65536 ? size : 65536) && !memcmp(ref, "PSAR", 4));
  CHECK(sb_feedsong_read(song, ref, (size_t)size + 1, 0) == size);
  sb_feedsong_drop(song);  // as after the game's last close: everything is made again
  pthread_t th[6];
  for (intptr_t i = 0; i < 6; i++) CHECK(!pthread_create(&th[i], 0, reader, (void *)i));
  for (int i = 0; i < 6; i++) CHECK(!pthread_join(th[i], 0));
  int64_t at = 0;
  for (size_t piece = 1; at < size; piece = piece * 3 % 70001 + 1) {
    int64_t r = sb_feedsong_read(song, b + at, piece, at);
    CHECK(r > 0);
    at += r;
  }
  CHECK(at == size && !memcmp(b, ref, (size_t)size));
  printf("key %s\nsize %lld\nnote %s\n", sb_feedsong_key(song), (long long)size, sb_feedsong_note(song));
  char key[64];
  snprintf(key, sizeof key, "%s", sb_feedsong_key(song));
  sb_feedsong_free(song);

  // again: from what was kept when there is a cache, from the package when there is none
  song = sb_feedsong_open(argv[1], platform, cache, books, err, sizeof err);
  CHECK(song && sb_feedsong_size(song) == size && !strcmp(sb_feedsong_key(song), key));
  CHECK(sb_feedsong_read(song, b, (size_t)size + 1, 0) == size && !memcmp(b, ref, (size_t)size));
  sb_feedsong_free(song);
  // and with no codebook library: only when the sizes were kept is there an archive, and its audio cannot be read
  sb_books none = lib;
  lib.n = 0;
  song = sb_feedsong_open(argv[1], platform, cache, books, err, sizeof err);
  CHECK(!song == !cache);
  if (song) {
    CHECK(sb_feedsong_read(song, b, 65536, 0) > 0 && !memcmp(b, ref, size < 65536 ? (size_t)size : 65536));
    CHECK(sb_feedsong_read(song, b, (size_t)size + 1, 0) == -1);
    sb_feedsong_free(song);
  }
  lib = none;

  int o = open(argv[5], O_RDWR | O_CREAT | O_TRUNC, 0644);
  CHECK(o >= 0 && write(o, ref, (size_t)size) == size);
  sb_arc *a = sb_parse(o, size);
  CHECK(a);
  sb_arc_free(a);
  CHECK(!close(o));
  printf("ok\n");
  return 0;
}
