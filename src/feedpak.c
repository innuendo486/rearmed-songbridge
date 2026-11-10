// See feedpak.h. The zip form is read by its central directory (stored and deflated files, zip64 sizes); the folder
// form by the files themselves. The manifest is YAML (libyaml): a mapping whose keys this knows are read, the rest
// left alone, as the format asks of a reader.
#include "feedpak.h"

#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <yaml.h>
#include <zlib.h>

#define MAX_FILE (1u << 30)       // a file of a package larger than this is not read
#define MAX_MANIFEST (4u << 20)
#define MAX_ZIP_ENTRIES 65536u
#define MAX_LIST 256u             // arrangements, stems: more are left out
#define MAX_WARN 32u

typedef struct {
  char *name;
  uint64_t csize, usize, local;  // packed and real size; where its local header is
  uint16_t method, flags;
} zent;

struct sb_pak {
  char *path;
  int fd;  // the zip; -1 for a folder
  zent *e;
  uint32_t n;
  char *prefix;  // a zip made of the package's folder: that folder's name and a '/', before every file's name
  sb_manifest m;
  char *warn[MAX_WARN];  // what was left out or repaired
  uint32_t nwarn;
};

// A line for the package's list of what was left out or repaired (the first MAX_WARN are kept)
static void warn(sb_pak *p, const char *fmt, ...) {
  char line[240];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  if (p->nwarn < MAX_WARN && (p->warn[p->nwarn] = strdup(line))) p->nwarn++;
}

static int fail(char *err, size_t errlen, const char *fmt, ...) {
  if (errlen) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
  }
  return 0;
}

static uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return le16(p) | le16(p + 2) << 16; }
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }

static int pread_all(int fd, void *buf, size_t n, uint64_t off) {
  uint8_t *b = buf;
  while (n) {
    ssize_t r = pread(fd, b, n, (off_t)off);
    if (r <= 0) return 0;
    b += r, n -= (size_t)r, off += (uint64_t)r;
  }
  return 1;
}

// A path as the format allows in a manifest: relative, forward slashes, no "..", no empty part, no ':' or '\'
static int path_ok(const char *s) {
  if (!s || !*s || *s == '/') return 0;
  for (const char *p = s; *p;) {
    const char *q = strchr(p, '/');
    size_t n = q ? (size_t)(q - p) : strlen(p);
    if (!n || (n == 2 && p[0] == '.' && p[1] == '.')) return 0;
    for (size_t i = 0; i < n; i++)
      if (p[i] == ':' || p[i] == '\\' || (unsigned char)p[i] < 0x20) return 0;
    if (!q) break;
    p = q + 1;
  }
  return 1;
}

// --- the zip -----------------------------------------------------------------------------------------------------
static int zip_open(sb_pak *p, uint64_t size, char *err, size_t errlen) {
  // the end record: in the last 64 KiB and a bit
  size_t tail = size < 66000 ? (size_t)size : 66000;
  uint8_t *t = malloc(tail + 1), *cd = 0;
  int ok = 0;
  if (!t || size < 22 || !pread_all(p->fd, t, tail, size - tail)) {
    fail(err, errlen, "not a zip file");
    goto out;
  }
  size_t eo = tail - 22;  // the end record, searched from the back (a comment may follow it)
  for (;; eo--) {
    if (le32(t + eo) == 0x06054b50 && eo + 22 + le16(t + eo + 20) <= tail) break;
    if (!eo) {
      fail(err, errlen, "not a zip file");
      goto out;
    }
  }
  uint64_t count = le16(t + eo + 10), cdsize = le32(t + eo + 12), cdoff = le32(t + eo + 16);
  if (count == 0xffff || cdsize == 0xffffffffu || cdoff == 0xffffffffu) {  // zip64: a locator before the end record
    uint8_t loc[20], rec[56];
    uint64_t end = size - tail + eo;
    if (end < 20 || !pread_all(p->fd, loc, 20, end - 20) || le32(loc) != 0x07064b50 ||
        !pread_all(p->fd, rec, 56, le64(loc + 8)) || le32(rec) != 0x06064b50) {
      fail(err, errlen, "a damaged zip file");
      goto out;
    }
    count = le64(rec + 32), cdsize = le64(rec + 40), cdoff = le64(rec + 48);
  }
  if (count > MAX_ZIP_ENTRIES || cdoff > size || cdsize > size - cdoff || cdsize > (256u << 20)) {
    fail(err, errlen, "a damaged zip file");
    goto out;
  }
  cd = malloc((size_t)cdsize + 1);
  p->e = calloc((size_t)count + 1, sizeof *p->e);
  if (!cd || !p->e || !pread_all(p->fd, cd, (size_t)cdsize, cdoff)) {
    fail(err, errlen, "a damaged zip file");
    goto out;
  }
  size_t o = 0;
  for (uint64_t i = 0; i < count; i++) {
    if (o + 46 > cdsize || le32(cd + o) != 0x02014b50) {
      fail(err, errlen, "a damaged zip file");
      goto out;
    }
    size_t nl = le16(cd + o + 28), xl = le16(cd + o + 30), cl = le16(cd + o + 32);
    if (o + 46 + nl + xl + cl > cdsize) {
      fail(err, errlen, "a damaged zip file");
      goto out;
    }
    zent *e = &p->e[p->n];
    e->flags = (uint16_t)le16(cd + o + 8), e->method = (uint16_t)le16(cd + o + 10);
    e->csize = le32(cd + o + 20), e->usize = le32(cd + o + 24), e->local = le32(cd + o + 42);
    // zip64: the sizes that did not fit are in an extra field, in this order
    for (size_t x = o + 46 + nl; x + 4 <= o + 46 + nl + xl;) {
      size_t id = le16(cd + x), len = le16(cd + x + 2), f = x + 4;
      if (f + len > o + 46 + nl + xl) break;
      if (id == 1) {
        if (e->usize == 0xffffffffu && f + 8 <= x + 4 + len) e->usize = le64(cd + f), f += 8;
        if (e->csize == 0xffffffffu && f + 8 <= x + 4 + len) e->csize = le64(cd + f), f += 8;
        if (e->local == 0xffffffffu && f + 8 <= x + 4 + len) e->local = le64(cd + f);
      }
      x += 4 + len;
    }
    if (nl && !memchr(cd + o + 46, 0, nl) && cd[o + 46 + nl - 1] != '/' && (e->name = malloc(nl + 1))) {
      memcpy(e->name, cd + o + 46, nl);
      e->name[nl] = 0;
      p->n++;
    }
    o += 46 + nl + xl + cl;
  }
  ok = 1;
out:
  free(t);
  free(cd);
  return ok;
}

// The zip's entry for the package's file rel: by its exact name, else by one that differs only in case
static const zent *zip_find(const sb_pak *p, const char *rel) {
  size_t pl = p->prefix ? strlen(p->prefix) : 0;
  for (int pass = 0; pass < 2; pass++)
    for (uint32_t i = 0; i < p->n; i++) {
      const char *name = p->e[i].name;
      if (pl && strncmp(name, p->prefix, pl)) continue;
      if (!(pass ? strcasecmp(name + pl, rel) : strcmp(name + pl, rel))) return &p->e[i];
    }
  return 0;
}

static uint8_t *zip_read(const sb_pak *p, const zent *e, size_t *n) {
  uint8_t h[30], *z = 0, *out = 0;
  if (e->flags & 1 || (e->method != 0 && e->method != 8) || e->usize > MAX_FILE || e->csize > MAX_FILE) return 0;
  if (!pread_all(p->fd, h, 30, e->local) || le32(h) != 0x04034b50) return 0;
  uint64_t data = e->local + 30 + le16(h + 26) + le16(h + 28);
  out = malloc((size_t)e->usize + 1);
  if (!out) return 0;
  if (e->method == 0) {
    if (e->csize != e->usize || !pread_all(p->fd, out, (size_t)e->usize, data)) goto fail;
  } else {
    z = malloc((size_t)e->csize + 1);
    if (!z || !pread_all(p->fd, z, (size_t)e->csize, data)) goto fail;
    z_stream s = {0};
    if (inflateInit2(&s, -15) != Z_OK) goto fail;
    s.next_in = z, s.avail_in = (uInt)e->csize, s.next_out = out, s.avail_out = (uInt)e->usize;
    int r = inflate(&s, Z_FINISH);
    uLong got = s.total_out;
    inflateEnd(&s);
    if (r != Z_STREAM_END || got != e->usize) goto fail;
  }
  free(z);
  out[e->usize] = 0;
  *n = (size_t)e->usize;
  return out;
fail:
  free(z);
  free(out);
  return 0;
}

// --- a file of the package ---------------------------------------------------------------------------------------
int64_t sb_pak_size(const sb_pak *p, const char *rel) {
  if (!path_ok(rel)) return -1;
  if (p->fd >= 0) {
    const zent *e = zip_find(p, rel);
    return e ? (int64_t)e->usize : -1;
  }
  char path[PATH_MAX];
  struct stat st;
  if (snprintf(path, sizeof path, "%s/%s", p->path, rel) >= (int)sizeof path || stat(path, &st) || !S_ISREG(st.st_mode)) return -1;
  return st.st_size;
}

uint8_t *sb_pak_read(const sb_pak *p, const char *rel, size_t *n) {
  if (!path_ok(rel)) return 0;
  if (p->fd >= 0) {
    const zent *e = zip_find(p, rel);
    return e ? zip_read(p, e, n) : 0;
  }
  char path[PATH_MAX];
  struct stat st;
  if (snprintf(path, sizeof path, "%s/%s", p->path, rel) >= (int)sizeof path) return 0;
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  uint8_t *out = 0;
  if (fd >= 0 && !fstat(fd, &st) && S_ISREG(st.st_mode) && (uint64_t)st.st_size <= MAX_FILE && (out = malloc((size_t)st.st_size + 1))) {
    if (pread_all(fd, out, (size_t)st.st_size, 0)) {
      out[st.st_size] = 0;
      *n = (size_t)st.st_size;
    } else {
      free(out);
      out = 0;
    }
  }
  if (fd >= 0) close(fd);
  return out;
}

// --- the manifest ------------------------------------------------------------------------------------------------
// The value of key in the mapping node, or 0 (no such key, node is no mapping, or the value is null)
static yaml_node_t *at(yaml_document_t *d, yaml_node_t *node, const char *key) {
  if (!node || node->type != YAML_MAPPING_NODE) return 0;
  size_t kl = strlen(key);
  for (yaml_node_pair_t *q = node->data.mapping.pairs.start; q < node->data.mapping.pairs.top; q++) {
    yaml_node_t *k = yaml_document_get_node(d, q->key), *v = yaml_document_get_node(d, q->value);
    if (!k || k->type != YAML_SCALAR_NODE || k->data.scalar.length != kl || memcmp(k->data.scalar.value, key, kl)) continue;
    if (v && v->type == YAML_SCALAR_NODE && v->data.scalar.style == YAML_PLAIN_SCALAR_STYLE) {
      const char *s = (const char *)v->data.scalar.value;
      if (!*s || !strcmp(s, "~") || !strcasecmp(s, "null")) return 0;
    }
    return v;
  }
  return 0;
}

static const char *scalar(yaml_node_t *n) {
  return n && n->type == YAML_SCALAR_NODE && !memchr(n->data.scalar.value, 0, n->data.scalar.length) ? (const char *)n->data.scalar.value : 0;
}

// A copy of the key's text, 0 when there is none
static char *text(yaml_document_t *d, yaml_node_t *map, const char *key) {
  const char *s = scalar(at(d, map, key));
  return s && *s ? strdup(s) : 0;
}

static int number(yaml_document_t *d, yaml_node_t *map, const char *key, double *out) {
  const char *s = scalar(at(d, map, key));
  char *end = 0;
  if (!s || !*s) return 0;
  double v = strtod(s, &end);
  if (*end || v != v || v > 1e12 || v < -1e12) return 0;
  *out = v;
  return 1;
}

// A path of the manifest: a copy, or 0 when the key is absent or its path is not one the format allows (it could
// leave the package: that file is left out, with a line saying so)
static char *path_of(sb_pak *p, yaml_document_t *d, yaml_node_t *map, const char *what, const char *key) {
  char *s = text(d, map, key);
  if (s && !path_ok(s)) {
    warn(p, "%s: %s is not a path inside the package: %.80s", what, key, s);
    free(s);
    s = 0;
  }
  return s;
}

static const char *codec_of(const char *file) {
  const char *dot = strrchr(file, '.');
  if (!dot) return 0;
  static const char *const ext[][2] = {{".ogg", "vorbis"}, {".wav", "pcm"}, {".opus", "opus"}, {".mp3", "mp3"}, {".flac", "flac"}};
  for (size_t i = 0; i < sizeof ext / sizeof *ext; i++)
    if (!strcasecmp(dot, ext[i][0])) return ext[i][1];
  return 0;
}

static void manifest_free(sb_manifest *m) {
  free(m->title), free(m->artist), free(m->album), free(m->album_artist), free(m->mbid), free(m->isrc);
  free(m->cover), free(m->preview), free(m->lyrics), free(m->timeline);
  for (uint32_t i = 0; i < m->narr; i++) free(m->arr[i].id), free(m->arr[i].name), free(m->arr[i].file), free(m->arr[i].type);
  for (uint32_t i = 0; i < m->nstems; i++) free(m->stem[i].id), free(m->stem[i].file), free(m->stem[i].codec);
  free(m->arr), free(m->stem);
  memset(m, 0, sizeof *m);
}

// The manifest, taking what it has: a part that is missing or makes no sense is left out or given its default, with
// a line in the package's list. It is no manifest only when it is not YAML, has no arrangement or has no audio.
static int manifest_read(sb_pak *p, const uint8_t *y, size_t n, char *err, size_t errlen) {
  sb_manifest *m = &p->m;
  yaml_parser_t ps;
  yaml_document_t d;
  if (!yaml_parser_initialize(&ps)) return fail(err, errlen, "out of memory");
  yaml_parser_set_input_string(&ps, y, n);
  if (!yaml_parser_load(&ps, &d)) {
    fail(err, errlen, "manifest.yaml: %s (line %zu)", ps.problem ? ps.problem : "not YAML", ps.problem_mark.line + 1);
    yaml_parser_delete(&ps);
    return 0;
  }
  int ok = 0;
  double v;
  yaml_node_t *root = yaml_document_get_root_node(&d), *list;
  if (!root || root->type != YAML_MAPPING_NODE) {
    fail(err, errlen, "manifest.yaml is not a mapping");
    goto out;
  }
  // a song with no title is named by its file, one with no artist has an unknown one
  m->title = text(&d, root, "title"), m->artist = text(&d, root, "artist");
  if (!m->title) {
    const char *base = strrchr(p->path, '/');
    base = base ? base + 1 : p->path;
    const char *dot = strrchr(base, '.');
    size_t len = dot && dot > base ? (size_t)(dot - base) : strlen(base);
    if ((m->title = malloc(len + 1))) memcpy(m->title, base, len), m->title[len] = 0;
    warn(p, "no title: named by its file");
  }
  if (!m->artist) {
    m->artist = strdup("Unknown Artist");
    warn(p, "no artist");
  }
  if (!m->title || !m->artist) {
    fail(err, errlen, "out of memory");
    goto out;
  }
  m->album = text(&d, root, "album"), m->album_artist = text(&d, root, "album_artist");
  m->mbid = text(&d, root, "mbid"), m->isrc = text(&d, root, "isrc");
  if (number(&d, root, "year", &v) && v > 0 && v < 100000) m->year = (int)v;
  if (number(&d, root, "duration", &v) && v > 0) m->duration = v;
  m->cover = path_of(p, &d, root, "the manifest", "cover"), m->preview = path_of(p, &d, root, "the manifest", "preview");
  m->lyrics = path_of(p, &d, root, "the manifest", "lyrics"), m->timeline = path_of(p, &d, root, "the manifest", "song_timeline");

  list = at(&d, root, "arrangements");
  size_t count = list && list->type == YAML_SEQUENCE_NODE ? (size_t)(list->data.sequence.items.top - list->data.sequence.items.start) : 0;
  if (count > MAX_LIST) {
    warn(p, "%zu arrangements: the first %u are read", count, MAX_LIST);
    count = MAX_LIST;
  }
  if (!count || !(m->arr = calloc(count, sizeof *m->arr))) {
    fail(err, errlen, "the manifest has no arrangements");
    goto out;
  }
  for (size_t i = 0; i < count; i++) {
    yaml_node_t *a = yaml_document_get_node(&d, list->data.sequence.items.start[i]), *tun;
    if (!a || a->type != YAML_MAPPING_NODE) {
      warn(p, "arrangement %zu is not a mapping: left out", i + 1);
      continue;
    }
    sb_pak_arr *r = &m->arr[m->narr];
    char what[96];
    if (!(r->id = text(&d, a, "id"))) {  // an arrangement with no id is known by its place
      char id[32];
      snprintf(id, sizeof id, "arrangement-%zu", i + 1);
      r->id = strdup(id);
      warn(p, "arrangement %zu has no id", i + 1);
    }
    r->name = text(&d, a, "name");
    if (!r->name && r->id) r->name = strdup(r->id);
    if (!r->id || !r->name) {
      free(r->id), free(r->name);
      memset(r, 0, sizeof *r);
      continue;
    }
    m->narr++;
    snprintf(what, sizeof what, "arrangement %.60s", r->id);
    r->type = text(&d, a, "type");
    r->file = path_of(p, &d, a, what, "file");
    r->drums = (r->type && !strcasecmp(r->type, "drums")) || at(&d, a, "drum_tab");
    r->vocals = r->type && !strcasecmp(r->type, "vocals");
    if (r->drums || r->vocals) free(r->file), r->file = 0;  // never a fretted chart
    r->strings = 6;
    tun = at(&d, a, "tuning");
    if (tun && tun->type == YAML_SEQUENCE_NODE) {
      size_t k = (size_t)(tun->data.sequence.items.top - tun->data.sequence.items.start);
      int good = k >= 4 && k <= 8, semis[8] = {0};
      for (size_t s = 0; good && s < k; s++) {
        const char *t = scalar(yaml_document_get_node(&d, tun->data.sequence.items.start[s]));
        char *end = 0;
        double x = t && *t ? strtod(t, &end) : 0;  // 0.0 by hand is 0
        good = t && *t && !*end && x >= -48 && x <= 48 && x == (int)x;
        semis[s] = (int)x;
      }
      if (good) {
        r->strings = (int)k;
        memcpy(r->tuning, semis, sizeof semis);
      } else {
        warn(p, "%s: a tuning that is not 4 to 8 semitone steps: read as standard", what);
      }
    } else if (tun) {
      warn(p, "%s: a tuning that is no list: read as standard", what);
    }
    if (number(&d, a, "capo", &v) && v >= 0 && v <= 24) r->capo = (int)v;
    if (number(&d, a, "centOffset", &v) && v >= -2400 && v <= 2400) r->cents = v;
  }
  if (!m->narr) {
    fail(err, errlen, "the manifest has no arrangements");
    goto out;
  }

  list = at(&d, root, "stems");
  count = list && list->type == YAML_SEQUENCE_NODE ? (size_t)(list->data.sequence.items.top - list->data.sequence.items.start) : 0;
  if (count > MAX_LIST) {
    warn(p, "%zu stems: the first %u are read", count, MAX_LIST);
    count = MAX_LIST;
  }
  if (count && !(m->stem = calloc(count, sizeof *m->stem))) count = 0;
  for (size_t i = 0; i < count; i++) {
    yaml_node_t *s = yaml_document_get_node(&d, list->data.sequence.items.start[i]);
    sb_pak_stem *r = &m->stem[m->nstems];
    char what[32];
    snprintf(what, sizeof what, "stem %zu", i + 1);
    r->file = path_of(p, &d, s, what, "file");
    r->id = text(&d, s, "id");
    if (r->file && !r->id) {  // a stem with no id is known by its file's name
      const char *base = strrchr(r->file, '/');
      base = base ? base + 1 : r->file;
      r->id = strdup(base);
      if (r->id && strrchr(r->id, '.')) *strrchr(r->id, '.') = 0;
    }
    if (!r->file || !r->id || !*r->id) {
      if (s) warn(p, "%s has no file: left out", what);
      free(r->id), free(r->file);
      r->id = r->file = 0;
      continue;
    }
    m->nstems++;
    r->codec = text(&d, s, "codec");
    if (r->codec) {
      for (char *c = r->codec; *c; c++) *c = (char)tolower((unsigned char)*c);
    } else if (codec_of(r->file)) {
      r->codec = strdup(codec_of(r->file));
    }
    const char *on = scalar(at(&d, s, "default"));  // a boolean, or by hand "on", "yes", ...
    r->on = on && (!strcasecmp(on, "true") || !strcasecmp(on, "on") || !strcasecmp(on, "yes") || !strcmp(on, "1"));
  }
  if (!m->nstems) {
    fail(err, errlen, "the manifest has no stems");
    goto out;
  }
  ok = 1;
out:
  yaml_document_delete(&d);
  yaml_parser_delete(&ps);
  if (!ok) manifest_free(m);
  return ok;
}

// --- the package -------------------------------------------------------------------------------------------------
const sb_manifest *sb_pak_manifest(const sb_pak *p) { return &p->m; }
const char *sb_pak_warning(const sb_pak *p, uint32_t i) { return i < p->nwarn ? p->warn[i] : 0; }

void sb_pak_close(sb_pak *p) {
  if (!p) return;
  manifest_free(&p->m);
  for (uint32_t i = 0; i < p->n; i++) free(p->e[i].name);
  for (uint32_t i = 0; i < p->nwarn; i++) free(p->warn[i]);
  free(p->e);
  if (p->fd >= 0) close(p->fd);
  free(p->prefix);
  free(p->path);
  free(p);
}

sb_pak *sb_pak_open(const char *path, char *err, size_t errlen) {
  struct stat st;
  if (errlen) err[0] = 0;
  sb_pak *p = calloc(1, sizeof *p);
  if (!p) return 0;
  p->fd = -1;
  if (!(p->path = strdup(path)) || stat(path, &st)) {
    fail(err, errlen, "cannot be read");
    goto bad;
  }
  if (!S_ISDIR(st.st_mode)) {
    if ((p->fd = open(path, O_RDONLY | O_CLOEXEC)) < 0) {
      fail(err, errlen, "cannot be read");
      goto bad;
    }
    if (!zip_open(p, (uint64_t)st.st_size, err, errlen)) goto bad;
    // a zip of the package's folder, not of what is in it: the one manifest a folder down says which folder
    if (!zip_find(p, "manifest.yaml")) {
      const char *in = 0;
      int many = 0;
      for (uint32_t i = 0; i < p->n; i++) {
        const char *name = p->e[i].name, *slash = strchr(name, '/');
        if (slash && !strcasecmp(slash + 1, "manifest.yaml") && strncmp(name, "__MACOSX/", 9)) many += in != 0, in = name;
      }
      if (in && !many && (p->prefix = strdup(in))) {
        strchr(p->prefix, '/')[1] = 0;
        warn(p, "the package is in the zip's folder %s", p->prefix);
      }
    }
  }
  int64_t msize = sb_pak_size(p, "manifest.yaml");
  size_t n = 0;
  uint8_t *y = msize >= 0 && msize <= MAX_MANIFEST ? sb_pak_read(p, "manifest.yaml", &n) : 0;
  if (!y) {
    fail(err, errlen, msize < 0 ? "no manifest.yaml" : "manifest.yaml cannot be read");
    goto bad;
  }
  int ok = manifest_read(p, y, n, err, errlen);
  free(y);
  if (ok) return p;
bad:
  sb_pak_close(p);
  return 0;
}
