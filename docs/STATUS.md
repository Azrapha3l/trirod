# Status

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
- Verified (Vulkan, test GPU):
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

**Shader prewarm investigation (2026-11-23):** The matching local EoW v1.0.0 RomFS is available at `[local path omitted]`. Its `region_common/shader` folder contains 96 `.bfsha` archives (62,298,464 bytes total). Parsing their BNSH variation/program/stage tables found 247 BNSH containers, 11,892 program variations, and 3,576 unique stage records (35,278,429 bytes in the first per-stage-file output); all parsed records passed the control/code length and magic checks. `ExeFS/main` is also available (26,093,400 bytes), but direct BNSH scanning of the compressed NSO is not a complete scan of its decompressed sections. Do not use published v1.1.0 / SDK 20.5.1 counts as v1.0 inventory.
- Intended workflow: Setup scans the user's installed game assets and creates a portable shader catalog from the Maxwell code and archive metadata. Gameplay adds code/variant/interface and pipeline-state combinations that static extraction cannot infer. On subsequent launches, the runtime translates catalog entries to this build's GLSL/SPIR-V and combines known runtime variants with the user's local Vulkan pipeline cache.
- Implementation started: `tools/shader_catalog.py` extracts BNSH control/code stage records and paired stage identities to a compact portable catalog. `tools/gen.py --shader-catalog` exposes it through the same packaged helper Setup already uses. `runtime/gamebuild.c` invokes it once while building the user's game data; output is placed in `extracted/shader_catalog/` and removed with the rest of that game's files. This creates the asset catalog; runtime pretranslation and GPU-specific warm pipelines are the next integration step.
- Current implementation gap: Vulkan `vk_warm_start` still reads only `cache/warm/code/` and `cache/warm/recipes.bin`, which are recorded from programs and pipelines encountered during gameplay. It does not yet consume the static catalog, translate a game-wide shader catalog, or seed compute programs. The on-disk Vulkan driver pipeline cache is local to each GPU/driver and cannot be distributed as a portable repository artifact.
- Next shader work: implement the separate headless preparation tool, invoked by the launcher, to load the catalog, translate known Maxwell stages/program pairs to GLSL/SPIR-V, and build pipelines for recipes available from the game's archive metadata and runtime-learned catalog. Keep GPU/driver-specific pipeline caches local to each player; the Vulkan specification says incompatible pipeline-cache data is initialized empty, and its contents depend on implementation/device details. Measure setup duration, catalog size, warmup time, and whether static seeding reduces first-seen in-game stutter. Include compute shaders. Game shader archives do not reveal every dynamic render-state combination, so the tool cannot guarantee every pipeline is prebuilt from shader files alone.
- Public comparison only: a survey of EoW 1.1.0 reported 9,858 unique stages and also distinguished extracted shader-library records from a smaller runtime-recorded pipeline seed. That supports the two-source design conceptually, but its counts/manifest are not reusable for this v1.0.0 / SDK 16.2.0 build.

Atualizado: 2026-11-23 (sessão 16c: árvores de Hyrule Town corrigidas — S2R lane id/máscaras + draws indiretos; sessão 16b: warmup de shaders/pipelines; sessão 16: Rift confirmado; Hyrule Town sem erros de validação; diagnósticos para o apoio invisível do pássaro).

## Sessão 16 (2026-11-23): problema do Rift confirmado como corrigido

- O usuário confirmou que as correções de runtime feitas após a análise do Claude corrigiram o problema do Rift no jogo.
- Isso confirma o problema visual/renderizado do Rift na configuração testada; não significa que todos os avisos de validação Vulkan estejam resolvidos.
- O usuário está testando os demais problemas identificados. Acompanhar os novos logs/resultados antes de marcar `VUID-vkCmdDrawIndexed-None-06479`, `SYNC-HAZARD-WRITE-AFTER-READ` ou outras hipóteses como corrigidos.
- Novo teste: o usuário observou um pássaro aparentemente apoiado em algo que não está visível e flashes pretos/glitches em Hyrule Town. O log `[tex] type mismatch: want 3 have 1 (1408x792x1 target 1 fmt 0x35 layers 1)` confirma que uma amostra tipo cubo (3) recebeu uma textura 2D (1), D24S8 (`0x35`), do tamanho do framebuffer; `vk_view` substitui incompatibilidades por uma textura dummy do tipo solicitado, que para cubo é preta. Isso é uma causa plausível para pixels/efeitos pretos se os draws afetados participarem da cena, mas ainda não prova relação com o flicker ou com o suporte invisível do pássaro; correlacionar shader, slot, draw e frame. As mensagens `fragment color output has no color attachment` mostram shaders com saída de cor executados sem attachment de cor, enquanto há depth; podem ser draws depth-only intencionais e precisam ser associados a cada draw antes de tratá-las como defeito. A linha `21>3=1.000 22>4=1.000 23>5=1.000` ainda não foi identificada.
- Próximo passo: registrar quais avisos persistem, as linhas `[vk] shadow sampler` / `[vk] copy alias` correspondentes, e qualquer impacto visual ou de desempenho observado.
- **Análise de `[local path omitted]` (23/11, Hyrule Town, com Vulkan Configurator) + screenshot do pássaro.**
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
    (`[local path omitted]`, 2×). Os números são `VkImageViewType` (3 = CUBE, 1 = 2D), não `TEXTYPE_*`. O 1408×792
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
  1. Copiar `build/win-native/link-kit/eowruntime.a` para `[local path omitted]` e religar pelo Setup.
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
- **Flash preto: causa identificada** (`[local path omitted]`, 23/11 09:22; o usuário viu o flash logo ao entrar e
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
  trava o jogo. O padrão assíncrono pulava draws em toda sessão, mesmo com os caches quentes (`[local path omitted]`:
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
- Medição (Vulkan, test GPU; `eowrecomp.exe` de teste ligado a partir de `build/win-native`; input roteirizado
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
  `[local path omitted]`), fechar, abrir de novo e conferir no log: `[warm] N recipes …`, `[warm] ready in …`, o título
  da janela durante a espera, a contagem `skipped:` e `pipelines: N new, M from warmup` ao entrar nas áreas já
  visitadas, e se os flashes / travadas de primeiro encontro sumiram.

## Sessão 15 (2026-11-22): SUST e erros do Vulkan Configurator no void/rift

- Origem: `[local path omitted]` (validação ligada) tem 20 × `VUID-vkCmdDrawIndexed-None-06479` (sampler de
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
- **Rift transparente ao vivo, correto no RenderDoc** (`[local path omitted]`, 22/11 21:30). Esse log ainda é do
  runtime antigo: o Setup liga `eowrecomp.exe` a partir de `[local path omitted]` (16:48, sem as
  correções), não de `[local path omitted]` — o `eowrecomp.exe` religado às 21:18 não contém as strings novas.
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
  `build/win-native/link-kit/eowruntime.a` para **`[local path omitted]`**, abrir Setup para religar o runtime
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
  link kit foram instalados em `[local path omitted]`; 19 DLLs SDL/FFmpeg/shaderc também estão ao lado do runtime. SHA-256 do
  Setup instalado: `2eafaed7f297649175b9c14ef8ad284c392a928bf10e4ca6542f724e922f3e56`.
- O EXE antigo na raiz e a DLL antiga ainda estão instalados até Setup refazer os dois. Para testar, abrir
  `[local path omitted]`, escolher **Setup Game Files**, esperar a geração e compilação terminarem e escolher
  **Play**. Não iniciar manualmente `extracted/eowrecomp.exe`; o caminho correto é o runtime na raiz, ao lado das DLLs.
  A compilação pelo Setup e o gameplay desta revisão ainda não foram validados.

## Sessão 13 (2026-11-20): crash durante a inicialização

- `[local path omitted]` registra um acesso inválido à página zero em `nn::am::InitializeApplicationInterfaces`,
  logo após o runtime solicitar `lm`, `apm` e `appletOE`. Esses três serviços aparecem como sem implementação no log,
  embora seus registros estejam no código e `am.o` esteja no arquivo `eowruntime.a`.
- Causa provável: o link normal de um arquivo estático omitia os objetos referenciados apenas pelo registro em seção.
  O link do EXE standalone agora envolve `eowruntime.a` com `--whole-archive`, preservando esses registros.
- `eowsetup.exe` foi recompilado e copiado para `[local path omitted]`; SHA-256 do build e da cópia:
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
- `tools/windows/build.sh` recompilou `eowsetup.exe` e `eowrecomp.exe`; os dois foram copiados para `[local path omitted]` e
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
  para os alvos `main` game-only/setup, builder e frontend setup. Os executáveis foram atualizados em `[local path omitted]`
  e os hashes conferem com o build local; a import library também confere. O runtime ainda não foi aberto nesta revisão.
- A DLL existente do jogo (`15f1ea1499a01c22`) tem um `GAMELIB_ID` diferente do build atual (`4353ba77a29b43ac`), então
  o setup pedirá uma atualização da DLL. A compilação deve reaproveitar as unidades antigas que passem na verificação
  de data; as próximas podem reutilizar traduções e objetos pelas chaves de conteúdo.
- Ainda pendente: substituir o aviso de nova versão/link externo por atualização automática com manifesto e hashes, e
  adicionar a janela de detalhes por etapa ao estilo Triforce. O build de `EoWRecompSetup.exe` (o instalador de pasta)
  não foi refeito; para testar diretamente, abra `[local path omitted]`.

## Sessão 9 (2026-11-20): retorno ao baseline local de shaders

- `references/eowrecomp` no commit local `b3daa34` é o baseline. Os arquivos do tradutor Maxwell e do caminho Vulkan de
  shaders/descritores foram restaurados desse snapshot; a seleção Ryujinx e o protótipo SUST experimental saíram do
  runtime ativo.
- Mantida a separação entre setup e runtime, junto com o estado de ROMFS do baseline. Mantido também o ajuste RGB565
  do baseline e o swap red/blue posterior para os formatos NVN RGB5/RGB5A1 (`0x3A`/`0x3B`) em `textures.c`.
- Os arquivos substituídos antes do snapshot foram preservados em `build/source-before-reference-shader-restore/`.

## Sessão 6 (2026-11-18): tela preta após PLAY

- Reprodução no Windows: a janela do launcher aparece, mas depois de PLAY fica preta. O log mostra renderização contínua
  (Vulkan, test GPU, centenas de draws/dispatches por quadro) e a abertura de `rom:/region_common/movie/rd000_Title.webm`
  falha porque o runtime procurava `extracted/romfs/...` como arquivo solto. O setup atual guarda os dados em
  `extracted/romfs.bin`, sem uma árvore `extracted/romfs/`; assim o vídeo da tela de título não podia ser lido.
- O runtime agora usa o índice RomFS existente para localizar e ler o vídeo dentro de `romfs.bin` através do AVIO do
  FFmpeg, mantendo suporte ao arquivo solto quando ele existe. Build do runtime Windows concluído em
  `build/win-native/eowrecomp.exe`; a correção ainda precisa de confirmação visual no jogo.
- Correção do pacote de teste: a primeira compilação feita fora do ambiente MSYS2 tinha `GAMELIB_ID` vazio, então não
  reconhecia a `eowgame.dll` instalada e mostrava a tela de configuração. O build foi refeito com o PATH UCRT64; agora
  incorpora `15f1ea1499a01c22`, igual ao ID da DLL, sem precisar recompilar/copiar a DLL do jogo.
- Runtime corrigido copiado e verificado em `[local path omitted]`. As DLLs de dependência já eram idênticas
  às do build. `eowgame.dll` permanece em `[local path omitted]`, onde o runtime procura o código do jogo.
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

## Sessão 3 (2026-10-20, Apple Silicon laptop 8 GB, macOS 27): macOS e Android

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
