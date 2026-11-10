#!/bin/sh
# tests/build_art_test.sh [arguments for tests/run_art_test.py]
# Builds the album art maker's test program twice (with the sanitizers, and as the mod is built, for the timings and
# the leak check) and runs tests/run_art_test.py with both.
# RS_REARMED: the rs-rearmed checkout whose tools/ read the sample archives (only with --samples)
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
rearmed=${RS_REARMED:-$HOME/src/rs-rearmed}
out=$here/build
mkdir -p "$out"
cc="clang -arch arm64 -mmacosx-version-min=11.0 -std=c11 -Wall -Wextra -I$here/src"
fw="-framework ImageIO -framework CoreGraphics -framework CoreFoundation"
$cc -O1 -g -fsanitize=address,undefined "$here/tests/arttest.c" "$here/src/art.c" $fw -o "$out/arttest"
$cc -O2 "$here/tests/arttest.c" "$here/src/art.c" $fw -o "$out/arttest_plain"
exec python3 -I "$here/tests/run_art_test.py" "$out/arttest" --plain "$out/arttest_plain" --tools "$rearmed/tools" "$@"
