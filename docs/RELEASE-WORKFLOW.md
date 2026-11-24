# Trirod release and update workflow

This document describes the Windows install, update, and build flow as implemented, and the maintainer steps for
publishing an update. The progress protocol between the launcher and its tools is in `docs/TOOL-PROTOCOL.md`.

## Application roles and names

| File | Source | Role |
| --- | --- | --- |
| `Trirod-Launcher.exe` | `runtime/launcher/launcher_win.cpp` | Configuration, update check and download, build orchestration, progress display, **Play**. Draws with SDL's 2D renderer (Direct3D 11/12, OpenGL, or software) and ImGui. It does not link Vulkan, shaderc, FFmpeg, or runtime code; the Makefile fails the link if it imports any of them. |
| `trbuild.exe` | `runtime/tools/trbuild.c` + `runtime/gamebuild.c`, `nxpkg.c`, `romfs_build.c` | Headless game builder: `check`, `files` (stage *Preparing game files*), `code` (stages *Translating game code for PC* and *Compiling and linking the generated code*). |
| `trgen.exe` | `tools/gen.py` (PyInstaller, `tools/trgen.spec`) | Code generator run by `trbuild`. Its `--shader-catalog` mode extracts the BNSH records. |
| `Trirod-ShaderPrep.exe` | `runtime/tools/shaderprep.c` linked with the same runtime objects as `Trirod.exe` | Headless shader preparation (stage *Preparing shaders for the graphics device*). It uses the runtime's Vulkan backend without a window. |
| `Trirod.exe` | linked by `trbuild code` from `tools/link-kit/eowruntime.a` into the data folder | The game runtime. It is the only program that renders the game. |
| `extracted/Trirod.dll` | built by `trbuild code` from the player's ExeFS | The game code, built from the player's own files. It is never distributed. |
| `Trirod-Tools-<version>.zip` + `trirod-release.json` | `setup/make_release.ps1` | The release assets. |

Install folder layout (`setup/pack_tools.ps1` stages it; a release zip unpacks to the same layout):

```
Trirod-Launcher.exe  trbuild.exe  Trirod-ShaderPrep.exe  <runtime DLLs>  icon.png  trirod.ico
tools\  trirod-tools.json  trgen\  zig\  link-kit\  gamelib-src\  source\
```

By default the data folder is the install folder. `--data <folder>` moves it elsewhere. It holds `extracted\` (game
files, `Trirod.dll`, `shader_catalog\`, `.gamebuild\`), `Trirod.exe`, `cache\` (SPIR-V, `vk_pipelines.bin`,
`warm\`, `shaderprep.json`), saves, `eowrecomp.ini`, `trirod-launcher.ini` (the chosen game files),
`trirod-install.json` (the last successful build), and `logs\last-build.log`.

## What the launcher decides

The launcher checks `tools\trirod-tools.json` (version, `gamelib_id`, `gamebuild_id`) against the built game:

- **Built for this version:** `extracted\Trirod.dll.id` equals `gamelib_id`, `Trirod.exe.id` equals `gamebuild_id`,
  and `extracted\exefs\main` and `romfs.bin` exist. Then **Play** is shown, with **Recompile** as a secondary action.
  Otherwise the launcher shows **Recompile** and the reason, for example "Trirod.exe was built from another runtime".
- **Game files:** **Choose game folder…** or **Choose .xci / .nsp…** stores the path in `trirod-launcher.ini` and runs
  `trbuild check`. **Recompile** is enabled only when the check passes, or when valid extracted files are already
  present. For a package, `prod.keys` must sit beside it. Its title and build ID are checked during *Preparing game
  files* because they are inside the encrypted package.
- **Updates (release builds):** the launcher sends a GET to `https://api.github.com/repos/<UPDATE_REPO>/releases/latest`.
  It downloads the release's `trirod-release.json` asset and compares its `version` with the installed tools, using
  the order `alpha_<n>` < `beta_<n>` < `v<x.y.z>`. A newer release is marked as requiring a recompile when its
  `build.gamelib_id` or `build.gamebuild_id` differs from the installed tools, or when it sets `required: true`.
  The launcher then offers **Update and recompile**, or **Update** with **Play installed version** when no
  recompile is needed. A release whose `min_launcher` is newer than the running launcher is not installed; the
  launcher shows the release page instead. If the check fails or the build has no `UPDATE_REPO`, the reason is
  shown and everything else keeps working.

## Recompile (and the build after an update)

All stages are visible. Each shows its tool's command line, current activity, and percentage. **Show details**
streams every line of tool output; the same text is written to `logs\last-build.log`.

1. *Updating Trirod tools* (only when updating): the launcher downloads only the `kind: "tools"` asset that the
   manifest names, from `https://github.com/<UPDATE_REPO>/releases/download/...`. It checks the size and SHA-256
   against the manifest, then unpacks the zip with `%SystemRoot%\System32\tar.exe` into
   `updates\<version>\stage`. It also checks that the unpacked `tools\trirod-tools.json` reports the manifest's version.
   It refuses packages whose top level contains player data names (`extracted`, `cache`, `save`, `Trirod.exe`,
   settings, and so on). Each top-level entry is moved aside into `updates\<version>\previous`, then the new entry is
   moved in. If any move fails, everything is moved back. Game files, saves, settings, caches, and the built game are
   never touched.
2. *Preparing game files*: `trbuild files`.
3. *Translating game code for PC* and 4. *Compiling and linking the generated code*: `trbuild code`. Generated C and
   objects in `.gamebuild` are reused when their content keys still match. `Trirod.dll` and `Trirod.exe` are replaced
   only after a successful link, from temporary files.
5. *Preparing shaders for the graphics device*: `Trirod-ShaderPrep` (below). Exit 3 means no Vulkan 1.3 device: the
   stage is shown as skipped. A failure is shown as a warning. Neither fails the build.

After the code stages succeed and the build state matches the tools, the launcher writes `trirod-install.json` and
shows **Play**. **Play** starts `Trirod.exe` with the data folder as its working directory and the install folder first on
`PATH`.

## Shader preparation: what is prepared and what is not

Data available, by kind:

| Data | Source | Portable? | Used for |
| --- | --- | --- | --- |
| Maxwell control and code records, plus the vertex/fragment/compute pairing of each program variation | the game's BNSH archives (`extracted\shader_catalog`, `trgen --shader-catalog`) | Yes: identical on every PC. It is still Nintendo's code, so it is never shipped in a release (the repository's `shader_catalog/` is the exception the user authorized, and `check_package.ps1` refuses it in packages). | Translation to GLSL and SPIR-V |
| Pipeline recipes: code IDs, both `ShaderKey`s, and every `PipeKey` field (vertex formats, targets, blend, depth state) | recorded by the runtime while the game runs (`cache\warm\recipes.bin` + `warm\code\`) | No: tagged with the GPU's vendor/device ID because formats are chosen per device | Complete pipelines |
| GLSL → SPIR-V | `cache\spirv\` (keyed by the GLSL text) | Portable in principle, but local | Skips glslang at run time |
| Driver pipeline cache | `cache\vk_pipelines.bin` | No: specific to device and driver; Vulkan discards incompatible data | Faster `vkCreate*Pipelines` |

`Trirod-ShaderPrep` does the following:

1. It opens the device the game would choose (the `eowrecomp.ini` GPU, otherwise discrete > integrated), with no
   window or swapchain.
2. It rebuilds every recorded recipe through the runtime's warmup path (`vk_warm_start`). These are whole pipelines.
3. It translates every catalog vertex+fragment pair and every compute shader with the runtime translator, then compiles
   each to SPIR-V into the runtime's cache. Compute shaders have only one key, so they match the runtime exactly.
   Graphics pairs use the key of a plain draw: float vertex inputs and colour targets, plus the window-origin and
   depth-mode flags from this PC's recipes or the game's defaults. **No pipelines are created from the catalog**:
   the archives do not contain vertex formats, render-target formats, blend/depth state, or each draw's `ShaderKey`.
   Programs with geometry or tessellation stages are counted and skipped, because the Vulkan backend builds only
   vertex+fragment programs.
4. It saves `vk_pipelines.bin` and the recipes, then writes `cache\shaderprep.json` (counts, device, runtime build
   ID). The launcher shows its summary under **Play**.

Anything not prepared, including any draw whose key or state differs, is still built by the runtime when first drawn,
as before (`exec.c` `compile_wait`).

Measured on 2026-11-23 on the development PC (test GPU, driver [driver details omitted]), in a scratch data folder with a
copy of that install's recipes:

- 967 recorded pipelines (502 programs) built in 178.6 s with an empty driver cache, and in 1.5 s on a second run.
- The catalog `trbuild files` extracts scans the whole RomFS image plus `main`: 313 BNSH containers, 9,763 stages,
  and 28,981 variations (one BNSH block is malformed and listed in the manifest). That gave 7,405 vertex+fragment pairs
  and 37 compute shaders. 7,436 of 7,442 were first translated and compiled to SPIR-V. The other six used `TMML` /
  `TLD4` / `TLD4S`, which the translator now implements (docs/STATUS.md). After that, all 7,442 compile. By comparison,
  the repository's `shader_catalog/` covers only `region_common/shader`: 3,576 stages.
- Of the 2,297 SPIR-V modules the runtime had compiled in that install's earlier game sessions, 1,238 (54%) are
  byte-for-byte identical in GLSL to the full catalog's output, so they no longer need glslang at first use. The
  others differ in key (integer vertex inputs or targets, stencil samplers) or come from shaders that are not in the
  archives. They still need the Maxwell translation at run time. Pipelines for them come only from recorded recipes.
- A complete **Recompile** through the launcher (`--dev`, scratch data folder, already-extracted v1.0.0 files) ran all
  four stages to **Play**. The first run failed to link `Trirod.exe`: the kit had no
  `stdc++.lib`, and `link.args` named it relative to the wrong folder. The Makefile now puts the import library in
  `link-kit/lib`, and `gamebuild.c` resolves `link-kit/` entries against the kit. The second run reused the generated objects
  and finished in about 70 s, including shader preparation.

These measurements are from the tool alone. **Not yet verified in game:** whether a prepared cache reduces
first-seen stutter in a real session.

## Diagnostic console and logs (alpha_2)

`Trirod-Launcher.exe` and `Trirod.exe` are GUI-subsystem programs and open no console window. Normally:

- the launcher writes each build's transcript to `<data>\logs\last-build.log` (and shows it under **Show details**);
- the game writes `eowrecomp.log` next to `Trirod.exe`: errors, warnings, and one-time facts (devices, services,
  modules). Periodic statistics, investigation notes on draws and textures, and repeated calls of stubbed service
  commands are left out. A stubbed command is logged on its first call only.

Diagnostic mode is opt-in, either way:

- `--console` on the command line of either program, or
- `EOWRECOMP_CONSOLE=1` in the environment.

Then the program prints to the console it was started from (a terminal), or opens its own console. The launcher also
prints its build log there and starts `Trirod.exe --console`, which attaches to the same console. In the game, this
also turns on the detailed diagnostics (`rt_verbose`, `runtime/log.c`). `EOWRECOMP_VERBOSE=1` turns on the same
detail without a console, into `eowrecomp.log`. Output that is already redirected to a file or pipe stays there.
The headless tools (`trbuild.exe`, `Trirod-ShaderPrep.exe`) are console programs that the launcher runs with pipes;
their output goes to the build log either way.

`Trirod.exe` loads SDL3, FFmpeg, and shaderc DLLs from the install folder. With the default data folder (the
install folder) they sit beside it. With `--data <other folder>`, start the game through the launcher's **Play**,
which puts the install folder first on `PATH`. Double-clicking that `Trirod.exe` shows Windows' "DLL not found"
error.

## Local development builds (no upload)

1. Edit the source in the checkout.
2. Run `./setup/build.ps1` (or `./setup/dev_rebuild.ps1`). It rebuilds `trgen.exe`, runs
   `make all` (launcher, `trbuild.exe`, `Trirod-ShaderPrep.exe`, link kit, `trirod-build.json`), and stages the
   installation in `build\setup-dev\` with `setup/pack_tools.ps1`. It downloads and uploads nothing.
3. Run `build\setup-dev\Trirod-Launcher.exe --dev` (add `--data <folder>` to keep game data elsewhere). Development
   mode makes no update check and no download. It uses the tools beside the executable, always offers
   **Recompile**, keeps the window open after **Play**, and logs each command line. `--recompile` starts the build
   as soon as the game files pass their check, which is useful for scripted rebuilds.
4. Tools can also be run directly, for example
   `build\setup-dev\trbuild.exe code --data <folder>` or `build\setup-dev\Trirod-ShaderPrep.exe --data <folder>`.

Development builds have no `UPDATE_REPO` and never contact GitHub, even without `--dev`.

## Maintainer release procedure

1. Make and review the source changes. Update `EOWRECOMP_VERSION` in `runtime/ui/launcher.mm`; it is the release tag
   the Makefile, the tools, and the manifest use. The build identifiers change by themselves when the generator,
   headers, or runtime sources change.
2. Build with the update source:
   `./setup/build.ps1 -UpdateRepo <owner>/<repo> -OutputDir build\setup-release`. Since alpha_2, every build makes
   the launcher and `Trirod.exe` GUI programs (no console window), so there is no separate release switch.
3. `./setup/make_release.ps1 -PackageDir build\setup-release -UpdateRepo <owner>/<repo> [-Required]`. It copies only
   the installation's own files into a fresh stage and runs `setup/check_package.ps1`, which refuses game files,
   packages, keys, shader archives or catalog, built game code, and caches. It zips the stage with `tar.exe` and writes
   `build\release\<version>\Trirod-Tools-<version>.zip` and `trirod-release.json`
   (`format: "trirod-release"`, `schema: 1`, `version`, `channel: "windows-x64"`, `min_launcher`, `required`,
   `build.{gamelib_id, gamebuild_id, protocol}`, `assets[{name, kind: "tools", size, sha256}]`). It uploads nothing.
4. Smoke-test the update path from the zip: fresh install, current install with no update, a tools update that
   requires recompilation, a failed download or hash check (corrupt the zip), and a failed recompile. The earlier
   `Trirod.dll`/`Trirod.exe` must stay playable.
5. Publish **both** files in one GitHub Release whose tag is the version. Never publish a manifest that refers to an
   asset the release does not have; the launcher refuses it.
6. Check update detection against the published release, then record the release and results in `docs/STATUS.md`.

A fresh user downloads the same zip from the release page, unpacks it, and starts `Trirod-Launcher.exe`.

## Not implemented yet

- The launcher does not replace a running launcher mid-session in a way that restarts it: when the update replaces
  `Trirod-Launcher.exe`, the new one is used from the next start.
- No release has been published with this format, so the GitHub path (`check_release` / `apply_update`) has only been
  built, not run against a real release.
- Linux and macOS keep the in-process start screen (`runtime/ui/launcher.mm`). This workflow is Windows-only.
