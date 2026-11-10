# rearmed-songbridge

A mod for RS Re-ARMed: each edition reads the other platform's song archives in place. The Mac edition reads
`*_p.psarc`, Learn & Play reads `*_m.psarc`. Nothing is converted or written to disk. A song that is in `dlc` in
both forms is shown once, from the edition's own archive.

A custom song (an archive with a `toolkit.version`) is served with the AppID custom songs are made with, 248750, so
the game lists it whatever AppID it carries. Other archives keep theirs.

An edition only looks in its own `dlc` folder. To share one edition's songs with the other, link its folder into
the other's (one way only, or the folders contain each other):

    ln -s "<RS Re-ARMed>/game/dlc" "<RS Re-ARMed>/learn-play/game/dlc/mac-edition"

Needs an RS Re-ARMed with the file provider interface (`rc_file.h`).

    ./build.sh             # build/songbridge.dylib
    ./build.sh --install   # into the mods folder of each installed edition
    ./build.sh --test      # no game needed

`RS_REARMED` names the rs-rearmed checkout to build against. The tests need sample archives and the offline
converter to compare with (`--samples DIR`, `--converter FILE`; `--all` for every sample).

MIT
