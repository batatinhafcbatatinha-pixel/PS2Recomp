# STATUS - Port nativo de MotorStorm: Arctic Edge (PS2, USA, SCUS_976.54 v1.00)

Iniciado em 03/11/2026 (Claude), no modelo do GTA LCS / GT4 / Ben 10 (PS2Recomp; worktree própria em
`third_party/PS2Recomp`, branch `msae-port` a partir de d8e5a9d).

## Disco

- ISO `E:\ISO\MotorStorm - Arctic Edge (USA)\MotorStorm - Arctic Edge (USA).iso` (2.314.305.536 bytes), só leitura.
- `scripts/extract_game_files.py`: 11 arquivos, 2.292.724.490 bytes; `game_files/` é junção para
  `E:\CodexBuilds\motorstorm-ae\game_files` (C: e D: quase cheios). SHA-256 em `analysis/game-files-verification.json`.
- `SYSTEM.CNF`: `BOOT2 = cdrom0:\SCUS_976.54;1`, `VER = 1.00`, NTSC.
- ELF 2.370.292 bytes, SHA-256 `b9b7d66d...2b0b06`, entry `0x100008`, um PT_LOAD em `0x100000`
  (filesz 0x22D0B4, memsz 0x24DB5C). `.text` 0x100000-0x2BD248, `.vutext` 0x2BD250 (microcódigo VU), `.data`
  0x2D0780, `.bss` até 0x34DB5C. Sem tabela de símbolos.
- IOP: `IRX/IOPRP310.IMG`, `IRX/STREAM.IRX` (streamer próprio), LIBSD, MCMAN/MCSERV, PADMAN, SIO2MAN.
- Dados: `\FE.PAK` (1,3 GB) e `\MAIN.PAK` (0,97 GB); `\DEMO.PAK` é referenciado mas não existe no disco.

## Pipeline

1. `ghidra-export.cmd` -> `config/ghidra.toml`, `config/ghidra-labels.csv`, `config/ghidra-functions-only.csv`.
2. `generate.cmd` (analyzer + `prepare_config` + geração + chunks + gap functions + SDK handlers + roteamento).
3. Build fora do Claude Code: `../ps2-perf-tools/msae-detached.cmd` (buildsup.py, status em `analysis/build-status.txt`).
4. `Play-MotorStorm.cmd`; teste de boot: `python scripts/smoke.py --seconds 60 --name boot1`.

## 04/11 (Codex) — reconstrução do estado em 05/11/2026

- Evidência anterior: `analysis/relink.log` termina em EXIT 0; `runs/h1` a `runs/w2` contêm boots limitados de 40–80 s, encerrados pelo timeout, não saídas limpas do jogo.
- Alterações já encontradas (preservadas): limites corretos do ELF em `gen_gap_functions.py`; hook de arena 0x25BAA0 em `project/main.cpp`; ajustes em EeScheduler/Thread; probes de memória e transferências GS no runtime local.
- `runs/w2/frame.png` mostra fundo preto e contornos, sem menu legível. FPS/present ativo não comprova jogo funcionando.
- Investigação atual: comparar o rasterizador GPU padrão do smoke com o backend hw do launcher, e verificar os comandos GS/VU antes de atribuir a falha a texturas.

## 05/11 (Claude)

- hw e gpu: jogo a 30 fps (VU1 17%), tela preta com poucas linhas, quadro parado de 15 a 60 s. Captura do GS
  (`analysis/gs-trace-menu.txt`): triângulos PSMT8 com `TEST=0x31001` (ATST NEVER + AFAIL FB_ONLY + DATE/DATM) e
  fbmsk 0xff000000, sprites rgba 0 `TEST=0x32003`; texto/menu depende do alpha de destino. Tarefa para o Codex em
  `analysis/TASK-gs-lines.md` (resultado em `ASK-gs-lines.md`). `PS2X_GS=cpu` não inicializa o runtime.
- Worktree: alocador do IOP first-fit (igual FO2/GT4). Build na fila `D:/CodexBuilds/bq.sh`.

## 07/11 madrugada (Claude)
- AOT dos IRX regenerado com o gerador corrigido (pareamento HI16/LO16 pela ordem da tabela, ver gta-lcs/NOTES.md);
  build 01:50. Menu continua preto: não era o AOT (PS2X_IOP_AOT=0 dá o mesmo).
- PS2X_TRX_TRACE / PS2X_TEXUPLOAD_TRACE (Codex, compilados agora): só 16 transferências na corrida inteira, todas a
  limpeza inicial da VRAM (16 x 256x256 CT32). Nenhum upload de textura depois. PS2X_GIF_ORDER_TRACE (25-26 s): só PATH1
  (VU1) e PATH2, nenhum PATH3/IMAGE; TEX0 aponta para TBP 0x3760 (nunca escrito). IOP entrega dados ao EE (17 DMAs de
  256 KB, "Exit MultiStream V7.1 Thread Load"). Próximo: achar no EE o gerenciador de texturas (quem monta BITBLTBUF/
  TRXDIR para 0x3760 ou um pacote PATH3) e por que não roda: esperando um evento/estado do carregamento?
