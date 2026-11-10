// The feedpak reader (src/feedpak.c) without a game: paktest PATH prints the package's manifest as the reader has
// it and, for every file the manifest names, its size and CRC-32; or "none: REASON" and exit 3. paktest PATH --fuzz N
// opens N damaged copies of the file (a zip): none may crash. tests/run_pak_test.py drives this.
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include "feedpak.h"

static void file(const sb_pak *p, const char *what, const char *rel) {
  if (!rel) return;
  size_t n = 0;
  uint8_t *d = sb_pak_read(p, rel, &n);
  if (!d) {
    printf("file %s %s size %lld unreadable\n", what, rel, (long long)sb_pak_size(p, rel));
    return;
  }
  if ((int64_t)n != sb_pak_size(p, rel)) printf("SIZE MISMATCH ");
  printf("file %s %s size %zu crc %08lx\n", what, rel, n, crc32(0, d, (uInt)n));
  free(d);
}

static void show(const sb_pak *p) {
  const sb_manifest *m = sb_pak_manifest(p);
  printf("title %s\nartist %s\nalbum %s\nalbum_artist %s\nmbid %s\nisrc %s\nyear %d\nduration %.3f\n", m->title, m->artist,
         m->album ? m->album : "-", m->album_artist ? m->album_artist : "-", m->mbid ? m->mbid : "-", m->isrc ? m->isrc : "-", m->year,
         m->duration);
  for (uint32_t i = 0; i < m->narr; i++) {
    const sb_pak_arr *a = &m->arr[i];
    printf("arrangement %s name %s type %s drums %d vocals %d capo %d cents %.1f tuning", a->id, a->name, a->type ? a->type : "-", a->drums, a->vocals, a->capo,
           a->cents);
    for (int s = 0; s < a->strings; s++) printf(" %d", a->tuning[s]);
    printf("\n");
    file(p, "chart", a->file);
  }
  for (uint32_t i = 0; i < m->nstems; i++) {
    printf("stem %s codec %s on %d\n", m->stem[i].id, m->stem[i].codec ? m->stem[i].codec : "-", m->stem[i].on);
    file(p, "stem", m->stem[i].file);
  }
  file(p, "cover", m->cover), file(p, "preview", m->preview), file(p, "lyrics", m->lyrics), file(p, "timeline", m->timeline);
  for (uint32_t i = 0; sb_pak_warning(p, i); i++) printf("warning %s\n", sb_pak_warning(p, i));
  printf("absent %lld %lld\n", (long long)sb_pak_size(p, "no/such/file"), (long long)sb_pak_size(p, "../manifest.yaml"));
}

static uint64_t rnd(uint64_t *s) {
  *s ^= *s << 13, *s ^= *s >> 7, *s ^= *s << 17;
  return *s;
}

int main(int argc, char **argv) {
  char err[200];
  if (argc == 4 && !strcmp(argv[2], "--fuzz")) {
    struct stat st;
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0 || fstat(fd, &st)) return 2;
    size_t n = (size_t)st.st_size;
    uint8_t *d = malloc(n), *b = malloc(n);
    if (!d || !b || read(fd, d, n) != (ssize_t)n) return 2;
    close(fd);
    char tmp[] = "/tmp/paktest.XXXXXX";
    int t = mkstemp(tmp);
    if (t < 0) return 2;
    uint64_t s = 0x9e3779b97f4a7c15ull;
    int opened = 0, total = atoi(argv[3]);
    for (int k = 0; k < total; k++) {
      memcpy(b, d, n);
      size_t len = n;
      if (k % 3 == 0) len = (size_t)(rnd(&s) % (n + 1));
      else
        for (int j = 0; j < 1 + k % 6; j++) {
          // mostly where the structure is: the end of the file (directory, end record)
          size_t span = k % 3 == 1 && n > 2000 ? 2000 : n;
          b[n - 1 - rnd(&s) % span] ^= (uint8_t)(1u << (rnd(&s) % 8));
        }
      if (ftruncate(t, 0) || pwrite(t, b, len, 0) != (ssize_t)len) return 2;
      sb_pak *p = sb_pak_open(tmp, err, sizeof err);
      if (p) {
        const sb_manifest *m = sb_pak_manifest(p);
        for (uint32_t i = 0; i < m->narr; i++)
          if (m->arr[i].file) {
            size_t fn;
            free(sb_pak_read(p, m->arr[i].file, &fn));
          }
        for (uint32_t i = 0; i < m->nstems; i++) {
          size_t fn;
          free(sb_pak_read(p, m->stem[i].file, &fn));
        }
        opened++;
        sb_pak_close(p);
      }
    }
    close(t), unlink(tmp);
    printf("ok %d damaged copies, %d still opened\n", total, opened);
    return 0;
  }
  if (argc != 2) return 2;
  sb_pak *p = sb_pak_open(argv[1], err, sizeof err);
  if (!p) return printf("none: %s\n", err), 3;
  show(p);
  sb_pak_close(p);
  return 0;
}
