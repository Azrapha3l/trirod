#!/bin/sh
# Variant fallback decisions (tools/test_variant_fallback.c): directed cases, then fuzzed programs translated with
# both keys of every accepted stand-in. CPU only, no game data.
# usage: tools/test_variant_fallback.sh [count] [seed]      (Windows / MSYS2 ucrt64, or any host with a C compiler)
set -e
cd "$(dirname "$0")/.."
OUT=build/test_variant_fallback
mkdir -p "$OUT"
CC=${CC:-clang}
$CC -O2 -w -Iruntime -D_FILE_OFFSET_BITS=64 tools/test_variant_fallback.c runtime/nvn/maxwell/translate.c \
    -o "$OUT/test_variant_fallback"
"$OUT/test_variant_fallback" runtime/nvn/maxwell/translate.c "${1:-20000}" "${2:-1}"
