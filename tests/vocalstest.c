// The lyrics song file (src/vocals.c) without a game:
//   vocalstest unpack mac|pc IN.sng OUT     a song file's plain payload
//   vocalstest payload LYRICS.json OUT      the payload made of a lyrics file; prints how many syllables, or "none: REASON"
//   vocalstest build mac|pc LYRICS.json OUT the packed song file
//   vocalstest fuzz LYRICS.json N           N damaged copies: none may crash
// tests/run_feedsong_test.py drives this.
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sng.h"
#include "vocals.h"

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
static int spit(const char *path, const uint8_t *d, size_t n) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  int ok = fd >= 0 && write(fd, d, n) == (ssize_t)n;
  if (fd >= 0) close(fd);
  return ok ? 0 : 2;
}
static int plat(const char *s) { return !strcmp(s, "mac") ? SB_MAC : SB_PC; }
static uint64_t rnd(uint64_t *s) {
  *s ^= *s << 13, *s ^= *s >> 7, *s ^= *s << 17;
  return *s;
}

int main(int argc, char **argv) {
  size_t n = 0, on = 0;
  uint32_t count = 0;
  char err[200];
  if (argc == 5 && !strcmp(argv[1], "unpack")) {
    uint8_t *in = slurp(argv[3], &n), *out = in ? sb_sng_unpack(in, n, plat(argv[2]), &on) : 0;
    return out ? spit(argv[4], out, on) : (puts("none"), 3);
  }
  if (argc == 4 && !strcmp(argv[1], "payload")) {
    uint8_t *in = slurp(argv[2], &n), *out = in ? sb_vocals_payload((char *)in, n, &on, &count, err, sizeof err) : 0;
    if (!out) return printf("none: %s\n", in ? err : "cannot read"), 3;
    printf("ok %u\n", count);
    return spit(argv[3], out, on);
  }
  if (argc == 5 && !strcmp(argv[1], "build")) {
    uint8_t *in = slurp(argv[3], &n), *out = in ? sb_vocals_sng((char *)in, n, plat(argv[2]), &on, &count, err, sizeof err) : 0;
    if (!out) return printf("none: %s\n", in ? err : "cannot read"), 3;
    printf("ok %u\n", count);
    return spit(argv[4], out, on);
  }
  if (argc == 4 && !strcmp(argv[1], "fuzz")) {
    uint8_t *in = slurp(argv[2], &n);
    if (!in || !n) return 2;
    uint64_t s = 0x9e3779b97f4a7c15ull;
    int made = 0, total = atoi(argv[3]);
    for (int k = 0; k < total; k++) {
      size_t len = k % 3 ? n : (size_t)(rnd(&s) % (n + 1));
      char *b = malloc(len + 1);
      memcpy(b, in, len);
      for (int j = 0; len && j < 1 + k % 7; j++) b[rnd(&s) % len] = (char)(k % 2 ? rnd(&s) : "[]{}\",:0-+ "[rnd(&s) % 12]);
      uint8_t *out = sb_vocals_payload(b, len, &on, &count, err, sizeof err);
      made += out != 0;
      free(out), free(b);
    }
    printf("ok %d damaged copies, %d still made a file\n", total, made);
    return 0;
  }
  return 2;
}
