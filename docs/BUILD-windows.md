# Building EoWRecomp on Windows (x86-64)

Windows is an active target, but game play has not yet been verified on Windows. The steps below build the Setup app
and the runtime link kit. Setup compiles the user's own v1.0.0 game files into a game DLL and links a separate runtime EXE.

Requirements: Windows 10 or later, an x86-64 CPU with AVX2, and a graphics driver with Vulkan 1.3. The game package,
generated C, and build outputs stay in ignored folders in this checkout.

## Toolchain

Install MSYS2 in `[local path omitted]`, open the **UCRT64** shell, update the package database and core (`pacman -Syu`, then
close and reopen the shell and repeat until it reports no more updates), then install:

```sh
pacman -S --needed base-devel unzip zstd mingw-w64-ucrt-x86_64-clang mingw-w64-ucrt-x86_64-lld \
  mingw-w64-ucrt-x86_64-sdl3 mingw-w64-ucrt-x86_64-vulkan-devel mingw-w64-ucrt-x86_64-shaderc \
  mingw-w64-ucrt-x86_64-spirv-tools mingw-w64-ucrt-x86_64-vulkan-memory-allocator \
  mingw-w64-ucrt-x86_64-vulkan-validation-layers
```

From the repository root in the UCRT64 shell, download the pinned Zig compiler and the local Windows runtime
dependencies:

```sh
tools/fetch_zig.sh
tools/windows/fetch_deps.sh
```

The second command places the LGPL FFmpeg libraries under `build/win/deps/ffmpeg`. The compiler and dependencies are
kept under the ignored `build/` directory.

## Python tools

The NSP reader and game-code generator use Python 3.12. From PowerShell at the repository root:

```powershell
uv python install 3.12 --install-dir build/python
$pythonDir = Get-ChildItem build/python -Directory -Filter 'cpython-3.12*' | Select-Object -First 1
$python = Join-Path $pythonDir.FullName 'python.exe'
uv venv --python $python tools/venv
uv pip install --python tools/venv/Scripts/python.exe `
  capstone==5.0.9 unicorn==2.1.4 lz4==4.4.5 cryptography
```

## Build

In the MSYS2 UCRT64 shell, from the repository root:

```sh
make -j8 B=build/win-native DEPS=build/win/deps/ffmpeg ZIG=build/zig/zig.exe
```

This builds `build/win-native/Trirod-Launcher.exe` (no Vulkan), `trbuild.exe`, `Trirod-ShaderPrep.exe`,
`trirod-build.json`, the runtime archive, and the import library used by the game DLL. `trbuild.exe` uses these,
`trgen`, Zig, and the user's extracted ExeFS to produce `extracted/Trirod.dll` and a runtime-only `Trirod.exe` in the
data folder. The usual path is `.\setup\build.ps1`, which also builds `trgen` and stages `build\setup-dev\`
(run `build\setup-dev\Trirod-Launcher.exe --dev`; see docs/RELEASE-WORKFLOW.md). For a source checkout without the
launcher:

```sh
make gen
make -j8 B=build/win-native DEPS=build/win/deps/ffmpeg ZIG=build/zig/zig.exe
```

The Setup app can prepare the game from a user's NSP and `prod.keys`. For manual setup, the extractor accepts the base-game
NSP or program NCA and writes the code to the ignored `extracted/` directory:

```sh
tools/venv/Scripts/python.exe tools/nca_extract.py /path/to/prod.keys /path/to/base-game.nsp extracted \
  --no-romfs --romfs-image
```

This baseline supports the unupdated v1.0.0 game. It does not use the 1.0.2 update.

## Separate Setup app and game output

Build the generator helper and package the native Windows Setup app. The Python environment must contain the pinned
packages listed above, including PyInstaller:

```powershell
tools\venv\Scripts\python.exe -m ensurepip --upgrade
tools\venv\Scripts\python.exe -m pip install pyinstaller==6.22.3
tools\venv\Scripts\python.exe -m PyInstaller --noconfirm --clean --log-level WARN `
  --distpath build/helper --workpath build/pyinstaller tools/trgen.spec
tools\venv\Scripts\python.exe tools/pe_no_cfg.py build/helper/trgen/trgen.exe
```

Then from PowerShell:

```powershell
.\tools\windows\package.ps1
```

This creates `build/win/dist/EoWRecompSetup/EoWRecompSetup.exe` and a `payload/` folder. The payload has the Setup app,
compiler, generator, runtime archive, and import libraries; it does not contain the runtime EXE or game DLL. Run Setup, select the base-game
`.nsp` file, and choose an output folder. Setup looks for `prod.keys` beside the NSP and then in `%USERPROFILE%\.switch`;
if neither location has it, Setup asks you to select the key file. It copies the runtime/compiler payload to the output
and opens `Trirod-Launcher.exe`. The NSP is read from its original location and isn't copied. The launcher extracts local game data,
generates and compiles the game code into `extracted/Trirod.dll`, then links the runtime-only `Trirod.exe` at the
output root. Choose Play in the Setup launcher to start the runtime executable. Keep the packaged `av*.dll`, SDL,
shaderc and other dependency DLLs beside it; Windows loads them from that directory.

On initial setup, the app stores the decrypted RomFS image at `extracted/romfs.bin`, the program modules at
`extracted/exefs/`, the game DLL under `extracted/`, and the runtime executable at the output root. The source NSP and key file are only read
during setup. To rebuild later, run Setup again, leave the game-files folder blank, and choose the same output folder;
it reuses generated C and objects from the saved ExeFS and relinks the runtime executable and game DLL when either build
id is stale. The generated C, object files, and per-module logs are kept in
`extracted/.gamebuild/`. Unchanged module translations and C objects are reused based on their content keys. The game
runtime executable itself reads game data only from the local RomFS image and never runs extraction or game-code compilation.

Older working folders with `romfs.src` are migrated to `romfs.bin` on launch; the old reference is removed only after
the image is fully extracted.
