// Builds game code from the player's own files: the generator turns ExeFS into C and the bundled compiler builds it.
// On Windows, the build tool (runtime/tools/trbuild.c, run by Trirod-Launcher.exe) builds Trirod.dll in <game_dir>
// and links the runtime-only Trirod.exe in the data root. Generated C and objects stay in .gamebuild so later builds
// can reuse completed work.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// What a child process of the build is (GameBuildOpts.tool)
enum { GAMEBUILD_TOOL_GENERATOR, GAMEBUILD_TOOL_COMPILER, GAMEBUILD_TOOL_LINKER, GAMEBUILD_TOOL_SHADER_CATALOG };

typedef struct {
    // now and then, from the calling thread: how far the whole build is (0..1) and what it's doing
    void (*progress)(double fraction, const char *step);
    int jobs;  // compiler processes at once (>= 1)
    // The fields below are for the Windows build tool (trbuild); zero elsewhere (the start screens' builds).
    // Which part runs now and how far it is (0..1): 0 = the generator translating the game code (compiles of its
    // finished files already run), 1 = the remaining compiles and the links. Called with progress.
    void (*phase)(int phase, double fraction);
    // A child process started (exit < 0) or ended (its exit code). argv is NULL-terminated; log has its output.
    void (*tool)(int kind, const char *const *argv, const char *log, int exit);
    bool skip_shader_catalog;  // the caller ran gamebuild_shader_catalog itself, as part of preparing the files
} GameBuildOpts;

// Builds it (a few minutes). false: why says what went wrong (details in the log).
bool gamebuild_run(const char *game_dir, const GameBuildOpts *opts, char *why, size_t why_size);
// The generator and the compiler are there (shipped builds; a source checkout with tools/venv and clang).
bool gamebuild_available(void);
// The portable shader catalog (<game_dir>/shader_catalog: the Maxwell control/code records of the game's BNSH
// archives, tools/shader_catalog.py) extracted with the generator helper. Only reads and copies records: nothing is
// translated or compiled here. true when it exists already.
bool gamebuild_shader_catalog(const char *game_dir, const GameBuildOpts *opts, char *why, size_t why_size);

#ifdef __cplusplus
}
#endif
