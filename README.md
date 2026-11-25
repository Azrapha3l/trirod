# Trirod

**A Windows launcher and static recompilation project for _The Legend of Zelda: Echoes of Wisdom_.**

Trirod is a continuation of [EoWRecomp](https://github.com/Nycolazs/EoWRecomp) and its contributors' work. This repository carries that project forward and develops the Trirod launcher, Windows build and update workflow, and external shader preparation tools.

> [!WARNING]
> Trirod is in early development. The game is playable, but compatibility, performance, and graphics issues are still being worked on. Dynamic resolution can cause visual artifacts, which may be related to shader compilation; this is still under investigation. These artifacts have not appeared with internal resolution set to 1x (native) or higher. A fix is planned for a future release. See [docs/STATUS.md](docs/STATUS.md) for current details.

> [!IMPORTANT]
> Trirod does not include Nintendo game files, game assets, or console keys. You must provide your own legally obtained base game, version **1.0.0** (title ID `01008CF01BAAC000`), and your own `prod.keys` file. The **1.1.0** game update has been tested and is working. Other game updates have not been tested. Instructions for obtaining game files or keys are not provided.

## Run Trirod on Windows

The Windows release is distributed as a tools package. The package contains the launcher, build tools, compiler, runtime dependencies, and shader preparation tool. It does not contain the game or files generated from the game.

### Requirements

- Windows 10 or later, 64-bit x86 CPU with AVX2
- A graphics driver that supports Vulkan 1.3
- Your own compatible Echoes of Wisdom 1.0.0 game files and `prod.keys`

### First launch

1. Download `Trirod-Tools-<version>.zip` from [Trirod Releases](https://github.com/Azrapha3l/trirod/releases/latest).
2. Extract the **entire ZIP** to a writable folder of your choice. Keep the launcher, tools, and DLLs together in that folder.
3. Run `Trirod-Launcher.exe`.
4. In the launcher, choose the folder containing your game files, or choose your `.xci` / `.nsp` package. If you select a package, place `prod.keys` beside it.
5. Select **Recompile**. The launcher shows progress while it prepares the game files, translates and compiles the game code, links the runtime, and prepares shaders for your graphics device.
6. When the build finishes, select **Play**.

The first build can take a while. The generated game code and device-specific shader/pipeline caches are kept locally. They are not uploaded to Trirod or included in releases.

On later launches, run `Trirod-Launcher.exe` again and choose **Play**. Release builds check GitHub for a newer Trirod tools package. The online update path is newly implemented and is still being tested; see [docs/RELEASE-WORKFLOW.md](docs/RELEASE-WORKFLOW.md) for its behavior and current verification status.

By default, game files, saves, settings, and caches are stored alongside the launcher. To keep them elsewhere, start the launcher with `--data <folder>`.

## Building from source

For Windows development builds, prerequisites, and the release packaging workflow, see:

- [Windows build instructions](docs/BUILD-windows.md)
- [Release and update workflow](docs/RELEASE-WORKFLOW.md)
- [Current project status](docs/STATUS.md)
- [Architecture](docs/ARCHITECTURE.md)

A local development build is created with `setup/build.ps1` and staged in `build/setup-dev/`. Run it with:

```powershell
build\setup-dev\Trirod-Launcher.exe --dev
```

Development mode uses the tools in the local build and does not check for or download GitHub updates. For the complete workflow and options such as `--data` and `--recompile`, see the release workflow document.

## Project lineage and credits

- **[EoWRecomp](https://github.com/Nycolazs/EoWRecomp)** — the upstream Echoes of Wisdom recompilation project. Trirod continues this project; this repository is not a clean-room implementation. Thanks to Nycolazs and the EoWRecomp contributors for the original work.
- **[LAS Recomp](https://github.com/peiikai/LASRecomp)** by Peiikai and its contributors (MIT) — the foundation on which EoWRecomp was built. The recompiler, Horizon runtime, NVN implementation, Maxwell shader translator, and Vulkan renderer originated there. The LAS Recomp copyright and license notices are retained.
- **[LASRecomp-Android-Plus](https://github.com/Nycolazs/LASRecomp-Android-Plus)** (MIT) — source of Android platform work and related techniques carried through the project lineage.
- **[Triforce Project](https://git.triforceproject.com/triforce/source)** — a reference for the separate launcher, external build tools, progress reporting, and shader preparation workflow. Trirod's implementation is its own.

Third-party libraries and their licenses are listed in the source tree and their respective license files. Trirod's own code is MIT licensed; see [LICENSE](LICENSE).

## Legal

Trirod is a fan project and is not affiliated with, endorsed by, or sponsored by Nintendo. _The Legend of Zelda_ and _Echoes of Wisdom_ are trademarks of Nintendo. The repository and releases do not include game dumps, decrypted game files, assets, keys, extracted game code, or user-generated game binaries. The repository does contain a limited Maxwell shader catalog used by the shader preparation tools; it does not replace the game files users must provide.
