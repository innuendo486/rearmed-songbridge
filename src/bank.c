// See bank.h. A song's bank (Wwise 2013, bank version 0x5b), all integers little-endian; a section is a 4-byte tag, a
// u32 size and that many bytes:
//
//   BKHD  u32 version 0x5b, u32 bank id, 20 bytes of 0 (language, feedback, padding: DATA's bytes start at 64)
//   DIDX  one entry: u32 the .wem's file id, u32 its place in DATA (0), u32 its size there
//   DATA  the first bytes of the .wem (the prefetch)
//   HIRC  u32 count 4, then objects as u8 type, u32 size, bytes:
//     2 Sound        u32 id, u32 codec 0x00040001 (Vorbis), u32 stream type 2 (streamed, with a prefetch),
//                    u32 source id, u32 file id (both the .wem's), u32 the prefetch's place in DATA, u32 its size,
//                    u8 0 (not by language) | u8 1, u8 0 (effects: its own, none), u32 bus (0: the parent's; in a
//                    preview's bank 0xf908c29a, FNV-1 of "previews"), u32 parent (the Actor-Mixer), 3 bytes 0 |
//                    properties: u8 count, a u8 id each, a 4-byte value each: 0x00 volume (float, dB), 0x2e = 1,
//                    0x2f = 3 (and in the game's newer banks 0x36) | u8 0 (no ranges) | u8 0 (positioning: the
//                    parent's), 4 bytes 0 (sends) | u8 virtual voice: 1 plays on from the elapsed time, u8 at the
//                    limit the newest is stopped, u8 0, u16 instance limit, u8 0, u8 0, u8 the limit is its own,
//                    u8 0 | 10 bytes 0 (no states, no game parameters)
//     7 Actor-Mixer  u32 id 0x26c77444, u8 0, u8 0, u32 bus 0x9bf0fc29, u32 parent 0, ..., u32 children 1, u32 the
//                    Sound's id. The same 50 bytes in every bank seen, the game's own too: the mixer and its bus are
//                    the game's, so the ids are not ours to choose.
//     3 Action       u32 id, u16 0x0403 (play), u32 the Sound's id, u8 0, u8 0, u8 0 (no properties, no ranges),
//                    u8 4 (fade curve: linear), u32 bank id
//     4 Event        u32 id, u32 count 1, u32 the Action's id
//   STID  u32 1, u32 count 1, u32 bank id, u8 length, the bank's name ("Song_AbbaDanc_Preview": not lower case)
//
// What was seen in 2293 banks (394 from the game's own songs, 1899 made by custom-song tools) and what is done here:
//   ids     The event's is FNV-1 of "play_<key>" or "play_<key>_preview" in all of them. The game's own banks have
//           FNV-1 of the bank's name as the bank id and 30-bit numbers for the .wem, Sound and Action; the tools
//           write random 31-bit numbers for all four. Here: the bank id as the game's own, the others a 32-bit FNV-1
//           folded to 31 bits (no id of 2^31 or more was seen for those, so none is made).
//   media   The game's own Sound says where the prefetch is (0, its size). The tools write three 0 bytes and a random
//           number there, which reads as a place and a size that are junk; the game plays those banks all the same.
//           Here: as the game's own.
//   prefetch  The game's own banks carry the .wem up to a packet's end about 5 s into the audio (23 to 131 KiB). The
//           tools carry the first 51200 bytes (72000 in some previews), or the whole file when it is smaller, cut
//           wherever that falls. All but one of the 2293 cover the .wem's header, seek table and setup packet; the
//           one is a 78-minute file whose seek table alone is longer, and whether it plays is not known. Here
//           min(size, 51200): it follows from the file's size alone, which the archive's table of contents needs,
//           and with the seek table wem.c writes (4 bytes for 16384 frames) it covers the setup of anything up to
//           about 70 minutes at 48 kHz.
//   flags   A preview's Sound has: virtual voice 1, stop the newest 1, limit 1 instance, the limit its own. A main one
//           has none of these but, in the game's own banks and the newer tools', virtual voice 1. Here: as those.
//           (The names of these bytes, and of the zeros around them, are Wwise's layout of that version as far as
//           it is known here; the values are what every bank seen has.)
//   0x36    216 of the game's own banks have a fourth property, 0x36, with values like 0x0163ffff that repeat among
//           songs released together. What it is is not known; the other 178 and every tool's bank lack it. Not written.
#include "bank.h"

#include <stdlib.h>
#include <string.h>

#define VERSION 0x5bu
#define VORBIS 0x00040001u
#define MIXER 0x26c77444u
#define MIXER_BUS 0x9bf0fc29u
#define PREVIEW_BUS 0xf908c29au  // FNV-1 of "previews"
#define SOUND_LEN 83u            // with three properties
#define MIXER_LEN 54u
#define ACTION_LEN 18u
#define EVENT_LEN 12u
// everything but the prefetch, the name and a fourth property
#define FIXED (8 + 28 + 8 + 12 + 8 + 8 + 4 + 4 * 5 + SOUND_LEN + MIXER_LEN + ACTION_LEN + EVENT_LEN + 8 + 13)

static uint32_t fnv_add(uint32_t h, const char *s) {
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    h = (h * 16777619u) ^ c;
  }
  return h;
}

uint32_t sb_bank_fnv(const char *name) { return fnv_add(2166136261u, name); }

// FNV-1 of a + key + b, folded to 1..2^31-1
static uint32_t id31(const char *a, const char *key, const char *b) {
  uint32_t h = fnv_add(fnv_add(fnv_add(2166136261u, a), key), b);
  h = (h >> 31) ^ (h & 0x7fffffffu);
  return h ? h : 1;
}

static int key_ok(const char *key) {
  size_t n = key ? strnlen(key, SB_BANK_KEY_MAX + 1) : 0;
  return n >= 1 && n <= SB_BANK_KEY_MAX;
}

uint32_t sb_bank_wem_id(const char *key, int preview) {
  if (!key_ok(key)) return 0;
  uint32_t m = id31("song_", key, ".wem"), p = id31("song_", key, "_preview.wem");
  if (p == m) p = m % 0x7fffffffu + 1;  // another number in the same range
  return preview ? p : m;
}

int sb_bank_ids_of(const char *key, int preview, sb_bank_ids *ids) {
  if (!key_ok(key)) return 0;
  const char *tail = preview ? "_preview" : "";
  ids->bank = fnv_add(fnv_add(fnv_add(2166136261u, "song_"), key), tail);
  ids->event = fnv_add(fnv_add(fnv_add(2166136261u, "play_"), key), tail);
  ids->wem = sb_bank_wem_id(key, preview);
  ids->sound = id31("song_", key, preview ? "_preview.sound" : ".sound");
  ids->action = id31("song_", key, preview ? "_preview.play" : ".play");
  return 1;
}

size_t sb_bank_prefetch(size_t wem_size) { return wem_size < SB_BANK_PREFETCH ? wem_size : SB_BANK_PREFETCH; }

size_t sb_bank_size(const char *key, int preview, size_t prefetch) {
  if (!key_ok(key) || !prefetch || prefetch > 0x7fffffffu) return 0;
  return FIXED + prefetch + 5 + strlen(key) + (preview ? 8 : 0);  // "Song_", "_Preview"
}

static uint8_t *w8(uint8_t *p, uint32_t v) {
  *p = (uint8_t)v;
  return p + 1;
}
static uint8_t *w16(uint8_t *p, uint32_t v) { return w8(w8(p, v), v >> 8); }
static uint8_t *w32(uint8_t *p, uint32_t v) { return w16(w16(p, v), v >> 16); }
static uint8_t *zeros(uint8_t *p, size_t n) {
  memset(p, 0, n);
  return p + n;
}
static uint8_t *section(uint8_t *p, const char *tag, uint32_t n) {
  memcpy(p, tag, 4);
  return w32(p + 4, n);
}
static uint8_t *object(uint8_t *p, unsigned type, uint32_t n) { return w32(w8(p, type), n); }

uint8_t *sb_bank_write(const sb_bank_spec *s, const uint8_t *prefetch, size_t n, size_t *out_n) {
  size_t name = s->name ? strnlen(s->name, 256) : 0;
  if (!name || name > 255 || !prefetch || !n || n > 0x7fffffffu) return 0;
  uint32_t sound = SOUND_LEN + (s->has_prop36 ? 5 : 0), vol;
  size_t total = FIXED + (sound - SOUND_LEN) + n + name;
  uint8_t *out = malloc(total), *p = out;
  if (!out) return 0;
  memcpy(&vol, &s->volume, 4);

  p = section(p, "BKHD", 28);
  p = zeros(w32(w32(p, VERSION), s->ids.bank), 20);
  p = section(p, "DIDX", 12);
  p = w32(w32(w32(p, s->ids.wem), 0), (uint32_t)n);
  p = section(p, "DATA", (uint32_t)n);
  memcpy(p, prefetch, n);
  p += n;

  p = section(p, "HIRC", 4 + 4 * 5 + sound + MIXER_LEN + ACTION_LEN + EVENT_LEN);
  p = w32(p, 4);
  p = object(p, 2, sound);
  p = w32(w32(w32(p, s->ids.sound), VORBIS), 2);
  p = w32(w32(w32(w32(p, s->ids.wem), s->ids.wem), s->media_off), s->media_size);
  p = w8(w8(w8(p, 0), 1), 0);
  p = zeros(w32(w32(p, s->preview ? PREVIEW_BUS : 0), MIXER), 3);
  p = w8(w8(w8(w8(p, s->has_prop36 ? 4 : 3), 0x00), 0x2e), 0x2f);
  if (s->has_prop36) p = w8(p, 0x36);
  p = w32(w32(w32(p, vol), 1), 3);
  if (s->has_prop36) p = w32(p, s->prop36);
  p = zeros(p, 1 + 1 + 4);  // no ranges; the parent's positioning; no sends
  p = w8(w8(w8(p, s->preview || s->from_elapsed ? 1 : 0), s->preview ? 1 : 0), 0);
  p = w16(p, s->preview ? 1 : 0);
  p = w8(w8(w8(w8(p, 0), 0), s->preview ? 1 : 0), 0);
  p = zeros(p, 10);

  p = object(p, 7, MIXER_LEN);
  p = w32(w8(w8(w32(p, MIXER), 0), 0), MIXER_BUS);
  p = zeros(p, 4 + 3 + 1 + 1);     // no parent, no properties, no ranges
  p = zeros(w8(w8(p, 1), 1), 25);  // its own positioning, a panner's; the rest unset
  p = w32(w32(p, 1), s->ids.sound);

  p = object(p, 3, ACTION_LEN);
  p = w32(w16(w32(p, s->ids.action), 0x0403), s->ids.sound);
  p = w32(w8(zeros(p, 3), 4), s->ids.bank);
  p = object(p, 4, EVENT_LEN);
  p = w32(w32(w32(p, s->ids.event), 1), s->ids.action);

  p = section(p, "STID", 13 + (uint32_t)name);
  p = w8(w32(w32(w32(p, 1), 1), s->ids.bank), (uint32_t)name);
  memcpy(p, s->name, name);
  p += name;
  if ((size_t)(p - out) != total) {  // the layout above and FIXED disagree: never
    free(out);
    return 0;
  }
  *out_n = total;
  return out;
}

uint8_t *sb_bank_build(const char *key, int preview, uint32_t wem_id, const uint8_t *prefetch, size_t n, float volume,
                       size_t *out_n) {
  char name[5 + SB_BANK_KEY_MAX + 8 + 1];
  sb_bank_spec s = {.name = name, .preview = preview, .volume = volume, .media_size = (uint32_t)n, .from_elapsed = 1};
  if (!wem_id || !sb_bank_ids_of(key, preview, &s.ids) || n > 0x7fffffffu) return 0;
  s.ids.wem = wem_id;
  memcpy(name, "Song_", 5);
  strcpy(name + 5, key);
  if (preview) strcat(name, "_Preview");
  return sb_bank_write(&s, prefetch, n, out_n);
}
