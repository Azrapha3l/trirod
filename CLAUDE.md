# EoWRecomp — regras permanentes

Recompilação estática de *The Legend of Zelda: Echoes of Wisdom* (Switch, 2024) para PC. Derivado do
LASRecomp (MIT, https://github.com/peiikai/LASRecomp, commit base `d23597d`). Não é emulador: o ARM64 do jogo
vira C em tempo de build; o runtime substitui o Horizon e a NVN. Arquitetura e plano: `docs/ARCHITECTURE.md`;
estado atual e próximo passo: `docs/STATUS.md` (leia antes de começar).

Fluxo de release/atualização do Windows: `docs/RELEASE-WORKFLOW.md`; protocolo das ferramentas:
`docs/TOOL-PROTOCOL.md`. `Trirod-Launcher.exe` (`runtime/launcher/launcher_win.cpp`) é só configuração,
atualização, orquestração, progresso e Play: desenha com o renderer 2D do SDL e **não pode** ligar Vulkan,
shaderc, FFmpeg nem código do runtime (a Makefile recusa o link). O trabalho fica nas ferramentas externas:
`trbuild.exe` (`runtime/tools/trbuild.c`: arquivos do jogo, `trgen.exe`, zig → `Trirod.dll` e `Trirod.exe`) e
`Trirod-ShaderPrep.exe` (`runtime/tools/shaderprep.c`: backend Vulkan sem janela). Eventos de progresso só em
linhas `@@` no stdout; o resto é diagnóstico. O caminho GitHub (manifesto `trirod-release.json`, SHA-256) está
implementado mas nunca foi testado contra uma release publicada; não o descreva como verificado.
- Pacotes/releases: só `setup/pack_tools.ps1` / `setup/make_release.ps1`, que passam por `setup/check_package.ps1`.
  DLLs do pacote são uma lista explícita (o diretório de build pode ter `eowgame.dll`/`Trirod.dll` do usuário).

## Arquivos da Nintendo
- **Não coloque arquivos do jogo no repositório**: nada de `extracted/`, `generated/`, chaves, XCI/NSP/NCA/NSO,
  fontes do sistema, texturas, modelos, música ou texto.
- **Exceção expressamente autorizada pelo usuário:** a base de shader Maxwell de EoW v1.0.0 e os registros portáteis
  de variantes descobertas podem ser mantidos no diretório dedicado `shader_catalog/` para que Setup possa processá-los.
  Só inclua os control/code records e metadados necessários à recompilação; não copie os arquivos `.bfsha` inteiros,
  RomFS/ExeFS, nem dados não relacionados a shader. A cache de pipeline Vulkan permanece local por GPU/driver.
- O `.gitignore` cobre arquivos extraídos; confira com `git status --ignored` antes de cada commit. Exceção adicional:
  capturas de tela do jogo rodando (`docs/screenshots/`, JPEG), para documentar o progresso.
- Os arquivos do usuário (XCI, `prod.keys`) ficam numa pasta própria, fora do repositório (o caminho local está na
  memória do agente, não aqui).
  **Só leitura.** Nunca copiar o XCI nem as chaves para dentro do repo. Não pedir, procurar nem sugerir fontes
  para esses arquivos.
- Extração: `tools/nca_extract.py` (o hactool não lê o RomFS comprimido deste NCA) para `extracted/`
  (`exefs/`, `romfs/`). Uma cópia filtrada das chaves, se precisar, vai para o scratchpad, nunca para o repo.
- Versão fixa: **title 01008CF01BAAC000, v0 (1.0.0)**, build IDs em `docs/STATUS.md`. Endereços de patches e HLE
  valem só para ela.

## Código
- Código gerado (`generated/`) nunca é editado à mão: mudanças vão em `tools/lifter.py`, `tools/gen.py` ou
  `tools/patches.py`.
- Todo patch confere a palavra de instrução original antes de aplicar (`expect()` em `tools/patches.py`).
- Seguir o estilo do código ao redor (nomes, estrutura, densidade de comentários). Comentários explicam o
  *porquê* (o que o jogo faz, o que é um endereço, que log/teste mostrou o problema).
- Para cada falha, achar a causa (Capstone, logs, Switchbrew, envytools) em vez de contornar. Recurso NVN ou
  instrução Maxwell faltando: implementar corretamente e adicionar teste em `tools/` (`test_translate`,
  `test_shaders`, `test_simd`, `test_lift`).
- Variáveis de ambiente de depuração usam o prefixo `EOWRECOMP_`.
- Logs: `LOG` só para erros, avisos e fatos de uma vez. Diagnóstico repetido (estatísticas periódicas, notas por
  draw/textura) usa `LOG_VERBOSE`; comando de serviço sem implementação usa `LOG_FIRST` (`runtime/log.c`). Não
  colocar chamadas com efeito colateral nos argumentos de `LOG_VERBOSE` (não são avaliados fora do modo verbose).
  O launcher e o `Trirod.exe` não abrem console; `--console` / `EOWRECOMP_CONSOLE=1` abre (`docs/RELEASE-WORKFLOW.md`).
- Vulkan GPL (`VK_EXT_graphics_pipeline_library`, `exec.c`): bibliotecas só são ligadas com fast link; o pipeline otimizado é o
  pipeline completo (link com LINK_TIME_OPTIMIZATION sem RETAIN deixou a tela preta; com RETAIN o cache foi a 1,2 GB). Mudança de pipeline/GPL: testar com `EOWRECOMP_VK_VALIDATION=1` e
  comparar com `EOWRECOMP_VK_GPL=0`; mudança que invalide caches antigos sobe `PIPELINE_SCHEME` (`device.c`).

## Verificação
- Uma correção só está verificada depois de vista funcionando (screenshot, log ou teste) — nunca declarar
  sucesso com base em "deve funcionar".
- Mudança no renderer compartilhado: dizer qual backend foi testado (Vulkan / Metal).
- Build do Windows: `./setup/build.ps1` (gera `build\setup-dev`); rodar `build\setup-dev\Trirod-Launcher.exe --dev`
  (`--data <pasta>` para testes, `--recompile` para começar a build sozinho). Não redirecionar a saída do script em
  Windows PowerShell 5.1: o stderr do PyInstaller vira erro com `$ErrorActionPreference = 'Stop'`.
- Medir em vez de estimar (rodar as ferramentas: `tools/nso.py`, `tools/analyze.py --census`, logs).
- Mudança em escala ou render targets: testar também abaixo de 1x (Dynamic, ou Match display numa janela menor que
  1920x1080), não só 1x/2x (a grade de luz 64x36 do jogo quebrava abaixo de 1x). Testes no jogo usam
  `EOWRECOMP_CACHE_DIR`, `EOWRECOMP_SAVE_DIR` e `EOWRECOMP_SETTINGS` em pastas de rascunho.
- Avançar por marcos (M1…M7, `docs/STATUS.md`); só passar ao próximo com o atual comprovado.

## Git
- Commits pequenos e descritivos; um assunto por commit.
- README: projeto de fã, sem afiliação com a Nintendo, sem arquivos do jogo, exige cópia legal do próprio
  usuário; créditos ao LASRecomp e a todas as bibliotecas usadas, com suas licenças.

## Fim de sessão
- Atualizar `docs/STATUS.md`: marco atual, o que funciona, o que quebrou, próximo passo, e o que o usuário deve
  testar e observar.
