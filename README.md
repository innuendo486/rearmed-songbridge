# rearmed-songbridge

A mod for RS Re-ARMed: each edition reads the other platform's song archives in place. The Mac edition reads
`*_p.psarc`, Learn & Play reads `*_m.psarc`. Nothing is converted or written to disk. A song that is in `dlc` in
both forms is shown once, from the edition's own archive.

Needs an RS Re-ARMed with the file provider interface (`rc_file.h`).

    ./build.sh             # build/songbridge.dylib
    ./build.sh --install   # into the mods folder of each installed edition
    ./build.sh --test      # no game needed

`RS_REARMED` names the rs-rearmed checkout to build against. The tests need sample archives and the offline
converter to compare with (`--samples DIR`, `--converter FILE`; `--all` for every sample).

MIT
