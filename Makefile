# EoWRecomp build (derived from LAS Recomp, MIT)
# macOS: Homebrew clang/SDL3/FFmpeg, Metal renderer.
# Windows: MSYS2 UCRT64 shell (see docs/BUILD-windows.md), clang + Vulkan renderer.
# Linux: clang + Vulkan renderer, x86-64 or aarch64 (see docs/linux.md).
# Windows from Linux: WINCROSS=1 (tools/windows/build.sh): zig cc for x86_64-windows-gnu, deps from tools/windows/fetch_deps.sh.
UNAME := $(shell uname -s)
comma := ,
ifeq ($(WINCROSS),1)
PLATFORM := windows
else ifneq ($(filter MINGW% MSYS% CYGWIN% Windows_NT,$(UNAME) $(OS)),)
PLATFORM := windows
else ifeq ($(UNAME),Linux)
PLATFORM := linux
else
PLATFORM := macos
endif

# Linux: ARCH (x86_64 / aarch64, default this machine's) picks the target, separately from PLATFORM. The generated code
# differs per arch (AArch64 hosts run the guest's FP/SIMD as inline asm, others use the portable C of simd.h), so a
# cross build has its own generated-<arch>/ and build/<arch>/; SYSROOT is the target's root filesystem (headers, libs).
GEN      ?= generated
ifeq ($(PLATFORM),linux)
HOSTARCH := $(patsubst arm64,aarch64,$(shell uname -m))
ARCH     ?= $(HOSTARCH)
ifneq ($(ARCH),$(HOSTARCH))
CROSS    := --target=$(ARCH)-linux-gnu $(if $(SYSROOT),--sysroot=$(SYSROOT))
GEN      := generated-$(ARCH)
B        ?= build/$(ARCH)
endif
GENSIMD  := --simd $(if $(filter aarch64,$(ARCH)),asm,c)
endif

# B: build dir. MACOS_MIN: oldest macOS to run on (default: this one). DEPS: prefix with SDL3 / FFmpeg instead of
# Homebrew's (make deps). `make app` sets all three (build/mac15, 15.0, build/deps); plain `make` uses none.
B         ?= build
MACOS_MIN ?=
DEPS      ?=
WIN_DEPS  ?= build/win/deps
MINFLAG  := $(if $(MACOS_MIN),-mmacosx-version-min=$(MACOS_MIN))
CC       := clang $(MINFLAG) $(CROSS)
CXX      := clang++ $(MINFLAG) $(CROSS)
ifeq ($(WINCROSS),1)
ZIG      ?= build/zig-full/zig
CC       := $(ZIG) cc -target x86_64-windows-gnu
CXX      := $(ZIG) c++ -target x86_64-windows-gnu
GENARCH  ?= -mcpu=x86_64_v3  # (zig's spelling of x86-64-v3)
endif
IMGUI    := third_party/imgui
GENFLAGS := -O1 -fno-strict-aliasing -Wno-return-type -Wno-unused-label -fvisibility=hidden -Iruntime
# FNPROF=1: game code with the function-entry profiler's hook (EOWRECOMP_FNPROF; runtime/cpu.h FN_ENTER)
GENFLAGS += $(if $(FNPROF),-DEOWRECOMP_FNPROF)

# The game code (made by `make gen` from your own game files) is its own library next to the app (runtime/gamelib.h);
# the app itself holds none of it
# the game's modules and their guest bases (runtime/modules.h): name:base pairs
GAME_MODULES := $(shell sed -n 's/^ *X(\([a-z0-9]*\), *\(0x[0-9A-Fa-f]*\)ull).*/\1:\2/p' runtime/modules.h)
GEN_SRC  := $(strip $(foreach m,$(GAME_MODULES),$(wildcard $(GEN)/$(firstword $(subst :, ,$(m)))/*.c)))
GEN_OBJ  := $(patsubst %.c,$(B)/obj/%.o,$(patsubst $(GEN)/%,%,$(GEN_SRC))) $(B)/obj/tools/gamelib_entry.o
RT_SRC   := $(wildcard runtime/*.c) $(wildcard runtime/hle/*.c) $(wildcard runtime/services/*.c) \
            $(wildcard runtime/nvn/*.c) $(wildcard runtime/nvn/maxwell/*.c) runtime/platform/$(PLATFORM).c
RT_HDRS  := $(wildcard runtime/*.h) $(wildcard runtime/platform/*.h) $(wildcard runtime/nvn/*.h) \
            $(wildcard runtime/nvn/maxwell/*.h) $(wildcard runtime/nvn/vulkan/*.h) $(wildcard runtime/ui/*.h)
UI_SRC   := $(wildcard runtime/ui/*.mm)
UI_OBJ   := $(patsubst runtime/%.mm,$(B)/rt/%.o,$(UI_SRC))
IMGUI_OBJ := $(B)/imgui/imgui.o $(B)/imgui/imgui_draw.o $(B)/imgui/imgui_tables.o $(B)/imgui/imgui_widgets.o \
             $(B)/imgui/imgui_impl_sdl3.o

ifeq ($(PLATFORM),macos)
SDL      := $(or $(DEPS),/opt/homebrew/opt/sdl3)
FFMPEG   := $(or $(DEPS),/opt/homebrew/opt/ffmpeg)
# MACOS_BACKEND: vulkan (default: the backend every other platform runs, through MoltenVK; Homebrew's molten-vk,
# vulkan-loader, vulkan-headers, shaderc) or metal (LAS's renderer, not yet carried over to EoW)
MACOS_BACKEND ?= vulkan
# (MoltenVK and the Vulkan headers: from Homebrew; the loader and shaderc too, or from DEPS when set: make app)
VKSDK    ?= /opt/homebrew
EXE      := $(B)/eowrecomp
DEPINC   := -I$(SDL)/include -I$(FFMPEG)/include
LDFLAGS  := -L$(SDL)/lib -lSDL3 -Wl,-rpath,$(SDL)/lib -L$(FFMPEG)/lib -lavformat -lavcodec -lavutil -lswscale -Wl,-rpath,$(FFMPEG)/lib \
            -framework CoreFoundation -framework Foundation -framework QuartzCore -framework AppKit -framework ImageIO -framework CoreGraphics -framework UniformTypeIdentifiers
ifeq ($(MACOS_BACKEND),metal)
OSFLAGS  := -DEOW_METAL
LDFLAGS  += -framework Metal -framework MetalFX
BACKEND_SRC := $(wildcard runtime/nvn/*.m)
BACKEND_OBJ := $(patsubst runtime/%.m,$(B)/rt/%.o,$(BACKEND_SRC))
IMGUI_OBJ += $(B)/imgui/imgui_impl_metal.o
UIFLAGS  := -fobjc-arc -DEOW_METAL
else
OSFLAGS  := -Wno-format -Wno-nullability-completeness  # (VMA's header)
DEPINC   += -I$(VKSDK)/include -Ibuild/deps-macos/include
LDFLAGS  += -L$(VKSDK)/lib -lvulkan -lshaderc_combined -Wl,-rpath,$(VKSDK)/lib
BACKEND_SRC := $(wildcard runtime/nvn/vulkan/*.c) $(wildcard runtime/nvn/vulkan/*.cpp)
BACKEND_OBJ := $(patsubst runtime/%.cpp,$(B)/rt/%.o,$(patsubst runtime/%.c,$(B)/rt/%.o,$(BACKEND_SRC)))
IMGUI_OBJ += $(B)/imgui/imgui_impl_vulkan.o
UIFLAGS  := -fobjc-arc
endif
GAMELIB  := $(B)/eowgame.dylib
GAMELIB_LDFLAGS := -dynamiclib -undefined dynamic_lookup  # (the runtime functions it calls: the app's own)
GAMELIB_SHARED := -shared -Wl,-undefined,dynamic_lookup  # (the same, for the start screen's build: zig cc)
else ifeq ($(PLATFORM),linux)
EXE      := $(B)/eowrecomp
DEPINC   := $(if $(DEPS),-I$(DEPS)/include)
ifeq ($(ARCH),x86_64)
GENARCH  ?= -march=x86-64-v3  # as on Windows: AVX2 + FMA for the lifted FP/SIMD code (checked at startup)
endif
GENFLAGS += $(GENARCH) -fPIC
OSFLAGS  := -Wno-format  # (%llx for uint64_t, which is unsigned long here: the same size)
SHADERC  ?= -lshaderc  # (Debian / Ubuntu's libshaderc.so; a shaderc built from source: -lshaderc_shared)
# -rdynamic: crash backtraces name host functions (runtime/platform/linux.c)
LDFLAGS  := -fuse-ld=lld -rdynamic $(if $(DEPS),-L$(DEPS)/lib) -lSDL3 -lavformat -lavcodec -lavutil -lswscale \
            -lvulkan $(SHADERC) -lpthread -ldl -lm
BACKEND_SRC := $(wildcard runtime/nvn/vulkan/*.c) $(wildcard runtime/nvn/vulkan/*.cpp)
BACKEND_OBJ := $(patsubst runtime/%.cpp,$(B)/rt/%.o,$(patsubst runtime/%.c,$(B)/rt/%.o,$(BACKEND_SRC)))
IMGUI_OBJ += $(B)/imgui/imgui_impl_vulkan.o
UIFLAGS  := -x c++  # the .mm UI sources are plain C++ apart from __APPLE__ blocks
GAMELIB  := $(B)/eowgame.so
GAMELIB_LDFLAGS := -shared -fuse-ld=lld  # (the runtime functions it calls: the app's own, -rdynamic)
GAMELIB_SHARED := -shared
else
EXE      := $(B)/Trirod.exe
DEPINC   := $(if $(DEPS),-I$(DEPS)/include)
OSFLAGS  := -D_FILE_OFFSET_BITS=64 -D_POSIX_THREAD_SAFE_FUNCTIONS -D_CRT_RAND_S -DWIN32_LEAN_AND_MEAN -DNOMINMAX
GENARCH  ?= -march=x86-64-v3  # AVX2 + FMA for the lifted FP/SIMD code (simd.h); override for older CPUs
GENFLAGS += -D_FILE_OFFSET_BITS=64 $(GENARCH)
LDFLAGS  := -fuse-ld=lld -Wl,--stack,16777216 $(if $(DEPS),-L$(DEPS)/lib) -lSDL3 -lavformat -lavcodec -lavutil -lswscale \
            -lvulkan-1 -lwinmm -lole32 -lshell32 -lcomdlg32 -luuid -lshaderc_shared \
            $(if $(RELEASE),-Wl$(comma)--subsystem$(comma)windows)  # release: no console window (log -> eowrecomp.log)
BACKEND_SRC := $(wildcard runtime/nvn/vulkan/*.c) $(wildcard runtime/nvn/vulkan/*.cpp)
BACKEND_OBJ := $(patsubst runtime/%.cpp,$(B)/rt/%.o,$(patsubst runtime/%.c,$(B)/rt/%.o,$(BACKEND_SRC)))
BACKEND_OBJ += $(if $(wildcard docs/icon/icon.ico),$(B)/rt/platform/windows_rc.o)  # the .exe's icon (none yet)
WINDRES  ?= windres
IMGUI_OBJ += $(B)/imgui/imgui_impl_vulkan.o
UIFLAGS  := -x c++ $(OSFLAGS)  # the .mm UI sources are plain C++ apart from __APPLE__ blocks
RUNTIME_KIT := $(B)/link-kit
RUNTIME_LIB := $(RUNTIME_KIT)/eowruntime.a
RUNTIME_ARGS := $(RUNTIME_KIT)/link.args
DLLTOOL ?= $(if $(filter 1,$(WINCROSS)),$(ZIG) dlltool,dlltool)
endif

# GAMELIB_ID: what the game code library is built from (the generator, the headers it is compiled against, the
# flags). A library with another id is rebuilt by the start screen, never loaded.
GAMELIB_SRCS := tools/gen.py tools/lifter.py tools/analyze.py tools/nso.py tools/passes.py tools/patches.py \
                tools/simd_c.py tools/gamelib_entry.c runtime/hle/hle_symbols.txt \
                runtime/cpu.h runtime/simd.h runtime/simd_fp.h runtime/gamelib.h runtime/modules.h tools/windows/eowrecomp.def
GAME_CODE_FLAGS := $(GENFLAGS)
GAMELIB_ID := $(shell (cat $(GAMELIB_SRCS); echo '$(GAME_CODE_FLAGS)') | (shasum -a 256 2>/dev/null || sha256sum) | cut -c1-16)
GAMEBUILD_SRCS := $(GAMELIB_SRCS) $(RT_SRC) $(BACKEND_SRC) $(UI_SRC) $(wildcard $(IMGUI)/*.cpp) $(wildcard $(IMGUI)/backends/imgui_impl_*.cpp) Makefile
GAMEBUILD_ID := $(shell (cat $(GAMEBUILD_SRCS); echo '$(PLATFORM) $(GAME_CODE_FLAGS)') | (shasum -a 256 2>/dev/null || sha256sum) | cut -c1-16)

RT_OBJ   := $(patsubst runtime/%.c,$(B)/rt/%.o,$(RT_SRC))
RTFLAGS  := -O2 -g -fno-strict-aliasing -Wall -Wno-unused-function -DEOW_RUNTIME -Iruntime -Iruntime/nvn $(DEPINC) $(OSFLAGS)
CXXFLAGS := -O2 -g -std=c++17 -DEOW_RUNTIME -I$(IMGUI) -I$(IMGUI)/backends -Iruntime -Iruntime/nvn $(DEPINC)

# The release tag (runtime/ui/launcher.mm, as tools/bundle_macos.sh reads it) and, for the Windows launcher's update
# check, the GitHub repository (owner/name) whose Releases carry trirod-release.json. UPDATE_REPO empty (the default,
# and every development build): the launcher never goes online. docs/RELEASE-WORKFLOW.md.
TRIROD_VERSION := $(shell sed -n 's/^#define EOWRECOMP_VERSION "\(.*\)"/\1/p' runtime/ui/launcher.mm)
UPDATE_REPO ?=

ifeq ($(PLATFORM),windows)
# Windows: Trirod-Launcher.exe (runtime/launcher: SDL's 2D renderer + ImGui, no Vulkan, no runtime code), the headless
# tools it runs - trbuild.exe (runtime/tools/trbuild.c: game files, trgen, zig) and Trirod-ShaderPrep.exe
# (runtime/tools/shaderprep.c: the runtime's Vulkan backend without a window) - and the link kit trbuild links the
# player's Trirod.exe and Trirod.dll with.
SETUP_EXE := $(B)/Trirod-Launcher.exe
TRBUILD_EXE := $(B)/trbuild.exe
SHADERPREP_EXE := $(B)/Trirod-ShaderPrep.exe
BUILD_IDS := $(B)/trirod-build.json
WIN_GAME_CORE_OBJ := $(filter-out $(B)/rt/main.o $(B)/rt/gamebuild.o $(B)/rt/nxpkg.o $(B)/rt/romfs_build.o,$(RT_OBJ))
WIN_GAME_UI_OBJ := $(filter-out $(B)/rt/ui/launcher.o,$(UI_OBJ))
WIN_GAME_RUNTIME_OBJ := $(filter-out $(B)/rt/gamelib.o,$(WIN_GAME_CORE_OBJ)) $(B)/rt/gamelib_runtime.o \
                       $(B)/rt/main_game.o $(BACKEND_OBJ) $(WIN_GAME_UI_OBJ) $(IMGUI_OBJ)
RUNTIME_IMPLIB := $(RUNTIME_KIT)/lib/eowrecomp.lib
LAUNCHER_OBJ := $(B)/launcher/launcher_win.o $(B)/imgui/imgui.o $(B)/imgui/imgui_draw.o $(B)/imgui/imgui_tables.o \
                $(B)/imgui/imgui_widgets.o $(B)/imgui/imgui_impl_sdl3.o $(B)/imgui/imgui_impl_sdlrenderer3.o \
                $(if $(wildcard docs/icon/icon.ico),$(B)/rt/platform/windows_rc.o)
# (no -lvulkan-1 / shaderc / FFmpeg: the link below fails if the executable imports any of them)
OBJDUMP ?= objdump
LAUNCHER_LDFLAGS := -fuse-ld=lld $(if $(DEPS),-L$(DEPS)/lib) -lSDL3 -lwinhttp -lbcrypt -lshell32 -lole32 -luuid \
                    $(if $(RELEASE),-Wl$(comma)--subsystem$(comma)windows)
TRBUILD_OBJ := $(B)/tools/trbuild.o $(B)/rt/gamebuild.o $(B)/rt/nxpkg.o $(B)/rt/romfs_build.o $(B)/rt/paths.o \
               $(B)/rt/platform/windows.o
SHADERPREP_OBJ := $(B)/tools/shaderprep.o $(filter-out $(B)/rt/main_game.o,$(WIN_GAME_RUNTIME_OBJ))
TOOL_DEFS := -DTRIROD_VERSION='"$(TRIROD_VERSION)"' -DGAMELIB_ID='"$(GAMELIB_ID)"' -DGAMEBUILD_ID='"$(GAMEBUILD_ID)"'
all: $(SETUP_EXE) $(TRBUILD_EXE) $(SHADERPREP_EXE) $(BUILD_IDS) $(RUNTIME_LIB) $(RUNTIME_ARGS) $(RUNTIME_IMPLIB) \
     $(if $(filter 1,$(WINCROSS)),,$(RUNTIME_KIT)/lib/stdc++.lib)
.PHONY: windows-runtime-deps
all: windows-runtime-deps
windows-runtime-deps: $(SETUP_EXE) $(TRBUILD_EXE) $(SHADERPREP_EXE) $(RUNTIME_LIB) $(RUNTIME_ARGS) $(RUNTIME_IMPLIB)
	@test -d "$(WIN_DEPS)/ffmpeg/bin" || { echo "run tools/windows/fetch_deps.sh first"; exit 1; }
	@for name in SDL3 libshaderc_shared libSPIRV-Tools libSPIRV-Tools-opt glslang SPIRV \
	    libglslang-default-resource-limits libstdc++-6 libgcc_s_seh-1 libwinpthread-1 libiconv-2 libcharset-1; do \
	    cp -f "$(WIN_DEPS)/ucrt64/bin/$$name.dll" "$(B)/"; \
	done
	@cp -f "$(WIN_DEPS)/ffmpeg/bin/"*.dll "$(B)/"
endif

$(B)/obj/%.o: $(GEN)/%.c runtime/cpu.h
	@mkdir -p $(dir $@)
	@$(CC) $(GENFLAGS) -c $< -o $@

$(B)/obj/tools/gamelib_entry.o: tools/gamelib_entry.c $(GAMELIB_SRCS)
	@mkdir -p $(dir $@)
	$(CC) $(GENFLAGS) -DGAMELIB_ID='"$(GAMELIB_ID)"' -c $< -o $@
$(B)/rt/gamelib.o: $(GAMEBUILD_SRCS)
$(B)/rt/gamelib.o: RTFLAGS += -DGAMELIB_ID='"$(GAMELIB_ID)"'
ifeq ($(PLATFORM),windows)
$(B)/rt/gamelib.o: RTFLAGS += -DGAMEBUILD_ID='"$(GAMEBUILD_ID)"' -DEOW_SETUP_APP
endif
# the start screen's build of it (runtime/gamebuild.c) uses the same flags
$(B)/rt/gamebuild.o: $(GAMELIB_SRCS) $(GAMEBUILD_SRCS) Makefile
$(B)/rt/gamebuild.o: RTFLAGS += -DGAMELIB_ID='"$(GAMELIB_ID)"' -DGAMEBUILD_ID='"$(GAMEBUILD_ID)"' -DGAMELIB_CFLAGS='"$(strip $(filter-out -Iruntime,$(GAME_CODE_FLAGS)))"' \
                                -DGAMELIB_SHARED='"$(GAMELIB_SHARED)"'

$(B)/rt/%.o: runtime/%.c $(RT_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(RTFLAGS) -c $< -o $@

# SMAA (third_party/smaa, MIT): the reference shader as a C string for runtime/nvn/smaa_data.c (both backends build
# their passes from it); the non-ASCII bytes in its comments become '?', and its CRLF line ends plain LF (a CR left in
# the string literals ends them early on macOS clang)
$(B)/inc/smaa_hlsl.inc: third_party/smaa/SMAA.hlsl
	@mkdir -p $(dir $@)
	LC_ALL=C tr -d '\r' < $< | LC_ALL=C tr '\200-\377' '?' | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$$/\\n"/' > $@
$(B)/rt/nvn/smaa_data.o: $(B)/inc/smaa_hlsl.inc
$(B)/rt/nvn/smaa_data.o: RTFLAGS += -I$(B)/inc -Ithird_party/smaa/Textures

$(B)/rt/%.o: runtime/%.cpp $(RT_HDRS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(OSFLAGS) -c $< -o $@

$(B)/rt/%.o: runtime/%.m $(RT_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(RTFLAGS) -fobjc-arc -c $< -o $@

# linked to a temp file, then renamed over: a copy of the game still running keeps its (old) file - relinking in
# place gets running copies killed by macOS (code signature of the changed pages). The temp file keeps the real name
# (in a folder of its own) so the game's import library resolves the Windows runtime DLL name consistently.
ifeq ($(PLATFORM),windows)
$(B)/rt/main_game.o: runtime/main.c $(RT_HDRS) Makefile
	@mkdir -p $(dir $@)
	$(CC) $(RTFLAGS) -DEOW_GAME_ONLY -c $< -o $@

# This object is linked into the runtime EXE built by setup. Its expected game-code ID is a compiler define, so track
# that value explicitly: Make cannot otherwise notice that GAMELIB_ID changed when only generator inputs changed.
$(B)/gamelib_runtime.id: $(GAMELIB_SRCS) Makefile
	@mkdir -p $(dir $@)
	@printf '%s\n' '$(GAMELIB_ID)' > $@.tmp
	@if ! cmp -s $@.tmp $@ 2>/dev/null; then mv -f $@.tmp $@; else rm -f $@.tmp; fi

$(B)/rt/gamelib_runtime.o: runtime/gamelib.c $(RT_HDRS) Makefile $(B)/gamelib_runtime.id
	@mkdir -p $(dir $@)
	$(CC) $(RTFLAGS) -DGAMELIB_ID='"$(GAMELIB_ID)"' -c $< -o $@

$(B)/rt/ui/launcher.o: CXXFLAGS += -DEOW_SETUP_APP

# (the version and update source are compiled in: rebuilt when either changes, as gamelib_runtime.id does for its id)
$(B)/launcher/config.id: FORCE
	@mkdir -p $(dir $@)
	@printf '%s %s\n' '$(TRIROD_VERSION)' '$(UPDATE_REPO)' > $@.tmp
	@if ! cmp -s $@.tmp $@ 2>/dev/null; then mv -f $@.tmp $@; else rm -f $@.tmp; fi
.PHONY: FORCE
FORCE:

$(B)/launcher/launcher_win.o: runtime/launcher/launcher_win.cpp Makefile $(B)/launcher/config.id
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(OSFLAGS) -DTRIROD_VERSION='"$(TRIROD_VERSION)"' -DTRIROD_UPDATE_REPO='"$(UPDATE_REPO)"' -c $< -o $@

$(SETUP_EXE): $(LAUNCHER_OBJ)
	@mkdir -p $(dir $@)link
	$(CXX) -o $(dir $@)link/$(notdir $@) $^ $(LAUNCHER_LDFLAGS)
	@if $(OBJDUMP) -p $(dir $@)link/$(notdir $@) | grep -iE 'DLL Name: (vulkan-1|libshaderc|avcodec|avformat)'; then \
	    echo "Trirod-Launcher.exe must not link the renderer or game runtime libraries"; exit 1; fi
	mv -f $(dir $@)link/$(notdir $@) $@

$(B)/tools/%.o: runtime/tools/%.c runtime/tools/protocol.h $(RT_HDRS) $(GAMELIB_SRCS) Makefile
	@mkdir -p $(dir $@)
	$(CC) $(RTFLAGS) $(TOOL_DEFS) -c $< -o $@

$(TRBUILD_EXE): $(TRBUILD_OBJ)
	@mkdir -p $(dir $@)link
	$(CC) -o $(dir $@)link/$(notdir $@) $^ -fuse-ld=lld $(if $(DEPS),-L$(DEPS)/lib) -lSDL3 -lwinmm -lole32 -lshell32 -luuid
	mv -f $(dir $@)link/$(notdir $@) $@

# The same runtime objects as Trirod.exe with another main: no game, no window (runtime/tools/shaderprep.c)
$(SHADERPREP_EXE): $(SHADERPREP_OBJ)
	@mkdir -p $(dir $@)link
	$(CXX) -o $(dir $@)link/$(notdir $@) $^ $(LDFLAGS)
	mv -f $(dir $@)link/$(notdir $@) $@

# What the tools of this build make, for setup/pack_tools.ps1's tools/trirod-tools.json (the launcher compares it with
# the installed Trirod.dll.id / Trirod.exe.id to decide whether to recompile)
$(BUILD_IDS): $(GAMEBUILD_SRCS)
	@mkdir -p $(dir $@)
	@printf '{\n  "version": "%s",\n  "gamelib_id": "%s",\n  "gamebuild_id": "%s",\n  "protocol": 1\n}\n' \
	    '$(TRIROD_VERSION)' '$(GAMELIB_ID)' '$(GAMEBUILD_ID)' > $@

# Setup links this runtime-only archive into Trirod.exe and links generated game objects into Trirod.dll.
$(RUNTIME_LIB): $(WIN_GAME_RUNTIME_OBJ)
	@mkdir -p $(dir $@)
	rm -f $@
	$(or $(AR),ar) rcs $@ $^
$(RUNTIME_ARGS): Makefile
	@mkdir -p $(dir $@)
	@printf '%s\n' '-Wl,--stack,16777216' '$(if $(RELEASE),-Wl$(comma)--subsystem$(comma)windows)' '-lSDL3' '-lavformat' '-lavcodec' '-lavutil' '-lswscale' '-lvulkan-1' '-lwinmm' '-lole32' '-lshell32' '-lcomdlg32' '-luuid' '-lshaderc_shared' '$(if $(filter 1,$(WINCROSS)),,link-kit/lib/stdc++.lib)' > $@
# The runtime's C++ objects (MSYS2 clang++) use libstdc++: link.args names its import library (libstdc++-6.dll ships
# with the runtime DLLs), which zig's own toolchain does not have. Installs used to need a hand-copied one.
RUNTIME_STDCXX := $(RUNTIME_KIT)/lib/stdc++.lib
$(RUNTIME_STDCXX):
	@mkdir -p $(dir $@)
	cp -f "$$($(CXX) -print-file-name=libstdc++.dll.a)" $@

$(RUNTIME_IMPLIB): tools/windows/eowrecomp.def
	@mkdir -p $(dir $@)
	$(DLLTOOL) -m i386:x86-64 -D Trirod.exe -d $< -l $@
else
all: $(EXE) $(if $(GEN_SRC),$(GAMELIB))
$(EXE): $(RT_OBJ) $(BACKEND_OBJ) $(UI_OBJ) $(IMGUI_OBJ)
	@mkdir -p $(dir $@)link
	$(CXX) -o $(dir $@)link/$(notdir $@) $^ $(LDFLAGS)
	mv -f $(dir $@)link/$(notdir $@) $@
endif

ifneq ($(PLATFORM),windows)
# Non-Windows builds still package game code as a shared library beside the runtime.
$(GAMELIB): $(GEN_OBJ) | $(EXE)
	$(if $(filter windows,$(PLATFORM)),$(file >$(B)/gamelib.rsp,$(GAMELIB_LDFLAGS) -o $@.tmp $(GEN_OBJ)))
	$(if $(filter windows,$(PLATFORM)),$(CC) @$(B)/gamelib.rsp,$(CC) $(GAMELIB_LDFLAGS) -o $@.tmp $(GEN_OBJ))
	mv -f $@.tmp $@
	echo $(GAMELIB_ID) > $@.id
endif

$(B)/rt/ui/%.o: runtime/ui/%.mm $(RT_HDRS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(UIFLAGS) -c $< -o $@

$(B)/imgui/%.o: $(IMGUI)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(B)/imgui/imgui_impl_%.o: $(IMGUI)/backends/imgui_impl_%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(B)/imgui/imgui_impl_metal.o: $(IMGUI)/backends/imgui_impl_metal.mm
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -fobjc-arc -c $< -o $@

# Python: tools/venv (macOS, Linux: bin/python, Windows: Scripts/python.exe)
PY := $(firstword $(wildcard tools/venv/bin/python) $(wildcard tools/venv/Scripts/python.exe))

gen:
	for mb in $(GAME_MODULES); do m=$${mb%%:*}; b=$${mb#*:}; \
	    (cd tools && ../$(PY) gen.py $$m ../extracted/exefs/$$m $$b ../$(GEN)/$$m --hle ../runtime/hle/hle_symbols.txt $(GENSIMD)) || exit 1; \
	done

gen-objs: $(GEN_OBJ)

# Runtime objects only (no generated code, no link): quick compile check
runtime-objs: $(RT_OBJ) $(BACKEND_OBJ) $(UI_OBJ)

$(B)/rt/platform/windows_rc.o: runtime/platform/windows.rc docs/icon/icon.ico
	@mkdir -p $(dir $@)
	$(WINDRES) -I docs/icon -O coff $< -o $@

# The game code generator as a standalone helper (PyInstaller, tools/trgen.spec): build/helper/trgen/, for the start
# screen's setup on the player's machine (runtime/gamebuild.c). GEN_VENV: its venv (make app: one on build/deps'
# Python, which runs on older macOS than Homebrew's).
GEN_PY  ?= $(if $(filter windows,$(PLATFORM)),py -3.12,python3)
GEN_VENV ?= build/gen-venv
GEN_VPY := $(if $(filter windows,$(PLATFORM)),$(GEN_VENV)/Scripts/python.exe,$(GEN_VENV)/bin/python)

gen-helper: $(GEN_VENV)/.ok
	$(if $(filter windows,$(PLATFORM)),USERPROFILE="$$(cygpath -w ~ 2>/dev/null || echo $$HOME)") \
	$(GEN_VPY) -m PyInstaller --noconfirm --clean --log-level WARN --distpath build/helper --workpath build/pyinstaller \
	    tools/trgen.spec
	$(if $(filter windows,$(PLATFORM)),$(GEN_VPY) tools/pe_no_cfg.py build/helper/trgen/trgen.exe)
	@du -sh build/helper/trgen

$(GEN_VENV)/.ok:
	$(GEN_PY) -m venv $(GEN_VENV)
	$(GEN_VPY) -m pip install -q capstone==5.0.9 unicorn==2.1.4 lz4==4.4.5 pyinstaller==6.22.3
	touch $@

# make app (macOS): build/EoWRecomp.app to ship - the runtime built against build/deps (tools/build_deps_macos.sh:
# LGPL-only FFmpeg, SDL3), MoltenVK, and what the start screen builds the game code with (the generator helper, zig).
# No game code or files inside (tools/bundle_macos.sh checks). For macOS 15+ (build/deps' minimum) unless MACOS_MIN.
APP_MIN := $(or $(MACOS_MIN),15.0)
app:
	$(MAKE) B=build/rel DEPS=$(CURDIR)/build/deps MACOS_MIN=$(APP_MIN) build/rel/eowrecomp
	$(MAKE) gen-helper GEN_PY=build/deps/python/bin/python3 GEN_VENV=build/gen-venv-rel
	[ -x build/zig/zig ] || tools/fetch_zig.sh
	MACOS_MIN=$(APP_MIN) tools/bundle_macos.sh build/rel/eowrecomp

.PHONY: all gen gen-helper gen-objs runtime-objs app

build/test_shaders: tools/test_shaders.m runtime/nvn/maxwell/translate.c runtime/nvn/maxwell/shader.h
	clang -O1 -fobjc-arc -framework Metal -framework Foundation -o $@ tools/test_shaders.m runtime/nvn/maxwell/translate.c
