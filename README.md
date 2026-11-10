# rearmed-songbridge

A mod for RS Re-ARMed: each edition reads the other platform's song archives in place. The Mac edition reads
`*_p.psarc`, Learn & Play reads `*_m.psarc`. Nothing is converted or written to disk. A song that is in `dlc` in
both forms is shown once, from the edition's own archive.

A custom song (an archive with a `toolkit.version`) is served with the AppID custom songs are made with, 248750, so
the game lists it whatever AppID it carries. Other archives keep theirs.

An edition only looks in its own `dlc` folder. To share one edition's songs with the other, link its folder into
the other's (one way only, or the folders contain each other):

    ln -s "<RS Re-ARMed>/game/dlc" "<RS Re-ARMed>/learn-play/game/dlc/mac-edition"

## Install

1. Download `Re-ARMed-Song-Bridge-<version>.zip` from the [Releases](https://github.com/innuendo486/rearmed-songbridge/releases)
   page, open it and move **Re-ARMed Song Bridge.app** to Applications.
2. Quit RS Re-ARMed, open the app and click **Install**. It puts the mod into the mods folder of each edition that is
   installed (`~/Library/Application Support/RS Re-ARMed/mods` and `.../learn-play/mods`); the game loads it at its
   next start. **Update** brings an older one up to date, **Remove** takes it out again; songs are never touched.

The app is not notarized: the first time, open it with a right-click and **Open** (or allow it in System Settings,
Privacy & Security). Needs RS Re-ARMed 1.14.0 or later.

## Feedpaks (experimental)

A [feedpak](https://github.com/got-feedBack/feedpak-spec) in `dlc` or a folder below it (`x.feedpak`, the zip or the
folder form) is listed as a song of the edition and played from the package: its charts become the game's song files,
its audio is repacked into the game's own format without being decoded, its cover becomes the album art, its lyrics
the song's vocals, and tones the package carries in the game's form are used as they are. Nothing of the song is written to disk; the sizes of
what a package turns into are kept in `~/Library/Caches/RS Re-ARMed/songbridge` (`RS_SONGBRIDGE_CACHE` names another
folder), so that only a package's first start takes a moment. That folder can be deleted at any time.

It is experimental: two packages have been played, in both editions. What it does not do yet:

- **More than one lyric track.** The package's primary lyrics are the song's; translations and transliterations are
  left out.
- **Audio the game cannot be given as it is.** Only Ogg Vorbis whose codebooks the game has (what libvorbis writes)
  can be repacked; a package with only Opus, MP3 or another encoder's Vorbis is not served. A package without a full
  mix plays one of its stems.
- **More than six strings, drums, keys, notation.** Arrangements the game has no place for are left out.
- **Tones made outside the game.** Without a tone in the game's own form an arrangement gets a plain default tone.
- **Phrase names.** The package format has none, so every phrase counts as its own: the game's difficulty for one
  chorus does not carry to the next.
- **A song that is also there as an archive** is shown twice: nothing says the two are one song.
- **Two songs whose audio gets the same number** (it is made from the song's name; unlikely, not checked).

What was left out of a package, or put right in it, is said in the game's log (`[songbridge]`). An empty file named
`trace` in the cache folder makes the mod log every open, read and close of what it serves.

## Building

Needs an RS Re-ARMed with the file provider interface (`rc_file.h`).

    ./build.sh             # build/songbridge.dylib
    ./build.sh --install   # into the mods folder of each installed edition
    ./build.sh --test      # no game needed
    app/build-app.sh       # app/build/Re-ARMed Song Bridge.app, the installer, with the mod inside
    app/build-app.sh --zip # and the release download; --test: the installer's tests

`VERSION` is the mod's version: it is in the library, where the installer reads it.

`RS_REARMED` names the rs-rearmed checkout to build against (default `~/src/rs-rearmed`). The tests need sample
archives and the offline converter to compare with (`--samples DIR`, `--converter FILE`; `--all` for every sample).
The parts a feedpak's archive is made of have tests of their own: `--test-pak`, `--test-varc`, `--test-wem`,
`--test-cache`, `--test-feed` (`--pak FILE` for each package to try), and `tests/build_*_test.sh` for the song file
writer, the manifests, the sound banks and the album art. Some need the game's codebook library, which the game
gives at run time and rearmed-mods' stems helper keeps a copy of; without it they say so and skip.

`vendor/` has libyaml and yyjson (MIT; `vendor/README.md`).

MIT
