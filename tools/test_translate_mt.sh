#!/bin/sh
# The translator from several threads at once (tools/test_translate_mt.c): every translation equals the
# single-thread one. CPU only, no game data.
# usage: tools/test_translate_mt.sh [programs] [seed] [threads] [rounds]
#   REF=<commit>: build that commit's translate.c instead (shows what the test catches)
set -e
cd "$(dirname "$0")/.."
OUT=build/test_translate_mt
mkdir -p "$OUT"
CC=${CC:-clang}
SRC=runtime/nvn/maxwell/translate.c
if [ -n "$REF" ]; then
  mkdir -p "$OUT/ref"
  git show "$REF:runtime/nvn/maxwell/translate.c" > "$OUT/ref/translate.c"
  git show "$REF:runtime/nvn/maxwell/shader.h" > "$OUT/ref/shader.h"
  SRC="$OUT/ref/translate.c"
fi
$CC -O2 -w -Iruntime -D_FILE_OFFSET_BITS=64 tools/test_translate_mt.c "$SRC" -o "$OUT/test_translate_mt" -lpthread
# 1: a mix of programs; 2: only programs with scaled texel fetches (RSC2), many rounds - where a race shows
"$OUT/test_translate_mt" runtime/nvn/maxwell/translate.c "${1:-3000}" "${2:-1}" "${3:-8}" "${4:-4}"
FETCH_ONLY=1 "$OUT/test_translate_mt" runtime/nvn/maxwell/translate.c 300 "${2:-1}" 16 200
