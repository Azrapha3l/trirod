# Character Stretch Artifact: Handoff Notes

## Corrected symptom

The user clarified that the stretched object is Link/the character, not the terrain. In the screenshots, a narrow dark/orange streak runs from the top edge of the scene to Link. The previous terrain-format explanation was a misread. Do not describe the terrain as the problem again.

## Work completed

- `runtime/nvn/vulkan/textures.c` has the earlier packed NVN RGB5/RGB5A1/RGB565 Vulkan format corrections. TOTK Recomp documents a terrain-map stretch caused by this format family, but the user says that is unrelated to the current character artifact. The RGB565 correction did fix the inverted hearts; keep that as a separate, user-confirmed fix.
- `runtime/nvn/vulkan/exec.c` now has opt-in diagnostics:
  - `EOWRECOMP_DEBUG_VERTEX_LAYOUT=1` logs vertex attribute formats/offsets and the first bytes for shader `0x72ee695400`.
  - `EOWRECOMP_SKIP_VS=<address>` skips all draws using a guest vertex shader; `<address>:<index count>[,<index count>...]` skips only selected indexed draw sizes.
- Windows runtime build succeeds with MSYS2 UCRT64. `build/win-native/eowrecomp.exe` includes these diagnostics. It has **not** been copied to `[local path omitted]`; it is a diagnostic build, not a user fix.
- All diagnostic test processes started in this work were stopped after the frame capture.

## Findings from frame 9000

Reproduce the first playable corridor using the `build/win-native` directory as the working directory and this scripted input:

```powershell
$env:EOWRECOMP_DEBUG_FRAME='9000'
$env:EOWRECOMP_DEBUG_FRAMES='1'
$env:EOWRECOMP_INPUT='2000:A:12,2600:A:12,3600:A:12,5200:UP:12,5400:A:12'
```

The capture is `build/win-native/build/pass_9000_038.png`. Logs are in `build/win-native/character-*.log`.

- A distinct skinned vertex shader, `0x72ee695400` (fragment shader `0x72ee696700`, index count 5574), uses a 28-byte vertex stride. Its inputs are half-float positions (`fmt 0x29`), packed normals (`0x69`), two-component signed normalized data (`0x13`), and an 8-bit unsigned index (`0x03`). Its vertex buffer is 37,268 bytes.
- Skipping shader `0x72ee695400` did **not** remove the long streak. Disabling the vertex cache with `EOWRECOMP_VK_VCACHE=0` also did not change it.
- The long streak and most of Link disappear when all draws using vertex shader `0x72ee694000` are skipped. This is the current best lead: the artifact comes from one or more draw batches using that shader, or from another pass that shares it.
- Skipping only index count `4506` (`EOWRECOMP_SKIP_VS='72ee694000:4506'`) changes Link to an orange-looking fragment and leaves a narrow orange streak. This batch is not the only source. The next target counts for shader `0x72ee694000`, logged at frame 9000, are:

| Index count | Vertex buffer bytes |
| ---: | ---: |
| 3465 | 20,704 |
| 4506 | 37,200 |
| 918 | 6,368 |
| 1320 | 8,288 |
| 2424 | 17,784 |
| 7044 | 60,760 |
| 960 | 9,184 |
| 2604 | 21,312 |

- Skipping count groups `3465,918,1320,2424` and `7044,960,2604` independently left the screenshot/streak essentially unchanged. The count-4506 skip still changes Link's colors but does not remove the streak; draw count alone is not enough to isolate the culprit.
- Suppressing `flip_y` for VS `0x72ee694000` made the streak much thicker and darkened Link, but did not fix the artifact. This confirms the output reacts to the Y convention, but is not evidence that the global viewport flip is the root cause.
- The Vulkan renderer currently applies a negative-height viewport globally. TOTK's Vulkan backend also uses a negative-height viewport for its y-up guest NDC. Triforce's WebGPU renderer instead explicitly builds a viewport transform uniform for its upper-left-origin host convention. These references reinforce that Y conversion must be consistent across viewport and shader position logic; they do not yet identify an EoW-specific defect.
- The logged `0x72ee695400` vertex formats match the corresponding NVN-to-Vulkan mappings in TOTK's `runtime/vk_pipeline.c`. The better current lead remains the other shader `0x72ee694000`; its actual attribute layout and vertex-stage constant buffers still need to be captured.
- Added an opt-in `EOWRECOMP_DEBUG_VS_DETAIL=<guest shader address>` logger to capture one record per unique draw count: index type/address and first indices, full index maximum, index byte count, vertex slot count, active attributes, stream sizes, and first words/floats from each vertex cbuf. It is enabled only when the frame capture logger is active.
- Corrected the logger's NVN cbuf mapping: shader cbuf 6 is bound uniform-buffer slot 3 (`st.ubuf[0][3]`), not slot 6. Discard earlier `vcb6` data in `character-vsdetail*.log`; those lines were mislabeled. `runtime/nvn/vulkan/exec.c` now applies the cbuf 3-to-slot 0 offset when printing shader cbufs.
- A fresh frame-9000 run with `EOWRECOMP_DEBUG_VS_DETAIL=72ee694000` verified **all eight** batches use NVN index type 1 (`UNSIGNED_SHORT`), corresponding to Vulkan `UINT16`. Each full index range stays within its matching interleaved vertex stream, and the max index is exactly `vertex_slots - 1`:

| Index count | Bytes (`count * 2`) | Vertex slots | Full max index | Stride | Position / other observed attributes |
| ---: | ---: | ---: | ---: | ---: | --- |
| 3465 | 6,930 | 647 | 646 | 32 | `0x22` float3, `0x69` packed SNORM, `0x03` R8_UINT |
| 4506 | 9,012 | 930 | 929 | 40 | `0x22` float3, `0x69` packed SNORM, `0x27` RGBA8_UINT, `0x2A` RGBA16_UNORM |
| 918 | 1,836 | 199 | 198 | 32 | `0x22`, `0x69`, `0x03` |
| 1320 | 2,640 | 259 | 258 | 32 | `0x22`, `0x69`, `0x03` |
| 2424 | 4,848 | 494 | 493 | 36 | `0x22`, `0x69`, `0x27`, `0x25` RGBA8_UNORM |
| 7044 | 14,088 | 1,519 | 1,518 | 40 | `0x22`, `0x69`, `0x27`, `0x2A` |
| 960 | 1,920 | 328 | 327 | 28 | `0x29` float4, `0x69`, `0x0F` RG8_UINT, `0x0D` RG8_UNORM |
| 2604 | 5,208 | 666 | 665 | 32 | `0x22`, `0x69`, `0x03` |

The latest log is `build/win-native/character-vsdetail-full.log`; the matching screenshot is `build/win-native/build/pass_9000_038.png`. These checks make an index-width, index-byte-count, or out-of-range-index cause unlikely. They do not rule out a bad vertex attribute semantic/default, a shader translation error, or incorrect bone palette data.

## Corrected cbuf and vertex-data capture

At frame 8900, the corrected logger captured the actual cbuf mapping in `build/win-native/character-cbuf6-correct.log` and the pass images in `build/win-native/build/pass_8900_*.png`.

- For the 3465, 4506, 918, 1320, 2424, and 7044 draws, shader cbuf 6 maps to slot 3 at address `0x727ef7c200`, size 2784 bytes (58 blocks of 48 bytes). For count 2604, cbuf 6 is a separate 48-byte range at `0x727ef8d800`.
- The 4506 batch has stride 40: position `0x22` float3 at offset 0; weights `0x2A` RGBA16_UNORM at offset 12; packed normal `0x69` at offset 20; joint indices `0x27` RGBA8_UINT at offset 36.
- Its first vertex has weights bytes `33 33 cc cc 00 00 00 00`, which decode as UNORM16 weights `0.2, 0.8, 0, 0`; its four index bytes are `6a 03 04 04` (106, 3, 4, 4).
- Generated GLSL for VS `0x72ee694000` uses values from `a7` to calculate dynamic cbuf 6 offsets in 48-byte steps. If those lanes are the skin joint indices, index 106 is outside the 58-entry cbuf 6 range despite a nonzero first weight. This is a strong lead, but still needs confirmation against the full per-vertex attribute stream, the exact shader branch, and RenderDoc or an equivalent draw inspection. It could indicate a bad attribute interpretation, palette/range mismatch, or CPU-side palette write problem.
- The cbuf mapping correction fixed the diagnostic; it has not changed rendering. The game process was stopped after this capture. The latest diagnostic executable remains in `build/win-native/eowrecomp.exe` and was not copied into `[local path omitted]`.

## Reference audit

- **TOTK Recomp** (`references/totk-pc-recomp-0.4.1/runtime/vk_pipeline.c`, `vk_exec_draw.c`): uses the same NVN index values (byte=0, short=1, int=2), a broad explicit NVN vertex-format table, and a negative-height Vulkan viewport for the guest's y-up convention. The EoW formats observed above map to the same Vulkan formats there.
- **Triforce** (`references/triforce-source/gpu/render/rx_draw.cpp`, `rx_shader.cpp`): decodes vertex inputs according to the game's recorded format/offset/stride and falls back to explicit CPU conversion when WebGPU cannot express a layout. It builds viewport/depth conversion data per draw because WebGPU's viewport convention differs. This is a helpful validation pattern, but its WebGPU draw backend is not directly portable to EoW's Vulkan path.
- **Eden mirror** (`references/mirror/src/video_core/renderer_vulkan/vk_rasterizer.cpp`, `fixed_pipeline_state.cpp`, and `src/shader_recompiler`): the Vulkan rasterizer derives Y behavior from the Maxwell window-origin mode, applying a negative viewport height for lower-left origin; depth-mode conversion is handled separately. Its shader frontend is a useful Maxwell instruction-semantics reference, but Eden is an emulator rather than a drop-in NVN recompilation runtime.
- EoW's enum mapping matches the local TOTK/Triforce headers: lower-left origin is `0`, upper-left is `1`; unsigned byte/short/int indices are `0/1/2`. EoW currently flips shader Y for lower-left origin and uses a negative-height viewport. The earlier no-flip diagnostic changed the symptom but did not fix it; reference agreement makes a global Vulkan Y-convention mismatch less likely.
- RenderDoc 1.47.0 was installed from the verified WinGet package and its installer hash matched. The local installation has `qrenderdoc.exe`, `renderdoccmd.exe`, `renderdoc.dll`, and `renderdoc_app.h`, but **does not contain `renderdoc.pyd` anywhere under `[local path omitted] Files\RenderDoc`**. The current Codex session has no RenderDoc MCP server connected. The user reported that the MCP route is not working as expected; do not pursue MCP setup unless they ask again. RenderDoc UI/CLI remains installed for a later manual capture.

## Next steps

1. Scan every vertex in the affected streams, decoding joint indices according to formats `0x03`, `0x0F`, and `0x27`, and decode weights according to `0x0D`, `0x25`, and `0x2A`. Count nonzero-weight indices outside the bound cbuf 6 range.
2. Confirm why the count-4506 first vertex references joint 106 against a 58-entry cbuf 6 range. Compare the buffer range and data against the NVN bind/update commands and the generated shader's dynamic cbuf reads; this will distinguish CPU-side palette writes from Vulkan binding or shader-input interpretation.
3. The user may use RenderDoc manually later. A capture should include the corridor gameplay frame and inspect the count-4506 draw's vertex inputs, cbuf 6 range, and post-VS positions. Resume the shader investigation only when the user is back.

## Build command used

The sandbox blocks MSYS2 startup with `NtCreateDirectoryObject`; an approved elevated tool call was required. From PowerShell, the successful runtime-only rebuild was:

```powershell
$buildScript = @'
export PATH=/ucrt64/bin:/usr/bin:$PATH
cd [local path omitted]
make -j8 B=build/win-native DEPS=build/win/diag-prefix build/win-native/eowrecomp.exe
'@
$buildScript | & '[local path omitted] -s
```

For a capture with a selected mesh batch omitted (previously tried; results were inconclusive):

```powershell
$env:EOWRECOMP_DEBUG_FRAME='9000'
$env:EOWRECOMP_DEBUG_FRAMES='1'
$env:EOWRECOMP_INPUT='2000:A:12,2600:A:12,3600:A:12,5200:UP:12,5400:A:12'
$env:EOWRECOMP_SKIP_VS='72ee694000:3465,918,1320,2424'
& .\eowrecomp.exe *> .\character-skip-group-a.log
```

Run from `[local path omitted]`; stop the game after `build/pass_9000_038.png` is saved. The current working tree already has many user/project changes; do not reset or discard unrelated modifications.
