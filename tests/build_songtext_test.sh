#!/bin/sh
# The song texts' tests: tests/build_songtext_test.sh [arguments to tests/run_songtext_test.py]
#   tests/build_songtext_test.sh                   the checks that need no archives
#   tests/build_songtext_test.sh --samples DIR     also against every .psarc in DIR
# RS_REARMED: the rs-rearmed checkout (its tools/psarc_extract.py reads the sample archives), as build.sh
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
rearmed=${RS_REARMED:-$HOME/src/rs-rearmed}
out=$here/build
mkdir -p "$out"
cc="clang -arch arm64 -mmacosx-version-min=11.0 -O1 -g -fsanitize=address,undefined"
# yyjson reads the test's song descriptions and the JSON made; vendored, so without warnings (vendor/README.md)
$cc -w -DYYJSON_DISABLE_WRITER -DYYJSON_DISABLE_UTILS -c "$here/vendor/yyjson/yyjson.c" -o "$out/songtext_yyjson.o"
$cc -Wall -Wextra -I"$here/src" -I"$here/vendor/yyjson" "$here/tests/songtexttest.c" "$here/src/songtext.c" \
  "$out/songtext_yyjson.o" -o "$out/songtexttest"
exec python3 -I "$here/tests/run_songtext_test.py" "$out/songtexttest" --tools "$rearmed/tools" "$@"
