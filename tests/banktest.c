// The sound bank builder (src/bank.c) and the preview cutter (src/wemcut.c) without a game:
//   banktest ids KEY...                    the ids of each song's two banks
//   banktest build KEY PREVIEW WEM OUT     the bank of WEM (PREVIEW 0 or 1) with the ids made from KEY
//   banktest write PREFETCH OUT NAME PREVIEW BANK WEM SOUND ACTION EVENT VOLUME-BITS OFF SIZE ELAPSED HAS36 P36
//                                          a bank with every field given (numbers in hex): to remake a real one
//   banktest wem CODEBOOKS IN.ogg OUT.wem  the Ogg as a .wem (src/wem.c; CODEBOOKS as tests/wemtest.c says)
//   banktest cut IN.wem OUT.wem START SECONDS   the preview; START below 0 and SECONDS 0 are the defaults
//   banktest fuzz IN.wem N                 N damaged copies of the .wem are cut, and N odd banks built: none may crash
// A refusal prints "none: <why>" and exits 3.
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bank.h"
#include "wem.h"
#include "wemcut.h"

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
  int o = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  return o >= 0 && write(o, d, n) == (ssize_t)n && !close(o);
}

static uint64_t rnd(uint64_t *s) {
  *s ^= *s << 13, *s ^= *s >> 7, *s ^= *s << 17;
  return *s;
}

static uint32_t hex(const char *s) { return (uint32_t)strtoul(s, 0, 16); }

static void print_ids(const char *what, const sb_bank_ids *i) {
  printf("%s bank=%08x wem=%08x sound=%08x action=%08x event=%08x\n", what, i->bank, i->wem, i->sound, i->action, i->event);
}

static int ogg_to_wem(const char *books, const char *in, const char *out) {
  size_t ln, n, on;
  uint8_t *l = slurp(books, &ln), *ogg = slurp(in, &n);
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
  uint8_t *w = sb_wem_from_ogg(ogg, n, &lib, &on, err, sizeof err);
  if (!w) return printf("none: %s\n", err), 3;
  if (!spit(out, w, on)) return 2;
  printf("ok %zu bytes\n", on);
  free(w), free(book), free(avail), free(l), free(ogg);
  return 0;
}

// One cut of its own allocation (a read past the end is caught): 1 when a preview was made
static int cut_copy(const uint8_t *d, size_t n, double start, double seconds) {
  char err[200];
  size_t on = 0, on2 = 0;
  uint64_t at;
  uint8_t *copy = malloc(n ? n : 1);
  memcpy(copy, d, n);
  uint8_t *w = sb_wem_preview(copy, n, start, seconds, &on, &at, err, sizeof err);
  free(copy);
  if (!w) return 0;
  // what was made is a .wem too: it must cut again, whole
  uint8_t *again = sb_wem_preview(w, on, 0, 1e9, &on2, 0, err, sizeof err);
  if (!again || on2 > on) fprintf(stderr, "a preview that does not cut again: %s\n", again ? "it grew" : err), abort();
  free(again), free(w);
  return 1;
}

static int fuzz(const char *in, int total) {
  size_t n;
  uint8_t *wem = slurp(in, &n);
  if (!wem || n < 200) return fprintf(stderr, "cannot read the input\n"), 2;
  uint64_t s = 0x243f6a8885a308d3ull;
  int made = 0, banks = 0;
  uint8_t *b = malloc(n);
  static const double starts[] = {-1, 0, 3.5, 1e12, -0.0, 29.99}, lens[] = {0, 30, 0.001, 1e12, -5, 7};
  for (int k = 0; k < total; k++) {
    memcpy(b, wem, n);
    size_t len = n, head = n < 400 ? n : 400, setup = n < 6000 ? n : 6000;
    switch (k % 6) {
      case 0: len = (size_t)(rnd(&s) % (n + 1)); break;                                              // cut short
      case 1: len = (size_t)(rnd(&s) % (setup + 1)); break;                                          // cut short early
      case 2: for (int j = 0; j < 1 + k % 5; j++) b[rnd(&s) % head] ^= (uint8_t)(1u << (rnd(&s) % 8)); break;   // bits of the header
      case 3: for (int j = 0; j < 1 + k % 7; j++) b[rnd(&s) % setup] = (uint8_t)rnd(&s); break;      // bytes of header and setup
      case 4: for (int j = 0; j < 8; j++) b[rnd(&s) % n] = (uint8_t)rnd(&s); break;                  // bytes anywhere
      case 5: for (size_t j = 12 + rnd(&s) % 90; j < n; j++) b[j] = (uint8_t)rnd(&s); break;         // garbage after a true start
    }
    made += cut_copy(b, len, starts[rnd(&s) % 6], lens[rnd(&s) % 6]);
  }
  // nothing, and noise that only says it is a .wem
  made += cut_copy(b, 0, -1, 0);
  for (size_t j = 0; j < n; j++) b[j] = (uint8_t)rnd(&s);
  made += cut_copy(b, n, -1, 0);
  memcpy(b, "RIFF\0\0\0\0WAVEfmt \x42\0\0\0\xff\xff", 22);
  made += cut_copy(b, n, -1, 0);
  // banks: keys of every length (a few past the limit), odd bytes in them, prefetches of every small size
  for (int k = 0; k < total; k++) {
    char key[SB_BANK_KEY_MAX + 8];
    size_t kl = (size_t)(rnd(&s) % (SB_BANK_KEY_MAX + 6)), pn = (size_t)(rnd(&s) % 3000), on = 0;
    for (size_t j = 0; j < kl; j++) key[j] = (char)(1 + rnd(&s) % 255);
    key[kl] = 0;
    int preview = (int)(rnd(&s) & 1);
    uint8_t *pre = malloc(pn ? pn : 1);
    memcpy(pre, wem, pn < n ? pn : n);
    uint8_t *bank = sb_bank_build(key, preview, 1 + (uint32_t)rnd(&s) % 1000, pre, pn, -7, &on);
    size_t want = sb_bank_size(key, preview, pn);
    if ((bank != 0) != (want != 0) || (bank && on != want)) fprintf(stderr, "a bank of %zu bytes, %zu foretold\n", on, want), abort();
    banks += bank != 0;
    free(bank), free(pre);
  }
  printf("ok %d damaged copies, %d still made a preview; %d odd banks, %d made\n", total + 3, made, total, banks);
  free(b), free(wem);
  return 0;
}

int main(int argc, char **argv) {
  const char *cmd = argc > 1 ? argv[1] : "";
  if (!strcmp(cmd, "ids") && argc >= 3) {
    for (int i = 2; i < argc; i++) {
      sb_bank_ids m, p;
      if (!sb_bank_ids_of(argv[i], 0, &m) || !sb_bank_ids_of(argv[i], 1, &p)) return printf("none: a bad key\n"), 3;
      print_ids("main", &m), print_ids("preview", &p);
    }
    return 0;
  }
  if (!strcmp(cmd, "build") && argc == 6) {
    size_t n, on;
    int preview = atoi(argv[3]);
    uint8_t *wem = slurp(argv[4], &n);
    if (!wem) return fprintf(stderr, "cannot read %s\n", argv[4]), 2;
    sb_bank_ids ids;
    size_t pre = sb_bank_prefetch(n), want = sb_bank_size(argv[2], preview, pre);
    uint8_t *bank = sb_bank_ids_of(argv[2], preview, &ids) ? sb_bank_build(argv[2], preview, ids.wem, wem, pre, SB_BANK_VOLUME, &on) : 0;
    if (!bank) return printf("none: a bad key or no prefetch (size foretold: %zu)\n", want), 3;
    if (!spit(argv[5], bank, on)) return 2;
    printf("size=%zu foretold=%zu prefetch=%zu\n", on, want, pre);
    print_ids("ids", &ids);
    free(bank), free(wem);
    return 0;
  }
  if (!strcmp(cmd, "write") && argc == 17) {
    size_t n, on;
    uint8_t *pre = slurp(argv[2], &n);
    if (!pre) return 2;
    sb_bank_spec s = {.name = argv[4], .preview = atoi(argv[5]),
                      .ids = {hex(argv[6]), hex(argv[7]), hex(argv[8]), hex(argv[9]), hex(argv[10])},
                      .media_off = hex(argv[12]), .media_size = hex(argv[13]), .from_elapsed = atoi(argv[14]),
                      .has_prop36 = atoi(argv[15]), .prop36 = hex(argv[16])};
    uint32_t vol = hex(argv[11]);
    memcpy(&s.volume, &vol, 4);
    uint8_t *bank = sb_bank_write(&s, pre, n, &on);
    if (!bank) return printf("none\n"), 3;
    if (!spit(argv[3], bank, on)) return 2;
    free(bank), free(pre);
    return 0;
  }
  if (!strcmp(cmd, "wem") && argc == 5) return ogg_to_wem(argv[2], argv[3], argv[4]);
  if (!strcmp(cmd, "cut") && argc == 6) {
    size_t n, on;
    uint64_t at;
    char err[200];
    uint8_t *wem = slurp(argv[2], &n);
    if (!wem) return fprintf(stderr, "cannot read %s\n", argv[2]), 2;
    uint8_t *w = sb_wem_preview(wem, n, atof(argv[4]), atof(argv[5]), &on, &at, err, sizeof err);
    if (!w) return printf("none: %s\n", err), 3;
    if (!spit(argv[3], w, on)) return 2;
    printf("ok bytes=%zu at=%llu\n", on, (unsigned long long)at);
    free(w), free(wem);
    return 0;
  }
  if (!strcmp(cmd, "fuzz") && argc == 4) return fuzz(argv[2], atoi(argv[3]));
  fprintf(stderr, "usage: see the top of tests/banktest.c\n");
  return 2;
}
