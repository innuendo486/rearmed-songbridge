// The Ogg to .wem repacker (src/wem.c) without a game: wemtest CODEBOOKS IN.ogg OUT.wem writes the .wem, or prints
// why there is none and exits 3. CODEBOOKS is the game's codebook library as rearmed-mods' stems helper keeps it
// (codebooks.bin: "RSCB", u32 count, count u32 offsets into the data, the data); it is the game's, not in this
// repository. With --fuzz N instead of OUT, N damaged copies of the file are converted: none may crash.
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "wem.h"

static uint8_t *slurp(const char *path, size_t *n) {
  struct stat st;
  int fd = open(path, O_RDONLY);
  if (fd < 0 || fstat(fd, &st)) return 0;
  uint8_t *d = malloc((size_t)st.st_size + 1);
  if (d && read(fd, d, (size_t)st.st_size + 1) != st.st_size) free(d), d = 0;
  close(fd);
  *n = (size_t)st.st_size;
  return d;
}

static uint64_t rnd(uint64_t *s) {
  *s ^= *s << 13, *s ^= *s >> 7, *s ^= *s << 17;
  return *s;
}

int main(int argc, char **argv) {
  if (argc < 4) return 2;
  size_t ln, n, on;
  uint8_t *l = slurp(argv[1], &ln), *ogg = slurp(argv[2], &n);
  if (!l || ln < 8 || memcmp(l, "RSCB", 4) || !ogg) return fprintf(stderr, "cannot read the inputs\n"), 2;
  uint32_t count;
  memcpy(&count, l + 4, 4);
  if (ln < 8 + 4 * (size_t)count) return 2;
  const uint8_t *data = l + 8 + 4 * (size_t)count;
  size_t dn = ln - (8 + 4 * (size_t)count);
  const uint8_t **book = calloc(count, sizeof *book);
  size_t *avail = calloc(count, sizeof *avail);
  for (uint32_t i = 0; i < count; i++) {
    uint32_t off;
    memcpy(&off, l + 8 + 4 * (size_t)i, 4);
    if (off > dn) return 2;
    book[i] = data + off, avail[i] = dn - off;
  }
  sb_books lib = {count, book, avail};
  char err[200];
  if (!strcmp(argv[3], "--fuzz") && argc > 4) {
    uint64_t s = 0x243f6a8885a308d3ull;
    int made = 0, total = atoi(argv[4]);
    uint8_t *b = malloc(n);
    for (int k = 0; k < total; k++) {
      memcpy(b, ogg, n);
      size_t len = n;
      switch (k % 4) {
        case 0: len = (size_t)(rnd(&s) % (n + 1)); break;                                        // cut short
        case 1: for (int j = 0; j < 1 + k % 5; j++) b[rnd(&s) % (n < 9000 ? n : 9000)] ^= (uint8_t)(1u << (rnd(&s) % 8)); break;  // bits of the headers
        case 2: for (int j = 0; j < 8; j++) b[rnd(&s) % n] = (uint8_t)rnd(&s); break;             // bytes anywhere
        case 3: memset(b + rnd(&s) % n, (int)(rnd(&s) & 255), 1 + (size_t)(rnd(&s) % 64) < n ? 1 + (size_t)(rnd(&s) % 64) : 1); len = n; break;
      }
      if (k % 4 == 3) len = n;
      uint8_t *copy = malloc(len ? len : 1);  // its own allocation: a read past the end is caught
      memcpy(copy, b, len);
      uint8_t *w = sb_wem_from_ogg(copy, len, &lib, &on, err, sizeof err);
      made += w != 0;
      free(w), free(copy);
    }
    printf("ok %d damaged copies, %d still made a file\n", total, made);
    return 0;
  }
  uint8_t *w = sb_wem_from_ogg(ogg, n, &lib, &on, err, sizeof err);
  if (!w) return printf("none: %s\n", err), 3;
  int o = open(argv[3], O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (o < 0 || write(o, w, on) != (ssize_t)on || close(o)) return 2;
  printf("ok %zu bytes\n", on);
  return 0;
}
