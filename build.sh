#!/bin/sh
# ./build.sh            build/songbridge.dylib
# ./build.sh --install  also copy it into RS Re-ARMed's mods folders (quit the game first)
# ./build.sh --test     run the tests instead (arguments after it go to tests/run_tests.py)
# ./build.sh --test-varc  the archive assembler's tests (arguments after it go to tests/run_varc_test.py)
# ./build.sh --test-wem   the Ogg to .wem repacker's tests (arguments after it go to tests/run_wem_test.py)
# ./build.sh --test-pak   the feedpak reader's tests (arguments after it go to tests/run_pak_test.py)
# RS_REARMED: the rs-rearmed checkout with tools/ws7/recomp/runtime/rc_file.h
set -eu
here=$(cd "$(dirname "$0")" && pwd)
rearmed=${RS_REARMED:-$HOME/src/rs-rearmed/.claude/worktrees/file-provider}
runtime=$rearmed/tools/ws7/recomp/runtime
[ -f "$runtime/rc_file.h" ] || { echo "set RS_REARMED: no $runtime/rc_file.h" >&2; exit 1; }
out=$here/build
mkdir -p "$out"
cc="clang -arch arm64 -mmacosx-version-min=11.0 -Wall -Wextra -I$here/src -I$runtime"
# the vendored libraries (vendor/README.md), built without warnings: they are not ours to fix
yaml="-DHAVE_CONFIG_H -DYAML_DECLARE_STATIC -I$here/vendor/libyaml"
vendored() {  # $1: more compiler flags; the objects' paths in $vobj
  vobj=
  for f in api reader scanner parser loader; do
    clang -arch arm64 -mmacosx-version-min=11.0 -O2 -w $1 $yaml -c "$here/vendor/libyaml/$f.c" -o "$out/yaml_$f.o"
    vobj="$vobj $out/yaml_$f.o"
  done
}

if [ "${1:-}" = --test-varc ]; then
  shift
  $cc -O1 -g -fsanitize=address,undefined "$here/tests/varctest.c" "$here/src/varc.c" "$here/src/psarc.c" -lz \
    -o "$out/varctest"
  exec python3 -I "$here/tests/run_varc_test.py" "$out/varctest" --tools "$rearmed/tools" "$@"
fi

if [ "${1:-}" = --test-pak ]; then
  shift
  vendored "-g -fsanitize=address,undefined"
  $cc $yaml -O1 -g -fsanitize=address,undefined "$here/tests/paktest.c" "$here/src/feedpak.c" $vobj -lz -o "$out/paktest"
  exec python3 -I "$here/tests/run_pak_test.py" "$out/paktest" "$@"
fi

if [ "${1:-}" = --test-wem ]; then
  shift
  $cc -O1 -g -fsanitize=address,undefined "$here/tests/wemtest.c" "$here/src/wem.c" -lz -o "$out/wemtest"
  exec python3 -I "$here/tests/run_wem_test.py" "$out/wemtest" "$@"
fi

if [ "${1:-}" = --test ]; then
  shift
  $cc -O1 -g -fsanitize=address,undefined -DSB_TEST "$here/tests/sbtest.c" "$here/src/psarc.c" \
    "$here/src/songbridge.c" "$runtime/rc_file.c" -lz -o "$out/sbtest"
  exec python3 -I "$here/tests/run_tests.py" "$out/sbtest" "$@"
fi

$cc -O2 -dynamiclib -Wl,-undefined,dynamic_lookup -install_name @rpath/songbridge.dylib \
  -I"$rearmed/tools/ws7/recomp/overrides" "$here/src/psarc.c" "$here/src/songbridge.c" -lz -o "$out/songbridge.dylib"
codesign --force -s - "$out/songbridge.dylib" 2>/dev/null || true
echo "$out/songbridge.dylib"

if [ "${1:-}" = --install ]; then
  home=${RS_NATIVE_HOME:-"$HOME/Library/Application Support/RS Re-ARMed"}
  if pgrep -xq Rocksmith2014 || pgrep -xq rs2014_lp; then echo "quit the game first" >&2; exit 1; fi
  found=0
  for dir in "$home" "$home/learn-play"; do
    [ -d "$dir/game" ] || continue
    mkdir -p "$dir/mods"
    cp "$out/songbridge.dylib" "$dir/mods/songbridge.dylib.new"
    mv -f "$dir/mods/songbridge.dylib.new" "$dir/mods/songbridge.dylib"
    echo "installed: $dir/mods/songbridge.dylib"
    found=1
  done
  [ $found = 1 ] || { echo "no RS Re-ARMed install at $home" >&2; exit 1; }
fi
