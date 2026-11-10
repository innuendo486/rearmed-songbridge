// The file provider: in the game's dlc folder and below, an archive of the other platform (x_p.psarc in the Mac
// edition, x_m.psarc in Learn & Play) is listed under this edition's suffix and served as this edition's archive
// (psarc.c); its own name is left out of the listing. A song (by its key: the archive's xblock names) is shown
// once: an archive of this edition's own wins over one of the other's, and of two of the other's the first by path.
// A file that is not a whole archive is left as it is.
//
// A feedpak (x.feedpak, a zip file or a folder: feedsong.h) there is listed as x with this edition's suffix and served
// as the song archive made of it, under the same rules: its song is shown once, and an archive wins over a package.
// The sizes of what it is made of are kept in ~/Library/Caches/RS Re-ARMed/songbridge (RS_SONGBRIDGE_CACHE names
// another folder), which can be deleted at any time.
//
// The dlc folder is the last folder named "dlc" on a path. It is walked when the game lists a folder in it (again
// when that was more than sb_rescan_ms ago; a file is read again only when its size or date changed).
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "feedsong.h"
#include "psarc.h"
#ifdef SB_TEST  // tests/sbtest.c starts it, for either edition
#include "rc_file.h"
#else
#include "rc_mod.h"
#endif

// The mod's version, where the installer app finds it in the file (app/Sources/SongBridgeApp/Installer.swift)
#ifndef SB_VERSION
#define SB_VERSION "0"
#endif
__attribute__((used)) const char sb_version_tag[] = "songbridge-version:" SB_VERSION;

#define SFX_LEN 8  // "_m.psarc"
#define MAX_ROOTS 8
#define MAX_DEPTH 8

int sb_rescan_ms = 500;

typedef struct arch {
  char *real, *lreal;    // the file; lower case
  char *lvpath, *vname;  // other platform's: the path it is served at (lower case), the name it is listed by
  int64_t size, mtime;
  int foreign, parsed, valid, shown, virt_failed, keep, dead, handles;
  int pak;      // a feedpak (foreign too: it is served, never read by the game itself)
  sb_arc *arc;  // other platform's
  sb_virt *virt;
  sb_feedsong *song;  // a feedpak's archive
  char **keys;
  int nkeys;
} arch;

typedef struct {
  char *path, *lpath;
  arch **a;      // by lreal
  arch **shown;  // by lvpath
  int n, nshown, scanned;
  uint64_t at;
} root;

typedef struct {
  arch *a;
  int fd;
  _Atomic uint64_t reads, bytes, upto;  // for the log: how much of it the game read
} handle;

static int own = -1;
static const char *sfx[2] = {"_m.psarc", "_p.psarc"};
static root roots[MAX_ROOTS];
static int nroots;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

static int verbose(void) {
  static int on = -1;
  if (on < 0) {
    // RS_SONGBRIDGE_LOG, or (the game is started as an app, with no place for a variable) a file named "trace" in
    // the cache folder
    const char *e = getenv("RS_SONGBRIDGE_LOG"), *home = getenv("HOME");
    char flag[PATH_MAX];
    on = e && *e && *e != '0';
    if (!on && home && snprintf(flag, sizeof flag, "%s/Library/Caches/RS Re-ARMed/songbridge/trace", home) < (int)sizeof flag)
      on = !access(flag, F_OK);
    sb_feedsong_log = on;
  }
  return on;
}

static uint64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static char *lower(const char *s) {
  char *l = strdup(s);
  for (char *p = l; l && *p; p++) *p = (char)tolower((unsigned char)*p);
  return l;
}

// 0 mac, 1 pc: the platform path's suffix names; -1 none
static int platform_of(const char *path) {
  size_t n = strlen(path);
  if (n <= SFX_LEN) return -1;
  for (int k = 0; k < 2; k++)
    if (!strcasecmp(path + n - SFX_LEN, sfx[k])) return k;
  return -1;
}

// 1 when the name is a feedpak's
static int pak_name(const char *name) {
  size_t n = strlen(name);
  return n > 8 && !strcasecmp(name + n - 8, ".feedpak");
}

// --- what a feedpak's archive needs of the game ----------------------------------------------------------------------
// The game's Vorbis codebook library (ak_vorbis_codebooks: a table of pointers to where each codebook starts, in one
// block), or 0 when this game does not name it. tests/sbtest.c has its own, from a file.
#ifdef SB_TEST
const sb_books *sb_test_books(void);
static const sb_books *books(void) { return sb_test_books(); }
#else
#define BOOK_SLACK 0x400  // the table does not say where the last codebook ends
static sb_books game_books;
static void books_once(void) {
  uint64_t a = RC_ADDR(ak_vorbis_codebooks);
  int64_t n = RC_OFF(ak_vorbis_codebook_count);
  if (!a || n == RC_NO_OFF || n < 1 || n > 4096) return;
  const uint8_t *table = (const uint8_t *)(uintptr_t)RC_A(a);
  const uint8_t **book = calloc((size_t)n, sizeof *book);
  size_t *avail = calloc((size_t)n, sizeof *avail);
  if (!book || !avail) return;
  for (int64_t i = 0; i < n; i++)
    if (!(book[i] = rc_mod_ptr(table + i * rc_guest_ptr_size)) || (i && book[i] <= book[i - 1])) return;
  for (int64_t i = 0; i < n; i++) avail[i] = i + 1 < n ? (size_t)(book[i + 1] - book[i]) + 8 : BOOK_SLACK;
  game_books = (sb_books){(uint32_t)n, book, avail};
}
static const sb_books *books(void) {
  static pthread_once_t once = PTHREAD_ONCE_INIT;
  pthread_once(&once, books_once);
  return game_books.n ? &game_books : 0;
}
#endif

static const char *cache_dir(void) {
  static char dir[PATH_MAX];
  if (!dir[0]) {
    const char *e = getenv("RS_SONGBRIDGE_CACHE"), *home = getenv("HOME");
    if (e && *e) snprintf(dir, sizeof dir, "%s", e);
    else if (home) snprintf(dir, sizeof dir, "%s/Library/Caches/RS Re-ARMed/songbridge", home);
  }
  return dir[0] ? dir : 0;
}

// The length of the dlc folder's path in `path` (a folder when is_dir, else a file), 0 when it is in none
static size_t root_len(const char *path, int is_dir) {
  size_t n = strlen(path), best = 0;
  if (path[0] != '/') return 0;
  for (size_t i = 0; i < n;) {  // i at a '/'
    size_t j = i + 1;
    while (j < n && path[j] != '/') j++;
    if (j - i == 4 && !strncasecmp(path + i + 1, "dlc", 3) && (j < n || is_dir)) best = j;
    i = j;
  }
  return best;
}

// --- one archive -------------------------------------------------------------------------------------------------
static void arch_free(arch *a) {
  sb_feedsong_free(a->song);
  sb_virt_free(a->virt);
  sb_arc_free(a->arc);
  for (int i = 0; i < a->nkeys; i++) free(a->keys[i]);
  free(a->keys), free(a->real), free(a->lreal), free(a->lvpath), free(a->vname);
  free(a);
}
// Out of the table: freed with its last open file
static void arch_retire(arch *a) {
  if (a->handles) a->dead = 1;
  else arch_free(a);
}

static void add_key(void *ctx, const char *key) {
  arch *a = ctx;
  char **k = realloc(a->keys, (size_t)(a->nkeys + 1) * sizeof *k);
  if (!k) return;
  a->keys = k;
  if ((k[a->nkeys] = strdup(key))) a->nkeys++;
}

static void arch_parse(arch *a) {
  a->parsed = 1;
  if (a->pak) {  // the package's archive, from what was kept of it or made now (then this takes a moment)
    char err[400];
    if ((a->song = sb_feedsong_open(a->real, own, cache_dir(), books, err, sizeof err))) {
      a->valid = 1;
      add_key(a, sb_feedsong_key(a->song));
      if (*sb_feedsong_note(a->song)) fprintf(stderr, "[songbridge] %s: %s\n", a->real, sb_feedsong_note(a->song));
    } else {
      fprintf(stderr, "[songbridge] %s is not served: %s\n", a->real, err);
    }
    return;
  }
  int fd = open(a->real, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return;
  sb_arc *arc = sb_parse(fd, a->size);
  close(fd);
  if (!arc) return;
  a->valid = 1;
  sb_arc_keys(arc, add_key, a);
  if (a->foreign) a->arc = arc;
  else sb_arc_free(arc);
}

// 1 when the archive can be served
static int arch_ready(arch *a) { return a->pak ? a->song != 0 : a->virt != 0; }
static int64_t arch_size(const arch *a) { return a->pak ? sb_feedsong_size(a->song) : sb_virt_size(a->virt); }

static void arch_build(arch *a) {
  if (a->pak) return;
  int fd = open(a->real, O_RDONLY | O_CLOEXEC);
  if (fd >= 0) {
    a->virt = sb_virt_build(a->arc, fd, own);
    close(fd);
  }
  a->virt_failed = !a->virt;
}

// --- the folder --------------------------------------------------------------------------------------------------
typedef struct {
  arch **a;
  int n, cap;
} alist;

static void walk(const char *dir, int depth, alist *out) {
  DIR *d = opendir(dir);
  if (!d) return;
  for (struct dirent *e; (e = readdir(d));) {
    if (e->d_name[0] == '.') continue;
    int plat = platform_of(e->d_name), pak = pak_name(e->d_name);
    if (plat < 0 && !pak && e->d_type == DT_REG) continue;
    char path[PATH_MAX];
    struct stat st;
    if (snprintf(path, sizeof path, "%s/%s", dir, e->d_name) >= (int)sizeof path || stat(path, &st)) continue;
    if (S_ISDIR(st.st_mode) && !pak) {
      if (depth < MAX_DEPTH) walk(path, depth + 1, out);
      continue;
    }
    if (pak ? !S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode) : plat < 0 || !S_ISREG(st.st_mode)) continue;
    if (pak && S_ISDIR(st.st_mode)) {  // a package that is a folder changes when its manifest does
      char mf[PATH_MAX];
      struct stat ms;
      if (snprintf(mf, sizeof mf, "%s/manifest.yaml", path) < (int)sizeof mf && !stat(mf, &ms)) st.st_size = ms.st_size, st.st_mtimespec = ms.st_mtimespec;
    }
    if (out->n == out->cap) {
      arch **g = realloc(out->a, (size_t)(out->cap = out->cap ? out->cap * 2 : 256) * sizeof *g);
      if (!g) break;
      out->a = g;
    }
    arch *a = calloc(1, sizeof *a);
    if (!a) break;
    a->real = strdup(path), a->lreal = lower(path);
    a->size = st.st_size, a->mtime = st.st_mtimespec.tv_sec;
    a->pak = pak;
    a->foreign = pak || plat != own;
    if (a->foreign && a->real) {
      size_t n = strlen(path);
      // x_p.psarc is served as x_m.psarc, x.feedpak as x_m.psarc (".feedpak" and "_m.psarc" are as long)
      memcpy(path + n - SFX_LEN, sfx[own], SFX_LEN);
      a->lvpath = lower(path);
      a->vname = strdup(strrchr(path, '/') + 1);
    }
    if (!a->real || !a->lreal || (a->foreign && (!a->lvpath || !a->vname))) {
      arch_free(a);
      break;
    }
    out->a[out->n++] = a;
  }
  closedir(d);
}

static int by_lreal(const void *x, const void *y) { return strcmp((*(arch *const *)x)->lreal, (*(arch *const *)y)->lreal); }
static int by_lvpath(const void *x, const void *y) { return strcmp((*(arch *const *)x)->lvpath, (*(arch *const *)y)->lvpath); }
static int by_str(const void *x, const void *y) { return strcmp(*(char *const *)x, *(char *const *)y); }

static arch *find_real(const root *r, const char *lreal) {
  int lo = 0, hi = r->n - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2, c = strcmp(lreal, r->a[mid]->lreal);
    if (!c) return r->a[mid];
    if (c < 0) hi = mid - 1;
    else lo = mid + 1;
  }
  return 0;
}
static arch *find_shown(const root *r, const char *lvpath) {
  int lo = 0, hi = r->nshown - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2, c = strcmp(lvpath, r->shown[mid]->lvpath);
    if (!c) return r->shown[mid];
    if (c < 0) hi = mid - 1;
    else lo = mid + 1;
  }
  return 0;
}

static void scan(root *r) {
  alist fl = {0};
  walk(r->path, 0, &fl);
  if (fl.n) qsort(fl.a, (size_t)fl.n, sizeof *fl.a, by_lreal);
  // a file as it was keeps what is known of it
  for (int i = 0; i < fl.n; i++) {
    arch *old = find_real(r, fl.a[i]->lreal);
    if (old && !old->keep && old->size == fl.a[i]->size && old->mtime == fl.a[i]->mtime && !strcmp(old->real, fl.a[i]->real)) {
      arch_free(fl.a[i]);
      fl.a[i] = old;
      old->keep = 1;
    }
  }
  for (int i = 0; i < r->n; i++)
    if (!r->a[i]->keep) arch_retire(r->a[i]);
  free(r->a);
  free(r->shown);
  r->a = fl.a, r->n = fl.n, r->shown = 0, r->nshown = 0;

  int nforeign = 0, nvalid = 0, ndup = 0, ntaken = 0, npak = 0;
  // archives before packages: of an archive and a package with one song, the archive is the one shown
  for (int pass = 0; pass < 2; pass++)
    for (int i = 0; i < r->n; i++) {
      arch *a = r->a[i];
      if (a->pak != pass) continue;
      a->keep = 0, a->shown = 0;
      if (!a->foreign) continue;
      nforeign++;
      if (!a->parsed) arch_parse(a);
      nvalid += a->valid;
    }
  if (nvalid) {
    // the songs this edition's own archives have
    char **keys = 0;
    size_t nkeys = 0, cap = 0;
    int oom = 0;
    for (int i = 0; i < r->n; i++) {
      arch *a = r->a[i];
      if (a->foreign) continue;
      if (!a->parsed) arch_parse(a);
      for (int k = 0; k < a->nkeys; k++) {
        if (nkeys == cap) {
          char **g = realloc(keys, (cap = cap ? cap * 2 : 1024) * sizeof *g);
          if (!g) {
            oom = 1;
            break;
          }
          keys = g;
        }
        keys[nkeys++] = a->keys[k];
      }
    }
    size_t nown = nkeys;
    if (nown) qsort(keys, nown, sizeof *keys, by_str);
    r->shown = calloc((size_t)nvalid, sizeof *r->shown);
    for (int k = 0; k < 2 * r->n && r->shown && !oom; k++) {
      arch *a = r->a[k % r->n];
      if (a->pak != k / r->n) continue;  // the other platform's archives first, then the packages
      if (!a->foreign || !a->valid || a->virt_failed) continue;
      if (find_real(r, a->lvpath)) {  // this edition's own archive has the name
        ntaken++;
        continue;
      }
      int dup = 0;
      for (int k = 0; k < a->nkeys && !dup; k++) {
        dup = nown && bsearch(&a->keys[k], keys, nown, sizeof *keys, by_str) != 0;
        for (size_t j = nown; j < nkeys && !dup; j++) dup = !strcmp(keys[j], a->keys[k]);  // an earlier shown one's
      }
      if (dup) {
        ndup++;
        continue;
      }
      if (!arch_ready(a)) arch_build(a);
      if (!arch_ready(a)) continue;
      if (nkeys + (size_t)a->nkeys > cap) {
        char **g = realloc(keys, (cap = (nkeys + (size_t)a->nkeys) * 2) * sizeof *g);
        if (!g) break;
        keys = g;
      }
      for (int k = 0; k < a->nkeys; k++) keys[nkeys++] = a->keys[k];
      a->shown = 1;
      npak += a->pak;
      r->shown[r->nshown++] = a;
    }
    free(keys);
    if (r->nshown) qsort(r->shown, (size_t)r->nshown, sizeof *r->shown, by_lvpath);
  }
  static int said_n = -1, said_dup = -1;
  if (verbose() || r->nshown != said_n || ndup + ntaken != said_dup) {
    fprintf(stderr, "[songbridge] %s: %d %s archives and feedpaks of %d served as %s (%d feedpaks); %d left out (the song is there already), %d not served\n",
            r->path, r->nshown, sfx[!own], nforeign, sfx[own], npak, ndup + ntaken, nforeign - nvalid);
    said_n = r->nshown, said_dup = ndup + ntaken;
  }
  r->scanned = 1;
  r->at = now_ms();
}

// The dlc folder of path, walked when it never was or (fresh) when that was a while ago; 0 when path is in none.
// With the lock held.
static root *root_of(const char *path, int is_dir, int fresh) {
  size_t n = root_len(path, is_dir);
  if (!n || n >= PATH_MAX) return 0;
  char buf[PATH_MAX];
  memcpy(buf, path, n);
  buf[n] = 0;
  root *r = 0;
  for (int i = 0; i < nroots && !r; i++)
    if (!strcasecmp(roots[i].path, buf)) r = &roots[i];
  if (!r) {
    struct stat st;
    if (nroots == MAX_ROOTS || stat(buf, &st) || !S_ISDIR(st.st_mode)) return 0;
    r = &roots[nroots];
    *r = (root){.path = strdup(buf), .lpath = lower(buf)};
    if (!r->path || !r->lpath) return 0;
    nroots++;
  }
  if (!r->scanned || (fresh && now_ms() - r->at >= (uint64_t)sb_rescan_ms)) scan(r);
  return r;
}

// The other platform's archive served at path, or 0. With the lock held.
static arch *served(const char *path) {
  if (own < 0 || platform_of(path) != own) return 0;
  root *r = root_of(path, 0, 0);
  if (!r || !r->nshown) return 0;
  char *l = lower(path);
  arch *a = l ? find_shown(r, l) : 0;
  free(l);
  return a;
}

// --- the provider ------------------------------------------------------------------------------------------------
static void *p_open(void *user, const char *path, rc_file_info *info) {
  (void)user;
  if (own < 0 || platform_of(path) != own || !root_len(path, 0)) return 0;
  pthread_mutex_lock(&mu);
  arch *a = served(path);
  handle *h = a ? calloc(1, sizeof *h) : 0;
  if (h) {
    struct stat st;
    h->fd = open(a->real, O_RDONLY | O_CLOEXEC);
    // (a package is read by its own means, and one that is a folder is known by its manifest: only that it is there)
    if (h->fd < 0 || fstat(h->fd, &st) || (!a->pak && (st.st_size != a->size || st.st_mtimespec.tv_sec != a->mtime))) {
      if (h->fd >= 0) close(h->fd);  // gone or changed since it was listed: the next listing sees it
      free(h);
      h = 0;
    } else {
      h->a = a;
      a->handles++;
      *info = (rc_file_info){arch_size(a), a->mtime};
    }
  }
  pthread_mutex_unlock(&mu);
  if (h && verbose()) fprintf(stderr, "[songbridge] open %s (from %s)\n", path, a->real);
  return h;
}

static int64_t p_read(void *user, void *file, void *buf, uint64_t n, int64_t off) {
  (void)user;
  handle *h = file;
  int64_t r = h->a->pak ? sb_feedsong_read(h->a->song, buf, n, off) : sb_virt_read(h->a->virt, h->fd, buf, n, off);
  if (r > 0) {
    h->reads++, h->bytes += (uint64_t)r;
    if ((uint64_t)(off + r) > h->upto) h->upto = (uint64_t)(off + r);
  }
  return r;
}

static void p_close(void *user, void *file) {
  (void)user;
  handle *h = file;
  close(h->fd);
  if (verbose())
    fprintf(stderr, "[songbridge] close %s: %llu reads, %llu bytes, up to %llu\n", h->a->vname, (unsigned long long)h->reads,
            (unsigned long long)h->bytes, (unsigned long long)h->upto);
  pthread_mutex_lock(&mu);
  arch *a = h->a;
  if (--a->handles == 0) {
    if (a->dead) arch_free(a);
    else if (a->pak) sb_feedsong_drop(a->song);
    else sb_virt_drop(a->virt);
  }
  pthread_mutex_unlock(&mu);
  free(h);
}

static int p_stat(void *user, const char *path, rc_file_info *info) {
  (void)user;
  if (own < 0 || platform_of(path) != own || !root_len(path, 0)) return 0;
  pthread_mutex_lock(&mu);
  arch *a = served(path);
  if (a) *info = (rc_file_info){arch_size(a), a->mtime};
  pthread_mutex_unlock(&mu);
  return a != 0;
}

static void p_list(void *user, const char *dir, rc_file_add add, void *ctx) {
  (void)user;
  if (own < 0 || !root_len(dir, 1)) return;
  pthread_mutex_lock(&mu);
  root *r = root_of(dir, 1, 1);
  size_t n = strlen(dir);
  for (int i = 0; r && i < r->nshown; i++) {
    const arch *a = r->shown[i];
    const char *slash = strrchr(a->real, '/');
    if ((size_t)(slash - a->real) == n && !strncasecmp(a->real, dir, n)) add(ctx, a->vname);
  }
  pthread_mutex_unlock(&mu);
}

static int p_hide(void *user, const char *dir, const char *name) {
  (void)user;
  if (own < 0 || platform_of(name) != !own || !root_len(dir, 1)) return 0;
  char path[PATH_MAX];
  if (snprintf(path, sizeof path, "%s/%s", dir, name) >= (int)sizeof path) return 0;
  char *l = lower(path);
  pthread_mutex_lock(&mu);
  root *r = root_of(dir, 1, 0);
  arch *a = r && l ? find_real(r, l) : 0;
  int hide = a && a->foreign && a->valid && !a->virt_failed;
  pthread_mutex_unlock(&mu);
  free(l);
  return hide;
}

// Registers the provider for target ("mac-x64" or "win-x86"): 0, or -1
int sb_init(const char *target) {
  if (own >= 0) return 0;
  int t = target && !strcmp(target, "mac-x64") ? SB_MAC : target && !strcmp(target, "win-x86") ? SB_PC : -1;
  if (t < 0) {
    fprintf(stderr, "[songbridge] off: no archives known for target %s\n", target ? target : "(none)");
    return -1;
  }
  static const rc_file_provider p = {.open = p_open, .read = p_read, .close = p_close, .stat = p_stat, .list = p_list, .hide = p_hide};
  if (rc_file_provide(&p)) return -1;
  own = t;
  fprintf(stderr, "[songbridge] %s: %s archives in dlc are read as %s\n", target, sfx[!own], sfx[own]);
  return 0;
}

#ifndef SB_TEST
__attribute__((constructor)) static void init(void) { sb_init(rc_target()); }
#endif
