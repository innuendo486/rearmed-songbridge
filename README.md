# rearmed-songbridge

A mod for RS Re-ARMed (both editions) that lets the game read song archives it was not made for, in place and
without converting them to new files: a Windows archive (`*_p.psarc`) in the Mac edition, a Mac archive
(`*_m.psarc`) in Learn & Play. Other formats (feedpak) may follow.

**Private. This repository holds the archives' song-file keys. Nothing here goes into rs-rearmed or
rearmed-mods, and nothing key-related is added there for this mod's sake.**

## How it works

RS Re-ARMed has a file provider interface for mods (`rc_file.h`, included by `rc_mod.h`; branch `file-provider`
of rs-rearmed until it is merged): a mod answers for files the game opens for reading, for their size and date,
and adds or hides names in a folder's listing. The game reads a served file as it reads any other.

This mod serves a *virtual native archive* for each foreign one:

| Part of the archive | What the mod does |
|---|---|
| Table of contents and name list | rebuilt in memory with the edition's paths (`songs/bin/macos/` and `generic/`, `audio/mac/` and `windows/`); the table's key is the same on both platforms |
| Aggregate graph | rewritten in memory with the edition's tags (`macos` and `dx9`, the paths above) |
| Song files (SNG) | decrypted with the foreign key and encrypted with the edition's own, when the game reads them; the length does not change |
| Audio, art, manifests, models | served straight from the existing file |

Listing: a foreign archive appears in the game's `dlc` folder under the edition's suffix. When the same song (by
the song key inside, not the file name) exists in both forms, only the native one is shown.

## Facts established so far

- Each game binary holds only its own platform's SNG key (stored byte-pair compressed and inverted); neither
  holds the other's. So the foreign key has to come from here.
- The Mac game reads archives with `fopen`/`fseek`/`ftell`/`fread`, and audio with `fopen`/`fstat`/`fseeko`;
  Learn & Play through the Win32 seam's `ReadFile`. No memory mapping or asynchronous I/O on either.
- A working offline converter of Windows archives to Mac ones exists outside this repository; its steps are the
  ones this mod does in memory.
- Not known: what the game does when two archives carry the same song key (so the mod must not let that happen),
  and whether the game caches anything by an archive's size or date.

## Building

One source, one arm64 dylib for both editions, built against rs-rearmed's runtime headers:

    clang -arch arm64 -dynamiclib -undefined dynamic_lookup -I<rs-rearmed>/tools/ws7/recomp/runtime \
      -I<rs-rearmed>/tools/ws7/recomp/overrides src/*.c -o songbridge.dylib

`rc_target()` says which game it is in (`mac-x64` or `win-x86`).
