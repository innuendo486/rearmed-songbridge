#!/bin/sh
# Builds and runs the chart reader's and song file writer's tests: arguments go to tests/run_sng_test.py
# (--tools DIR --samples DIR --feedpak FILE ...; see its head).
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
out=$here/build
mkdir -p "$out"
arch="-arch arm64 -mmacosx-version-min=11.0"
clang $arch -O1 -g -fsanitize=address,undefined -w -DYYJSON_DISABLE_WRITER -DYYJSON_DISABLE_UTILS -c "$here/vendor/yyjson/yyjson.c" \
  -o "$out/sng_yyjson.o"
clang $arch -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -I"$here/src" -I"$here/vendor/yyjson" \
  -DYYJSON_DISABLE_WRITER -DYYJSON_DISABLE_UTILS "$here/tests/sngtest.c" "$here/src/chart.c" "$here/src/sng.c" "$out/sng_yyjson.o" -lz \
  -o "$out/sngtest"
exec python3 -I "$here/tests/run_sng_test.py" "$out/sngtest" "$@"
