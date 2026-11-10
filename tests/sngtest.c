// The chart reader and song file writer (src/chart.c, src/sng.c) without a game. tests/run_sng_test.py drives this.
//   sngtest unpack mac|pc IN.sng OUT        a song file's plain payload
//   sngtest pack mac|pc IN OUT.sng          a payload as a song file
//   sngtest build mac|pc ARR.json TIMELINE.json|- OUT.sng [LENGTH [BASS]]
//                                           the song file of an arrangement; prints what the manifest needs, or
//                                           "none: REASON" and exit 3
//   sngtest fuzz ARR.json N [SEED]          N damaged copies of the file are read and built: none may crash
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chart.h"
#include "sng.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

static uint8_t *slurp(const char *path, size_t *n) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    fprintf(stderr, "cannot read %s\n", path);
    exit(2);
  }
  CHECK(!fseek(f, 0, SEEK_END));
  long len = ftell(f);
  CHECK(len >= 0 && !fseek(f, 0, SEEK_SET));
  uint8_t *d = malloc((size_t)len + 1);
  CHECK(d && fread(d, 1, (size_t)len, f) == (size_t)len);
  fclose(f);
  d[len] = 0, *n = (size_t)len;
  return d;
}

static void spit(const char *path, const uint8_t *d, size_t n) {
  FILE *f = fopen(path, "wb");
  CHECK(f && fwrite(d, 1, n, f) == n && !fclose(f));
}

static int platform(const char *s) {
  if (!strcmp(s, "mac")) return SB_MAC;
  if (!strcmp(s, "pc")) return SB_PC;
  fprintf(stderr, "platform: mac or pc\n");
  exit(2);
}

static void repairs_print(const sb_chart *c) {
  printf("repairs %u", c->repairs.total);
  for (int i = 0; i < SB_R_COUNT; i++)
    if (c->repairs.n[i]) printf(" %s=%u", sb_repair_name(i), c->repairs.n[i]);
  printf("\nrepaired %s\n", c->repairs.text);
}

static int build(int argc, char **argv) {
  size_t n, tn = 0, out_n = 0, mac_n = 0;
  char err[256];
  uint8_t *json = slurp(argv[3], &n), *tl = strcmp(argv[4], "-") ? slurp(argv[4], &tn) : 0;
  sb_sng_opts o = {argc > 6 ? atof(argv[6]) : 0, argc > 7 ? atoi(argv[7]) : -1};
  sb_chart *c = sb_chart_parse((const char *)json, n, (const char *)tl, tn, err, sizeof err);
  if (!c) {
    printf("none: %s\n", err);
    return 3;
  }
  sb_sng_facts f;
  uint8_t *sng = sb_sng_build(c, platform(argv[2]), &o, &f, &out_n, err, sizeof err);
  if (!sng) {
    printf("none: %s\n", err);
    sb_chart_free(c);
    return 3;
  }
  // the same bytes again, the same size on the other platform, and its payload back
  size_t again_n = 0, pn = 0, qn = 0;
  uint8_t *again = sb_sng_build(c, platform(argv[2]), &o, 0, &again_n, err, sizeof err);
  CHECK(again && again_n == out_n && !memcmp(again, sng, out_n));
  uint8_t *other = sb_sng_build(c, !platform(argv[2]), &o, 0, &mac_n, err, sizeof err);
  CHECK(other && mac_n == out_n);
  uint8_t *p = sb_sng_payload(c, &o, 0, &pn, err, sizeof err), *q = sb_sng_unpack(sng, out_n, platform(argv[2]), &qn);
  CHECK(p && q && pn == qn && !memcmp(p, q, pn));
  free(again), free(other), free(p), free(q);
  spit(argv[5], sng, out_n);
  repairs_print(c);
  printf("bass %d strings %d capo %d cents %.1f tuning %d %d %d %d %d %d\n", f.bass, f.strings, f.capo, f.cents, f.tuning[0], f.tuning[1],
         f.tuning[2], f.tuning[3], f.tuning[4], f.tuning[5]);
  printf("max_difficulty %d song_length %.3f first_beat %.3f first_note %.3f last_note %.3f\n", f.max_difficulty, f.song_length,
         f.first_beat, f.first_note, f.last_note);
  printf("notes %u chords %u ignored %u easy %u medium %u hard %u points %.6f techniques %x\n", f.notes, f.chords, f.ignored, f.notes_easy,
         f.notes_medium, f.notes_hard, f.points_per_note, f.techniques);
  printf("made%s%s%s%s\n", f.made & SB_MADE_BEATS ? " beats" : "", f.made & SB_MADE_SECTION ? " section" : "",
         f.made & SB_MADE_ITERATIONS ? " iterations" : "", f.made & SB_MADE_ANCHOR ? " anchor" : "");
  for (uint32_t i = 0; i < f.nphrases; i++)
    printf("phrase %u %s max %d iterations %u\n", i, f.phrases[i].name, f.phrases[i].max_difficulty, f.phrases[i].iterations);
  for (uint32_t i = 0; i < f.niters; i++)
    printf("iter %u phrase %u %.3f %.3f max %d levels %d %d %d notes %u\n", i, f.iters[i].phrase, f.iters[i].t, f.iters[i].end,
           f.iters[i].max_difficulty, f.iters[i].easy, f.iters[i].medium, f.iters[i].hard, f.iters[i].notes);
  for (uint32_t i = 0; i < f.nsections; i++)
    printf("section %u %s %d %.3f %.3f iters %u %u\n", i, f.sections[i].name, f.sections[i].number, f.sections[i].t, f.sections[i].end,
           f.sections[i].first_iter, f.sections[i].last_iter);
  for (uint32_t i = 0; i < f.ntemplates; i++)
    printf("template %u [%s] arp %d midi %d %d %d %d %d %d\n", i, f.templates[i].name, f.templates[i].arp, f.templates[i].midi[0],
           f.templates[i].midi[1], f.templates[i].midi[2], f.templates[i].midi[3], f.templates[i].midi[4], f.templates[i].midi[5]);
  printf("uses %u\n", f.nuses);
  for (uint32_t i = 0; i < f.ntones; i++) printf("tone %u %s\n", i, f.tones[i]);
  for (uint32_t i = 0; i < f.nchanges; i++) printf("change %.3f %u\n", f.changes[i].t, f.changes[i].id);
  sb_sng_facts_free(&f);
  free(sng), sb_chart_free(c), free(json), free(tl);
  return 0;
}

static uint64_t rnd(uint64_t *s) {
  *s ^= *s << 13, *s ^= *s >> 7, *s ^= *s << 17;
  return *s;
}

// Damage that keeps most copies JSON: digits and words swapped for others, as well as plain noise and cuts
static int fuzz(int argc, char **argv) {
  static const char *const words[] = {"-1",  "99", "1e99", "-0.5",  "null", "true", "[]",        "{}",
                                      "\"x\"", "500", "0",    "1e-9", "\"7\"", "NaN",  "2147483648"};
  size_t n;
  uint8_t *src = slurp(argv[2], &n);
  long rounds = atol(argv[3]), built = 0, refused = 0;
  uint64_t s = argc > 4 ? strtoull(argv[4], 0, 0) | 1 : 0x9e3779b97f4a7c15ull;
  char *d = malloc(n + 64), err[200];
  CHECK(d && n > 16);
  for (long r = 0; r < rounds; r++) {
    size_t len = n;
    memcpy(d, src, n);
    int kind = (int)(rnd(&s) % 8), hits = 1 + (int)(rnd(&s) % 6);
    for (int h = 0; h < hits; h++) {
      size_t at = (size_t)(rnd(&s) % len);
      if (kind == 0) d[at] = (char)rnd(&s);
      else if (kind == 1) len = at + 1;
      else {
        // the number or word at or after `at`, replaced
        while (at < len && !(d[at] >= '0' && d[at] <= '9') && d[at] != 't' && d[at] != 'f') at++;
        size_t end = at;
        while (end < len && ((d[end] >= '0' && d[end] <= '9') || d[end] == '.' || (d[end] >= 'a' && d[end] <= 'z'))) end++;
        const char *wd = words[rnd(&s) % (sizeof words / sizeof *words)];
        size_t wl = strlen(wd);
        if (at == len || len - (end - at) + wl > n + 60) continue;
        memmove(d + at + wl, d + end, len - end);
        memcpy(d + at, wd, wl);
        len = len - (end - at) + wl;
      }
    }
    sb_chart *c = sb_chart_parse(d, len, r % 3 ? 0 : d, r % 3 ? 0 : len, err, sizeof err);
    if (!c) {
      CHECK(err[0]);
      refused++;
      continue;
    }
    size_t out_n = 0;
    sb_sng_facts f;
    uint8_t *sng = sb_sng_build(c, (int)(r & 1), 0, r & 2 ? &f : 0, &out_n, err, sizeof err);
    if (sng) {
      size_t pn = 0;
      uint8_t *p = sb_sng_unpack(sng, out_n, (int)(r & 1), &pn);
      CHECK(p && pn > 100);
      free(p);
      if (r & 2) sb_sng_facts_free(&f);
      built++;
    } else {
      CHECK(err[0]);
      refused++;
    }
    free(sng), sb_chart_free(c);
  }
  printf("fuzz: %ld copies, %ld built, %ld refused\n", rounds, built, refused);
  free(d), free(src);
  return 0;
}

int main(int argc, char **argv) {
  if (argc >= 5 && (!strcmp(argv[1], "unpack") || !strcmp(argv[1], "pack"))) {
    size_t n, out_n = 0;
    uint8_t *in = slurp(argv[3], &n);
    uint8_t *out = argv[1][0] == 'u' ? sb_sng_unpack(in, n, platform(argv[2]), &out_n) : sb_sng_pack(in, n, platform(argv[2]), &out_n);
    if (!out) {
      printf("none\n");
      return 3;
    }
    spit(argv[4], out, out_n);
    free(in), free(out);
    return 0;
  }
  if (argc >= 6 && !strcmp(argv[1], "build")) return build(argc, argv);
  if (argc >= 4 && !strcmp(argv[1], "fuzz")) return fuzz(argc, argv);
  fprintf(stderr, "usage: sngtest unpack|pack mac|pc IN OUT | build mac|pc ARR TIMELINE|- OUT [LENGTH [BASS]] | fuzz ARR N [SEED]\n");
  return 2;
}
