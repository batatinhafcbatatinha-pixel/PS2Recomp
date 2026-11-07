# STATUS - Port nativo de Gran Turismo 4 (USA) v2.00

Atualizado em 03/11/2026 (Claude assumiu; OpenCode ficou sem cota em 02/11 e o supervisor foi parado).

## 03/11 - geracao e build (Claude)

- Boot direto no executavel do jogo: `game_files/CORE.ELF` (reconstruido de `CORE.GT4` por
  `scripts/build_core_elf.py`). O launcher `SCUS_973.28` nao e recompilado. O crt0 do CORE e o padrao
  (SetupThread com pilha 0x1FF8000, EndOfHeap 0x8A215C, main 0x107F08), e a pasta do ELF vira `cdrom0:`.
- `gt4/game_files` e uma juncao para `D:\CodexBuilds\gt4\game_files`; o layout `.disc` foi preparado com
  `--root D:/CodexBuilds/gt4` (o instalador recusa a juncao).
- `ps2_recomp` novo, do fonte atual (`build-recompiler.cmd` -> `D:\CodexBuilds\gt4ecompiler`): o exe do CoD
  e de 21/10 e tem o bug do SQRT.S. Geracao: 24.611 funcoes, 771 chunks, 9.099 funcoes-buraco, 160 handlers SDK
  nativos (`generate.cmd` refaz tudo).
- Projeto no modelo do Ben 10 (`project/CMakeLists.txt`, `project/main.cpp`, `GT4_PROBE=<hex>`). Build em
  `D:\CodexBuilds\gt4uild
ative`, iniciado fora do Claude Code (`ps2-perf-tools/start-build.ps1 gt4`).
- Launcher `Play-GT4.cmd`; teste de boot: `python scripts/smoke.py --seconds 60 --name boot1`.
- Risco conhecido: o jogo carrega modulos IOP proprios da Polyphony (pdicdvd, pdispu2, pdistr, libpdi),
  que precisam rodar no IOP interpretado.

## Feito

- **Etapa 1 concluida.** `git init` em `work/gt4`, identidade local
  `batatinhafcbatatinha-pixel` / `262420390+batatinhafcbatatinha-pixel@users.noreply.github.com`,
  `.gitignore` no padrao dos outros ports (nada de dados originais, builds ou codigo gerado),
  `build-location.txt` apontando para `D:\CodexBuilds\gt4`.
- **Etapa 2 concluida.** ISO lida somente para leitura, sem copia e sem alteracao.
  `scripts/extract_game_files.py` extraiu **137 arquivos, 5.270.980.724 bytes** para
  `D:\CodexBuilds\gt4\game_files`, com SHA-256 conferido contra os extents relidos
  (`analysis/game-files-verification.json`). O disco do ISO nao foi tocado.
  A ISO tem **dois volumes ISO9660** (DVD9): o volume da camada 1 comeca no setor 16 e o da
  camada 2 tem setor 0 em **1.317.040** (descritores em 1.317.056). O contrato do extrator
  cobre os dois; sem isso o `GT4L1.VOL` nao seria encontrado.
- **Etapa 3 em andamento.** Fatos ja estabelecidos:
  - `SYSTEM.CNF`: `BOOT2 = cdrom0:\SCUS_973.28;1`, `VER = 2.00`, `VMODE = NTSC`.
  - Boot ELF: `SCUS_973.28`, 273.020 bytes, ELF32 LE MIPS (machine 8), entry `0x01000008`,
    SHA-256 `f8f10823160e2b5cef5c7032628134632b291b1df87a9aee3c144794dd8019fa`.
    `.text` em `0x01000000` com 0x2DC10 bytes, `.data` em `0x0102DC80`, `.bss` ate ~`0x010519AC`.
  - O boot ELF e **apenas o launcher**: strings `core.gt4`, `cdrom0:\IRX\` (sio2man,
    mtapman, mcman, mcserv, padman, ...), classes `ImageLoader`/`DiskSource`/`HostSource`/
    `MCardSource`, `cdrom0:\GAMEDATA`, `cdrom0:\IOPRP300.IMG;1`. Ele carrega `CORE.GT4`
    (2.020.861 bytes) e dados do `GT4.VOL`.
  - `IOPRP300.IMG` e o pacote IOP "ROMDIR" com modulos de kernel (LOADCORE, FILEIO,
    CDVDMAN, CDVDFSV, SIFCMD/SIFMAN, LOADFILE, IOPMAN, MODLOAD, TIMEMANI, ROMDRV, ...).
    Ha tambem 26 IRX avulsos em `IRX\`.
  - `GT4.VOL` = 2.459.502.592 bytes, `GT4L1.VOL` = 2.595.657.728 bytes; juntos somam
    5.055.160.320 bytes de arquivo de dados (o VOL e dividido entre as duas camadas do DVD9).
    `GT4.VOL` comeca com o magic `0xACB990AD` seguido de uma tabela de offsets/IDs;
    o formato do indice ainda nao foi decodificado.
  - `CORE.GT4` nao e ELF (comeca em `01 01 CC 5E ...`), entropia alta: candidato a
    ELF comprimido/cifrado com o proprio loader.
  - **REA**: o MCP `rea` abriu `SCUS_973.28` como ELF/MIPS e reportou SHA-256 e arquitetura,
    mas o provider Ghidra do REA no Windows recusa analise de ELF MIPS
    (`target_format_unsupported`; so PE e 6502 raw). Pela regra da equipe, a analise de codigo
    segue com Ghidra R5900 + extensao EmotionEngine ja instalada
    (`ghidra-emotionengine-reloaded`, processador `r5900:LE:32:default`).

## Em andamento (arquivos que estou mexendo)

- `gt4/scripts/` (extracao, probes, exportacao Ghidra) e `gt4/config/` (configs do PS2Recomp).
- `gt4/NOTES.md` (registro tecnico), `gt4/analysis/` (relatorios locais, fora do git).

## Bloqueios

- Nenhum bloqueio de disco: `C:` ~12,7 GB livres, `D:` ~38 GB livres (5,3 GB usados pelo
  `game_files`), acima dos limites de 8 GB / 15 GB.
- Pendente de investigacao: formato do `GT4.VOL` e do `CORE.GT4`; sem isso nao se sabe quais
  overlays de codigo existem e nao ha como gerar/rodar o jogo.

## Proximo passo

1. Indexar o `GT4.VOL` (magic `0xACB990AD`) e localizar ELFs/overlays dentro dele.
2. Analisar `SCUS_973.28` no Ghidra R5900 (ExportPS2Functions.java) e exportar
   `config/ghidra-functions-only.csv`.
3. Montar `config/recomp.toml` e `scripts/generate.py` no padrao do `boku2`.

Nada foi executado: nenhum jogo, runner ou launcher rodou. Nao ha validacao de runtime.
