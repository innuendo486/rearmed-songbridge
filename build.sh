#!/bin/sh
# ./build.sh            build/songbridge.dylib
# ./build.sh --install  also copy it into RS Re-ARMed's mods folders (quit the game first)
# ./build.sh --test     run the tests instead (arguments after it go to tests/run_tests.py)
# RS_REARMED: the rs-rearmed checkout with tools/ws7/recomp/runtime/rc_file.h
set -eu
here=$(cd "$(dirname "$0")" && pwd)
rearmed=${RS_REARMED:-$HOME/src/rs-rearmed/.claude/worktrees/file-provider}
runtime=$rearmed/tools/ws7/recomp/runtime
[ -f "$runtime/rc_file.h" ] || { echo "set RS_REARMED: no $runtime/rc_file.h" >&2; exit 1; }
out=$here/build
mkdir -p "$out"
cc="clang -arch arm64 -mmacosx-version-min=11.0 -Wall -Wextra -I$here/src -I$runtime"

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
