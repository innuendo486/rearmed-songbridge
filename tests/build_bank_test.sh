#!/bin/sh
# tests/build_bank_test.sh [arguments for tests/run_bank_test.py]
# Builds banktest (the bank builder and the preview cutter, with the sanitizers) and runs their tests.
# RS_REARMED: the rs-rearmed checkout whose tools/psarc_extract.py reads the sample archives (as build.sh)
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
rearmed=${RS_REARMED:-$HOME/src/rs-rearmed}
out=$here/build
mkdir -p "$out"
clang -arch arm64 -mmacosx-version-min=11.0 -std=c11 -Wall -Wextra -O1 -g -fsanitize=address,undefined \
  -fno-sanitize-recover=undefined -I"$here/src" "$here/tests/banktest.c" "$here/src/bank.c" "$here/src/wemcut.c" \
  "$here/src/wem.c" -lz -o "$out/banktest"
exec python3 -I "$here/tests/run_bank_test.py" "$out/banktest" --tools "$rearmed/tools" "$@"
