// A test mod, not part of songbridge: logs every read the game makes of chosen archives. It serves each archive
// whose path contains a line of ~/Library/Caches/rs-readtrace/targets (lower case) from the file itself and writes
// ~/Library/Caches/rs-readtrace/<target>.log: "ms open|read off n|close", ms since the mod loaded.
//   clang -arch arm64 -O2 -dynamiclib -Wl,-undefined,dynamic_lookup -I<rs-rearmed>/tools/ws7/recomp/runtime \
//     readtrace.c -o readtrace.dylib   (then into the edition's mods folder)
#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "rc_mod.h"

#define MAX_TARGETS 16
static char targets[MAX_TARGETS][128];
static int ntargets;
static FILE *logf;
static char dir[PATH_MAX];
static struct timespec t0;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
  int fd, id;
} handle;
static int next_id;

static double ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)(t.tv_sec - t0.tv_sec) * 1000 + (double)(t.tv_nsec - t0.tv_nsec) / 1e6;
}

static void say(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  pthread_mutex_lock(&mu);
  if (logf) {
    fprintf(logf, "%.1f ", ms());
    vfprintf(logf, fmt, ap);
    fputc('\n', logf);
    fflush(logf);
  }
  pthread_mutex_unlock(&mu);
  va_end(ap);
}

static void *t_open(void *user, const char *path, rc_file_info *info) {
  (void)user;
  size_t n = strlen(path);
  if (n < 7 || n >= PATH_MAX || strcasecmp(path + n - 6, ".psarc")) return 0;
  char l[PATH_MAX];
  for (size_t i = 0; i <= n; i++) l[i] = (char)tolower((unsigned char)path[i]);
  int hit = 0;
  for (int i = 0; i < ntargets && !hit; i++) hit = strstr(l, targets[i]) != 0;
  if (!hit) return 0;
  struct stat st;
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0 || fstat(fd, &st)) {
    if (fd >= 0) close(fd);
    return 0;
  }
  handle *h = malloc(sizeof *h);
  if (!h) {
    close(fd);
    return 0;
  }
  h->fd = fd;
  pthread_mutex_lock(&mu);
  h->id = ++next_id;
  pthread_mutex_unlock(&mu);
  *info = (rc_file_info){st.st_size, st.st_mtimespec.tv_sec};
  say("open %d %s size %lld", h->id, path, (long long)st.st_size);
  return h;
}

static int64_t t_read(void *user, void *file, void *buf, uint64_t n, int64_t off) {
  (void)user;
  handle *h = file;
  ssize_t r = pread(h->fd, buf, n, off);
  say("read %d %lld %llu", h->id, (long long)off, (unsigned long long)n);
  return r;
}

static void t_close(void *user, void *file) {
  (void)user;
  handle *h = file;
  say("close %d", h->id);
  close(h->fd);
  free(h);
}

__attribute__((constructor)) static void init(void) {
  clock_gettime(CLOCK_MONOTONIC, &t0);
  const char *home = getenv("HOME");
  if (!home) return;
  snprintf(dir, sizeof dir, "%s/Library/Caches/rs-readtrace", home);
  char p[PATH_MAX];
  snprintf(p, sizeof p, "%s/targets", dir);
  FILE *f = fopen(p, "r");
  if (!f) return;
  while (ntargets < MAX_TARGETS && fgets(targets[ntargets], sizeof targets[0], f)) {
    char *t = targets[ntargets];
    t[strcspn(t, "\r\n")] = 0;
    for (char *c = t; *c; c++) *c = (char)tolower((unsigned char)*c);
    if (*t) ntargets++;
  }
  fclose(f);
  if (!ntargets) return;
  const char *target = rc_target();
  snprintf(p, sizeof p, "%s/%s.log", dir, target ? target : "unknown");
  logf = fopen(p, "w");
  static const rc_file_provider prov = {.open = t_open, .read = t_read, .close = t_close};
  if (!logf || rc_file_provide(&prov)) return;
  fprintf(stderr, "[readtrace] %d targets, log %s\n", ntargets, p);
}
