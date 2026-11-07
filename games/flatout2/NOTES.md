# FlatOut 2 (PS2, USA, SLUS-21251) — port nativo

Pipeline PS2Recomp, no modelo do GTA VCS (`gtavcs`), identificadores `Fo2`/`FO2_`.

## Disco

- ISO: `E:\ISO\FlatOut 2 (USA)\FlatOut 2 (USA).iso`, um volume ISO9660 (1.970.720 setores).
- `SYSTEM.CNF`: `BOOT2 = cdrom0:\SLUS_212.51;1`, `VER = 1.02`, NTSC.
- `SLUS_212.51`: 3.964.564 bytes, SHA-256 `658d5449f2851fef697c151651914058003cae3cf181b9b9d9ddb6074090f1ee`,
  entry `0x2B6EF0`. Um segmento `0x100000`, filesz `0x3A5148`, memsz `0x54A074`. `.text 0x100000-0x3FD998`,
  `.vutext 0x3FD9A0` (0x205F0), `.data 0x41E000`, `.rodata 0x454080`, `.bss 0x4A5400-0x64A074`.
  ~80 overlays DVP (microcódigo VU). Sem tabela de símbolos.
- Arquivos: `FLATOUT.BFS` (556 MB, dados), `DATA/` (INTRO, SONGS1/2, SOUND, VIDEO), `MODULES/` (FMOD.IRX,
  LGAUD.IRX, LIBSD, SDRDRV, SNDRV*, CDVDSTM, MCMAN/MCSERV, PAD2/, rede SN Systems/DNAS), `NTGUI.ELF` (GUI de rede).
- `EMPTY.BIN` (1 GB de enchimento) não é extraído. `scripts/extract_game_files.py` extrai e confere;
  `scripts/prepare_layout.py` escreve `game_files/.disc` com os LBNs originais do disco.

## Pastas

C: e D: estão quase cheios: `game_files` e `build` são junções para `E:\CodexBuilds\flatout2`.
PS2Recomp: worktree próprio `third_party/PS2Recomp` (branch `flatout2-port`, de `d8e5a9d`) + as correções SIF do GT4
ainda não commitadas no checkout compartilhado (`analysis/shared-uncommitted.patch`).
Recompilador: cópia de `gtavcs/build/recompiler` em `build/recompiler`.

## Estado

- 03/11 21:30: arquivos extraídos, Ghidra rodando.
- 03/11 22:58: build completo em `E:\fo2b` (buildsup, 1 job após um reinício por RAM). Disparar com
  `ps2-perf-tools/fo2-detached.cmd` (Win32_Process.Create); status em `analysis/build-status.txt`.

## 04/11 - primeiro boot

- `smoke.py --seconds 30`: janela + GS gpu sobem, `SifInitRpc`, DEV9.IRX carregado no IOP, depois o EE gira sem
  frames (`[stall] pc=0x2b6ef0 cycle=0`). cdb (`-p PID -c "~*k 60; .detach; q"`; o `-pv` mata o processo) mostra o
  GameThread em `FUN_002bd688`: é o `sceCdSearchFile` da libcdvd, que repete `sceSifBindRpc(0x80000597)` para sempre.
  O wrapper `(fp, name)` em `0x2bd988` agora vai para o handler nativo (`config/sdk-handlers-extra.txt`), como no LCS.
- Trazida do LCS a correção do `CreateThread` com prioridade 0 (EeScheduler.cpp, worktree do FO2).
- REA não abre ELF MIPS no Windows (Ghidra do REA só aceita PE); usar o projeto `ghidra/` ou os comentários de
  disassembly em `project/runner/*.cpp`.

## 04/11 (tarde) - até o vídeo de abertura

- `sceFsSemInit` (TODO no runtime) vinha do `sceDevctl` guest (0x3F8E40, fileio fno 0x17): o thread de desligamento
  do jogo (0x1790A0) mandava `dev9x:` 0x4402 / `cdrom0:` 0x4323. Handlers locais em `project/main.cpp`:
  `Fo2Devctl` (dev9 -> 0, resto -> ENODEV) e `Fo2PowerOffHandler` (0x3F7D00, guest também usava o cliente fileio).
- Causa do desligamento: `StartThread` do runtime devolvia 0; o EE real devolve o id do thread. O FO2 compara com o
  id do CreateThread e, se difere, apaga o semáforo que o thread recém-iniciado espera -> WaitSema = -408 -> desliga.
  Corrigido em `ps2xRuntime/src/lib/Kernel/Syscalls/Thread.cpp` (worktree FO2; levar para o compartilhado).
- Agora: EE gira em 0x3B84B0 = `sceCdStInit` em loop com buffer 0. O buffer IOP (0x20010) vem do RPC do lgAud
  (cmd 0x7D via 0x2C9650), que usa o pool do FMOD: "FMOD: Allocated 292kb for memory pool, 47kb left" e depois
  `FSOUND_Memory_Pool_Alloc ... largest freeblock 0` já na primeira alocação. Pool do FMOD quebrado no IOP emulado.
- Próximo: rodar com `PS2X_TRACE_IOPMEM=1` (trace novo de AllocSysMemory/QueryMaxFree em ps2xIOP iop_sysmem.cpp,
  build 12:51 já tem) e ver por que o pool fica sem bloco livre; `modload:18` não tratado também aparece no lgAud.

## 05/11 - memória do IOP (FMOD sem pool)

- `PS2X_TRACE_IOPMEM=1`: o FMOD aloca 292 KB (0x49390 em 0x19ad00); depois as alocações do ra=0xaca24 esgotam o heap
  (maxFree 0x18f0) e falham 0x4b18 / 0x20020. O heap do runtime era só 0x120000-0x1F0000, bump allocator (nunca
  reaproveitava blocos liberados), e a faixa entre o fim dos módulos (~0xE5000) e 0x120000 ficava perdida.
- Worktree PS2Recomp (ps2xIOP): `IopMemory::allocate` agora é first-fit num arena único 0x10000-0x1F0000 para módulos
  (o loader pede o espaço ao `allocate`) e blocos do sysmem; `maxFreeMemory(total)` = maior lacuna / soma (Query 7/8).
  Build na fila `D:/CodexBuilds/bq.sh` (status `analysis/build-status.txt`). Próximo: smoke com o trace e ver o vídeo.

## 06/11 - interrupção GS VSINT (Claude)

- Travava depois do 1º vídeo: thread main em WaitSema do semáforo de quadro 0x4A52A4 (id 27). O único produtor é o
  handler INTC causa 0 (GS) `FUN_002e3170`, que exige CSR.VSINT (bit 3); o runtime nunca setava VSINT nem disparava a
  causa 0 (análise opencode em analysis/ASK-vsync-sema.md).
- Worktree: VBlankStart agora seta CSR bit 3 e chama dispatchIrq(false, 0) se IMR.VSMSK (0x800) estiver livre
  (`PS2X_GS_VSINT=0` desliga); CSR bits 0..4 são write-one-to-clear (era 0..1).
- Resultado (run fo2/v1): vídeos EMPIRE/BUGBEAR/DOLBY/INTRO abrem e terminam (só 8 setores lidos cada: decodificação do
  PSS a ver), tela LOADING aparece e a barra enche; depois FMOD.IRX repete "open failed: (nome vazio)". Tarefa
  analysis/TASK-fmod-open.md (opencode).

## 06/11 (tarde) - FMOD abre os arquivos; tela de aviso de autosave
- Causa do "FMOD : ERROR - failed to open" com nome vazio: no runtime, `sceSifSendCmd` lia dest/size dos dados extras da
  pilha, mas no EE (EABI) os argumentos 5 e 6 estão em t0/t1 -> o nome nunca ia para o IOP. Corrigido (RPC.cpp, todos os
  worktrees + compartilhado); idem `SifRegisterRpc` (t0-t2). Dados extras agora vão para a RAM do IOP
  (`writeIopMemory`, endereços espelhados 2 MB x4 dobrados em IopMemory::physicalAddress).
- Todos os 17+ IRX recompilados estaticamente (project/iop_aot). Logs: `PS2X_SIFCMD_LOG=1`; ioman "open failed" mostra
  name@/ra.
- Resultado (runs fo2/v6, v7): tela legal -> aviso de autosave ("X OK") desenhado pelo jogo. X não avança: EE 97% ocioso,
  t1 de novo em WaitSema do semáforo de quadro 27 (0x2e365c); o token vem do handler GS 0x2e3170 (VSINT + contador
  0x4A20B8/0x4A20B4 + flag "armado" 0x4A20A4; ver analysis/ASK-vsync-sema.md H2/H3). Próximo: instrumentar o handler
  (quantas vezes roda, em que ramo sai) e o laço VIF1 STR de FUN_002e3580.

## 06/11 (noite) - controle, memory card, menu e carreira (Claude)
- O "semáforo de quadro" não estava travado: com `PS2X_GS_VSINT_LOG=4A20A4` (log novo em EeScheduler.cpp, a cada 60
  vblanks: IMR, máscara INTC, handlers da causa 0 e 8 palavras do jogo) os contadores 0x4A20AC/B0 sobem ~30/s.
  A tela de autosave esperava o X, e o X nunca chegava.
- Causa: o FO2 usa **libpad2** (scePad2*, IOP SIO2D+DBCMAN+DS2U); o DBCMAN do ps2xIOP é stub. Agora a libpad2 do EE é
  nativa no runtime (`Kernel/Stubs/Pad.cpp`: scePad2Init/End/CreateSocket/DeleteSocket/GetState/Read/GetButtonInfo/NoInfo;
  socket = porta; Read devolve 18 bytes na ordem do perfil DS2; GetButtonInfo: 0-15 bits ativo-baixo -> 1/0, 16-31 bytes).
  Ligada por endereço em `project/main.cpp` (0x2bffa0 Init ... 0x2c03c8 GetButtonInfo; 0x3e5c90 = consulta DBCMAN cmd 3 -> 0).
- Memory card: todo Mkdir dava -4 ("Save failed!"). `ensureMcRootExists` guardava "pronto" por porta; a raiz muda de
  <cwd>/mc0 para <dir do ELF>/mc0 depois que o runtime conhece o ELF, e a raiz real nunca era criada. Cache agora por
  caminho (MemoryCard.cpp, aplicado no compartilhado e em LCS/VCS/MotorStorm/Sopranos/FO2). Log `[MC] Mkdir`.
- Resultado (run fo2/mc3): aviso -> verificação do cartão -> save criado (game_files/mc0/BASLUS-21251) -> menu ->
  CAREER (Chili, 2000 CR) -> GO RACE -> tela LOADING da corrida.
- Aberto: (1) LOADING da corrida para depois do `sceGetstat(AUDIENCE.FSB)`; threads EE ociosas (t3/t4 são a fila de jobs
  em 0x2c9230, t1 no semáforo de quadro), IOP 0,2%, thread IOP 14 (FMOD, módulo 12 base 0xd7300) chamando GetThreadId
  em laço (wrapper 0x24cc0). Nenhum RPC/CD/alocação depois. Próximo: ver o RPC do lgAud (0x2C9650) e o estado do FMOD.
  (2) Listras coloridas na parte de baixo da tela (área exibida passa do framebuffer; ver DISPLAY/altura 448 vs 512).
- 21:15 SPU2 nativo trazido do Sopranos (merge 3 vias; iop_spu2.*, iop_audio.h): o FMOD esperava a voz chegar ao bloco
  de silêncio 0x5000 (sceSdGetParam NAX 0x2240) -> com SPU2 real o carregamento termina; CORRIDA rodando (pos 3/8,
  volta 1/3, "GO!", música Papa Roach anunciada). ~28 fps de jogo (alvo 30, VU1 98%).
- Listras embaixo: SMODE2=3 (entrelaçado, field) e DISPLAY DH=895 = meias-linhas; altura agora (DH+1)/2 nos 3
  decodificadores (gs_hw_backend, gs_gpu_scanout.inl, gs_cpu_backend) como o PCSX2 -> 640x448 sem lixo.
  `PS2X_GS_DISPLAY_LOG=1` (gs_frontend.cpp) loga PMODE/SMODE2/DISPFB/DISPLAY.
- VU1 nativo: captura GOW_VU1_CAPTURA=E:/CodexBuilds/flatout2/vu1cap (4 imagens) -> codwaw-ff/scripts/vu1n_gen.py
  flatout2/project/vu1n (100 blocos); build_native.py passa -DPS2X_VU1N_DIR. Build na fila.
