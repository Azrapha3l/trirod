# Trirod tool progress protocol (version 1)

`Trirod-Launcher.exe` runs the headless tools as child processes and shows what they report. The tools are
`trbuild.exe` (`runtime/tools/trbuild.c`) and `Trirod-ShaderPrep.exe` (`runtime/tools/shaderprep.c`); the C helpers
are in `runtime/tools/protocol.h`, and the launcher's parser is `handle_event` in `runtime/launcher/launcher_win.cpp`.

## Channels

- **stdout, lines starting with `@@`:** events. Each event is one whole line, UTF-8, with no newline inside a value.
  The launcher relies only on these events for stages, percentages, errors and completion.
- **The rest of stdout, and all of stderr:** diagnostic output. The launcher shows it in **Show details** and writes
  it to `<data>\logs\last-build.log`. It never parses diagnostic output for meaning.
- **Exit status:** `0` done, `1` failed, `2` bad arguments, `3` skipped (only `Trirod-ShaderPrep`: no usable
  Vulkan 1.3 device).

A tool that stops before `@@done` failed, even when its exit status is 0. The launcher reports the last `@@error`
text, or the exit status when the tool gave no error event.

## Events

| Event | Meaning |
| --- | --- |
| `@@protocol <n> <tool> <version>` | First line: protocol version, tool name, release tag. |
| `@@stage <id> <title>` | A stage begins. Ids: `files`, `translate`, `compile`, `shaders`. A new stage ends the previous one. |
| `@@progress <0..1> [text]` | How far the current stage is, with optional text for the current activity. |
| `@@tool <name> start <command line>` | The tool starts a child process (`trgen`, `cc`, `link`, `shader-catalog`). For the thousands of compiles, `trbuild` reports only the first command line. |
| `@@tool <name> end <exit> <log>` | That child ended. Its full output is in `<log>`. On failure, the tool copies the last lines of the log to stderr. |
| `@@info <text>` | A result worth showing under the stage after it finishes, such as counts or output paths. |
| `@@warn <text>` | A problem that did not stop the stage. |
| `@@skip <text>` | The stage was not run. `Trirod-ShaderPrep` leaves shader work to the game in this case. |
| `@@check ok <text>` / `@@check bad <text>` | The answer to `trbuild check`. With packages, `ok` names the base and update found by their contents (program NCA, main build ID), not their file names. |
| `@@extracted <none\|current\|stale\|incomplete> <text>` | From `trbuild check`: whether `extracted/` matches the chosen files. `stale` = another base, an update added / removed / replaced, another keys file, or another version than the selection makes; `incomplete` = files missing or still to be finished. The launcher offers **Play** only for `current`. |
| `@@error <text>` | Fatal: the tool stops with a non-zero exit status. |
| `@@done` | Finished successfully. |

## The build the launcher runs

Each command runs with the working directory and `--data` set to the data folder. `PATH` begins with the install folder,
which holds the runtime DLLs.

| Visible stage | Command | Work |
| --- | --- | --- |
| Updating Trirod tools (only when updating) | done by the launcher: WinHTTP download, SHA-256 check, then `%SystemRoot%\System32\tar.exe -xf <zip> -C <stage>` | Download, verify, unpack, and swap the release's tools package |
| Preparing game files | `trbuild.exe files --data <data> [--source <folder or .xci/.nsp>] [--update <.nsp>] [--keys <prod.keys>]` | Finds the player's files and identifies each package by content (base v1.0.0, update v1.1.0; title `01008CF01BAAC000`). Reuses `extracted/` only when its import record (`extracted/import.txt`: the chosen packages' and keys file's path, size and time, and main's build ID) matches the selection; an extraction from before records is reused when its main is the version the selection makes, and the record is written then. Otherwise it extracts ExeFS and the RomFS image (`nx_setup` / `nx_setup_update`, or copy and `romfs_build`) into `extracted/.import`, checks the main build ID, and only then swaps the new files in (the old ones, with their shader catalog, are removed after the swap; a failed import leaves them as they were). Then runs `trgen --shader-catalog`, which **only copies** BNSH control/code records into `extracted/shader_catalog`. |
| Translating game code for PC | `trbuild.exe code --data <data>` | `trgen` lifts each ExeFS module to C. Compiles of finished files already run during this stage. |
| Compiling and linking the generated code | same process (stage event `compile`) | `zig cc` for the remaining files, then links `extracted\Trirod.dll` and `<data>\Trirod.exe` |
| Preparing shaders for the graphics device | `Trirod-ShaderPrep.exe --data <data>` | See `runtime/tools/shaderprep.c` and `docs/RELEASE-WORKFLOW.md`. A failure or skip does not fail the build. |

`trbuild.exe check --data <data> [--source <path>] [--update <.nsp>] [--keys <prod.keys>]` validates a chosen source before
**Recompile** is offered and reports what it found and whether the existing extraction is current for it (`@@extracted`). It reads only
the packages' headers and ExeFS directory; nothing is written.

`--keys` names the player's `prod.keys` wherever it is (RecompLauncher passes its own copy of the shared key file;
the launcher stores it as `keys=` in `trirod-launcher.ini`). Without it, packages need `prod.keys` beside them, or in the
chosen folder. `title.keys` is looked for beside whichever `prod.keys` is used.
