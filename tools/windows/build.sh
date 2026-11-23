#!/bin/bash
# Windows x86-64 build from Linux: Setup plus the runtime/link kit it needs to build the runtime EXE and game DLL.
# Needs build/zig-full (zig 0.17), the trgen helper, and tools/windows/fetch_deps.sh.
set -euo pipefail
cd "$(dirname "$0")/../.."
# Windows .NET SDK installs beside MSYS2; expose it to make when its path is not already on PATH.
[ ! -x "/c/Program Files/dotnet/dotnet.exe" ] || export PATH="/c/Program Files/dotnet:$PATH"
D=build/win/deps
[ -d "$D/ucrt64" ] || tools/windows/fetch_deps.sh
P=$PWD/build/win/prefix  # one prefix for the Makefile's DEPS: MSYS2 UCRT64 + FFmpeg
rm -rf "$P" && mkdir -p "$P/include" "$P/lib"
for x in "$D"/ucrt64/include/* "$D"/ffmpeg/include/*; do ln -sfn "$PWD/$x" "$P/include/"; done
[ -e "$P/include/vma/vk_mem_alloc.h" ] || { echo "no vk_mem_alloc.h in the deps"; exit 1; }
# import libraries only (the DLLs ship next to the exe), named <name>.lib: zig's linker doesn't look for lib<name>.dll.a
for x in "$D"/ucrt64/lib/lib*.dll.a; do n=$(basename "$x" .dll.a); ln -sf "$PWD/$x" "$P/lib/${n#lib}.lib"; done
for x in "$D"/ffmpeg/lib/*.lib; do ln -sf "$PWD/$x" "$P/lib/"; done
make -j"$(nproc)" WINCROSS=1 B=build/win DEPS="$P" "$@"
# The setup application runs this standalone generator on the player's machine. Rebuild it from the current lifter
# and generator sources before packaging; reusing an older helper can stamp stale generated code with a new build ID.
GENPY=build/gen-venv/Scripts/python.exe
[ -x "$GENPY" ] || GENPY=tools/venv/Scripts/python.exe
if [ -x "$GENPY" ]; then
    "$GENPY" -m PyInstaller --noconfirm --clean --log-level WARN --distpath build/helper --workpath build/pyinstaller \
        tools/trgen.spec
    "$GENPY" tools/pe_no_cfg.py build/helper/trgen/trgen.exe
else
    make WINCROSS=1 B=build/win DEPS="$P" gen-helper
fi
OUT=build/win/dist/EoWRecompSetup
rm -rf "$OUT" && mkdir -p "$OUT"
cp build/win/Trirod-Launcher.exe build/win/trbuild.exe build/win/Trirod-ShaderPrep.exe "$OUT/"  # (the launcher runs the other two)
cp -r build/win/link-kit "$OUT/"
mkdir -p "$OUT/link-kit/lib"
cp "$P"/lib/* "$OUT/link-kit/lib/"
cp -r build/zig-full "$OUT/zig"
cp -r build/helper/trgen "$OUT/"
mkdir -p "$OUT/gamelib-src"
cp runtime/cpu.h runtime/simd.h runtime/simd_fp.h runtime/gamelib.h runtime/modules.h tools/gamelib_entry.c runtime/hle/hle_symbols.txt "$OUT/gamelib-src/"
for dll in SDL3 libshaderc_shared libSPIRV-Tools libSPIRV-Tools-opt glslang SPIRV libglslang-default-resource-limits \
           libstdc++-6 libgcc_s_seh-1 libwinpthread-1 libiconv-2 libcharset-1; do
    [ -f "$D/ucrt64/bin/$dll.dll" ] && cp "$D/ucrt64/bin/$dll.dll" "$OUT/"
done
cp "$D"/ffmpeg/bin/*.dll "$OUT/"
cp third_party/fonts/Baloo2/Baloo2-SemiBold.ttf third_party/fonts/Baloo2/OFL.txt README.md LICENSE "$OUT/"
echo "$OUT: $(du -sh "$OUT" | cut -f1)"
