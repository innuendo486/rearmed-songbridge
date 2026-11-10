// The archive assembler (src/varc.c) without a game: varctest DIR OUT builds the archive of the files under DIR (in
// the order of names.txt in DIR when there is one, else by name), reads it whole in several ways, each compared with
// the others, checks that the mod's own reader takes it for an archive, and writes it to OUT. tests/run_varc_test.py
// drives this and checks what it writes.
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "psarc.h"
#include "varc.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
#define BLOCK 0x10000

typedef struct {
  char *name;
  uint8_t *data;
  int64_t len;
} file;
static file *files;
static uint32_t nfiles, cap;
static _Atomic long calls;

static void add(const char *root, const char *rel) {
  char path[PATH_MAX];
  CHECK(snprintf(path, sizeof path, "%s/%s", root, rel) < (int)sizeof path);
  struct stat st;
  CHECK(!stat(path, &st));
  if (S_ISDIR(st.st_mode)) {
    DIR *d = opendir(path);
    CHECK(d);
    for (struct dirent *e; (e = readdir(d));) {
      if (e->d_name[0] == '.') continue;
      char sub[PATH_MAX];
      CHECK(snprintf(sub, sizeof sub, "%s%s%s", rel, *rel ? "/" : "", e->d_name) < (int)sizeof sub);
      add(root, sub);
    }
    closedir(d);
    return;
  }
  if (!strcmp(rel, "names.txt")) return;
  if (nfiles == cap) CHECK((files = realloc(files, (cap = cap ? cap * 2 : 64) * sizeof *files)));
  file *f = &files[nfiles++];
  f->name = strdup(rel), f->len = st.st_size, f->data = malloc((size_t)st.st_size + 1);
  int fd = open(path, O_RDONLY);
  CHECK(f->name && f->data && fd >= 0 && read(fd, f->data, (size_t)st.st_size + 1) == st.st_size && !close(fd));
}

static int by_name(const void *a, const void *b) { return strcmp(((const file *)a)->name, ((const file *)b)->name); }

static int vread(void *ctx, uint32_t i, void *buf, uint64_t n, int64_t off) {
  (void)ctx;
  CHECK(i < nfiles && off >= 0 && n && (uint64_t)off + n <= (uint64_t)files[i].len);
  memcpy(buf, files[i].data + off, n);
  calls++;
  return 1;
}

static uint64_t rnd(uint64_t *s) {
  *s ^= *s << 13, *s ^= *s >> 7, *s ^= *s << 17;
  return *s;
}

static sb_varc *arc;
static uint8_t *ref;
static int64_t size;

static void *reader(void *arg) {
  uint64_t s = 0x9e3779b97f4a7c15ull * (uint64_t)((intptr_t)arg + 1);
  uint8_t *buf = malloc(300000);
  CHECK(buf);
  for (int k = 0; k < 3000; k++) {
    int64_t off = (int64_t)(rnd(&s) % (uint64_t)(size + 100));
    size_t n = (size_t)(rnd(&s) % (k % 7 ? 5000 : 300000));
    int64_t want = off >= size ? 0 : (int64_t)n > size - off ? size - off : (int64_t)n;
    CHECK(sb_varc_read(arc, buf, n, off) == want && !memcmp(buf, ref + off, (size_t)want));
  }
  free(buf);
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 3) return 2;
  add(argv[1], "");
  CHECK(nfiles);
  qsort(files, nfiles, sizeof *files, by_name);
  char p[PATH_MAX];
  snprintf(p, sizeof p, "%s/names.txt", argv[1]);
  FILE *order = fopen(p, "r");
  if (order) {  // the files in the listed order: each name once
    file *sorted = files;
    CHECK((files = calloc(nfiles, sizeof *files)));
    uint32_t k = 0;
    for (char line[PATH_MAX]; fgets(line, sizeof line, order);) {
      line[strcspn(line, "\r\n")] = 0;
      if (!*line) continue;
      file key = {.name = line}, *f = bsearch(&key, sorted, nfiles, sizeof *sorted, by_name);
      CHECK(f && f->data && k < nfiles);
      files[k++] = *f, f->data = 0;
    }
    CHECK(k == nfiles);
    fclose(order);
  }

  sb_ventry *e = calloc(nfiles, sizeof *e);
  CHECK(e);
  int ntail = 0;
  for (uint32_t i = 0; i < nfiles; i++) {
    size_t last = (size_t)(files[i].len % BLOCK);
    int32_t t = sb_varc_tail(files[i].data + files[i].len - last, last);
    CHECK(t >= 0);
    e[i] = (sb_ventry){files[i].name, files[i].len, (uint32_t)t};
    ntail += t > 0;
  }
  CHECK((arc = sb_varc_build(e, nfiles, vread, 0)));
  size = sb_varc_size(arc);
  CHECK(calls == 0);  // nothing of an entry is asked for before the archive is read

  // one read of all of it is the reference
  uint8_t *b = malloc((size_t)size + 1);
  CHECK((ref = malloc((size_t)size + 1)) && b);
  CHECK(sb_varc_read(arc, ref, (size_t)size + 1, 0) == size && !memcmp(ref, "PSAR", 4));
  CHECK(sb_varc_read(arc, b, 1, size) == 0 && sb_varc_read(arc, b, 1, -1) == -1);
  // the header and the table of contents again: no entry is asked for
  long before = calls;
  uint32_t toc_len = (uint32_t)ref[12] << 24 | (uint32_t)ref[13] << 16 | (uint32_t)ref[14] << 8 | ref[15];
  CHECK(sb_varc_read(arc, b, toc_len, 0) == toc_len && calls == before);
  // the packed last blocks made again, under several readers at once
  sb_varc_drop(arc);
  pthread_t th[8];
  for (intptr_t i = 0; i < 8; i++) CHECK(!pthread_create(&th[i], 0, reader, (void *)i));
  for (int i = 0; i < 8; i++) CHECK(!pthread_join(th[i], 0));
  // in order, in pieces of odd sizes
  int64_t at = 0;
  for (size_t piece = 1; at < size; piece = piece * 3 % 70001 + 1) {
    int64_t r = sb_varc_read(arc, b + at, piece, at);
    CHECK(r > 0);
    at += r;
  }
  CHECK(at == size && !memcmp(b, ref, (size_t)size));

  int o = open(argv[2], O_RDWR | O_CREAT | O_TRUNC, 0644);
  CHECK(o >= 0 && write(o, ref, (size_t)size) == size);
  sb_arc *a = sb_parse(o, size);  // the mod's reader of real archives
  CHECK(a);
  sb_arc_free(a);
  CHECK(!close(o));
  // a size that was kept wrong is an error, not other bytes
  for (uint32_t i = 0; i < nfiles; i++)
    if (e[i].tail) {
      e[i].tail++;
      sb_varc *bad = sb_varc_build(e, nfiles, vread, 0);
      if (bad) {
        CHECK(sb_varc_read(bad, b, (size_t)size + 1, 0) == -1);
        sb_varc_free(bad);
      }
      e[i].tail--;
      break;
    }
  sb_varc_free(arc);
  printf("ok %lld bytes, %u entries, %d packed last blocks\n", (long long)size, nfiles, ntail);
  return 0;
}
