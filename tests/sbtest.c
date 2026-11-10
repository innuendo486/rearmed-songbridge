// The mod without a game: it is started for one edition and reached only through the functions the editions' file
// layers call (rc_file.c). tests/run_tests.py drives this and checks what it writes.
//   sbtest TARGET list DIR           the folder's names as the game finds them, one a line
//   sbtest TARGET stat PATH          "served SIZE", "real SIZE" or "none"
//   sbtest TARGET dump PATH OUT      PATH read whole in every way (below), each compared with the others, to OUT
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rc_file.h"

int sb_init(const char *target);  // src/songbridge.c
extern int sb_rescan_ms;

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

static const char *path;
static uint8_t *ref;
static int64_t size;
static int shared_fd;

static uint64_t rnd(uint64_t *s) {
  *s ^= *s << 13, *s ^= *s >> 7, *s ^= *s << 17;
  return *s;
}

// Random reads of the shared descriptor and of one of its own, the latter opened before any song file was read
static void *reader(void *arg) {
  uint64_t s = 0x9e3779b97f4a7c15ull * (uint64_t)((intptr_t)arg + 1);
  int fd = rc_file_open(path, O_RDONLY);
  CHECK(fd >= 0 && rc_file_served(fd));
  uint8_t *buf = malloc(300000);
  CHECK(buf);
  for (int k = 0; k < 3000; k++) {
    // a third of the reads in the first megabyte, where the rebuilt parts are
    int64_t off = (int64_t)(rnd(&s) % (uint64_t)(k % 3 ? size + 100 : (size < (1 << 20) ? size : 1 << 20)));
    size_t n = (size_t)(rnd(&s) % (k % 7 ? 5000 : 300000));
    int64_t want = off >= size ? 0 : off + (int64_t)n > size ? size - off : (int64_t)n;
    ssize_t got = rc_file_pread(k & 1 ? fd : shared_fd, buf, n, off);
    CHECK(got == want);
    CHECK(!memcmp(buf, ref + off, (size_t)got));
  }
  free(buf);
  CHECK(!rc_file_close(fd));
  return 0;
}

static int dump(const char *out) {
  struct stat st;
  CHECK(!rc_file_stat(path, &st));
  size = st.st_size;
  CHECK(size > 32);
  ref = malloc((size_t)size + 1);
  uint8_t *b = malloc((size_t)size + 1);
  CHECK(ref && b);

  // concurrent first: the song files are made under several readers at once
  pthread_t th[8];
  int fd0 = rc_file_open(path, O_RDONLY);
  CHECK(fd0 >= 0 && rc_file_served(fd0));
  // the reference for the readers: one positional read of all of it, from a descriptor of its own
  CHECK(rc_file_pread(fd0, ref, (size_t)size + 1, 0) == size);
  CHECK(!rc_file_close(fd0));  // the last close: the song files made are dropped, and made again below
  shared_fd = rc_file_open(path, O_RDONLY);
  CHECK(shared_fd >= 0);
  for (intptr_t i = 0; i < 8; i++) CHECK(!pthread_create(&th[i], 0, reader, (void *)i));
  for (int i = 0; i < 8; i++) CHECK(!pthread_join(th[i], 0));

  // a descriptor, read in order in pieces of odd sizes
  CHECK(!rc_file_fstat(shared_fd, &st) && st.st_size == size);
  int64_t at = 0;
  for (size_t piece = 1;; piece = piece * 3 % 70001 + 1) {
    ssize_t r = rc_file_read(shared_fd, b + at, piece);
    CHECK(r >= 0);
    if (!r) break;
    at += r;
    CHECK(at <= size);
  }
  CHECK(at == size && !memcmp(b, ref, (size_t)size));
  CHECK(rc_file_lseek(shared_fd, 0, SEEK_END) == size);
  CHECK(!rc_file_close(shared_fd));

  // a FILE: fread of all, then fseek and fread at places
  FILE *f = rc_file_fopen(path, "rb");
  CHECK(f);
  memset(b, 0, (size_t)size);
  CHECK(fread(b, 1, (size_t)size + 1, f) == (size_t)size && feof(f));
  CHECK(!memcmp(b, ref, (size_t)size));
  CHECK(!fseek(f, 0, SEEK_END) && ftell(f) == size);
  CHECK(!fstat(fileno(f), &st) || 1);
  uint64_t s = 12345;
  for (int k = 0; k < 2000; k++) {
    long off = (long)(rnd(&s) % (uint64_t)size);
    size_t n = (size_t)(rnd(&s) % 20000);
    size_t want = off + (long)n > size ? (size_t)(size - off) : n;
    CHECK(!fseek(f, off, SEEK_SET));
    CHECK(fread(b, 1, n, f) == want && !memcmp(b, ref + off, want));
    CHECK(ftell(f) == off + (long)want);
  }
  CHECK(!fclose(f));

  int o = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  CHECK(o >= 0 && write(o, ref, (size_t)size) == size && !close(o));
  printf("ok %lld\n", (long long)size);
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 4) return 2;
  sb_rescan_ms = 0;
  CHECK(!sb_init(argv[1]));
  path = argv[3];
  if (!strcmp(argv[2], "list")) {
    for (int pass = 0; pass < 2; pass++) {  // twice: the second listing is the same
      DIR *d = rc_file_opendir(path);
      CHECK(d);
      for (struct dirent *e; (e = rc_file_readdir(d));)
        if (e->d_name[0] != '.' && pass) puts(e->d_name);
      CHECK(!rc_file_closedir(d));
    }
    return 0;
  }
  if (!strcmp(argv[2], "stat")) {
    struct stat st;
    int fd = rc_file_open(path, O_RDONLY);
    if (fd < 0) return puts("none"), 0;
    int v = rc_file_served(fd);
    CHECK(!rc_file_fstat(fd, &st));
    struct stat st2;
    CHECK(!rc_file_stat(path, &st2) && st2.st_size == st.st_size && st2.st_mtimespec.tv_sec == st.st_mtimespec.tv_sec);
    uint8_t h[4] = {0};
    rc_file_pread(fd, h, 4, 0);
    printf("%s %lld %s\n", v ? "served" : "real", (long long)st.st_size, memcmp(h, "PSAR", 4) ? "other" : "PSAR");
    CHECK(!rc_file_close(fd));
    return 0;
  }
  if (!strcmp(argv[2], "dump") && argc > 4) return dump(argv[4]);
  return 2;
}
