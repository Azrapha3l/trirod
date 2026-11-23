# GPU and crash diagnostics

The Vulkan renderer has focused, opt-in logging hooks. Start with draw summaries: they link each guest draw to the
vertex and fragment shader code addresses, constant-buffer masks, render targets, viewport, and vertex attribute mask.
The code addresses also name the optional shader dumps, so the log can be matched to the translated source.

## Capture shader and draw information on Windows

Open PowerShell in the game's output folder (for example, `[local path omitted]`) and run:

```powershell
$env:EOWRECOMP_LOG_DRAWS = "1"
$env:EOWRECOMP_DUMP_SHADERS = "1"
$env:EOWRECOMP_SAVE_MSL = "1"
.\eowrecomp.exe 2> .\gpu-debug.log
```

PowerShell gives the GUI app a stderr handle, so explicitly redirect it to `gpu-debug.log` in the game folder as shown
above. When launched without a console or redirection, the release executable instead writes stderr to
`eowrecomp.log` beside `eowrecomp.exe`. Draw summaries begin with `[draw]` and include `prog vs=<address> fs=<address>`.

To restrict verbose output to a frame, use `EOWRECOMP_DEBUG_FRAME` instead of `EOWRECOMP_LOG_DRAWS`; the frame number
is the runtime's presented-frame count. Add `EOWRECOMP_DEBUG_DATA=1` only when needed; it also prints the first
available vertex-stream values and fragment constant-buffer values for each logged draw.

The shader artifacts are written under the game's working directory:

- `build/shaders/<address>_vs_code.bin` and `_fs_code.bin`: guest NVN shader blobs used by the translator.
- `build/glsl/<address>_vs.glsl` and `_fs.glsl`: generated Vulkan GLSL before shaderc compilation.
- `[shader] ... translate failed` log lines: translator rejection and guest code address.

For a stretch seen in RenderDoc, locate the `[draw]` line whose vertex/fragment address matches the draw's shader
module, then inspect that address's `_vs.glsl` and `_vs_code.bin`. Save the draw line and adjacent state lines with the
shader source so we can compare the affected pipeline and its inputs. For a crash, preserve the complete log from
startup through the final error; enabling every guest-function trace would produce excessive output and can change
timing, so add narrower subsystem tracing after the failing area is identified.

## Diagnose a missing guest-function target

The recompiler translates AArch64 guest instructions into C, then compiles that C to the host executable format. If
the runtime reports `no function at <address>`, that address can be inside a loaded guest module but not at one of
the recompiler's known function entries. The diagnostic line now includes the guest transfer kind (`blr`, `branch`,
or `bl`), source instruction address, target, LR, and several argument/scratch registers. This distinguishes an
indirect function call from a computed branch before investigating the AArch64 decoder and function-boundary scan.

On Windows, open PowerShell in `[local path omitted]` and run this to capture the failure:

```powershell
Remove-Item .\missing-function.log -ErrorAction SilentlyContinue
.\eowrecomp.exe 2> .\missing-function.log
```

The default behavior still stops at the missing target. To test whether the fault is an indirect call whose callee
is absent from the function table, set the opt-in diagnostic mode before launch:

```powershell
$env:EOWRECOMP_MISSING_FUNCTION = "skip-call"
.\eowrecomp.exe 2> .\missing-function.log
```

This mode returns zero only when the recorded source is an AArch64 `BLR` call and LR matches that instruction's
fall-through address. Missing `BR` targets and unverified transfers remain fatal. A skipped call is diagnostic only:
it may leave game state incomplete, so use the first log line to identify the transfer and then unset the variable.
