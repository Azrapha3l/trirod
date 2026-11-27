#!/bin/sh
# EXIT in GLSL shaders with branches: epilogue + return (tools/test_exit_return.c). CPU only, no game data.
# usage: tools/test_exit_return.sh      (Windows / MSYS2 ucrt64, or any host with clang and shaderc)
set -e
cd "$(dirname "$0")/.."
OUT=build/test_exit_return
mkdir -p "$OUT"
CC=${CC:-clang}
$CC -O2 -w -Iruntime -D_FILE_OFFSET_BITS=64 tools/test_exit_return.c runtime/nvn/maxwell/translate.c \
    -o "$OUT/test_exit_return" -lshaderc_shared
"$OUT/test_exit_return"
