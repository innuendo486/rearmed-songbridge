// The album art maker (src/art.c) without a game:
//   arttest dds IN PX OUT       the texture of edge PX of the picture file IN, written to OUT; prints "picture" or
//                               "placeholder". IN "-" is no picture (a null pointer), "--" an empty one.
//   arttest dxt1 IN PX OUT      IN is PX x PX raw pixels (red, green, blue, one more byte): the blocks alone
//   arttest loop IN PX N        N conversions, all of which must give the same bytes: prints the milliseconds each
//                               took and the process's memory after the first tenth and at the end
//   arttest threads IN N        8 threads, N conversions of each size in each, against one thread's result
//   arttest fuzz IN N           N damaged copies of the picture: none may crash, each gives a whole texture
//   arttest cmyk OUT            writes a 96 x 64 CMYK JPEG (a test input that sips cannot make)
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "art.h"

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
  FILE *f = fopen(path, "wb");
  if (!f) return 0;
  int ok = fwrite(d, 1, n, f) == n;
  return fclose(f) == 0 && ok;
}

static uint64_t rnd(uint64_t *s) {
  *s ^= *s << 13, *s ^= *s >> 7, *s ^= *s << 17;
  return *s;
}

static double now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

// the memory the process holds, as Activity Monitor counts it
static uint64_t footprint(void) {
  task_vm_info_data_t info;
  mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
  if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) != KERN_SUCCESS) return 0;
  return info.phys_footprint;
}

// a texture of the right size with the header's fixed fields
static int whole(const uint8_t *dds, int px) {
  uint32_t edge;
  memcpy(&edge, dds + 12, 4);
  return !memcmp(dds, "DDS |\0\0\0", 8) && !memcmp(dds + 84, "DXT1", 4) && edge == (uint32_t)px;
}

typedef struct {
  const uint8_t *in;
  size_t n;
  int rounds, bad;
  uint8_t *want[3];
} job;
static const int g_edge[3] = {64, 128, 256};

static void *worker(void *arg) {
  job *j = arg;
  uint8_t *out = malloc(sb_art_size(256));
  for (int r = 0; r < j->rounds && out; r++)
    for (int e = 0; e < 3; e++) {
      memset(out, 0xee, sb_art_size(g_edge[e]));
      if (sb_art_dds(j->in, j->n, g_edge[e], out) != 1 || memcmp(out, j->want[e], sb_art_size(g_edge[e]))) j->bad++;
    }
  if (!out) j->bad++;
  free(out);
  return 0;
}

static int cmyk(const char *path) {
  enum { W = 96, H = 64 };
  static uint8_t pix[W * H * 4];
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      uint8_t *p = pix + 4 * (y * W + x);
      p[0] = (uint8_t)(x * 255 / (W - 1)), p[1] = (uint8_t)(y * 255 / (H - 1)), p[2] = x < W / 2 ? 0 : 200, p[3] = 20;
    }
  CGColorSpaceRef cs = CGColorSpaceCreateDeviceCMYK();
  CGDataProviderRef dp = CGDataProviderCreateWithData(0, pix, sizeof pix, 0);
  CGImageRef img = CGImageCreate(W, H, 8, 32, W * 4, cs, (CGBitmapInfo)kCGImageAlphaNone, dp, 0, 0, kCGRenderingIntentDefault);
  CFStringRef s = CFStringCreateWithCString(0, path, kCFStringEncodingUTF8);
  CFURLRef url = CFURLCreateWithFileSystemPath(0, s, kCFURLPOSIXPathStyle, 0);
  CGImageDestinationRef dst = CGImageDestinationCreateWithURL(url, CFSTR("public.jpeg"), 1, 0);
  int ok = img && dst;
  if (ok) {
    CGImageDestinationAddImage(dst, img, 0);
    ok = CGImageDestinationFinalize(dst);
  }
  if (dst) CFRelease(dst);
  CFRelease(url), CFRelease(s);
  CGImageRelease(img), CGDataProviderRelease(dp), CGColorSpaceRelease(cs);
  return ok ? 0 : 1;
}

int main(int argc, char **argv) {
  if (argc == 3 && !strcmp(argv[1], "cmyk")) return cmyk(argv[2]);
  if (argc < 4) return 2;
  const char *mode = argv[1];
  size_t n = 0;
  uint8_t *in = 0;
  if (!strcmp(argv[2], "--")) in = malloc(1);
  else if (strcmp(argv[2], "-") && !(in = slurp(argv[2], &n))) return fprintf(stderr, "cannot read %s\n", argv[2]), 2;

  if (!strcmp(mode, "dds") && argc == 5) {
    int px = atoi(argv[3]);
    size_t size = sb_art_size(px);
    if (!size) return fprintf(stderr, "no texture has the edge %d\n", px), 2;
    // the exact size, so that the sanitizer sees a byte written past it
    uint8_t *out = malloc(size);
    int r = sb_art_dds(in, n, px, out);
    if (r < 0 || !whole(out, px) || !spit(argv[4], out, size)) return 1;
    puts(r ? "picture" : "placeholder");
    // an edge that is no texture's, or nowhere to write: -1 and nothing written
    uint8_t guard[4] = {1, 2, 3, 4};
    if (sb_art_dds(in, n, 100, guard) != -1 || sb_art_dds(in, n, px, 0) != -1 || memcmp(guard, "\1\2\3\4", 4)) return 1;
    if (sb_art_size(0) || sb_art_size(32) || sb_art_size(512) || sb_art_size(-64)) return 1;
    free(out);
  } else if (!strcmp(mode, "dxt1") && argc == 5) {
    int px = atoi(argv[3]);
    if (px < 4 || px % 4 || n != (size_t)px * (size_t)px * 4) return fprintf(stderr, "not %d x %d pixels\n", px, px), 2;
    uint8_t *out = malloc(n / 8);
    sb_art_dxt1(in, px, out);
    if (!spit(argv[4], out, n / 8)) return 1;
    free(out);
  } else if (!strcmp(mode, "loop") && argc == 5) {
    int px = atoi(argv[3]), rounds = atoi(argv[4]);
    size_t size = sb_art_size(px);
    if (!size || rounds < 10) return 2;
    uint8_t *first = malloc(size), *out = malloc(size);
    int kind = sb_art_dds(in, n, px, first);
    uint64_t early = 0;
    double t0 = now_ms();
    for (int i = 0; i < rounds; i++) {
      memset(out, 0xee, size);
      if (sb_art_dds(in, n, px, out) != kind || memcmp(out, first, size)) return printf("round %d differs\n", i), 1;
      if (i == rounds / 10) early = footprint();
    }
    double ms = (now_ms() - t0) / rounds;
    printf("%.3f ms each, %llu KiB after %d, %llu KiB after %d\n", ms, (unsigned long long)early >> 10, rounds / 10 + 1,
           (unsigned long long)footprint() >> 10, rounds);
    free(first), free(out);
  } else if (!strcmp(mode, "threads") && argc == 4) {
    enum { THREADS = 8 };
    job j[THREADS];
    pthread_t t[THREADS];
    int bad = 0;
    memset(j, 0, sizeof j);
    for (int e = 0; e < 3; e++) {
      j[0].want[e] = malloc(sb_art_size(g_edge[e]));
      if (sb_art_dds(in, n, g_edge[e], j[0].want[e]) != 1) return printf("not a picture\n"), 1;
    }
    j[0].in = in, j[0].n = n, j[0].rounds = atoi(argv[3]);
    for (int i = 1; i < THREADS; i++) j[i] = j[0];
    for (int i = 0; i < THREADS; i++) pthread_create(&t[i], 0, worker, &j[i]);
    for (int i = 0; i < THREADS; i++) pthread_join(t[i], 0), bad += j[i].bad;
    for (int e = 0; e < 3; e++) free(j[0].want[e]);
    printf("%d threads, %d conversions each, %d wrong\n", THREADS, 3 * j[0].rounds, bad);
    if (bad) return 1;
  } else if (!strcmp(mode, "fuzz") && argc == 4) {
    int rounds = atoi(argv[3]), pictures = 0;
    uint64_t seed = 0x9e3779b97f4a7c15ull;
    uint8_t *copy = malloc(n + 1), *out = malloc(sb_art_size(64));
    for (int i = 0; i < rounds && n; i++) {
      memcpy(copy, in, n);
      size_t m = n;
      if (i % 3 == 0) m = rnd(&seed) % n;  // cut short
      for (unsigned k = 0, hits = i % 3 == 1 ? 1 : (unsigned)(rnd(&seed) % 16); k < hits && m; k++)
        copy[rnd(&seed) % m] = (uint8_t)rnd(&seed);
      int r = sb_art_dds(copy, m, 64, out);
      if (r < 0 || !whole(out, 64)) return printf("round %d: no texture\n", i), 1;
      pictures += r;
    }
    printf("%d damaged copies: %d pictures, %d placeholders\n", rounds, pictures, rounds - pictures);
    free(copy), free(out);
  } else {
    return 2;
  }
  free(in);
  return 0;
}
