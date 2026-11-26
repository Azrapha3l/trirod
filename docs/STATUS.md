# Status

Atualizado: 2026-11-24 (GPL black screen found and fixed in the scratch test; cache identity, ShaderPrep progress, in-game preparation after a GPU change; waiting for the user's in-game confirmation).

## First-use shader stalls and per-frame GPU waits (2026-11-25): four changes, tested in game (Vulkan), not committed

From the performance audit of the same day. Measured first (warm cache, 1x, `EOWRECOMP_VK_PROF=1`): 60 fps held,
~2.3 ms of render-thread CPU and ~2.6-4.4 ms of GPU per frame, so the frame is not CPU-bound and low CPU use is
expected (the game steps its world at 60 Hz). The trouble was first use: in a cold run (empty cache, fresh save,
Dynamic, 230 s prologue route with A presses) 34 of 112 two-second windows were under 55 fps, and frames 6980-6995
took ~425 ms each (2.4 fps for ~6 s).

- **1. Wait budget per build** (`compile_wait`): full-screen / one-shot / warmup draws wait up to 400 ms per frame
  for their build; that budget restarted every frame, so one build that stayed in the queue for ~6 s made every
  frame wait 400 ms and skip anyway. Now a build also gets at most 400 ms of waits in total, over all frames; after
  that its draws skip like scene geometry (backend_present still holds the last complete image a while). Counter
  `waits used up N` in the fps line; verbose `[vk] draw not waiting: ...`. `EOWRECOMP_VK_BUILD_WAIT_MS=<ms>` sets the
  per-build budget (0: none, the old behaviour); `EOWRECOMP_VK_BUILD_DELAY_MS=<ms>` (test) makes every program build
  that much slower.
- **2. Translation on several threads.** The translator wasn't re-entrant (translate.c `RSC2` returned its texel
  coordinates in a static 4-entry ring), so exec.c had one translator thread and ShaderPrep a global lock; a waiting
  draw's program sat behind every translation queued before it. The ring is now per translation (`Ctx`), exec.c runs
  `cores / 6` translator threads (2-8; 4 with 28 logical CPUs; `EOWRECOMP_VK_TRANSLATORS=<n>`, 1 = as before), the
  translator's globals are set once (`translator_globals`), and ShaderPrep translates without the lock.
  Test: `tools/test_translate_mt.sh` (programs from test_translate.c translated on one thread, then on 8 / 16 at
  once; every result must equal). Passes; with `REF=f85ef0a` (the old translator) the fetch-only pass had 193 of
  960,000 translations differ. `REF=HEAD tools/test_translate.sh 4000 8 both`: 0 MSL differences, GLSL 0
  compile/validation failures, extent 0 differences.
- **3. Build timings.** The session line (`[vk] session ...`, every 30 s verbose and at exit) adds the time of each
  build step summed over programs: translate queue, translate, compile queue, SPIR-V, stage libraries, and driver time
  by kind (complete pipelines, libraries, fast links). A program build over 250 ms logs `[vk] slow shader build: vs ..
  fs .. took N ms = ...` (first 32). The summary buffers grew to 1024 bytes (the line was cut at 512).
- **4. GPU-stored constant buffers read in place.** A draw binding as a constant buffer memory a compute shader
  stores to in unfinished work flushed and waited for the GPU (`gstore_sync_cbufs`): once a frame in normal play,
  ~0.7 ms. The heap import (`gmem_buf`) now also has uniform-buffer usage (checked against the import's memory type)
  and such a cbuf is bound in place (offset alignment and range limits checked; else the wait as before): the GPU
  reads it after the store, as the Switch does. `gpu-stored cbufs in place N` in the fps line;
  `EOWRECOMP_VK_GSTORE_DIRECT=0` for the old wait.

**Results (RTX 5060, 28 logical CPUs, `D:\Games\EoW` dev install, scratch cache/save/settings):**

| | before | after |
|---|---|---|
| Cold route, 2 s windows under 55 fps | 34 of 112 | 3 of 112 |
| Cold route, lowest window / mean fps | 2.3 / 54.3 | 37.5 / 58.7 |
| Cold route, draws skipped while building | 2,770 | 484 |
| Warm 1x, render-thread CPU per 2 s (`cpu exec`) | 241 ms | 152 ms |
| Warm 1x, GPU waits for GPU-stored cbufs per 2 s | 86 ms (120 waits) | 0 |
| Warm 1x, GPU time per 2 s | 399 ms | 414 ms (+4%: those cbufs are read from host memory) |
| Warm 1x, submissions per frame | 3 | 2 |

- Cold run build times (96 programs): translate queue 58 ms, translate 236 ms, compile queue 196 ms, SPIR-V 1,947 ms,
  stage libraries 621 ms in total - GLSL to SPIR-V (shaderc) is now the largest step; no build went over 250 ms.
- The 6 s build of the old run did not happen again, so its cause is unknown (no timings then); the new log line
  names the shader if it comes back. So change 1 was not exercised by the normal cold run ("waits used up" 0).
- Change 1 on demand: cold 90 s route with `EOWRECOMP_VK_BUILD_DELAY_MS=1500` (every program build 1.5 s slower).
  Per-build budget: mean 51.1 fps, 5 of 41 windows under 20 fps, 3,020 draws skipped without waiting, 10,622 skipped
  in all. `EOWRECOMP_VK_BUILD_WAIT_MS=0` (old rule): mean 41.6 fps, 9 windows under 20 fps, 6,566 skipped. Both still
  start with windows at 2.3 fps (many new builds at once, each with its own 400 ms); screenshots normal in both. The
  slow-build log named each shader and put the delay in the SPIR-V step, as expected.
- ShaderPrep, catalog only (`--no-recipes`, scratch caches, 7,450 pairs + 37 compute): 1,280 s with the old binary,
  1,205 s with the new one (-6%); the same 8,991 SPIR-V files, all 7,450 library pairs created. The translation lock
  was not ShaderPrep's bottleneck (stage-library creation and shaderc are).
- Images: warm A/B (`EOWRECOMP_VK_GSTORE_DIRECT=0` vs default, same inputs): static frames within 0.01% of pixels;
  the fight frames differ by camera/animation timing only (checked side by side). Cold-run contact sheet normal.
- Validation (`EOWRECOMP_VK_VALIDATION=1`, 120 s route): only the known "c0 without a colour attachment" warning.
  The validation layer itself crashed at ~106 s (a C++ exception inside `VkLayer_khronos_validation.dll`, under
  `vkWaitSemaphores` from `nvnSyncWait`); with in-place cbufs off and one translator thread it crashed the same way
  at ~108 s (an access violation in the layer), so it is not from these changes. Not traced.
- Not tested: Metal (unchanged: `metal.m` has its own paths), GPUs other than this one (AMD/Intel minimum UBO
  alignments differ; a misaligned range keeps the wait), `EOWRECOMP_VK_GPL=0`, gameplay past the prologue corridor.
- **Next:** shaderc is now the largest first-use step (SPIR-V ~20 ms a program on average in the cold run); the
  audit's other items (per-pass full barriers, dynamic state 3, DLSS / frame generation) are not started.
- **What the user should test:** play a first visit with an empty `cache` copy (or a new area): fewer long freezes
  when shaders build. Watch the fps line in `--console` for `waits used up` and the log for `[vk] slow shader build`
  (send those lines if a freeze comes back). Effects that read compute results (the void / rift, Hyrule Town trees)
  should look as before; if one looks wrong, compare with `EOWRECOMP_VK_GSTORE_DIRECT=0`.

## Dynamic resolution lattice (2026-11-25): light grid scaled below 1x; fixed (Vulkan), tested in game, not committed

- **Seen:** a dark lattice of tile-sized blocks over walls and floors, with stair-stepped edges and smeared floor
  blocks. It shows from the first cutscene (2-5 s in) and in the torch corridors of the prologue.
- **Cause (reproduced):** any internal resolution below 1x, not the resolution change itself.
  - Dynamic gets there because cold-start frames are slow: 1.00 -> 0.50 within the first seconds, `[dynres]` lines.
  - Match display in a window smaller than 1920x1080 also renders below 1x. In a 1280x720 window it renders at
    0.667x, and the lattice shows at frame 60, before any change. 75% / 50% / 25% should show it too (not run).
  - The game draws light volumes into small 16:9 render targets, 64x36 and 128x72 (fmt 0x25; fs `72a46dd400` /
    `72a46e1400`): a per-tile light grid, one texel per screen tile. `textures.c` `tex_scalable` treats any 16:9
    render target of at least 32x16 as an image and scaled them: 64x36 became 43x24, with viewport 42.67x24
    (`EOWRECOMP_DEBUG_FRAME` draw log).
  - Below one pixel per tile, whole tile rows and columns are never rasterized. The geometry pass then reads missing
    or neighbouring tiles. The per-pass dump shows the lattice already in the geometry pass (pass 67 of frame 120),
    before any post-processing.
  - At 1x and above every tile keeps a pixel: clean at 1x.
- **Not involved** (controlled runs, one condition changed at a time, cold cache, fresh save):
  - Shader builds: the lattice is the same with a warm cache.
  - The resolution change itself: it is there at a constant 0.667x.
  - Validation: no new messages; only the known "FS writes c0 with no color attachment" warning.
  - Separately, a cold cache can still draw plants black for about 1 s while one long translation job is queued: a
    skipped draw after the 500 ms hold. That is a shader-build artifact, unrelated to resolution.
  - The quoted `[tex] new render-target image ... covers the same guest memory` line doesn't exist in this source.
- **Fix:** `textures.c` `target_scale`. Scalable targets narrower than 256 guest pixels are never allocated below
  their guest size; larger targets still follow the scale, so Dynamic keeps its savings. Shaders read every texture
  through its own scale (rscale table) and passes take their viewport scale from their own target, so mixed scales
  are consistent.
  - Metal (`metal.m`) has the same allocation rule and very likely the same bug: not changed, not tested.
- **New diagnostics (verbose only):**
  - `[dynres] factor ...`: what each half-second decision measured.
  - `[vk] render target reallocated for scale ...`: tex, guest size, format, address, old -> new size, frame.
- **Tested** (Vulkan, GPL on; `<test-install>` v1.1.0; scratch `EOWRECOMP_CACHE_DIR`, `EOWRECOMP_SAVE_DIR`,
  `EOWRECOMP_SETTINGS`, so the install's cache, save and settings are untouched; 1280x720 window):

  | Run | Scale | Lattice |
  |---|---|---|
  | before, Dynamic, cold | 0.667 -> 0.333 (factor 1.0 -> 0.5) | yes, from frame 60 |
  | before, Match, cold (CRLF settings run, see below) | 0.667 | yes (torch walls) |
  | before, 1x, cold | 1.0 | no |
  | fix, Dynamic, cold, start + prologue route to the first fight | 28 changes | no |
  | fix, Dynamic, warm (same route's cache) | 0.667 | no |
  | fix, Dynamic, cold, `EOWRECOMP_VK_VALIDATION=1` | changing | no; same messages as before |

  `./setup/build.ps1` passes, with no warnings in the changed files.
- **Not tested:** fullscreen (the user's setup; Dynamic starts from the display size); upward scale steps above 1x with
  non-integer factors (a tile could map to a neighbouring texel: a hypothesis, nothing seen); Metal.
- **CRLF settings files (found while testing, fixed):**
  - On Windows the game opens every file in binary mode (`platform/windows.c` sets `_fmode = _O_BINARY`), so
    `settings_load` kept the `\r` of CRLF lines.
  - With a CRLF file, every value missed its match and silently fell back to its default: `resolution = dynamic`
    became Match display, `frame_rate = 60` the game's 30, and the `gpu` id was dropped. Notepad and PowerShell's
    `Set-Content` write CRLF.
  - Fix: the value now ends at a CR too (`settings.c`); control bindings go through the same line.
  - Test: `tools/test_settings.c` now uses binary mode like the game and loads the round-trip file rewritten with
    CRLF. It passes with the fix and fails on the old parser (the gpu id is lost).
  - In game (`<test-install>`, scratch CRLF settings file with `resolution = dynamic`): `[dynres]` decisions and scale
    changes from the start. The same CRLF file before the fix gave none (it ran at Match display).
  - The runtime's other line readers already strip `\r\n` or stop at whitespace.
- **For the user to check:** fullscreen, Dynamic, a new game from a cold cache. Expected: no lattice in the opening
  cutscene and prologue. With `--console`, the `[dynres]` lines show the scale going below 1.

## Shader variant fallback (2026-11-25): stand-in draws while a variant builds; tested in game on a cold cache, not committed

- **What it does.** A guest program (VS + FS) is translated once per `ShaderKey` pair. The first draw of each new
  variant (other vertex formats, target formats or stencil views) was skipped, or waited, until the translator,
  glslang and the pipeline were done. Now `draw_` (`exec.c`) lets an already built variant of the **same guest code**
  draw in its place when the translator gives both keys identical output. The stand-in's pipeline is built for this
  draw's own state: formats, vertex layout, blend, depth/stencil. The exact variant keeps building, and every draw
  looks it up again, so it takes over at the first draw after it is ready.
  - Program still building: the first ready, compatible sibling draws. If its pipeline for this state doesn't exist,
    it is queued at the front with no recipe and no optimized replacement (a fast link with GPL).
  - Program ready, pipeline still building: a compatible sibling's pipeline for exactly this state is used if it
    is already ready. Nothing is built for this case.
  - Nothing compatible and ready: the previous behaviour (one-shot / full-screen wait, otherwise skip and hold).
- **Compatibility rules** (`runtime/nvn/vulkan/variant_fallback.h` `vf_check`, from every key read in
  `translate.c`):
  - same guest VS and FS code ids and extents (content hashes, not addresses; id 0 = unknown = never);
  - equal `glsl`, `flip_y`, `flip_x`, `depth_minus_one` in both keys;
  - equal `attr_kind` for every attribute the VS reads (its `attr_mask`); unread ones may differ;
  - equal `rt_kind` for every target the FS writes (its `out_mask`; tint only when target 0 is written); unwritten
    ones may differ;
  - equal `stencil_tex`;
  - FS `vs_out_mask` ignored: the translation sets it from the VS code, and a stand-in's own VS feeds its own FS;
  - every other field must be equal.
- **Counters:** the `[vk] session pipelines` line ends with "variant fallbacks: N draws, N started, N replaced by
  the exact variant, N stand-in pipelines". With verbose output, the first 64 starts and replacements are logged, and
  the first 16 rejected siblings with the rule that rejected them. `EOWRECOMP_VK_VARIANT_FALLBACK=0` turns it off.
  The pipeline cache format is unchanged (no `PIPELINE_SCHEME` bump).
- **Tests:** `tools/test_variant_fallback.sh` (`tools/test_variant_fallback.c`, CPU only), 1822 checks, all pass:
  - 36 directed cases, including every reject;
  - 20000 fuzzed programs (7601 translatable pairs): each accepted stand-in (1784, 1341 with other keys) translates
    byte-identically to the exact variant, ShaderInfo included.
  - With the attribute or target rule removed, it fails (27 and 1037 failures).
- **Built:** `./setup/build.ps1` passes, with no warnings in `exec.c`. `Trirod.exe` relinked into `<test-install>`
  with `trbuild code` (game code reused).
- **In game** (`<test-install>`, v1.1.0, scripted save load, `EOWRECOMP_CACHE_DIR` pointing at an empty scratch
  folder, so a first-run session; verbose):

  | Run | Stand-in draws | Variants started / replaced | Stand-in pipelines | Skipped at the load |
  |---|---|---|---|---|
  | fallback on (GPL) | 35 | 8 / 8 | 1 | 101 (46 program, 55 pipeline) |
  | `EOWRECOMP_VK_VARIANT_FALLBACK=0` | 0 | 0 / 0 | 0 | 109 (47 program, 62 pipeline) |
  | on, `EOWRECOMP_VK_GPL=0` | 34 | 8 / 8 | 1 | 123 (46 program, 77 pipeline) |

  - One sibling was rejected in game for a read vertex input of another kind.
  - Scene after the load: correct (screenshot at frame 4500).
  - With the prepared cache, nothing needed a stand-in (0 skipped, 0 missed).
  - `EOWRECOMP_VK_VALIDATION=1`: the same messages with the fallback on and off (the known "FS writes c0 with no
    color attachment" depth pre-pass warning, 10 times). The "on" run also hit a validation-layer timeline-semaphore
    timeout and then an exception inside the layer, about 100 s after the last fallback event. It has not been seen
    again and has not been traced.
- **Limits, where a draw can still be skipped:**
  - the first variant of a guest program (no sibling exists);
  - siblings still building, or failed;
  - a sibling that differs in a read vertex input kind, a written target kind, stencil samplers or clip-space flags;
  - code ids unknown (`EOWRECOMP_VK_WARM=0`);
  - a stand-in whose own pipeline for this state is not built yet (skips like before, or waits under the one-shot
    rule);
  - pipeline still building for a ready program while no sibling has a ready pipeline for that exact state.
  - Compute dispatches are unchanged.
  - Most of the load's skips are still first variants: in this scene the fallback covered 8 of 62 programs.
- **For the user to check:** play an area not yet in the prepared cache (or use an empty `EOWRECOMP_CACHE_DIR`), and
  look for fewer missing objects during the first seconds. Run with `--console` and check the "variant fallbacks"
  part of `[vk] session pipelines`. If something looks wrong only while shaders build, compare with
  `EOWRECOMP_VK_VARIANT_FALLBACK=0`.

## Launcher self-update (2026-11-25): staged install + restart, tested end to end locally, not against a GitHub release

- **Root cause (traced in `launcher_win.cpp`):**
  - `check_release` refused any release whose `min_launcher` was newer than the running launcher, and only showed the
    release page. `make_release.ps1` defaults `min_launcher` to the release's own version, so every release was "too
    new" for every older launcher and could not be installed from the launcher at all.
  - When an update did install, `apply_update` swapped every top-level entry, `Trirod-Launcher.exe` included.
    Windows allows renaming a running executable, so the swap succeeded, but nothing restarted: the old process kept
    running its old code and DLLs into the recompile, and the new launcher took over only at the next manual start.
- **Fix:** the new launcher installs itself from its unpacked stage folder after the old one has closed
  (`stage_update` → `hand_off_update` → `apply_mode` with rollback; `--updated` / `--update-error` on restart;
  `--recompile` passed on when the release needs it). One launcher per installation and data folder (named mutex).
  `min_launcher` no longer blocks; the new `min_updater` manifest field (`make_release.ps1 -MinUpdater`) is the
  refusal for future format changes. No new packaged file: the launcher is its own updater, so the release allowlist
  and `check_package.ps1` are unchanged. `--update-from <folder>` / `--update` give a local, scripted update path.
  Details: `docs/RELEASE-WORKFLOW.md`, "Recompile (and the build after an update)", step 1.
- **Tested:** `tools/test_self_update.ps1` (real processes, local release, launcher built as the next patch
  version), all checks pass:
  1. the update closes the old launcher, installs, and restarts from the installation with the same `--data` /
     `--dev` plus `--updated`; one instance runs; the previous files are kept; the data folder is unchanged;
  2. a restart doesn't update again;
  3. a wrong SHA-256 is refused with the installation untouched;
  4. a package whose launcher can't start is refused before anything is replaced;
  5. a file in use during the swap rolls back and restarts the previous launcher with the error;
  6. a second launcher for the same folders is refused.
- **Not verified:** a real GitHub release download (the GitHub path still has never run against a published release),
  and launchers older than this change (v0.1.0 still refuses releases whose `min_launcher` is newer).

## NSP update support (v1.1.0): import and loader code done, NOT verified on the real update or in game

Goal: a v1.0.0 base NSP plus its v1.1.0 update NSP. v1.0.2 is **not supported** (no package to inspect). The user's
packages and `prod.keys` stay outside the repository, read-only. Nothing below may be called "v1.1.0 supported" until
the real-package import and a gameplay test pass (the list at the end).

### v1.1.0 audio (2026-11-25): REV15 playback restored; delay effect implemented (next section)
- Cause: nnSdk 20 opens the audio renderer as **REV15** (v1.0.0: REV11; the log prints it as "REV?", '0' + 15).
  From REV15 each voice's input is `VoiceInfo::InParameter2` (0x188 bytes, float biquads, every field after them 0x18
  further on; yuzu `audio_core/renderer/voice/voice_info.h` 152-220, `common/feature_support.h`
  `VoiceInParameterV2 = 15`). `audio.c` read 0x170-byte voices at the old offsets, so no voice had a wave buffer.
- Fix: `runtime/services/audren_voice.h` (both layouts, chosen by revision; biquads read as floats from REV15),
  `audio.c` `update_voice` / `request_update` read through it. REV11 parsing is unchanged (same offsets).
  `SplitterDestinationV2b` (also REV15) only changes the splitter section, which isn't implemented (0 splitters here).
- `EOWRECOMP_AUDIO_WAV` capture in the v1.1.0 test installation: title screen 39 s before = all zeros; after = RMS
  ~0.0025 throughout; scripted save load + walking: peak 0.36, 5 s RMS up to 0.054. The user confirms audio playback works. The delay effect is now implemented; its parameters were read in game, its sound still needs a listening check (next section). `tools/test_audren_voice.c` (63
  checks): both layouts parsed from buffers built at the reference's offsets; a REV15 voice read the old way loses its
  wave buffers.
- To listen for: music and effects at normal level, biquad-filtered sounds (e.g. muffled / underwater) not harsh.
  Effect type 3 is the delay effect; its processing path is being checked separately from the REV15 voice-layout fix.
- Testing note: a scripted save load that walks autosaves (4 save files changed); they were restored from a copy.

### Delay effect (type 3) and v1.0.0 compatibility (2026-11-25)
- **Delay implemented:** `runtime/services/audren_delay.h` (parameters at the effect entry's +0x20,
  `DelayInfo::ParameterVersion1/2` in yuzu `audio_core/renderer/effect/delay.h`; processing after
  `command/effect/delay.cpp`: per-channel line, feedback matrix direct / cross by channel spread, one-pole lowpass,
  dry + wet output), run by `audio.c` `apply_effects` in effect order on the same mix buffers as the reverb.
  `EOWRECOMP_NO_DELAY=1` turns it off. Read in game (log `[audren] delay init`), the same in v1.0.0 and v1.1.0:
  6 channels, 160 ms (max 2500), in 0.6, feedback 0.6, wet 1.0, dry 0.4, spread 0, lowpass 0.7; the sample rate
  parameter is 48000 Hz in Q14 (48 samples/ms). `tools/test_audren_delay.c` (23 checks): echo timing, decay,
  dry / wet, stereo cross-feed, lowpass, 6-channel routing, update vs. re-initialization. Not yet listened to.
- **v1.0.0 checked with this runtime** in a scratch data folder (base package only; imported through the new
  temporary-folder path in 153 s, game code built from v1.0.0's modules in 790 s): boots (DT_RELA path, no packed
  relocations), sets the hold type, renders the title screen (screenshot), audio REV11 with reverb and delay, no
  abort. Not played past the title.
- **Audio level:** v1.1.0's title looked 5-15x quieter than v1.0.0's fresh-save title, but v1.0.0 with a copy of the
  same save as `<test-install>` gives the identical level (peak 0.0152, RMS 0.0025): the save's in-game volume
  settings, not a renderer difference.
- **Behaviour changes v1.0.0 also gets:** the delay (audible), `GetNpadJoyHoldType` (hid 121) returns the hold type the
  game set instead of always Vertical, and HID shared memory's NpadCondition is filled (v1.0.0's SDK doesn't read
  it). The rest of the v1.1.0 work only applies to what v1.1.0 sends (REV15 voices, packed relocations, GNU hash,
  `IApplicationFunctions` 210).
- Test notes: the runtime re-launches itself at start when the guest address range is taken (`main.c`
  `reserve_guest_space`), so a script's process handle can end while the game keeps running: stop `Trirod.exe` by
  name. `EOWRECOMP_SHOT_AT` fails in a data folder without a `build` folder (it creates only `build\frames`).

### Existing extraction vs. a changed base / update selection (2026-11-25): implemented, checked on real packages (read-only) and by synthetic tests
- Before: `trbuild files` reused `extracted/` whenever `exefs/main` had a supported build ID, so adding an update to
  a base-only extraction (or removing it) kept the old files; the launcher only looked at whether `exefs/main` existed.
  Packages in a folder were told apart by file size.
- Now:
  - `nx_identify` (`nxpkg.c`) reads each package's program NCA and its main's build ID; `trbuild` (`classify`) picks
    the base and the newest supported update by content (`VERSIONS` table: v1.0.0 base, v1.1.0 update). **v1.0.2
    isn't recognized:** its main build ID is unknown here (no package); such an update is reported by build ID and not
    imported. Adding it is one `VERSIONS` line once its ID is read; the runtime has never run it.
  - `extracted/import.txt` records what was imported (packages' and keys file's path, size, time; main's build ID; no
    key material). `runtime/tools/import_state.h` `import_decide`: current / stale (base, update or keys changed, or
    the extracted main isn't what the selection makes) / incomplete / none. An extraction from before records is kept
    when its main is the version the selection makes, and the record is written then (no needless 8 GB re-import).
  - Stale or missing: the import goes to `extracted/.import`, is checked (title, main build ID) and only then swapped
    in (`import_swap`: old files aside, new ones in, roll back on failure); the old shader catalog goes with the old
    files. A failed import leaves the working extraction. Needs room for a second RomFS image while importing.
  - `trbuild check` reports `@@extracted <state> <why>` (docs/TOOL-PROTOCOL.md); the launcher shows "Detected: base
    game v1.0.0 (...) and update v1.1.0 (...)" and "Extracted files: current / out of date / incomplete", offers Play
    only when current, re-checks after a build, and has "Choose update .nsp..." / "No update" (`update=` in
    trirod-launcher.ini, `--update` to trbuild) for an update kept outside the game folder.
- Checked on `<test-install>` (v1.1.0 extraction from before records), `trbuild check`, nothing written: folder with both
  packages → base v1.0.0 + update v1.1.0, current; base file alone → stale ("extracted v1.1.0, the chosen files make
  v1.0.0"); base + `--update` → current; update file alone → refused ("an update needs its base game"); nothing chosen
  → current. `tools/test_import_state.c` (41 checks): every decision branch including "base-only extraction, then an
  update chosen" (fails 4 checks with the version comparison removed), the record, a successful swap and one that
  fails half-way and restores everything.
- Not run on real data: an actual re-import after a selection change, and the swap/rollback with an 8 GB image (only
  synthetic); the launcher UI was built but not looked at.

### v1.1.0 "stuck" shader loading screen (2026-11-25): game hang after the warm-up, fixed, reaches gameplay
- Seen: shader preparation at 100% with "programs 575/577, pipelines 1115/1117", never
  leaving it; the log's exit line said all 1117 built. The warm-up was not slow (`[warm] ready in 1.9 s`, every run
  from the prepared cache). `warm_boot` (`device.c`) leaves its loop when the build ends without drawing a last
  frame, so the window keeps its previous frame (575/577) until the game presents. The game never did.
- Cause: right after creating its window the v1.1.0 update calls `IApplicationFunctions` 210 (v1.0.0 doesn't); `am.c`
  answered with no handle, and a guest thread then polled `WaitSynchronization(handle, timeout 0)`, which failed with
  0xE401 (invalid handle) forever: about 267 000 SVCs in 30 s (`EOWRECOMP_TRACE_SVC`), no present. yuzu
  (`application_functions.cpp` 540, `applet.cpp` 18) returns a readable event that nothing signals; `am.c` now does
  the same, as for 130 / 140.
- Verified on `<test-install>` (v1.1.0; GPU model omitted; game relinked with `build\setup-dev\trbuild.exe code`, which
  recompiled all game code): first present, title screen (frame 1500), scripted save load
  (`EOWRECOMP_INPUT='2000:A:12,2600:A:12,3600:A:12,3800:UP:12,3950:A:12'`) into the game by frame 7000; warm-up 1.9 s;
  save files unchanged (hashes compared). Not checked: Esc / B on the panel (it lasts 2 s with a prepared cache);
  the panel still shows a stale last frame for the instant before the game's first present.

### v1.1.0 startup abort after `[sm] GetService hid` (2026-11-25): cause found and fixed; v1.1.0 now boots past it (seen in `<test-install>`)
- Log: `svcBreak result 0x58cca (module 202, description 710)` at 0x700c17565c. That PC is the sdk's `svcBreak` stub
  (sdk + 0x17565c), reached from `nn::diag::detail::Abort`; the info pointer is the failing Result. HID 710 is
  `ResultNpadNotConnected` (yuzu `hid_core/hid_result.h` 41).
- The only code in any module of either version that builds 0x58cca is v1.1.0's `nn::hid::detail::GetNpadJoyHoldType`
  (sdk 0x5083f0). nnSdk 20 no longer asks the service (command 121, what v1.0.0's sdk 0x490560 does); it reads
  `NpadCondition` through `NpadSharedMemoryAccessor::GetNpadCondition` (accessor + 0x68, set by `Attach` at
  0x5080a0 to shared memory + 0x3E200): the u32 at +8 if the byte at +0xC is set, else 710, which
  `nn::hid::GetNpadJoyHoldType()` (0x5154b0) aborts on. The game calls it (main imports Get/SetNpadJoyHoldType in both
  versions). `GetLastActiveNpadController` (0x50afb0) reads +0 / +4 of the same structure (else HID 751). Our
  0x40000 block was all zeros, so both fail.
- Layout: yuzu `hid_core/resources/shared_memory_format.h` 227 / 246 (`npad_condition` at 0x3E200) and
  `npad/npad_types.h` 168-175 (`{u32; u32 is_initialized; u32 hold_type; u32 is_valid}`), which agrees with the reads
  above. Fix (`runtime/services/hid_shmem.h`, `hid_set.c`): the shared memory's `on_map` writes the condition
  {npad 0 (player 1, the controller `hle/input.c` reports), 1, hold type, 1}; `SetNpadJoyHoldType` (120: aruid at
  +0, hold type u64 at +8, yuzu `cmif_serialization.h` 141-150) now records the hold type, rewrites the condition, and
  121 returns it (before: always Vertical, whatever was set; that also changes v1.0.0's answer after a Set). Default
  Vertical (yuzu `npad_data.cpp` 98, and what 121 always answered); yuzu's condition initialiser says Horizontal but
  nothing reads it there.
- Test: `tools/test_hid_shmem.c` re-implements the two guest reads; the zeroed block gives 0x58cca / 0x5deca (the
  log's abort), the written one the hold type and npad 0; only those 0x10 bytes are written. With the write disabled
  it fails 3 checks.
- Still to check in the game: that boot passes this point (the next abort, if any, shows the next nnSdk 20 change),
  the hold type the game sets (`[hid] NpadJoyHoldType` log line), and v1.0.0 still boots with input. The other
  nnSdk 20 shared-memory readers (`GetNpadStates`, colors, `GetNpadStyleSet`, six-axis, ...) are either HLE'd at
  the public `nn::hid` level (`hle/input.c`) or not yet exercised; they read the npad entries at 0x9A00, still zeros.

### Implemented
- `runtime/nxpkg.c` `nx_setup_update`: two candidate packages in either order, roles by content (`update_roles`: the
  update is the program NCA whose RomFS has a BKTR relocation table), same title ID required; writes **only the
  update's ExeFS** (a patch ExeFS replaces the base's whole) and the merged RomFS (`romfs.update.bin`, then renamed to
  `romfs.bin`; no `romfs.src` with a derived title key on this path). A second package that isn't an update (DLC,
  another game, unreadable) falls back to the base-only `nx_setup`; `trbuild` then requires the v1.0.0 main build ID,
  with an update the v1.1.0 one (`9f221e1afa65ba54`).
- Update RomFS reading, checked against the local yuzu port of Atmosphere's fssystem:
  - **Counter word order, resolved:** `fssystem_nca_header.h` 155-161 defines `NcaAesCtrUpperIv` as
    `{u32 generation; u32 secure_value}` in a `u64 value` union; line 295 places it right after `patch_info`, whose
    `Offset` is 0x100 and size 0x40 (139-152), so generation is at fs header 0x140 and secure_value at 0x144.
    `fssystem_aes_ctr_storage.cpp` 15-24 (`AesCtrStorage::MakeIv`) stores `upper` as a big-endian u64, so the IV's
    first 4 bytes are secure_value's, then generation's, then the block number. `fssystem_aes_ctr_counter_extended_
    storage.cpp` 226-231 builds that upper value from the **entry's** generation and `m_secure_value`, which
    `fssystem_nca_file_system_driver.cpp` 924 takes from the header's `upper_iv.part.secure_value`. So CTR-Ex IV =
    BE(secure_value) BE(entry generation) BE(block), i.e. `ctr_hi[0..3]` then the entry generation. The code had
    `ctr_hi[4..7]` (the header's generation word), against its own comment; fixed. The earlier partial run worked
    with the old order, which is only possible if this update's two header words are equal: then both orders give the
    same IV and the fix changes nothing for it (a wrong order fails loudly in `patch_load`, as the tests show).
  - CTR-Ex entries (`fssystem_aes_ctr_counter_extended_storage.h` 33-42): `encryption_value` at +8, 1 = stored
    unencrypted (`.cpp` 223); honoured and validated. CTR-Ex data spans `[0, aes_ctr_ex_offset)` (driver `.cpp`
    911); IndirectStorage source 1 spans `[0, indirect_offset)` (driver `.cpp` 1036-1060); both enforced.
  - **Patch meta hash layer** (newer authoring): `fssystem_nca_reader.cpp` 491-494 (`ExistsPatchMetaHashLayer`: meta
    hash data size at fs header 0x1A8 != 0 and a relocation table), header layout `fssystem_nca_header.h` 286-299 and
    192-196 (`meta_data_hash_type` at 0x5, `NcaMetaDataHashDataInfo` at 0x1A0). With it, `CreatePatchMetaStorage`
    (driver `.cpp` 1072-1120, the AES-CTR storage at 1117) decrypts **both tables as one ordinary AES-CTR region** from `indirect_offset` to
    the end of the hash data and verifies it; without it the relocation table is CTR-Ex data. Both layouts are now
    read (hashes not verified, as for every layer here); another meta hash type is refused. Whether the supplied
    update uses the layer is in its encrypted NCA header, so it can't be known without reading the package;
    `patch_load` now logs which layout it found (counts only).
  - Encryption types other than 4 (e.g. AES-CTR-Ex with skipped layer hash) are refused with a clear error.
- `runtime/loader.c` + new `runtime/elf_dyn.h`, `tools/nso.py`: **nnSdk 20 dynamic sections** (see below):
  symbol count from `DT_GNU_HASH` when `DT_HASH` is absent, and `DT_RELR` packed relative relocations (applied by
  the loader; listed by nso.py as `R_AARCH64_RELATIVE` with the stored word as addend, which is what `gen.py` 95 and
  `analyze.py` 61 use to find code pointers). v1.0.0 behaviour is unchanged (it has DT_HASH and no DT_RELR).
- `tools/shader_catalog.py`: the manifest's `game_version` came from a constant "1.0.0"; now from main's build ID
  (`main_build_id` recorded too; "unknown" otherwise).
- Earlier fixes: base-only read path added `data_off` twice (broke every v1.0.0 import); bounds and overflow-safe
  checks; `rt_remove_game_files` removes the temporary RomFS files (decrypted data was left behind).

### What was measured (no package read: v1.0.0 `extracted/exefs` and the earlier partial import's
`the ExeFS from a temporary import-check folder`; numbers and API names only)
- The v1.1.0 ExeFS is right: every segment of main, sdk, subsdk0, subsdk1 matches the SHA-256 in its NSO header;
  main build ID `9f221e1afa65ba54`.
- Every module changed. v1.1.0's sdk is **nnSdk 20** (v1.0.0: 16). sdk/subsdk0/subsdk1 have no `DT_HASH`; all four
  have `DT_RELR` and no relative entries left in `DT_RELA` (v1.0.0 main: 467 775 there). Before this change the
  loader would have found 0 symbols in them and relocated none of those pointers.
- With the new code: GNU-hash counts equal DT_HASH's on all five modules that have both (758 / 111 / 11 625 /
  27 633 / 749); v1.1.0 counts 186 / 10 110 / 23 031. Decoded RELR: main 448 722, subsdk0 70 917, subsdk1 3 424,
  sdk 15 002 relocations, every one 8-aligned, unique, and pointing inside its module; the C decoder
  (`elf_dyn.h`) and nso.py give identical results on main, sdk and subsdk0.
- HLE binding: the 57 resolvable `HLE_FUNC` names resolve in v1.1.0 as in v1.0.0 (2 others are not module
  symbols in either). main's 7 imports new in v1.1.0 are all defined by its modules. SVCs used: v1.1.0's are a
  subset of v1.0.0's (0x48 / 0x49 no longer used). NVN entry-point names: identical sets (569 both, 493
  implemented). Module images fit `runtime/modules.h` (main 0x34d3000, subsdk0 0x15ef000, subsdk1 0x66c000, sdk
  0xc52000).

### Tests (no game files; Windows, clang `-Wall`, all pass)
- `tools/test_nxpkg.c` (25 checks): AES vectors; BKTR routing (base/patch, two generations, an unencrypted range,
  unaligned reads, end-of-image and relocation-table refusal); bucket-tree walk; base/update roles; the unpatched
  `data_off` case; and a synthetic update section **described by a real fs header** parsed by `section_info`, in
  both layouts (CTR-Ex relocation table / patch meta hash layer), whose IVs the test builds from the raw header
  bytes by the reference's definition (`ref_iv`), with distinct generation / secure_value words. Reverting the
  counter-order fix fails 5 checks; ignoring the meta hash layer fails 2.
- `tools/test_elf_dyn.c` / `tools/test_nso.py`: GNU-hash counts on linker-style tables; RELR decoding of
  lld-style encodings (single address, dense runs over several bitmaps, gaps, window edges, out-of-image entries);
  the catalog's game version from main's build ID.
- `trbuild.exe` builds without warnings (scratch build dir `build/nsp-audit`) and imports no network API; `loader.c`
  compiles with the runtime's flags. The full runtime (`./setup/build.ps1`) was not rebuilt this round.

### Still needs the real package or the game (don't claim v1.1.0 support before these pass)
- **Import** (blocked on the security question below): import into a temporary folder outside the repository and game
  installation; check the `[nxpkg] update RomFS` line (which layout), that main is `9f221e1afa65ba54`,
  the RomFS header, and that every file in the merged RomFS has a valid magic (as for v1.0.0). That also confirms the
  counter order on real data.
- **Code generation**: run trgen on the v1.1.0 modules (now that symbols and RELR pointers are visible) and build the
  game library; `tools/patches.py` has no active sites, so nothing asserts, but nothing was generated yet.
- **Runtime, needs a gameplay test** (nnSdk 16 → 20 changed every module): the services the SDK talks to (IPC
  command IDs, hid / time shared-memory layouts; unimplemented commands show up as `LOG_FIRST`); the ABI of the
  HLE'd SDK functions (hid `Npad*State`, vi, pl shared font, time, swkbd) and of the movie middleware classes in
  `runtime/hle/movie.c` (names resolve; layouts unverified); NVN behaviour (same entry points, semantics unverified);
  save data written by v1.0.0 opened by v1.1.0; the loader's new RELR / GNU-hash paths in a real boot.
- Version-specific leftovers: `shader_catalog/` warm recipes and pipeline caches are v1.0.0's (performance only;
  the per-install catalog is rebuilt from the user's RomFS); debug-only shader addresses in `exec.c`
  (`EOWRECOMP_DEBUG_*`, `[checkcs]`); the macOS launcher (`runtime/ui/launcher.mm`) still accepts v1.0.0 only and
  has no update import.
- **Full import blocked on a safety question, not retried.** A Codex automatic review stopped the earlier run at
  about 28% after it saw an unexpected telemetry connection to a Microsoft endpoint; no transmission of game data is
  known. `trbuild.exe` imports no network API (KERNEL32, CRT, SHELL32, USER32, WINMM, SDL3, libwinpthread), nor does
  SDL3.dll; `trbuild files` starts no process before the shader catalog step; the tree's only network code is the
  launcher's GitHub update check (off when `UPDATE_REPO` is empty). The operating system may have initiated the connection; this has not been confirmed. The review record was not available, so the source and destination remain unknown. To continue the user must give
  the review's details, or approve a run with an outbound block for `trbuild.exe` (Defender's own cloud lookups
  being OS behaviour outside TriRod), or run it themselves.

## Shared shader warm catalog (implemented; awaiting Windows build and install test)

- `<test-install>\cache\warm` held 1,089 learned pipeline recipes. `tools/export_shader_warm.py` validated recipe
  checksums and referenced guest-code hashes, then exported 1,089 unique recipes and 818 code records (about 5.7 MB)
  into `shader_catalog\warm\`. This is the folder to update after future playthroughs.
- The catalog uses recipe schema 2 with wildcard GPU IDs. Setup packages only `shader_catalog\warm\` under
  `tools\shader_catalog\warm`; `Trirod-ShaderPrep` loads it with the player's local recipes, validates Vulkan format
  support, rebuilds compatible pipelines on the player's GPU, and merges recipes/code into local `cache\warm`.
- `cache\vk_pipelines.bin` stays per device/driver. ShaderPrep fills `cache\spirv`; `cache\glsl` is optional
  diagnostic output (`EOWRECOMP_SAVE_GLSL_CACHE=1`). Neither is part of the shared catalog or release package.
- Package checks allow only curated warm-catalog files under `tools\shader_catalog\warm`; other game data and opaque
  driver-cache checks remain in place.
- Built and run in the scratch data folder: 1089 bundled recipes plus local ones gave 1133 complete pipelines on the
  GPU model omitted, none skipped. The gain in familiar scenes is not yet measured in gameplay.

## Shader preparation and GPL (2026-11-24): black-screen fix, cache identity, progress, GPU change — awaiting in-game confirmation

**Black screen: root cause (reproduced and fixed here; not yet confirmed by the user in game).**
- Reproduced in the scratch data folder on a GPU, at the title screen (frame 900).
- With GPL on, the frame was black: 720 draws per interval, **0 skipped**, so the GPL pipelines were bound and drew
  nothing. With `EOWRECOMP_VK_GPL=0` (new switch), the title rendered.
- The Khronos validation layer (`EOWRECOMP_VK_VALIDATION=1`) reported the cause: the background "optimized"
  pipeline was a `LINK_TIME_OPTIMIZATION` link of four libraries created without
  `RETAIN_LINK_TIME_OPTIMIZATION_INFO`. That is invalid usage, and once such a pipeline existed, `draw_` bound it.
- Adding RETAIN fixed the picture, but the prepared `vk_pipelines.bin` grew to **1.19 GB**, from about 9 MB, because
  the driver keeps the compiler input of ~15k shader-stage libraries.
- Final design, as in DXVK:
  - Libraries carry no RETAIN flag and are only fast-linked.
  - The optimized replacement is the complete (monolithic) pipeline for the same key, built on a low-priority
    worker (`exec.c` `optimize_gpl_pipeline` → `build_pipeline_mode(pp, true)`). `draw_` switches to it once it is
    ready.
- After the change, the title renders on a cold cache with GPL on and no validation messages. The cache was 3 MB
  after 15 s.
- The dynamic-state split (17 states across the four library types) matches the monolithic `DYN[]` list.
- Fallback to monolithic pipelines when:
  - GPL or fast linking is unavailable;
  - polygon mode is wireframe;
  - a library link fails (logged).

**Cache identity.**
- `vk_pipelines.bin` has a tag, `vk_pipelines.tag`: pipeline scheme 3, `prepared`, vendor, device, driver, GPU.
- At start, `device.c` `pipeline_cache_check` compares the driver header (vendor, device, `pipelineCacheUUID`) and
  the scheme. On a mismatch it starts an empty cache and writes the reason to `cache\shaders-stale.txt`.
- Tested: the untagged cache from the black run was rejected as "the shader cache is from an older Trirod build".

**Other bugs from the GPL change, fixed:**
- **Recipes were being lost.** The game no longer runs the warmup, but `vk_warm_save` still rewrote `recipes.bin`
  with only that session's recipes. On the user's PC, 1133 merged recipes became 126. The game now loads the saved
  records at start without building them (`vk_warm_load_records`), so a save merges.
- **Waits on exit and reboot.** Saving the cache waited for every shader job, including all queued optimized
  pipelines. The cache is internally synchronized, so the save no longer waits; ShaderPrep waits itself, with
  progress. A reboot drops queued optimized jobs before it waits.

**ShaderPrep: what it builds, progress.**
- The step shows one percentage over every unit of work, counted only when finished, with a line for the phase:
  - recorded pipelines: programs prepared, complete pipelines created;
  - game shader catalog: translated to SPIR-V, shader-stage libraries;
  - optimizing pipelines: the background complete pipelines.
- `@@info` lines say what was built and skipped, and why. `shaderprep.json` adds `optimized_pipelines` and
  `optimized_queued`.
- Complete pipelines come only from recipes, because the shader archives have no vertex formats, render targets or
  blend state. The catalog provides SPIR-V plus, with GPL, per-pair shader-stage libraries. The game reuses those for
  every draw state of the program by fast-linking them with small vertex-input and output libraries.
- Scratch run on a GPU, through the launcher's **Prepare shaders**, with the RETAIN build: 1333 s.
  - recipes: 599/599 programs, 1133/1133 complete pipelines, none skipped;
  - catalog: 7442/7442 translated to SPIR-V; 7405/7405 shader-stage library pairs;
  - optimized: 1133/1133.

  Final build:
  - `Trirod.exe` ran the same preparation itself in 29 s (helped by the GPU driver's shader cache from the
    earlier run).
  - Catalog only, into empty cache folders: 44 s and a **1.13 GB** `vk_pipelines.bin` with shader-stage
    libraries; 39 s and 36 bytes without GPL. About 150 KB of driver cache per vertex+fragment pair: the cost
    of prebuilding every stage library.
  - Game start to frame 900: 21.8 s and 26.2 s with the prepared cache, 19.8 s with an empty one. The title
    scene skipped the same 9 draws in its first interval either way, so its gain is not measured. Larger scenes
    and how often the game's ShaderKey matches the catalog's plain-draw key are still to be measured.

**GPU or driver change.**
- When Setup has prepared shaders on this PC before (`cache\shaderprep.json` exists) but the cache doesn't match
  the selected GPU, driver or scheme, `Trirod.exe` runs `Trirod-ShaderPrep` itself before the game starts. It shows
  a progress panel in the window (`menu_set_progress`), then merges the prepared cache into its own
  (`vkMergePipelineCaches`).
- Esc, B or Back skips it. It runs once per device and driver (`cache\prep-attempt.txt`;
  `EOWRECOMP_PREPARE_SHADERS=0/1`). Output: `logs\shaderprep-game.log`.
- The launcher shows the stale reason under **Play** and offers **Prepare shaders** (`--prepare-shaders`).
- There is one cache per data folder: switching back to the first GPU needs another preparation.

**First-use black or half-drawn scenes (user report, save load into a house area, GPU model omitted).**
- **Cause.** The GPL change had removed the boot build of recorded pipelines from `Trirod.exe`.
  - Setup's preparation only fills the driver cache. Every first draw of a session still waited for its program's
    translation (one serialized thread) and `vkCreateGraphicsPipelines`, and was skipped meanwhile.
  - `draw_` still looked for warm pipelines, but none existed, so the 1133 recipes were never used in game.
- **How the user's frames came about.** Their log has no verbose counts, so this is inferred, not measured on their
  PC. The image held by the 500 ms rule is the last complete frame, which on a load is the black transition. Once
  the limit expired, frames were shown with the scene's material draws skipped: the silhouettes in their first
  screenshot.
- **Also found.** The user's in-game preparation ran a stale `Trirod-ShaderPrep.exe` from the data folder
  (09:19, old report format). The tool is now taken from `PATH` first and only when its `--version` runtime ID
  matches `Trirod.exe.id`.
- **Changes, general (not only save loads):**
  - Boot build of the recipes, with a panel and Esc / B to skip (`device.c` `warm_boot`).
  - Optimized replacements on an idle queue tier, so they never delay a build something waits for.
  - TOTK's one-shot rule: a draw into a target nothing drew into last frame waits instead of being skipped
    (`EOWRECOMP_VK_ONESHOT_WAIT=0` turns it off).
  - Compute dispatches wait up to 250 ms for their build (`EOWRECOMP_VK_COMPUTE_WAIT=0` turns it off).
  - Session counters (`[vk] session pipelines: ...`): prepared, reused ready / still building, missed (new),
    programs, and driver cache hits / compiles / fast links from `VK_PIPELINE_CREATION_FEEDBACK`.
  - The hold still applies to anything skipped; with verbose output, the stats line shows queued shader jobs, and
    every held stretch is logged.
- **Measured.** Scratch data, a copy of the user's save, scripted load
  (`EOWRECOMP_INPUT='2000:A:12,2600:A:12,3600:A:12,3800:UP:12,3950:A:12'`), each run from the same prepared cache:

  | Run | Boot build | First draws | Programs | Driver in play | Skipped at the load | Held |
  |---|---|---|---|---|---|---|
  | before (boot build and waits off) | — | 136 missed | 79 new | 124 cache, 12 compiled | 270 (150 program, 114 pipeline, 6 compute) | 118 ms, 260 draws |
  | after | 1133 in 2.2 s | 107 reused ready, 16 missed | 70 reused, 9 new | 4 cache, 12 compiled | 52 + 23 | 52 ms, 75 draws |
  | after, 2nd session | 1149 in 1.9 s | 123 reused ready, 0 missed | 79 reused, 0 new | 0 compiled | **0** | none |

  - The 16 misses of the first session were recorded and built at the next boot. That is the collection loop:
    play, record, prepare, reuse.
  - A cold boot build (no prepared cache) took 137 s before the idle tier existed. Setup's preparation is what keeps
    it to seconds.
- **Coverage, compared with the references.**
  - Shader-stage libraries are not complete pipelines. The catalog gives SPIR-V and stage libraries for all 7405
    pairs, but a pipeline also needs each draw's vertex formats, targets and blend state, which only recipes have.
  - TOTK builds the pipeline keys seen in play at boot. Triforce records every pipeline's full description and
    rebuilds all of them at start. Trirod now does the same from `cache\warm`: 1133 bundled-plus-local recipes, now
    1149.
  - What is still missing is breadth. The bundled catalog comes from one player's sessions. Each area someone plays
    adds recipes locally. `tools/export_shader_warm.py` merges a data folder's `cache\warm` into
    `shader_catalog\warm\`, the portable set Setup ships and prepares on each GPU.
  - Draws whose program was never seen remain first-use builds. The slow part is the serialized Maxwell translator
    plus the driver compile.
  - Next steps worth measuring: a persistent translation cache (translated GLSL and its reflection data per code
    hash and key), so a prepared program skips translation; and a hold limit tied to build progress rather than a
    fixed 500 ms.

**Not verified:**
- the fix in the user's game on a GPU;
- whether the user's own save load is now clean (tested here only with a copy of their save, on scratch data);
- gameplay beyond the scripted save load;
- the Esc / B skip key on the preparation panel (only ending the tool was tested);
- how long a full preparation takes on the user's PC.

## TODO

**alpha_2 (2026-11-24, in preparation; alpha_1 is published at github.com/Azrapha3l/trirod).** Console and log
cleanup. Nothing committed, published, or tagged.
- Version: `EOWRECOMP_VERSION` in `runtime/ui/launcher.mm` → `alpha_2` (the Makefile, tools manifest, and release
  manifest read it from there).
- No console by default:
  - `Trirod-Launcher.exe` and `Trirod.exe` are always GUI programs (Makefile `LAUNCHER_LDFLAGS`, `link.args`).
  - `RELEASE=1` and `build.ps1 -Release` are gone.
  - SDL's own log output is redirected: its default on Windows attached to the parent's console and printed into the
    terminal even without `--console` (found by the test below).
- Opt-in console:
  - `--console` or `EOWRECOMP_CONSOLE=1` makes the program attach to the parent's console or open its own
    (`runtime/platform/windows.c`, `launcher_win.cpp`). A redirected stderr stays redirected.
  - The launcher prints its build log there and starts the game with `--console`.
- Log levels (`runtime/log.c`, `rt.h` `LOG_VERBOSE` / `LOG_FIRST`):
  - Verbose only (`rt_verbose`: console, or `EOWRECOMP_VERBOSE=1` for the file):
    - periodic statistics: `[vk] t=… fps`, `[movie] acq_in`, `[vk] copy alias` counts, `command counts`;
    - investigation notes: gpu-stored memory read as a cbuf, texture slot samples the dummy, alpha test ignored,
      draw skipped/waited, fragment output with no colour attachment (its ring of 32 could repeat without limit), and
      `copy alias: draw samples`;
    - `[input] GetNpadState` and the `[audren] mix` matrix.
  - Stubbed service commands (`[ipc] STUB`, `[am]`, `[fs]`, `[time]`, `[audren]`/`[audout] … cmd N (ok)`) log their
    first call per (service, command).
  - Errors, warnings, the capped hazard warning, and one-time facts are unchanged.
  - Only log calls changed: `renderer_stats()` (which also saves recipes every 30 s) still runs every interval.
- Build: `setup/build.ps1` OK. The launcher's PE subsystem is GUI, `trbuild`/`Trirod-ShaderPrep` are console
  programs, and `link.args` has `--subsystem,windows`.
- Verified (scratch data folder, started from a console session, probed with `AttachConsole` from another process):
  - Launcher: normal has no console. `--console` and `EOWRECOMP_CONSOLE=1` show a console with the log.
  - Game (60 s, hidden window, to the title screen): normal has no console. `eowrecomp.log` got 207 lines, all one-time
    facts, with 0 fps-stats, mix, or command-count lines, and stubs once each. `--console` and `EOWRECOMP_CONSOLE=1`
    show a console with the detailed diagnostics, including fps stats.
- **Not verified:**
  - The click on **Play** forwarding `--console`: SDL ignores posted mouse messages, so it was not automated. The
    code is in `play()`.
  - The case with no parent console (launched from Explorer + `--console` → `AllocConsole`).
  - Gameplay beyond the title screen.
- What the user should test:
  1. Run `.\setup\build.ps1`.
  2. Double-click `build\setup-dev\Trirod-Launcher.exe`: no console should appear.
  3. **Play**: no console.
  4. Run `Trirod-Launcher.exe --console` (from Explorer, e.g. a shortcut): a console should open. **Play** should
     print the game's log in the same console.
  5. Play a while in normal mode and check that `eowrecomp.log` stays small.

**Windows launcher and build workflow (2026-11-23, implemented).** Details are in `docs/RELEASE-WORKFLOW.md`; the
tool protocol is in `docs/TOOL-PROTOCOL.md`.
- Implemented:
  - `Trirod-Launcher.exe` (`runtime/launcher/launcher_win.cpp`) uses the SDL 2D renderer and ImGui, with no Vulkan,
    shaderc, FFmpeg, or runtime code. The Makefile checks its imports.
  - The headless `trbuild.exe` (`runtime/tools/trbuild.c`) runs `check`, `files`, and `code`.
  - The headless `Trirod-ShaderPrep.exe` (`runtime/tools/shaderprep.c`) uses the same runtime objects as `Trirod.exe`.
  - `@@` events go on stdout and diagnostics on stderr. Full transcript: `<data>\logs\last-build.log`.
  - Build state uses `tools\trirod-tools.json` IDs compared with `Trirod.dll.id` and `Trirod.exe.id`. A successful
    build writes `trirod-install.json`.
  - Release manifest `trirod-release.json`, with SHA-256-checked download, `tar` unpack, and swap with rollback.
  - `--dev`, `--data`, and `--recompile`.
  - `setup/make_release.ps1` and `setup/check_package.ps1`, which refuses game data. `pack_tools.ps1` used to copy every
    `*.dll` in the build folder, including a player's `eowgame.dll`; it now uses an explicit list.
- Fixes found by the end-to-end run:
  - The link kit had no `stdc++.lib`; the working D: install had a hand-copied `libstdc++.dll.a`. `make all` now copies
    it into `link-kit/lib`.
  - `link.args`' `link-kit/...` entries are now resolved against the kit folder (`gamebuild.c`).
  - `--data` was ignored because `SDL_setenv_unsafe` doesn't reach the CRT `getenv`; it now uses `_putenv_s`.
  - Catalog translation now stops at NVN's const offset (`control + 0x700`), as `exec.c` does. Before, 312 programs read
    their constants as instructions.
- Verified (Vulkan; GPU model omitted):
  - `setup/build.ps1` passes.
  - Full **Recompile** in the launcher (`--dev`, scratch data folder with copies of the v1.0.0 files) ran all four
    stages through to **Play**.
  - Shader preparation: 967/967 recorded pipelines in 178.6 s cold and 1.5 s with the driver cache. Catalog
    (9,763 stages from the whole RomFS image + `main`): 7,436 of 7,442 to SPIR-V. 1,238 of the 2,297 SPIR-V modules
    from earlier game sessions are identical.
  - `make_release.ps1` produced a 192 MB zip and the manifest locally.
  - The launcher's GitHub query to `Azrapha3l/trirod` returned 404 (no published release), and the launcher kept
    working.
- **Not verified:**
  - Download and installation of a real release (nothing was published).
  - Extracting from an XCI/NSP through `trbuild files`. The code is the old start screen's, moved, but this run reused
    already-extracted files.
  - Clicking **Play** and the game itself with the new `Trirod.exe`.
  - Whether prepared shaders reduce stutter in game.
- **Translator gaps the catalog exposed: fixed (GLSL/Vulkan).**
  - The 6 fragment shaders were three instructions (identified with nvdisasm SM53):
    - `TMML.LOD`: mip-level query (`df58…`, 4 shaders).
    - `TLD4.R`: gather (`c838…`).
    - `TLD4S.R`: short gather (`df02…`).
  - Implemented in `runtime/nvn/maxwell/translate.c` (`emit_tmml` / `emit_tld4` / `emit_tld4s`) with the field layouts
    and results from Eden's decoders:
    - `TMML` element 1 is the clamped mip level as 8.8 fixed point (`textureQueryLod().x × 256`), and element 0 is the
      unclamped LOD (`.y`).
    - Gathers use `textureGather` with a component, or a reference value with `.DC`.
  - Not done, with an explicit error: offsets (AOFFI/PTP), the `TLD4` sparse predicate, `TMML` elements 2–3, `TMML`
    outside a fragment shader, and the MSL path (it keeps failing, like `SUST`, so the MSL comparison does not change).
  - Test: `texq_cases` in `tools/test_translate.c`, 13 cases (the game's words plus variants), compiled with shaderc and
    validated with SPIRV-Tools.
  - `REF=HEAD sh tools/test_translate.sh 2000 8 both` (run with Git's `sh`; MSYS2's `sh` drops `REF`):
    - TEXQ/SUST/S2R all ok.
    - GLSL: 0 compile and 0 validation failures.
    - Extent: 0 differences.
    - The 213 MSL "differences" are all programs with a new opcode that fail in both translators with a different
      message (checked: the 160 dumps contain new opcodes, and the reference's MSL is null).
  - `test_translate.sh` gained `-Dmaxwell_code_extent=old_…`: with `REF=HEAD` the link failed on the duplicate symbol.
  - Catalog result: **7,442 of 7,442** translated and compiled to SPIR-V.
  - `EOWRECOMP_SHADERPREP_DUMP=<dir>` makes `Trirod-ShaderPrep` write each stage it can't translate, for nvdisasm.
  - **Not verified in game:** whether these shaders draw correctly (`TMML` feeds the mip level of a `TEXS.LL`).
- The user should test:
  1. Run `.\setup\build.ps1`.
  2. Run `build\setup-dev\Trirod-Launcher.exe --dev --data <a test folder>`, choose the game folder or `.xci`/`.nsp`
     (with `prod.keys` beside it), click **Recompile**, then **Play**.
  3. In game, check whether the first entry into Hyrule Town has fewer `[vk] draw skipped` messages than before.
- The old in-process start screen (`runtime/ui/launcher.mm`) is still used on macOS, Linux, and Android.
  `tools/windows/package.ps1` (the old installer) has not been updated.

**Shader prewarm investigation (2026-11-23):** The matching local EoW v1.0.0 RomFS is available at `<test-install>/extracted/romfs`. Its `region_common/shader` folder contains 96 `.bfsha` archives (62,298,464 bytes total). Parsing their BNSH variation/program/stage tables found 247 BNSH containers, 11,892 program variations, and 3,576 unique stage records (35,278,429 bytes in the first per-stage-file output); all parsed records passed the control/code length and magic checks. `ExeFS/main` is also available (26,093,400 bytes), but direct BNSH scanning of the compressed NSO is not a complete scan of its decompressed sections. Do not use published v1.1.0 / SDK 20.5.1 counts as v1.0 inventory.
- Implemented: `tools/shader_catalog.py` extracts BNSH control/code stage records and paired stage identities. `tools/gen.py --shader-catalog` exposes it through the packaged helper, and `runtime/gamebuild.c` creates `extracted/shader_catalog/` from the user's own game files. `Trirod-ShaderPrep` now translates catalog vertex/fragment pairs and compute shaders to cached SPIR-V, then prepares complete GPU pipelines from local and bundled runtime recipes. Setup runs this headless tool before Play.
- Coverage gap: shader archives identify shader code and program pairs, but not every runtime shader key or pipeline state (vertex formats, attachments, depth/blend state). ShaderPrep cannot build every final pipeline from archive data alone. Additional pipeline combinations are discovered during play and saved as portable recipes; the driver cache is rebuilt per GPU/driver.
- Current first-use issue: the runtime still skips pending ordinary draws, and fullscreen/known-warm draws can still be skipped after their wait budget. ShaderPrep prepares known shader stages and recorded pipelines, but this policy can expose missing content when an unprepared combination is first encountered. `Trirod.exe` no longer repeats the batch recipe warmup at startup; it uses the driver's pipeline cache prepared by Setup and builds misses on demand.
- Implemented in source on 2026-11-24: optional `VK_EXT_graphics_pipeline_library` support. ShaderPrep prepares reusable VS/pre-raster and FS libraries from catalog shader pairs into the player's GPU-specific cache. Gameplay combines those with per-state vertex-input and fragment-output libraries for a fast linked pipeline, then queues a link-time-optimized replacement in the background. The monolithic fallback remains for devices without GPL and wireframe pipelines. The build log reports whether GPL and driver fast linking are available.
- Still needed: a target-aware wait/skip policy so an important render-to-texture producer is not discarded while its first-use program/pipeline is pending. Measure actual ShaderPrep duration, cache reuse, first-use waits, skipped draws, and dynamic-resolution artifacts. The translator remains serialized on one runtime thread; shaderc and pipeline work already use workers, so raising thread counts is not the first fix.
- Verification: syntax-only Windows compilation passed for `device.c`, `exec.c`, and `shaderprep.c` (two pre-existing const warnings in warm-code loading). Full `setup/build.ps1` did not reach compilation because MSYS `make.exe` fails to create a named object (`NtCreateDirectoryObject ... 0xC0000022`), including with `-Jobs 1`. GPL must still be built, run through ShaderPrep, and validated in-game.
- Public comparison only: a survey of EoW 1.1.0 reported 9,858 unique stages and also distinguished extracted shader-library records from a smaller runtime-recorded pipeline seed. That supports the two-source design conceptually, but its counts/manifest are not reusable for this v1.0.0 / SDK 16.2.0 build.

Atualizado: 2026-11-23 (sessão 16c: árvores de Hyrule Town corrigidas — S2R lane id/máscaras + draws indiretos; sessão 16b: warmup de shaders/pipelines; sessão 16: Rift confirmado; Hyrule Town sem erros de validação; diagnósticos para o apoio invisível do pássaro).

## Sessão 16 (2026-11-23): problema do Rift confirmado como corrigido

- O usuário confirmou que as correções de runtime feitas após a análise do Claude corrigiram o problema do Rift no jogo.
- Isso confirma o problema visual/renderizado do Rift na configuração testada; não significa que todos os avisos de validação Vulkan estejam resolvidos.
- O usuário está testando os demais problemas identificados. Acompanhar os novos logs/resultados antes de marcar `VUID-vkCmdDrawIndexed-None-06479`, `SYNC-HAZARD-WRITE-AFTER-READ` ou outras hipóteses como corrigidos.
- Novo teste: o usuário observou um pássaro aparentemente apoiado em algo que não está visível e flashes pretos/glitches em Hyrule Town. O log `[tex] type mismatch: want 3 have 1 (1408x792x1 target 1 fmt 0x35 layers 1)` confirma que uma amostra tipo cubo (3) recebeu uma textura 2D (1), D24S8 (`0x35`), do tamanho do framebuffer; `vk_view` substitui incompatibilidades por uma textura dummy do tipo solicitado, que para cubo é preta. Isso é uma causa plausível para pixels/efeitos pretos se os draws afetados participarem da cena, mas ainda não prova relação com o flicker ou com o suporte invisível do pássaro; correlacionar shader, slot, draw e frame. As mensagens `fragment color output has no color attachment` mostram shaders com saída de cor executados sem attachment de cor, enquanto há depth; podem ser draws depth-only intencionais e precisam ser associados a cada draw antes de tratá-las como defeito. A linha `21>3=1.000 22>4=1.000 23>5=1.000` ainda não foi identificada.
- Próximo passo: registrar quais avisos persistem, as linhas `[vk] shadow sampler` / `[vk] copy alias` correspondentes, e qualquer impacto visual ou de desempenho observado.
- **Análise de `<test-install>/EowOutput.txt` (23/11, Hyrule Town, com Vulkan Configurator) + screenshot do pássaro.**
  - Validação: **0 erros**. Os 06479 (sampler de sombra com view RGBA8) e o SYNC-HAZARD sumiram neste run. Sobra só o
    aviso `Undefined-Value-ShaderOutputNotConsumed-DynamicRendering` (10×, depois suprimido pela camada).
  - `[vk] fragment color output has no color attachment` (fs `72a576c600`, `72a5c2d600`, `72e92a9000`, `72a6c0d600`,
    `72a590e200`, `72a576b200` e outros): todos com `depth=000001C60BFC43A0` fmt `0x35`, a mesma textura que o
    sampler de sombra recebe (1024×1024 D24S8). São os passes depth-only do **shadow map**, desenhados com o fragment
    shader do material, cuja saída de cor a NVN descarta. Esperado; é a origem do aviso acima. Nenhum indício de ligação
    com o pássaro.
  - `[vk] shadow sampler stage 1 t4 … 2d array: fallback dummy`: é o caminho de feedback. Nesses passes do shadow map o
    material amostra `t4` (`sampler2DArrayShadow`), que é o próprio depth attachment. `resolve_feedback` tira um
    snapshot, mas `texture_info` trocava qualquer leitura 2D array de um snapshot pela dummy. **Corrigido:** agora usa
    uma view `2D_ARRAY` de uma camada do snapshot (válida para imagem 2D). Efeito visual esperado: nenhum, porque a cor
    desses draws é descartada. Fora do feedback, `vk_view` já convertia 2D → 2D array, e a cena principal lê o shadow
    map real (nenhuma linha de log).
  - `[tex] type mismatch: want 3 have 1 (1408x792 … fmt 0x35)` **não aparece neste log**, só no run do void
    (`the local Vulkan Configurator stdout log`, 2×). Os números são `VkImageViewType` (3 = CUBE, 1 = 2D), não `TEXTYPE_*`. O 1408×792
    D24S8 tem o tamanho da depth da cena. Não tem relação com o pássaro: não ocorreu neste run. Shader e slot ainda são
    desconhecidos; ver o diagnóstico novo abaixo.
  - `21>3=1.000 22>4=1.000 23>5=1.000`: vem de `runtime/services/audio.c` `update_mix`. É a matriz de volume do mix
    de áudio, no formato `buffer de entrada>buffer de destino=ganho`: o mix 1 tem 24 buffers (4 grupos de 6 canais)
    somados nos 6 canais do mix 0. É informativo e não tem relação com gráficos. No log anterior a linha só aparecia
    cortada.
  - Pássaro em Hyrule Town: nada no log identifica o draw do apoio invisível. Em regime estável há 0 draws pulados, e
    não há falha de tradução, draw indireto (opcodes 39/40/42 ausentes nas `command counts`) nem erro de validação.
    Portanto o objeto é desenhado e sai invisível, ou nem é desenhado pelo jogo. Achado lateral confirmado: o backend
    Vulkan **ignora o alpha test NVN** (`nvnColorStateSetAlphaTest`, byte 2 do ColorState; `SetAlphaRef` chamado ~1700×
    e nunca lido). Isso deixaria pixels a mais, não a menos, então dificilmente explica o objeto sumido; agora é
    registrado no log.
  - Desempenho neste run: 15–18 fps em Hyrule Town com validação, ~50 `gpu-stored cbuf waits`/s (todas do compute
    `72a4715000`, lendo `c4` em `727e80a300` / `727e809e00`) e ~7000 passes/s. Os números não são comparáveis com o run
    anterior, que foi em outra cena.
- **Diagnósticos novos** (`exec.c`, `device.c`; build `build/win-native` OK, só os warnings já existentes do WIP
  `pipe_note*`):
  - `[vk] texture slot samples the dummy: stage … shader … tN (handle …) wants <tipo>; tex …`: handle ligado que mesmo
    assim cai na dummy preta. Mostra o shader, o slot, o tipo pedido, a textura e se ela é attachment do passe. Uma vez
    por (shader, estágio, slot), no máximo 32. Identifica o pedido "cubo" do void.
  - `[vk] alpha test func N ref R ignored …`: uma vez por (fs, função), no máximo 16.
  - `EOWRECOMP_VK_DRAW_LABELS=1`: um label `draw f<frame> #<n> vs=… fs=…` antes de cada draw, para o event browser e o
    pixel history do RenderDoc. Usa `VK_EXT_debug_utils`, que o RenderDoc fornece.
  - `EOWRECOMP_VK_TRACE_FS=<hex>[,<hex>…]` (endereço VS ou FS): para cada draw correspondente (64 no total, 4 por frame)
    registra frame/número (os mesmos do label), a linha `[draw]`, o alpha test, os cbufs e vertex streams (marcando os
    que um shader em voo pode estar gravando) e cada slot de textura dos dois estágios.
- Testes: `REF=HEAD sh tools/test_translate.sh 2000 8 both`: os 8 casos SUST passam; GLSL 1340 compilados+validados,
  0 falhas. As 51 diferenças MSL contêm todas uma palavra SUST (0xEB20..0xEB3F) que o `translate.c` do HEAD rejeita como
  desconhecida (o tradutor não mudou nesta etapa). **Nada disto foi verificado no jogo.**
- O que o usuário deve testar:
  1. Copiar `build/win-native/link-kit/eowruntime.a` para `<test-install>/tools/link-kit/` e religar pelo Setup.
  2. Em Hyrule Town, conferir se o apoio do pássaro aparece com `EOWRECOMP_VK_NO_HOST_IMPORT=1` ou dentro do RenderDoc.
     Se aparecer, é outra leitura de memória gravada pela GPU, como no Rift.
  3. No RenderDoc, com `EOWRECOMP_VK_DRAW_LABELS=1`, usar o pixel history nos pés do pássaro para achar o draw (o label
     traz `fs=…`).
  4. Rodar ao vivo com `EOWRECOMP_VK_TRACE_FS=<esse fs>` e enviar as linhas `[trace]`, `[vk] texture slot samples the
     dummy` e `[vk] alpha test`.
- **Resultado do usuário:** o apoio do pássaro também falta no RenderDoc e com `EOWRECOMP_VK_NO_HOST_IMPORT=1`. Logo não
  é o problema de tempo do Rift: a causa é determinística. Occlusion query também está descartada (o jogo só pede
  `ReportCounter type 0`, timestamps). A grama aparece sob o pássaro, então o objeto não grava cor nem depth. Próximo
  passo para o pássaro: pixel history no alvo de cena 1408×792 e os testes A/B `EOWRECOMP_NO_DEPTH_TEST=1` /
  `EOWRECOMP_VK_FLIP_WINDING=1`.
- **Trace enviado (fs `72ef15f300`, provável causa dos flashes pretos):** é uma cadeia de blur/downsample em tela
  cheia, sem relação com o pássaro: alvo 176×99 fmt `0x3f`, quad de 6 índices. No draw 446 do frame 1489 a fonte
  352×198 (`000002010A4EBC40`) **não tinha imagem na GPU** (`lv 0 -> 0x0`, `layers 0`). No draw 447 a fonte 176×99
  tinha. Cada `NvnTexture` tem sua própria imagem (`tex_backend`), e nada compartilha imagens entre objetos com a mesma
  memória guest. Se o jogo renderiza num objeto e amostra outro na mesma memória, ou se o objeto é recriado, a amostra
  vem da memória guest (preta), não do render. Isso explicaria flashes pretos. **Hipótese, ainda não confirmada.**
- Diagnóstico novo (`textures.c` `shared_memory_note`): `[tex] new <render-target|sampled> image for … while …
  covers the same guest memory and is <GPU-rendered|uploaded from guest memory>`, uma vez por par, no máximo 40. As linhas
  `[trace]` agora também trazem o endereço guest de cada textura, `(no GPU image yet: filled from guest memory)` / `(from
  guest memory)`, e os objetos/endereços dos render targets (`[trace]   rt c0 …`). Limite: 96 draws, 12 por frame.
  Build `build/win-native` OK; não verificado no jogo.
- Pedir ao usuário: rodar com `EOWRECOMP_VK_TRACE_FS=72ef15f300` até ver um flash e enviar as linhas `[tex] new …`, as
  `[trace]` (incluindo `rt`) e a hora aproximada do flash.
- **Flash preto: causa identificada** (`<test-install>/EowOutput.txt`, 23/11 09:22; o usuário viu o flash logo ao entrar e
  andar). No frame 1496, draw 447, a cadeia de blur (fs `72ef15f300`) amostra o objeto 352×198 fmt `0x3f`
  `…CE070` (`728c16e000`) **sem imagem na GPU**: nenhum draw tinha renderizado nele. No frame 1497 ele já tem imagem.
  Não há linha `[tex] new sampled image … covers the same guest memory`, então não é um segundo objeto na mesma
  memória. As linhas `[tex] new render-target …` do log são só a resolução dinâmica recriando os alvos de cena (1408×792
  → 1472×828 → 1600×900 → 1920×1080 → 1408×792). Nesse intervalo (t=10–12 s) **1684 draws foram pulados** porque o
  programa ou o pipeline ainda compilava. O passe que produz a fonte 352×198 foi pulado, e o blur leu uma imagem
  preenchida com a memória guest (preta).
- **Correção** (`exec.c` `compile_wait`): draws de tela cheia (≤ 6 índices/vértices, 1 instância: cadeias de
  bloom/blur/downsample, tone mapping, quads de UI) esperam o próprio build em vez de serem pulados. O job vai para a
  frente das filas de tradução e de compilação (`jobs_bump`), com no máximo 400 ms de espera por frame. A geometria da
  cena continua sendo pulada enquanto compila. `EOWRECOMP_VK_SKIP_QUADS=1` volta ao comportamento antigo. Logs:
  `[vk] full-screen draw waited N ms …` e `[vk] draw skipped, … building: … -> c0 … addr …` (os demais pulados,
  primeiros 32). A/B mais forte: `EOWRECOMP_SYNC_SHADERS=1`, que nunca pula. Build `build/win-native` OK.
  **Não verificado no jogo.**

## Sessão 16c (2026-11-23): árvores ausentes em Hyrule Town (investigação, sem mudança de renderização)

- Referência visual (Eden): duas árvores grandes ao lado da fonte. No Recomp falta o objeto inteiro: copa, tronco,
  raízes e também a **sombra** no chão. O "apoio invisível" do pássaro é a copa de uma delas. O pássaro pousa na
  altura da copa, então a lógica do jogo conhece a árvore.
- Log do usuário (`EowOutput.txt` 09:31, Vulkan Configurator, `EOWRECOMP_SYNC_SHADERS=1`): 0 erros de validação, 0
  draws pulados, nenhuma falha de tradução. O único `texture slot samples the dummy` é o fs `72e92a9000` t6
  (`c[0x90]`, pede cubo e recebe a depth 1920×1080 D24S8 `…35BE10`); não há relação demonstrada com as árvores.
- Reprodução roteirizada (cópia do save, `EOWRECOMP_INPUT='2000:A:12,2600:A:12,3600:A:12,5200:UP:12,5400:A:12,5700:STICK_UP:220'`,
  frame 6100 = praça da fonte): as árvores também faltam.
- Frame de depuração 6090 (`EOWRECOMP_DEBUG_FRAME`, 514 draws, 221 dumps) + diff linha a linha dos dumps: um objeto de 7
  malhas (23016, 432, 5172, 180, 4200, 16116, 180 índices) é desenhado três vezes e **não muda nenhum pixel**:
  - pré-passe de depth (`vs=72a5c2b000 fs=72a5c2c200`, `ds 010102`);
  - shadow map 1024×1024 (mesmos shaders);
  - passe de cor com EQUAL (`vs=72a5c0b600 fs=72a5be5300`, `ds 010003`).
  Como a grama aparece onde a árvore deveria estar, o pré-passe também não gravou depth ali.
- `EOWRECOMP_VK_EQUAL_TOL=1`: árvores continuam ausentes (não é diferença de depth entre pré-passe e cor).
- O VS `72a5c2b000` (GLSL) lê três linhas de matriz da memória global (LDG) no endereço `c0[0x110]` (descritor do
  storage buffer 0 do estágio de vértice) + offset; um endereço fora do heap importado lê 0.
  `EOWRECOMP_VK_TRACE_FS` (agora com storage buffers): `vs s0 72xxxxxxxx+20000`, sempre "may be written by a shader in
  flight", ou seja, o buffer de matrizes se sobrepõe a uma faixa que um shader com STG (compute) gravou numa submissão
  ainda pendente.
- Hipótese principal (não provada): as matrizes que o VS lê de `s0` estão zeradas ou erradas (produzidas por um compute,
  ou offsets lidos de atributos mal decodificados), e o objeto colapsa nos três passes. Descartados com evidência: draw
  pulado, falha de compilação, EQUAL, erros de validação.
- Diagnóstico novo no trace (só log): linhas `[trace]   vs sN …` para shaders que leem memória global, e
  `last written by cs <addr> in frame <n> (its storage …)`, a partir de um anel com os últimos 64 ranges gravados por
  dispatches com STG.
- As duas últimas execuções roteirizadas rodaram a ~2700 fps (janela sem apresentar: minimizada ou coberta) e não
  chegaram ao gameplay; ainda falta a captura com o nome do compute.
- Próxima captura (usuário): religar com o `eowruntime.a` novo, rodar com
  `EOWRECOMP_VK_TRACE_FS=72a5c2b000,72a5c0b600` e `EOWRECOMP_SYNC_SHADERS=1`, ficar na praça da fonte uns segundos e
  enviar as linhas `[trace]` (principalmente `vs s0` e `last written by cs`).

- **Captura com o nome do compute (frame 5667):** o `s0` das árvores é exatamente o `sb1` do compute
  **`cs 72a4711000`**, despachado no mesmo frame (1–3 grupos; `sb0` 128 KB de entrada, `sb1` 128 KB de saída, `sb2`/`sb3`
  parâmetros de 0x28/0x50 bytes). O compute lê e grava pela cópia rápida (GMap/GFast) e usa `atomicAdd`. Com o heap
  importado, o resultado volta ao `gmem_buf` com barreiras completas (`gfast_finish`), e o usuário já tinha visto o apoio
  do pássaro faltar também com `EOWRECOMP_VK_NO_HOST_IMPORT=1` e no RenderDoc. Isso aponta para o **conteúdo** que o
  compute produz, não para a ordem. Ainda não provado.
- Diagnóstico novo: `EOWRECOMP_VK_CHECK_CS=<endereço>` espera a GPU após um dispatch desse compute (cerca de um a cada
  300 frames, no máximo 40) e registra `[checkcs]` com, para cada storage buffer, palavras, quantas são não nulas,
  quantos inf/NaN e as primeiras palavras. Só diagnóstico: trava a cada amostra.
- As execuções roteirizadas seguintes não apresentaram quadros (filme em laço, sem linha `[vk] t=`; tela desligada ou
  janela sem apresentar). A amostra `[checkcs]` fica para o usuário: `EOWRECOMP_VK_CHECK_CS=72a4711000`, ficar ~20 s
  na praça da fonte e enviar as linhas `[checkcs]`.

- **`[checkcs]` do usuário (VKConfig, 10 amostras, frames 1066–3766): `sb1` do `cs 72a4711000` sai 100% zerado** em
  todas. `sb0` (entrada) tem 542–578 palavras não nulas com matrizes (1.0, ±0.707, translações ~300), e o contador
  `sb2[1]` fica 0. Portanto o VS das árvores lê matrizes zeradas e o objeto colapsa nos três passes. **Causa da
  ausência das árvores: confirmada.** Causa do zero: ainda não.
- O compute é um passe de culling + compactação na GPU: cada thread testa uma instância (registro de 64 bytes em
  `sb0`, três linhas de matriz), e as sobreviventes reservam vaga com `atomicAdd` em `sb2+4` (contador de instâncias
  dos argumentos de draw indireto em `sb2`: `0x4e0` = 1248 índices, `0x540` = 1344) e copiam o registro para `sb1`.
  `sb3` = parâmetros (palavras 0–11 parecem uma AABB −1.6..1.6 / 0..3 / −1.5..1.5 e a posição da câmera
  296.8, −11.8, 208.3). Nenhuma thread chega ao store: todas saem antes do `pc d98`. O primeiro teste
  (`pc 50`) sai se `workgroup*32 + lane >= sb3[16]`; a palavra 16 ainda não foi vista.
- **Defeito confirmado no tradutor** (não é a causa do zero, pois vem depois do teste; mas quebra a compactação
  assim que alguma thread passar): `S2R` traduz `SR_LANEID` (0x00) como `0u` e `SR_LTMASK` (0x39) cai no default `0u`.
  Esse compute elege a lane mais alta do ballot para o `atomicAdd` (`r4 == laneid`) e usa
  `popcount(ballot & ltmask)` como posição. Correção proposta (ainda não aplicada, a pedido do usuário):
  `gl_SubgroupInvocationID` e `gl_SubgroupLtMask.x` no GLSL (e EQ/LE/GT/GE 0x38..0x3c), com teste em
  `tools/test_translate`.
- `[checkcs]` agora imprime buffers pequenos inteiros (≤ 64 palavras) e as 16 primeiras palavras dos grandes. Próxima
  captura: a mesma (`EOWRECOMP_VK_CHECK_CS=72a4711000`), para ver `sb3[16]` (a contagem) e o primeiro registro de
  `sb0`. Minhas execuções roteirizadas voltaram a rodar a ~2500 fps sem apresentar quadros e não servem.

- **2ª captura (Hyrule Town, frame 1688):** `sb3[16]` = `0x44` = 68 instâncias (3 grupos = 96 threads), `sb3[17]` = 3
  (os três blocos de 5 palavras de args em `sb2`). A contagem está certa: o teste de culling rejeita todas. O teste é
  frustum em clip space (cantos da caixa local `sb3[0..2]`/`sb3[4..6]`, transformados pela matriz da instância em
  `sb0`, comparados com ±w), e a view-projection vem de **`c4` palavras 0–15**. `sb3[8..14]` parece uma segunda caixa
  em mundo (297..327, −11.8..23.2, 222..257.5).
- `[checkcs]` agora imprime os cbufs lidos (`c4`) e refaz o teste na CPU (`CPU frustum test: N instances, X visible
  (c4 rows), Y visible (c4 columns); GPU wrote counter Z`), nos dois layouts de matriz. É uma aproximação feita a
  partir do GLSL e pode faltar um teste extra (a caixa `sb3[8..14]`, distância).
  - X ou Y > 0 com Z = 0: a tradução do shader está errada.
  - X = Y = 0: a entrada (`c4`) está errada.

- **3ª captura (frame 1669):** 25 instâncias. A CPU também acha 0 visíveis e a GPU gravou contador 0: concordam.
  `c4` é a view-projection da câmera atual: o centro da caixa `sb3[8..14]` (312, 5, 226) projeta no meio da tela, e
  a faixa visível em x (~298..326) bate com `sb3[8]`/`sb3[12]` (= a área visível). A instância 0 (x = 340.5) está
  mesmo fora. Para esse conjunto, o zero **pode estar correto**. Mas o shader roda várias vezes por frame (conjuntos
  diferentes) e o `[checkcs]` só pegava o primeiro dispatch do frame. Agora pega até 8 dispatches do frame amostrado
  (10 frames) e lista as posições das instâncias. Falta ver o conjunto das árvores.

- **4ª captura (frame 1433, 6 dispatches do mesmo frame): causa confirmada.** No 4º dispatch (20 instâncias), a CPU
  acha **3 visíveis** (com `c4` como linhas; o outro layout dá 0, então é o do jogo): instâncias em (306, 223.5),
  (318, 223.5) e (318, 217.5), junto da fonte (centro da vista ≈ 312, 226). A GPU gravou contador **0**, mas `sb1`
  tem **60 palavras não nulas fora do slot 0**: as sobreviventes gravaram, só que no slot errado, e ninguém
  incrementou o contador.
  - É exatamente o defeito de `S2R`: com `SR_LANEID` = 0, a eleição do líder (`lane mais alta do ballot == laneid`)
    falha em todas as lanes. Nenhuma lane faz o `atomicAdd`, `r5` fica com o popcount do ballot (3), e com
    `SR_LTMASK` = 0 todas as lanes caem no slot 3.
  - O draw das árvores lê contador 0 e matrizes zero nos slots que usa, e o objeto colapsa.
  - Correção proposta (aguardando OK do usuário): em `translate.c` (S2R, GLSL), `0x00` → `gl_SubgroupInvocationID`,
    `0x38..0x3c` → `gl_Subgroup{Eq,Lt,Le,Gt,Ge}Mask.x` (o shader já usa ballot `.x`, ou seja, warps de 32), com
    casos dirigidos em `tools/test_translate.c`. O caminho MSL fica como está.
- Na saída: `UNASSIGNED-Threading-MultipleThreads-Write`. `vkDeviceWaitIdle` (handler de saída) roda ao mesmo
  tempo que `vkQueueSubmit2`/`vkQueuePresentKHR` em outro thread. É uma corrida só no fechamento; não investigado.

- **Correção aplicada (OK do usuário):** `translate.c` S2R, só no GLSL: `SR_LANEID` (0x00) → `gl_SubgroupInvocationID`,
  `0x38..0x3c` → `gl_Subgroup{Eq,Lt,Le,Gt,Ge}Mask.x`. O MSL continua emitindo 0, sem mudança. Teste novo
  `s2r_cases` em `tools/test_translate.c`: 6 casos (S2R r8 + STG.E.128 do próprio compute) com o builtin presente,
  compilando, validando e sem mudar o MSL; os 6 passam. Fuzz `REF=HEAD tools/test_translate.sh 4000 8 both`: GLSL
  2681 compilados+validados, 0 falhas; extensão 4000/0; MSL 105 diferenças, as mesmas palavras SUST de antes (o MSL
  não mudou). Build `build/win-native` OK (11:29). O cache SPIR-V é chaveado pelo texto GLSL, então o shader novo é
  recompilado sozinho, e as receitas do warmup continuam válidas. **Não verificado no jogo:** conferir as árvores da
  praça e o apoio do pássaro. Com `EOWRECOMP_VK_CHECK_CS=72a4711000`, o dispatch das árvores deve mostrar
  `GPU wrote counter 3` com `3 visible`.

- **Depois da correção do S2R (captura do usuário, frame 1323):** o culling passou a funcionar. No conjunto das
  árvores, CPU 3 visíveis e GPU contador 3. `sb1` tem registros corretos (o primeiro rotacionado 180° em
  318, 7.5, 223.5) e `sb2` = `{0x5f4 = 1524 índices, 3 instâncias, 0, 0, 0}` + `{0x540, 3, …}`: argumentos de **draw
  indireto** indexado e instanciado. As malhas de 23016 índices que eu suspeitava eram outro objeto. As árvores
  continuaram ausentes porque o backend Vulkan **descartava** `CMD_DRAW_ELEMENTS_INDIRECT` / `CMD_DRAW_ARRAYS_INDIRECT`
  (sem `case` em `exec.c`).
- **Implementado (`exec.c` `draw_indirect`):** lê os argumentos da memória guest depois de a GPU terminar de
  gravá-los. Se um shader em voo pode cobri-los (`gstore_overlaps`), espera, como `gstore_sync_cbufs`. Depois desenha
  pelo caminho direto (`{modo, tipo, contagem, índices + firstIndex*tamanho, baseVertex, baseInstance, instâncias}`;
  arrays `{modo, first, contagem, baseInstance, instâncias}`). Contagem 0 não desenha nada. Log `[vk] indirect …
  draw: args …` (16 primeiros) e `| indirect draws N (M empty, K waits)` na linha `[vk] t=`. Continuam sem suporte:
  `DispatchComputeIndirect` e `MultiDraw*IndirectCount` (no-ops). Build `build/win-native` OK (11:35). **Verificado no jogo pelo usuário (Vulkan): as árvores da praça voltaram.**

## Sessão 16b (2026-11-23): aquecimento de shaders e pipelines (warmup)

- Motivo: com `EOWRECOMP_SYNC_SHADERS=1` não há flash preto (confirmado pelo usuário), mas cada primeiro encontro
  trava o jogo. O padrão assíncrono pulava draws em toda sessão, mesmo com os caches quentes (`<test-install>/cache`:
  1861 `.spv`, `vk_pipelines.bin` de 78 MB).
- O que cada cache poupava (auditoria):
  - `cache/spirv` (`shaders.c`, chave = texto GLSL): só o glslang. A tradução Maxwell→GLSL rodava em toda sessão, no
    único thread tradutor.
  - `cache/vk_pipelines.bin` (cache do driver): deixa `vkCreateGraphicsPipelines` mais barato, mas a chamada continua
    acontecendo no primeiro uso, depois que o programa existe.
  - `cache/glsl` (WIP do usuário, `save_glsl_source`): só diagnóstico.
  - O rascunho `PipeNote` / `pipe_note_save` gravaria a `PipeKey` crua (com o ponteiro `Prog*`) e nunca foi chamado.
    Substituído pela serialização campo a campo abaixo. Os hashes de fonte GLSL, `cache/glsl`, `PIPE_TAB 65536` e
    `pipe_key_hash` ficaram como estavam.
  - Em resumo: o primeiro encontro em cada sessão ainda custava tradução + leitura do SPIR-V + criação do módulo +
    criação do pipeline, e o draw era pulado enquanto isso.
- Opções:
  - (1) Traduzir no Setup: impossível sem um parser dos arquivos de shader do RomFS. O código guest só existe na memória
    depois que o jogo o carrega.
  - (2)/(3) **Escolhida:** gravar receitas na sessão e reconstruir na inicialização, com o código guest guardado no
    cache local do usuário.
  - Triforce não foi inspecionado.
- Implementação (`exec.c`, módulo "shader warmup" abaixo de `build_pipeline`; `translate.c`; `device.c`; `shaders.c`):
  - `maxwell_code_extent` (`translate.c`): os bytes que o tradutor lê (cabeçalho, SPH e código até a palavra que encerra
    a varredura). A varredura passou a ser uma função só (`code_scan`), usada pelos dois. Teste novo em
    `tools/test_translate.c`: cada programa é retraduzido a partir de uma cópia com exatamente esses bytes, e de outra
    com lixo depois deles. 4000 + 2000 programas, 0 diferenças.
  - Identidade de programa = hash do conteúdo desses bytes (VS e FS) + chaves de shader. Endereços guest mudam entre
    execuções; o conteúdo não.
  - `<cache>/warm/code/<id>-<bytes>.bin`: o código guest. São dados do jogo: ficam só na pasta de cache do usuário,
    nunca no repositório.
  - `<cache>/warm/recipes.bin`: cabeçalho (magic `EOWWARM`, schema 1, tamanho do registro, vendor/device da GPU) +
    registros de 808 bytes: ids, tamanhos, `ShaderKey` VS/FS e todos os campos da `PipeKey`, serializados campo a campo,
    sem ponteiros nem handles, com checksum. A gravação vai para um arquivo temporário renomeado no lugar (na saída, a
    cada 30 s com mudanças, e antes do `_exit` dos testes roteirizados).
  - Na inicialização (`vk_warm_start`, após `vk_shaders_init`), cada programa é traduzido a partir do código em cache e
    seus pipelines são criados nas filas de **baixa prioridade** (novas listas em `JobQueue`). `backend_init` espera o
    conjunto (`vk_warm_wait`, até 3 min) e o título da janela mostra `TriRod - preparing shaders N%`.
  - `get_program` acha o programa quente pelo conteúdo; `draw_` acha o pipeline quente pelo mesmo hash de chave. Um
    draw cujo build quente ainda não terminou espera, e o job vai para a frente da fila.
  - Robustez: registro com checksum errado, campos fora da faixa ou formato que a GPU não suporta é descartado.
    Arquivo de outra GPU ou de outro schema é ignorado e substituído. Código ausente ou com hash diferente faz o draw
    cair no build normal. Se, depois de traduzir, o `attr_mask` do shader não bate com o da receita, o pipeline é
    descartado. A versão do tradutor não invalida nada: programas quentes são traduzidos pelo tradutor atual.
  - Chaves: `EOWRECOMP_VK_WARM=0` desliga o aquecimento e a gravação. `EOWRECOMP_VK_WARM_WAIT=0` inicia o jogo sem
    esperar. `EOWRECOMP_CACHE_DIR=<pasta>` muda a pasta de cache (`spirv`, `glsl`, `vk_pipelines.bin`, `warm`); serve
    para testes a frio sem tocar no cache do jogador.
  - Correções achadas no caminho:
    - `draw_` copiava os 8 `pass_color` para a `PipeKey`, incluindo formatos de passes anteriores além de `ncolor`. O
      mesmo pipeline era recriado para cada sobra, e 204 de 316 receitas eram duplicatas inválidas. Agora só `ncolor`
      entradas.
    - O motivo de skip lia o estado de novo depois da espera e contava "pipeline failed" para builds que tinham acabado
      de ficar prontos.
- Medição (Vulkan; GPU model omitted; `eowrecomp.exe` de teste ligado a partir de `build/win-native`; input roteirizado
  `2000:A,2600:A,3600:A,5200:UP,5400:A` com cópia do save; screenshot e saída no frame 7000; entrada no gameplay ≈
  frame 5550; cache numa pasta de rascunho):

  | Execução | Draws/dispatches pulados | Pipelines novos | Pior frame | Gameplay |
  |---|---:|---:|---:|---:|
  | Frio (cache vazio) | 613 (232 prog., 357 pipe., 24 compute) | 179 | 388 ms | ~22 fps |
  | Base: SPIR-V e driver quentes, warmup off (= antes) | 440 (183 prog., 233 pipe., 24 compute) | — | 164 ms | ~22 fps |
  | Warmup (164 receitas) | 26 (1 prog., 1 pipe., 24 compute) | 1 | 155 ms | ~21 fps |
  | Warmup, 2ª vez | 25 (1 prog., 24 compute) | 0 | 176 ms | ~21 fps |

  - Aquecimento na inicialização: 81 programas e 164/165 pipelines em **0,2 s**, com SPIR-V e cache do driver quentes.
    Com o SPIR-V frio não foi medido; será mais lento: glslang por programa.
  - Screenshots de base e warmup iguais (mesma cena e quadro).
  - O pior frame (~150–170 ms na entrada) não é compilação: aparece com 0 pulados. Provavelmente é o carregamento da
    área.
  - Os ~21 fps no gameplay são iguais nos quatro casos (cena pesada: ~480 draws e ~400 passes por frame); não têm
    relação com o warmup.
  - Resta no primeiro encontro: **compute** (24 dispatches pulados em todos os casos; `build_compute` não tem
    receitas), e programas/pipelines nunca vistos antes.
- Observado: a primeira execução a frio parou de produzir frames por volta do frame 8400 (sem trace; morta pelo
  `timeout` de 600 s). As execuções seguintes pararam no frame 7000 e não reproduziram. Não investigado.
- Testes: `REF=HEAD sh tools/test_translate.sh` (4000 e 2000) com os 8 casos SUST, GLSL 0 falhas e extensão 0
  diferenças; as diferenças MSL são as palavras SUST de sempre. Build `build/win-native` sem warnings novos.
- **Precisa de teste do usuário:** religar pelo Setup, jogar uma sessão normal (grava receitas em
  `<test-install>/cache/warm`), fechar, abrir de novo e conferir no log: `[warm] N recipes …`, `[warm] ready in …`, o título
  da janela durante a espera, a contagem `skipped:` e `pipelines: N new, M from warmup` ao entrar nas áreas já
  visitadas, e se os flashes / travadas de primeiro encontro sumiram.

## Sessão 15 (2026-11-22): SUST e erros do Vulkan Configurator no void/rift

- Origem: `<test-install>/EowOutput.txt` (validação ligada) tem 20 × `VUID-vkCmdDrawIndexed-None-06479` (sampler de
  sombra `t4`, `sampler2DArrayShadow` dos materiais principais, com uma view `R8G8B8A8_UNORM`) e 2 ×
  `SYNC-HAZARD-WRITE-AFTER-READ` (a mesma depth do passe amostrada por um draw e depois gravada pelo store do passe).
- **SUST** (`translate.c`): a máscara `0xfff8…` fixava os bits 51 (bound) e 52 (.D tipado) em zero; as formas
  `0xEB28` (bound), `0xEB30`/`0xEB38` (tipadas) caíam em "unknown instruction". Layout conferido no
  `Ryujinx.Graphics.Shader.dll` local (InstTable, InstSust/B/D/DB, EmitSust): bit 51 bound com TidB nos bits 36..48
  (índice de palavra, ×4), bit 52 tipado com tamanho nos bits 20..22. Agora a forma bound P 2D RGBA usa o slot de imagem
  NVN (c[2] 0x120..0x158); as tipadas falham com diagnóstico próprio (não há evidência de uso no jogo; precisariam de
  view inteira do tamanho certo). O jogo só usou `eb20138600f70018` (bindless) nos logs.
- **Bindless SUST** (`exec.c` `stage_set`): o array do pool era gravado com `VK_NULL_HANDLE` nas entradas que não são
  imagem, inválido sem `nullDescriptor`; agora recebem o sink 1×1 2D de armazenamento.
- **Alias de cópia de depth** (`alias_draw_check`): com a origem presa como depth (sem escrita) e o draw amostrando o
  destino, `texture_info` lia o destino via `alias_read` — a própria imagem do attachment — sem snapshot nem quebra de
  passe (o caminho do SYNC-HAZARD). Agora esse destino recebe a cópia real antes do passe; os outros aliases continuam.
  Log: `[vk] copy alias: draw samples …`.
- **Sampler de sombra** (`texture_info` → `shadow_view`): slot sem handle, entrada ausente, view rejeitada ou dummy de
  fallback recebem uma depth dummy D32 = 1.0 (`vk_dummy_depth_view`); textura de cor real é mantida (não sabemos o que o
  jogo espera dela) e só registrada. Log: `[vk] shadow sampler … : <rota> …` (24 primeiras).
- Testes: `tools/test_translate.sh` (agora com `REF=HEAD`; o REF padrão `652e234` não existe mais neste histórico, e
  faltava renomear `maxwell_loop_guard`) — 8000 programas, GLSL 0 falhas de compilação/validação, contagens iguais às de
  antes; as 159 diferenças MSL são todas programas com palavra SUST bound/tipada que falham nos dois tradutores (só a
  mensagem mudou). Casos SUST dirigidos (8) passam; contra o `translate.c` do HEAD, 5 falham (o bug).
  `tools/test_vk_pipelines.sh 4000 7`: 0 falhas, mas **sem** a camada de validação (o arquivo de configuração do
  Vulkan Configurator bloqueia a camada fora dele). `eowruntime.a` compila nos dois caminhos Windows
  (`build/win-native`, o do `setup/dev_rebuild.ps1`, e o cruzado `build/win`); build no Linux/Metal não feito. Sob o
  `make` do MSYS2 o zig precisa de `ZIG_GLOBAL_CACHE_DIR`/`ZIG_LOCAL_CACHE_DIR` em forma Windows (`cygpath -m`), senão
  falha com `AppDataDirUnavailable`.
- **Rift transparente ao vivo, correto no RenderDoc** (`the local Vulkan Configurator stdout log`, 22/11 21:30). Esse log ainda é do
  runtime antigo: o Setup liga `eowrecomp.exe` a partir de `<test-install>/tools/link-kit/eowruntime.a` (16:48, sem as
  correções), não de `<test-install>/link-kit/` — o `eowrecomp.exe` religado às 21:18 não contém as strings novas.
  O RenderDoc não expõe `VK_EXT_external_memory_host`, então lá o runtime usa o caminho de computação em staging,
  que espera cada dispatch que grava e copia o resultado de volta antes de gravar os próximos draws. Com o import
  (ao vivo), os constant buffers continuavam sendo copiados pela CPU na gravação (`cbuf_info` → `guest_copy`),
  antes de o dispatch que os escreve rodar na GPU: dado velho. Correção aplicada no runtime; o usuário confirmou na sessão 16 que o problema do Rift foi corrigido. A contribuição individual desta mudança ainda não foi isolada:
  `gstore_note`/`gstore_sync_cbufs` guardam as faixas de storage dos shaders que gravam até a submissão terminar, e
  um draw/dispatch que vá copiar um constant buffer sobreposto espera a GPU antes do passe. Log: `[vk] gpu-stored
  memory read as a constant buffer: …`; contagem em `| gpu-stored cbuf waits N` na linha `[vk] t=`. A/B:
  `EOWRECOMP_VK_GSTORE_SYNC=0` desliga; `EOWRECOMP_VK_NO_HOST_IMPORT=1` roda o caminho do RenderDoc sem o RenderDoc.
  O mesmo log tem 06479 também em `t2` (binding 34, `vkCmdDraw`) e 2 × `[tex] type mismatch: want 3 have 1`
  (D24S8 1408×792 amostrada como cubo); ainda em análise.
- **Registro histórico da sessão 15:** naquele momento ainda não havia verificação em jogo. Após a confirmação do Rift na sessão 16, os avisos de validação restantes continuam em teste. Para conferi-los, rodar `setup/dev_rebuild.ps1` ou copiar
  `build/win-native/link-kit/eowruntime.a` para **`<test-install>/tools/link-kit/`**, abrir Setup para religar o runtime
  (conferir que `eowrecomp.exe` contém `gpu-stored memory read`), entrar no void/rift com o Vulkan Configurator (validação + sincronização) e conferir:
  sumiram 06479 e o SYNC-HAZARD; quais rotas aparecem em `[vk] shadow sampler` (se for "colour texture bound", o 06479
  continua e precisa decidir a semântica); se `[vk] copy alias: draw samples` aparece; e qualquer impacto de desempenho.

## Sessão 14 (2026-11-20): Setup gera runtime EXE e game DLL

- O usuário executou `extracted/eowrecomp.exe`; esse EXE estava no diretório errado e não tinha ao lado as dependências
  Windows (`av*.dll`, SDL, shaderc etc.) que o pacote instala junto ao Setup. O launcher, por sua vez, sempre inicia
  `eowrecomp.exe` ao lado de `eowsetup.exe`.
- Fluxo corrigido: Setup compila `extracted/eowgame.dll` e liga o runtime-only `eowrecomp.exe` ao lado de Setup. O
  runtime usa a import library empacotada para chamadas da DLL do jogo. O link do runtime inclui todos os membros de
  `eowruntime.a`, preservando os registros de serviços HLE.
- Setup verifica os IDs de build da DLL e do EXE separadamente para decidir se o build está atualizado. As dependências
  SDL/FFmpeg/shaderc continuam empacotadas ao lado do runtime EXE, onde o carregador do Windows pode encontrá-las.
- Build Windows concluído: o pacote inclui `eowruntime.a`, `eowrecomp.lib`, o Setup, o compilador e o gerador. Setup e
  link kit foram instalados em `<test-install>`; 19 DLLs SDL/FFmpeg/shaderc também estão ao lado do runtime. SHA-256 do
  Setup instalado: `2eafaed7f297649175b9c14ef8ad284c392a928bf10e4ca6542f724e922f3e56`.
- O EXE antigo na raiz e a DLL antiga ainda estão instalados até Setup refazer os dois. Para testar, abrir
  `<test-install>/eowsetup.exe`, escolher **Setup Game Files**, esperar a geração e compilação terminarem e escolher
  **Play**. Não iniciar manualmente `extracted/eowrecomp.exe`; o caminho correto é o runtime na raiz, ao lado das DLLs.
  A compilação pelo Setup e o gameplay desta revisão ainda não foram validados.

## Sessão 13 (2026-11-20): crash durante a inicialização

- `<test-install>/eow-console.log` registra um acesso inválido à página zero em `nn::am::InitializeApplicationInterfaces`,
  logo após o runtime solicitar `lm`, `apm` e `appletOE`. Esses três serviços aparecem como sem implementação no log,
  embora seus registros estejam no código e `am.o` esteja no arquivo `eowruntime.a`.
- Causa provável: o link normal de um arquivo estático omitia os objetos referenciados apenas pelo registro em seção.
  O link do EXE standalone agora envolve `eowruntime.a` com `--whole-archive`, preservando esses registros.
- `eowsetup.exe` foi recompilado e copiado para `<test-install>/eowsetup.exe`; SHA-256 do build e da cópia:
  `431e9f292d6e6787d26fec65d4a3caf203311c6f9b4972c13fd9d8f1a53aefc1`. O jogo não foi iniciado após a correção.
- Próximo passo: abrir Setup e deixar que ele relinke `eowrecomp.exe`; depois iniciar o jogo e conferir se `lm`, `apm`
  e `appletOE` deixam de aparecer como serviços sem implementação. Essa execução ainda precisa confirmar a correção.

## Sessão 11 (2026-11-20): primeira etapa de legibilidade no NVN

- IDs de formato NVN ganharam nomes em `runtime/nvn/tegra.h`, e a tabela de metadados em `runtime/nvn/formats.c`
  agora usa esses nomes e macros que explicitam bytes por texel ou por bloco comprimido. Os comentários registram
  como os metadados entram no cálculo de layout Tegra; esta etapa mantém o mapeamento legado.
- A limpeza será feita em partes pequenas, preservando comportamento e evitando reformatar arquivos que já têm outras
  mudanças locais. Próxima área de foco: nomear os campos/opcodes Maxwell e documentar a tradução entre instrução,
  registradores e estado NVN.
- Não foram executados build ou testes nesta etapa.

## Sessão 12 (2026-11-20): diagnóstico por draw e shader

- `EOWRECOMP_LOG_DRAWS=1` agora emite o resumo existente de estado para cada draw Vulkan, mesmo fora de um frame de
  depuração. Cada linha relaciona os endereços de código dos shaders VS/FS ao draw, aos buffers constantes usados, ao
  viewport, aos alvos e à máscara de atributos. O modo é opt-in porque pode gerar bastante log.
- `docs/DEBUG-GPU.md` explica como ligar o log no Windows e combinar os endereços com os dumps Maxwell e GLSL já
  disponíveis. `EOWRECOMP_DEBUG_FRAME` continua sendo a opção para limitar o resumo a um frame.
- `tools/windows/build.sh` recompilou `eowsetup.exe` e `eowrecomp.exe`; os dois foram copiados para `<test-install>` e
  os hashes dos arquivos instalados conferem com o build local. Setup continua compilando o código do jogo, não o
  executável de runtime.
- A DLL instalada ainda tem ID `15f1ea1499a01c22`, enquanto o runtime recém-compilado espera `4353ba77a29b43ac`; rode
  Setup e deixe-o reconstruir a DLL a partir do ExeFS já extraído antes de iniciar o jogo. Não houve teste de execução.

## Sessão 10 (2026-11-20): setup separado do runtime no Windows

- O alvo `eowrecomp.exe` agora liga sem a tela de setup, o gerador ou o compilador de código do jogo; ele inicia o jogo
  diretamente. O novo alvo `eowsetup.exe` mantém o frontend, extração e builder e inicia `eowrecomp.exe` ao escolher
  Play. O pacote de setup inclui os dois executáveis.
- `gamebuild.c` conserva `.gamebuild/` entre execuções. Chaves de conteúdo permitem reaproveitar a tradução de um NSO
  inalterado e os objetos dos chunks C que não mudaram; o link final da DLL continua sendo atualizado quando necessário.
  Na primeira execução depois desta mudança, fontes C e objetos antigos sem chave são comparados por data com o NSO,
  a fonte gerada e os cabeçalhos do compilador; só unidades que passam nessa verificação migram para o cache novo.
  `gamelib_entry.o` sempre é recompilado porque inclui o `GAMELIB_ID` atual.
- `shaders/` e cache de pipeline Vulkan permanecem etapas próprias do runtime. A preparação atual ainda é sob demanda,
  conforme o jogo usa cada shader/pipeline.
- Verificação feita: `tools/windows/build.sh` compilou e linkou os dois executáveis; análise sintática também passou
  para os alvos `main` game-only/setup, builder e frontend setup. Os executáveis foram atualizados em `<test-install>`
  e os hashes conferem com o build local; a import library também confere. O runtime ainda não foi aberto nesta revisão.
- A DLL existente do jogo (`15f1ea1499a01c22`) tem um `GAMELIB_ID` diferente do build atual (`4353ba77a29b43ac`), então
  o setup pedirá uma atualização da DLL. A compilação deve reaproveitar as unidades antigas que passem na verificação
  de data; as próximas podem reutilizar traduções e objetos pelas chaves de conteúdo.
- Ainda pendente: substituir o aviso de nova versão/link externo por atualização automática com manifesto e hashes, e
  adicionar a janela de detalhes por etapa ao estilo Triforce. O build de `EoWRecompSetup.exe` (o instalador de pasta)
  não foi refeito; para testar diretamente, abra `<test-install>\eowsetup.exe`.

## Sessão 9 (2026-11-20): retorno ao baseline local de shaders

- `references/eowrecomp` no commit local `b3daa34` é o baseline. Os arquivos do tradutor Maxwell e do caminho Vulkan de
  shaders/descritores foram restaurados desse snapshot; a seleção Ryujinx e o protótipo SUST experimental saíram do
  runtime ativo.
- Mantida a separação entre setup e runtime, junto com o estado de ROMFS do baseline. Mantido também o ajuste RGB565
  do baseline e o swap red/blue posterior para os formatos NVN RGB5/RGB5A1 (`0x3A`/`0x3B`) em `textures.c`.
- Os arquivos substituídos antes do snapshot foram preservados em `build/source-before-reference-shader-restore/`.

## Sessão 6 (2026-11-18): tela preta após PLAY

- Reprodução no Windows: a janela do launcher aparece, mas depois de PLAY fica preta. O log mostra renderização contínua
  (Vulkan; GPU model omitted, centenas de draws/dispatches por quadro) e a abertura de `rom:/region_common/movie/rd000_Title.webm`
  falha porque o runtime procurava `extracted/romfs/...` como arquivo solto. O setup atual guarda os dados em
  `extracted/romfs.bin`, sem uma árvore `extracted/romfs/`; assim o vídeo da tela de título não podia ser lido.
- O runtime agora usa o índice RomFS existente para localizar e ler o vídeo dentro de `romfs.bin` através do AVIO do
  FFmpeg, mantendo suporte ao arquivo solto quando ele existe. Build do runtime Windows concluído em
  `build/win-native/eowrecomp.exe`; a correção ainda precisa de confirmação visual no jogo.
- Correção do pacote de teste: a primeira compilação feita fora do ambiente MSYS2 tinha `GAMELIB_ID` vazio, então não
  reconhecia a `eowgame.dll` instalada e mostrava a tela de configuração. O build foi refeito com o PATH UCRT64; agora
  incorpora `15f1ea1499a01c22`, igual ao ID da DLL, sem precisar recompilar/copiar a DLL do jogo.
- Runtime corrigido copiado e verificado em `<test-install>\eowrecomp.exe`. As DLLs de dependência já eram idênticas
  às do build. `eowgame.dll` permanece em `<test-install>\extracted\`, onde o runtime procura o código do jogo.
- O usuário confirmou que esta versão está funcionando e superou a tela preta. A abertura do vídeo pelo `romfs.bin`
  está confirmada no uso; gameplay completo e shaders ainda precisam de validação separada.
- Próximo passo: substituir apenas `eowrecomp.exe` na instalação e conferir se o vídeo/tela de título aparece. Se a tela
  continuar preta, coletar o novo log e seguir a próxima falha; shader SUST e RED permanecem incompletos.

## Sessão 5 (2026-11-18): SUST de imagem bindless

- O erro `eb20138600f70018` decodifica como SUST.P, escrita bindless, tipeless, 2D, máscara RGBA completa. O runtime agora
  traduz essa forma para `imageStore`, cria uma matriz Vulkan de imagens de armazenamento usando os IDs do pool NVN,
  ativa as capacidades Vulkan necessárias e emite uma barreira de memória depois do dispatch.
- Build Windows do runtime passou. Esta mudança ainda não foi testada no jogo; os SUST tipados, tipos de imagem além
  de 2D, handles bound e outras formas de SUST continuam sem suporte. RED assinado também continua pendente.
- Como a tradução de shaders é feita pelo runtime, esse ciclo exige só substituir `eowrecomp.exe`; não exige relift nem
  recompilar `eowgame.dll`. A compilação do jogo não foi executada nesta sessão.

## Sessão 4 (2026-11-18): Windows e instalação local

- `tools/windows/setup.cpp` cria o app independente `EoWRecompSetup.exe`. Ele recebe o arquivo NSP, encontra `prod.keys`
  ao lado dele (ou em `%USERPROFILE%\.switch`) e só pede a chave se não encontrá-la; copia o payload para a pasta de
  instalação e inicia o setup no `eowrecomp.exe`.
- O runtime (`runtime/services/fs.c`) abre somente `extracted/romfs.bin`; NSP/XCI/chaves são lidos no setup. A cópia
  antiga foi migrada: `romfs.bin` local de 8 033 071 180 bytes; `romfs.src` removido após a extração.
- O setup separado pode ser reaberto com a pasta de jogo vazia para recompilar a partir de `extracted/exefs/`. A
  geração Python e compilação Zig produziram `extracted/eowgame.dll` (326 666 752 bytes) nesta sessão; `.gamebuild/`
  conserva 1 811 fontes C, objetos e logs. Essa recompilação usou apenas ExeFS extraído, sem reabrir o NSP.
- `tools/windows/package.ps1` cria `build/win/dist/EoWRecompSetup/` com o app de setup e `payload/`. O código do
  jogo gerado pelo usuário não entra no payload.
- A compilação repetível do runtime e o pacote de Setup foram concluídos. O jogo ainda precisa de uma execução completa
  com a nova pasta de saída para validar gameplay no Windows; os erros de shader relatados antes continuam pendentes.

## Jogo fixado (único build suportado)

| | |
|---|---|
| Título | The Legend of Zelda: Echoes of Wisdom |
| Title ID | **01008CF01BAAC000** |
| Versão | **v0 (0x0)**, display version **1.0.0**, sem update (partição update do XCI vazia) |
| Requisito de sistema (CNMT) | 16.0.0 (0x40000000) |
| SDK | NintendoSDK 16.2.0 (program NCA: SDK 16.2.0.0, key generation 0x11) |
| Program NCA | `0449fda9301caaad241256ec54b3c89f.nca` (5 804 949 504 bytes) |
| Nome interno do `main` | `Edward.nss` |

Build IDs dos módulos (endereços de patches/HLE valem só para estes):

| Módulo | Build ID | Arquivo |
|---|---|---|
| main | `a581406bc985865113d18cb19f11d5448ceea783` | 26 093 400 B |
| sdk (nnSdk) | `3053b22935e32ddf15ef81997603e5c267534a90` | 6 215 109 B |
| subsdk0 (glslc) | `e0d8adcc446cf14d6af1a51193f72e30ed3db9ac` | 5 392 097 B |
| subsdk1 (multimedia) | `ff0149d0b5e882b87c2f06a981302b130c1edb32` | 3 436 212 B |
| rtld | `2f1967113a281280a48ce3fc6dc23049327bf924` | 7 358 B |

RomFS: 10 963 arquivos, 8 032 371 686 bytes (todos com magic válido: FRES, BNTX, MsgS, FSTM, SARC, FSHA, webm…).

## Sessão 3 (2026-10-20): macOS e Android

**Bloqueio**: o NSP entregue nesta sessão (`~/Downloads/data`, ver memória do agente) está **truncado**: 5 637 144 576
bytes de 5 806 819 063 (faltam 162 MiB, cópia interrompida em exatamente 5376 MiB). O ExeFS (código) e a tabela de
compressão do RomFS ficam justamente no fim do NCA: nada do jogo pôde ser extraído, gerado ou rodado no Mac. Um vigia
em segundo plano retoma sozinho quando um NSP íntegro aparecer na pasta. **O usuário precisa copiar o NSP de novo.**

### Feito e verificado (sem os arquivos do jogo)
- **macOS = Vulkan via MoltenVK** (padrão; `MACOS_BACKEND=metal` mantém o renderer do LAS, sem as correções do EoW).
  Portabilidade (`VK_KHR_portability_enumeration`/`_subset`). Tela inicial renderizada no M1 (MoltenVK 1.4.2).
  `tools/macos/dev_deps.sh`, `docs/BUILD-macos.md`.
- **Leitura do pacote no runtime** (`runtime/nxpkg.c`): NSP/XCI/NCA com as chaves do usuário, AES (ARMv8 AES no CTR),
  XTS, LZ4 e BucketTree em C. O setup extrai só o ExeFS (~40 MB) e lê o RomFS direto do pacote (`romfs.src`): sem
  `romfs.bin` de 8 GB. `tools/test_nxpkg.sh`: vetores FIPS-197 / SP 800-38A / IEEE 1619 e blocos LZ4; com o NCA real:
  cabeçalho e cabeçalho do RomFS corretos. `nca_extract.py` lê NSP e recusa pacote truncado.
- **Android** (`android/`, `docs/BUILD-android.md`): app Gradle/CMake (NDK 27.2, volk, SDL3, FFmpeg LGPL, shaderc),
  camada de plataforma e JNI (do LASRecomp-Android-Plus), deslocamento do layout do guest (`GUEST_SHIFT`,
  `gen.py --shift`), setup pelo seletor de pastas do sistema lendo o pacote por *file descriptor* (sem copiar o
  jogo), controles de toque (`runtime/ui/touch.mm`), ciclo de vida (superfície recriada ao voltar do segundo plano),
  swapchain pré-rotacionado, afinidade de núcleos. `tools/android/build_game.sh` gera o APK pessoal com o código do
  jogo. **Verificado no emulador Android 15 arm64** (AVD próprio `eow_test`): tela inicial em paisagem, pré-rotação
  90°, seletor → JNI → leitura do pacote (pacote de teste truncado: mensagem certa), fundo/frente.
- **Código do jogo**: `Cpu *__restrict` nas funções geradas e `FN_ENTER` compilado fora (`FNPROF=1` o traz de volta):
  ~10% menos instruções no LAS-Android. **Ainda não medido no EoW.**
- **Tradutor Maxwell**: RED S32 min/max e INC/DEC (laço CAS no GLSL); auxiliares do GLSL renomeados (`mw_*`) — no
  MoltenVK `popcount`/`clz`/`fmin` colidiam com built-ins do Metal (1015 de 3200 shaders do fuzz falhavam; agora 63
  de 5882, todos artefatos do método: textura 1D, atributo 31). `tools/test_moltenvk.sh`.
- **Release macOS**: `make app` → `build/EoWRecomp.app` (FFmpeg LGPL de `tools/build_deps_macos.sh`, MoltenVK + loader
  embutidos e forçados, gerador + zig para compilar o código do jogo na máquina do usuário). Tela inicial ok.
- gfast (cópias de buffers antes de compute) só em GPU dedicada por padrão (memória unificada: só custo). Não medido.
- **Build do código do jogo no celular** (`tools/android/ondevice/`, `-PonDeviceBuild=true`): CPython 3.14 Android +
  capstone/unicorn/lz4, zig 0.17 (clang 22 + lld), kit do NDK; `gamebuild.c` com ramo Android (geradores um por vez,
  compiladores com nice e controle de memória, link com ld.lld contra `libmain.so`, instalação no armazenamento
  interno), `BuildService` (serviço em primeiro plano). **No emulador**: zig roda da pasta de bibliotecas nativas, o
  Python importa capstone/unicorn/lz4 e roda `gen.py`, `gamelib_entry.c` compila e linka contra `libmain.so`.
  `check_public_apk.sh`: APK de 79 MB sem código do jogo.
- **Lifter no ARM64** (`test_lift.py --synth`, sem NSO): 15 000 codificações sintetizadas contra o Unicorn; achou e
  corrigiu um crash (LDR literal SIMD opc 3) e C com UB em formas UNDEFINED de 32 bits. O Unicorn 2.1.4 erra
  LDSMAX/LDSMIN de 8/16/32 bits (compara sem sinal): `tools/test_lse.c` confere as 9 operações LSE x 4 tamanhos contra
  o próprio M1 — todas certas.
- Botão Voltar do Android abre o menu.
- **Release alpha_1** (2026-10-21, pré-release no GitHub, pedida pelo usuário mesmo sem o jogo rodado no Mac/Android):
  `.app` para **macOS 15+** — o loader do Vulkan, o shaderc e o Python do gerador (python-build-standalone) agora vêm
  de `build_deps_macos.sh` (os do Homebrew exigiam macOS 27); `bundle_macos.sh` confere todos os Mach-O. APK assinado
  com chave de release própria (fora do repo: `android/keystore.properties` → `~/.eowrecomp-signing/`, **guardar**:
  sem ela, as próximas versões não instalam por cima) e com o build no celular; `check_public_apk.sh` ok; tela inicial
  vista no M1 (Vulkan/MoltenVK) e no emulador Android 15. A tela inicial agora consulta as releases do repositório
  (`alpha_<n>` < `beta_<n>` < `v1.x`).

### Próximo passo (assim que houver um NSP íntegro)
1. `tools/venv/bin/python -I tools/nca_extract.py <prod.keys> <nsp> extracted --no-romfs --romfs-image` (ou setup pela
   tela inicial), `make gen`, `make -j8`, rodar no M1: abertura, primeira cena, áudio, save. Medir fps.
2. Comparar -O1 x -O2 no código do jogo no M1; medir o efeito do `restrict`/`FN_ENTER`.
3. SUST (imagens de armazenamento: tradutor + descritores) com o shader real `eb20…`.
4. Android: `build_game.sh --apk` e rodar no emulador (arm64 nativo no M1) e num celular. Em drivers sem
   `VK_EXT_external_memory_host` (comum no Android) a memória global dos compute shaders lê zero: precisa de um caminho
   por cópia.

## Marco atual (sessão 2): M4 concluído, M5 parcial (Linux / Vulkan)

Comprovado por screenshot (`EOWRECOMP_SHOT_AT`, capturas em `docs/screenshots/`): o jogo inicializa, toca a abertura
(masmorra do prólogo, Link e os moblins), entra na **primeira cena controlável** (HUD com corações e itens) e
**responde a input** (input roteirizado `EOWRECOMP_INPUT`: Link anda pelo corredor, entra na sala; prompts
"Swing sword", "Place / Toss" aparecem). Execução de 330 s / 2.819 frames sem travar. Backend testado: **Vulkan no
Linux** (integrated GPU, Mesa ANV). Metal não foi tocado nem testado.

### Funciona
- M1: geração dos 4 módulos sem `Unsupported` (296 mil funções); lifter com LDXP/STXP, CAS/CASP, LSE (testado contra
  Unicorn); jump tables resolvidas também em funções divididas.
- M2/M3: boot pelo `nn::init::Start` do SDK 16.2 (3 callbacks), sm via TIPC, fs (RomFS original), acc, hid, am, time,
  audren REV11 (abre e é aceito pelo SDK), threads, mutex/condvar com a semântica do Horizon.
- M4: imagem correta da abertura. M5 (parcial): primeira cena jogável com input.

### Corrigido nesta sessão (cada um com a causa achada no código do jogo / SDK)
- Z-cull com tamanho 0; NVNsamplerBuilder maior que os 0x40 bytes que o jogo reserva (sobrescrevia a pilha);
  memória de comandos NVN (sombra 8x); revisão REV11 do áudio (status de efeito 0x90); `VFormatString` (jump table
  em função dividida); cabeçalho de 0x100 bytes dos compute shaders do NVN novo; tradutor Maxwell: BFE.BREV,
  HSETP2/HSET2 imediatos, VOTE, RED, LEA.HI, FLO.SH/.INV (o FLO errado fazia o laço de luzes rodar demais);
  laços estruturados nos shaders; timeout do `WaitProcessWideKeyAtomic` não readquire o mutex (travava a thread
  principal após ~1.200 frames).

### Problemas abertos (por prioridade)
1. **Desempenho de GPU**: ~9–12 fps na abertura numa integrated GPU. O material mais caro (fs `72ee66f300`, laço de
   luzes) ainda domina o passe principal; medir de novo sem outra carga na máquina (as últimas medidas tiveram a
   compilação do Windows em paralelo). Ferramentas: `EOWRECOMP_VK_PROF=1`, `EOWRECOMP_GPUPROF_AT=<frame>`,
   `EOWRECOMP_SKIP_FSHASH=<hash>`, `EOWRECOMP_SHADER_GUARD=<n>`, `EOWRECOMP_SAVE_MSL=1` (GLSL em `build/glsl/`).
2. **Artefato**: faixa escura vertical do topo da tela até o Link na abertura. Próximo passo: `EOWRECOMP_DEBUG_FRAME`
   (imagens por passe) para achar o draw.
3. Compute shaders: a primeira forma de `SUST` bindless 2D está implementada, ainda sem validação no jogo; `RED` com
   min/max assinado e as outras formas de SUST ainda podem pular dispatches.
4. Áudio não verificado a ouvido; save não testado (o diretório monta).
5. **Windows**: o `.exe` cruzado linka (7 exports para a DLL do jogo); a compilação da `eowgame.dll` foi
   interrompida a pedido do usuário em 44%. Rodar `tools/windows/build.sh all` de novo (só recompila o que falta) e
   testar no Windows.

### Como testar (Linux)
- `tools/run.sh` (SECS=60; janela oculta; `SHOW=1` mostra) — log em `build/run.log`, pilha do jogo resolvida
  (`tools/symgame.py`).
- Screenshots: `EOWRECOMP_SHOT_AT=300,600,900` → `build/frames/`. Input roteirizado:
  `EOWRECOMP_INPUT="1550:STICK_UP:150,1820:A:10"` (frame:botão:duração).
- Travamentos: rodar dentro do `gdb` e mandar `SIGINT` (o `ptrace_scope` impede anexar depois).

### Como compilar (Linux, sem sudo)
1. `tools/venv` (Python: capstone, unicorn, lz4, cryptography, pillow).
2. `tools/linux/dev_deps.sh`: zig 0.17.0 (clang 22 + lld, hash conferido) e os pacotes -dev (SDL3, Vulkan, shaderc,
   FFmpeg, VMA) via `apt-get download`, sem instalar nada no sistema.
3. `make gen` (84 s) e `PATH=$PWD/build/bin:$PATH make -j12 DEPS=$PWD/build/deps-linux GENARCH=-mcpu=x86_64_v3`
   (código do jogo: ~1 h na primeira vez; depois só o que muda).

### Pendências conhecidas
- `docs/linux.md` / `docs/BUILD-windows.md` ainda são do LAS.
- Widescreen, ícones de botão, menu sem abas específicas do EoW: M7.
