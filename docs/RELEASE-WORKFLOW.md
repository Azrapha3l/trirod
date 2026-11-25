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
       shader_catalog\warm\recipes.bin  shader_catalog\warm\code\
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
  recompile is needed. Every update replaces the launcher as well and restarts into it (below), so a release whose
  `min_launcher` is newer than the running launcher is installed like any other ("includes a new launcher: it
  restarts"). Only a release whose `min_updater` is newer than the running launcher is refused, with its release page
  shown: set that when the package or update format changes in a way older self-updates can't apply. Launchers
  built before the self-update (v0.1.0) still refuse any release whose `min_launcher` is newer than themselves:
  their users install the first self-updating release by hand. If the check fails or the build has no
  `UPDATE_REPO`, the reason is shown and everything else keeps working.

## Recompile (and the build after an update)

All stages are visible. Each shows its tool's command line, current activity, and percentage. **Show details**
streams every line of tool output; the same text is written to `logs\last-build.log`.

1. *Updating Trirod tools* (only when updating). The package replaces the launcher too, and a running executable
   can't be replaced, so the **new** launcher installs it after the old one has closed
   (`runtime/launcher/launcher_win.cpp`, "self-update"):
   - The running launcher downloads only the `kind: "tools"` asset that the manifest names, from
     `https://github.com/<UPDATE_REPO>/releases/download/...`, checks its size and SHA-256 against the manifest, and
     unpacks it with `%SystemRoot%\System32\tar.exe` into `updates\<version>\stage`. It checks that
     `tools\trirod-tools.json` reports the manifest's version, that the top level holds no player data names
     (`extracted`, `cache`, `save`, `Trirod.exe`, settings, and so on), that `Trirod-Launcher.exe`, `trbuild.exe`,
     `Trirod-ShaderPrep.exe`, `SDL3.dll` and `tools\trirod-tools.json` are there, and that the staged
     `Trirod-Launcher.exe --version` starts and reports the release's version. It writes `updates\<version>\update.json`
     (installation and data folders, `--dev` / `--console`, whether a recompile follows).
   - It starts `stage\Trirod-Launcher.exe --apply-update <dir> --wait-pid <its pid>` and closes once that signals it
     has started. If any check fails or no signal comes within 30 s, the update fails with the reason and the
     installed version is unchanged and still open.
   - The staged launcher waits for the old process to end, then moves each installed top-level entry aside into
     `updates\<version>\previous` and the package's entry in. It starts the installed (new) launcher with the same
     `--data`, `--dev` and `--console`, plus `--updated <version>` and `--recompile` when the release needs a recompile,
     and waits up to 60 s for it to start drawing. Then it removes older `updates\` folders and exits.
   - If a move fails (a file in use) or the new launcher doesn't start, everything is moved back and the previous
     launcher is started with `--update-error <reason>`, which it shows. Each step is logged in
     `updates\<version>\apply.log`.
   - Only one launcher runs per installation and data folder; a second shows "already running" and exits. The restarted
     launcher waits for the closing one. After the restart, the installed version equals the release's, so the update
     isn't offered again.
   - Game files, saves, settings, caches and the built game are never touched. The previous version stays in
     `updates\<version>\previous` until the next update.
   - `--update-from <folder>` reads a release from disk (its `trirod-release.json` and the zip it names) instead of
     GitHub, also in development mode, with the same checks; `--update` starts the update as soon as the check finds a
     newer release. `tools/test_self_update.ps1` uses both for its end-to-end test.
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
| Maxwell control and code records, plus the vertex/fragment/compute pairing of each program variation | the game's BNSH archives (`extracted\shader_catalog`, `trgen --shader-catalog`) | Prepared from each player's game files. The source tree has an authorized shader catalog; releases ship only the curated runtime warm recipes described below. | Translation to GLSL and SPIR-V |
| Pipeline recipes and referenced Maxwell code | gameplay captures exported to the tracked `shader_catalog\warm\` directory | Portable recipe schema; each device validates formats and rebuilds compatible pipelines locally | Complete pipelines for previously observed draw states |
| Additional local recipes | recorded by the runtime while each player runs the game (`cache\warm\recipes.bin` + `warm\code\`) | Tagged with that player's GPU; retained in the player's data folder | Newly discovered local variants |
| GLSL → SPIR-V | `cache\spirv\` (keyed by the GLSL text) | Portable in principle, but local | Skips glslang at run time |
| Driver pipeline cache | `cache\vk_pipelines.bin` | No: specific to device and driver; Vulkan discards incompatible data | Faster `vkCreate*Pipelines` |

The tracked `shader_catalog\warm\` is the shared exception: its schema-2 recipe header uses wildcard GPU IDs. Each
device validates the recorded Vulkan formats and rebuilds compatible recipes with its own driver. The game continues
recording additional recipes into the player's local cache, tagged for that GPU. ShaderPrep merges compatible shared
recipes with local ones, copies their verified guest code into the player's data folder, and saves the resulting local
recipe set and driver cache. The source catalog does not include the `glsl` diagnostic folder or the large local
`spirv` cache: ShaderPrep regenerates SPIR-V from the shared recipe code and the player's extracted game shader catalog.

`Trirod-ShaderPrep` does the following:

1. It opens the device the game would choose (the `eowrecomp.ini` GPU, otherwise discrete > integrated), with no
   window or swapchain.
2. It loads local recipes and the packaged schema-2 warm catalog through the runtime's warmup path (`vk_warm_start`).
   It checks shader-code hashes and each device's Vulkan format support, copies verified shared code into the player's
   `cache\warm\code\`, and rebuilds compatible whole pipelines with the local driver. Shared recipes are merged into
   the player's device-tagged local recipe file for future game starts.
3. It translates every game-catalog vertex+fragment pair and every compute shader with the runtime translator, then compiles
   each to SPIR-V into the runtime's cache. When the selected Vulkan device supports `VK_EXT_graphics_pipeline_library`,
   ShaderPrep also creates reusable vertex/pre-raster and fragment-shader libraries for each graphics pair. At runtime,
   Trirod links those with small vertex-input and render-target libraries, then creates an optimized replacement in the
   background. Compute shaders have one key and match the runtime exactly. Graphics pairs use a plain-draw key; a draw
   whose `ShaderKey` differs still translates on demand. The catalog libraries do not encode vertex formats, target
   formats, blend/depth state, or every draw's key, so complete draw pipelines are still formed from runtime or captured
   pipeline recipes. Devices without GPL or without a driver fast-linking guarantee use the existing monolithic path.
   Programs with geometry or tessellation stages
   are counted and skipped, because the Vulkan backend builds only vertex+fragment programs.
4. With graphics pipeline libraries, each recipe pipeline's optimized replacement (the complete pipeline) is built in the
   background while the catalog runs. ShaderPrep waits for those jobs, with progress, before it saves.
5. It saves `vk_pipelines.bin`, its tag `vk_pipelines.tag` (pipeline scheme, `prepared 1`, vendor, device, driver,
   GPU name) and the recipes, then writes `cache\shaderprep.json` (counts, device, runtime build ID). The launcher shows
   its summary under the step.

**Progress.** The step shows one percentage over every unit of work, counted only when it has finished, plus a line
for the current phase with completed/total counts:
- "Recorded pipelines": programs prepared and complete pipelines created.
- "Game shader catalog": shaders translated to SPIR-V and shader-stage libraries created.
- "Optimizing pipelines for this graphics card": the background replacements.

The `@@info` lines afterwards say what was built and what was skipped, and why. Complete pipelines come only from
recipes, because the shader archives have no vertex formats, render targets or blend state.

**Using the cache.** At start, `Trirod.exe` uses `vk_pipelines.bin` only when all of these hold:
- the driver header's vendor, device and `pipelineCacheUUID` match the selected GPU (the UUID changes with driver
  updates);
- the tag's pipeline scheme matches (scheme 3; caches written before 2026-11-24 have no tag and are not used).

Otherwise it starts an empty cache and writes the reason to `cache\shaders-stale.txt`; it does the same when the cache
was built by the game rather than prepared by Setup. The launcher shows that reason under **Play**. **Prepare shaders**
(or `Trirod-Launcher.exe --prepare-shaders`) runs only `Trirod-ShaderPrep` for the already-built game and clears the
notice.

**After a GPU or driver change.** If Setup prepared shaders on this PC before (`cache\shaderprep.json` exists) but
the cache doesn't match the selected GPU, driver or scheme, `Trirod.exe` runs `Trirod-ShaderPrep` itself before the
game starts. It looks for the tool on `PATH` (Play puts the install folder first), then beside the game, and accepts
only a copy whose `--version` runtime ID matches `Trirod.exe.id`, so a stale copy in the data folder is not used:
- It shows a progress panel with the reason, phase counts, percentage and elapsed time, then merges the result into
  its pipeline cache.
- Esc, B or Back skips the preparation, and the game builds shaders as it meets them.
- It runs once per device and driver (`cache\prep-attempt.txt`). `EOWRECOMP_PREPARE_SHADERS=0` never runs it;
  `=1` also repeats it.
- Output goes to `logs\shaderprep-game.log`.
- A data folder keeps one cache: switching back to the first GPU prepares again.

**Shader-stage libraries and optimized pipelines (GPL).**
- A draw's first pipeline is fast-linked from the program's two shader-stage libraries plus small vertex-input and
  output libraries.
- The optimized replacement is the complete pipeline for the same state, built on a low-priority worker (as in
  DXVK).
- Libraries are never linked with link-time optimization: that needs RETAIN info. Without it the link is invalid and
  drew nothing on the tested GPU; with it the cache grows.
- Cost measured on the test system: the catalog's 7405 library pairs take about 1.13 GB of `vk_pipelines.bin`, and game
  start to the title was 2–6 s slower with that cache.

**In the game: prepared pipelines are built at boot.** Setup's preparation fills only the driver's cache. Each
pipeline still needs its program translated by the runtime (one serialized translator thread) and
`vkCreateGraphicsPipelines` called before a draw can use it.

So before the game starts, `Trirod.exe` builds every recorded recipe from `cache\warm` on the shader threads, as TOTK
does with pipeline keys seen in play and Triforce with its precompile at start.
- With a prepared cache, 1133 pipelines took 1.7 s on the test system, every one from the driver cache.
- A progress panel appears if it takes longer than 0.3 s; Esc or B starts the game while the rest keeps building.
- `EOWRECOMP_VK_WARM_WAIT=<s>` sets the longest wait (default 180 s); `EOWRECOMP_VK_WARM=0` turns recipes off.
- Optimized replacements of fast-linked pipelines run on an idle tier, so they never delay this build or any draw.

**When a draw meets something still building:**
- A draw into a target that nothing drew into last frame (a one-shot pass) waits for its build, within the
  per-frame budget, instead of being skipped. This is TOTK's rule.
- A compute dispatch waits up to 250 ms.
- Any other skipped draw makes the frame held: the window keeps the last complete image.

Each session logs `[vk] session pipelines: ...` at exit (and every 30 s with verbose output on). It gives:
- how many were prepared from recipes;
- first draws that reused a ready or still-building pipeline, and first draws that missed (new);
- programs reused and new;
- driver cache hits, compiles and fast links (from `VK_PIPELINE_CREATION_FEEDBACK`).

Misses are recorded as recipes in the player's `cache\warm`, so the next preparation and boot include them. To share
them with all players, export them with `tools\export_shader_warm.py` (below).

Anything not prepared, including any draw whose key or state differs, is still built by the runtime when first drawn,
on the shader threads (`exec.c` `compile_wait`). A frame in which draws were skipped because their program or
pipeline was still building is not shown; the window keeps the last complete image for up to 500 ms in a row
(`EOWRECOMP_VK_HOLD_MS`, `0` = off). After that, frames are shown as they are until a complete one arrives.
Diagnostics:
- `EOWRECOMP_VK_GPL=0`: monolithic pipelines only.
- `EOWRECOMP_VK_GPL_OPTIMIZE=0`: fast-linked pipelines without optimized replacements.
- With `--console` or `EOWRECOMP_VERBOSE=1`, the stats line shows queued shader jobs, and the log shows each held
  stretch.

The runtime does not export captures automatically. After playing a development build, merge its learned recipes into
the tracked catalog from the repository root:

```powershell
python tools\export_shader_warm.py "D:\path\to\Trirod-data\cache"
```

The exporter validates each recipe checksum and referenced shader-code hash, merges with the existing catalog, and
writes only `shader_catalog\warm\recipes.bin` plus its `code\` records. Review and commit those files with the source
update. `setup/pack_tools.ps1` includes only that curated directory under `tools\shader_catalog\warm` in the install;
`setup/check_package.ps1` still rejects driver caches, raw `.spv` files, game archives, and other game data.

Measured on 2026-11-23 on a test system (specific GPU and driver details omitted), in a scratch data folder with a
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

1. Make and review the source changes. If gameplay added shader variants, export them with
   `python tools/export_shader_warm.py "<data folder>\cache"`, review `shader_catalog\warm\`, and include those
   reviewed files in the source commit. Build identifiers change automatically when generator, headers, or runtime
   sources change.
2. Run the single local deployment command, supplying the release version:
   `./setup/deploy.ps1 -Version alpha_3`. It sets `EOWRECOMP_VERSION` in `runtime/ui/launcher.mm`, builds with
   `Azrapha3l/trirod`, and creates the release files. `-MinLauncher` defaults to `alpha_1`, so alpha_1 and every
   later comparable launcher version can install the release. Override it when needed, for example
   `./setup/deploy.ps1 -Version alpha_3 -MinLauncher alpha_2`. Use `-Required` to mark an update as required.
   The script does not upload or publish anything. Since alpha_2, every build makes the launcher and `Trirod.exe`
   GUI programs (no console window), so there is no separate release switch.
3. The script stages only
   the installation's own files into a fresh stage and runs `setup/check_package.ps1`, which refuses game files,
   packages, keys, shader archives, uncurated shader data, built game code, and caches. It includes the curated
   `tools\shader_catalog\warm` records, then zips the stage with `tar.exe` and writes
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
