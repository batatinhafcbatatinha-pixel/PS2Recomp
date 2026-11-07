# GTA Vice City Stories (PS2, USA, SLUS-21590) — port nativo

Pipeline PS2Recomp, no modelo do Ben 10 (`ben10-af`).

## Disco

- ISO: `D:\ISO\Grand Theft Auto - Vice City Stories (USA)\...iso`, DVD5 de um volume ISO9660 (2.025.968 setores).
- `SYSTEM.CNF`: `BOOT2 = cdrom0:\SLUS_215.90;1`, `VER = 1.00`, NTSC.
- `SLUS_215.90`: 3.993.368 bytes, SHA-256 `3b43be57d60a29df35e41504d67bbf728d5ce2fc5aab6bbf928cbbc5963fe547`,
  entry `0x1DD6A0`. Um segmento carregado `0x100000`, filesz `0x3CD0A8`, memsz `0x64DF54`.
  `.text 0x100000-0x480548`, `.vutext 0x480550`, `.data 0x481880`, `.vudata 0x492000`, `.rodata 0x499000`,
  `.bss 0x4CD700-0x74DF54`. Quatro overlays DVP (microcódigo VU).
- 341 arquivos, 2,16 GB: `GTA3PS2.IMG`, `BEACH/MAINLA/MALL.IMG` + `.LVZ`, `MAIN.SCM`, `PS2/GAME.DTZ`,
  `AUDIO/` (VB/RAW/SDT), `MOVIES/*.PSS`, `MODULES/` (IOPRP300.IMG, CDSTREAM.IRX da Rockstar, CDVDSTM, LIBSD,
  SDRDRV, MCMAN/MCSERV, PADMAN, SIO2MAN).
- `scripts/extract_game_files.py` extrai e confere SHA-256 (relatório em `analysis/`); o `.disc/layout.tsv`
  do port-installer serve as leituras de setor sem ISO.

## Geração (03/11/2026)

1. `ghidra-export.cmd` (Ghidra 12.1.4, R5900): 9.550 funções em ~5 min.
2. `build-recompiler.cmd`: o FetchContent do toml11 falha por caminho longo no Windows; usei as fontes já
   baixadas em `D:/CodexBuilds/gt4/recompiler/_deps` (`-DFETCHCONTENT_SOURCE_DIR_*`).
3. `python scripts/prepare_game_code.py`: 9.550 funções sem erro de tradução; 3.053 funções-buraco
   (1.003 instruções não tratadas em buracos que são dados no meio do .text, ex. `gap_002ac800`);
   134 handlers SDK nativos (sceIpuInit excluído, como no Ben 10).
4. Build: `ps2-perf-tools/gtavcs-detached.cmd` (buildsup.py, jobs pela RAM), status em `analysis/build-status.txt`.

PS2Recomp: worktree próprio `third_party/PS2Recomp` (branch `gtavcs-port`, a partir de `d8e5a9d`), porque o
checkout compartilhado tem mudanças não commitadas de outra sessão.

## Estado 03/11 17:45

- Build nativo em andamento (buildsup, 1 job, junto com o build do gta-lcs); ~223/373 objetos às 17:40, um
  reinício automático por pouca RAM. Próximo passo: ao linkar, `python scripts/smoke.py --seconds 30`, ler
  `runs/smoke/runtime.log`, depois apagar `runs/smoke`.

## Estado 04/11

- Build de 03/11 21:48 linka e boota: carrega os 8 IRX, lê `AMERICAN.GXT`, procura os VB de cutscene, cria as
  threads do jogo (2 = ociosa 0x40e110, 3 = decodificador 0x31a6f0) e começa o vídeo inicial (`sceCdStStart`
  lbn 0xfba03). O smoke de 03/11 falhou só porque rodou com `PS2X_GS=cpu`.
- Travamento: o decodificador entrega 2 quadros e para; a thread principal (prio 0) gira em
  `sceMpegDemuxPssRing` com BACKPRESSURE. O loop do vídeo (0x40d930) cede com `RotateThreadReadyQueue(1)`,
  o que só funciona com a principal na prio 1. Quem faz isso é o `InitThread` da libkernel (0x468188:
  cria a top thread e chama `ChangeThreadPriority(GetThreadId(), 1)`); o handler nativo do runtime só devolve
  o id. Correção: `InitThread` em `config/sdk-handlers-exclude.txt` (roda o código original).
  Obs.: o mesmo handler nativo vale para os outros jogos PS2 do runtime compartilhado.
- Com o InitThread do jogo e `createThread` aceitando prio 0 (o EE aceita 0..127; a top thread da libkernel nasce
  em 0), o vídeo `MOVIES/STINGS.PSS` (lbn 0xfba03, 1473 setores, 512x448) toca inteiro: 320 quadros.
- Depois disso o player girava: o `sceMpegIsEnd` do jogo (0x452f58, `return mpeg->sys->isEnd`) não foi
  reconhecido pelo analisador, então rodava a cópia do jogo, que lê um campo que o decodificador nativo nunca
  escreve. Também faltavam `sceMpegDelete` (0x452e78) e `sceMpegIsRefBuffEmpty` (0x452f68). Os três foram para
  `config/sdk-handlers-extra.txt`.

## 04/11 noite (Claude)

- Build de 12:58 falhava no `project/main.cpp`: string do `VCS_PROBE_DUMP` quebrada em duas linhas (`\n` virou
  quebra de linha real, C2001). Corrigido; o erro de `ps2_stubs` do mesmo log já não ocorre com os headers de 16:18.
  Rebuild 19:08 OK.
- Boot (`smoke.py --seconds 120`, `PS2X_PAD_FILE` alternando Start/X a cada 1 s a partir de 25 s,
  `PS2X_FRAME_DUMP`): STINGS.PSS inteiro, segundo vídeo (lbn 0xf3359), leituras do IOP (`ff-iop-cd`, 16 setores
  perto do lsn 1.033.000) e a **tela legal "Grand Theft Auto Vice City Stories" desenhada pelo jogo**.
  Depois: EE ~99% ocioso, `game_fps=0`, sem novas leituras. Próximo: ver o que a thread principal espera ali.
- Parado na tela legal: 89 leituras do IOP pelo mesmo CDSTREAM da Rockstar (sid 0x65686577; fn 1 pedido -> 1,
  fn 2 status 0xFA ocupado -> 0 pronto), a última termina normalmente e nenhuma outra é pedida. Threads 1, 3 e 4
  giram em 0x40DC30 (`RotateThreadReadyQueue(1)`, yield), a 2 (top thread prio 0) espera a sema 3. Ainda não se
  sabe que condição a principal espera. Não é o laço de repetição do LCS (lá a mesma leitura se repete).

## 05/11 madrugada (Claude)

- A "tela legal" é um vídeo tocado por `FUN_0040d930` (player PSS). O semáforo 3 em que a thread 2 espera é da
  top thread da libkernel (criado por `InitThread`, ra 0x4681CC; `VCS_PROBE=4676a0` = stub CreateSema): normal.
- Laço: a principal alimenta o anel e o demux (`FUN_00456ce8`); o decodificador (thread 3, `FUN_0031a780`) espera um
  buffer livre na fila de quadros (`&DAT_00673094 + DAT_00486b08`: +0xC cheios, +0x10 capacidade; `FUN_00288d70`).
  Com a fila cheia e o áudio bufferizado (`FUN_001ec8f8`: +0x48 <= +0x54, ambos 0x6000) a exibição começa
  (`FUN_003e3570(1)`: +0x800 = 1).
- Consumo por dois handlers registrados em `FUN_0040dd30` (0x467380 / 0x4673B0 com a0=2):
  `FUN_003e3318` lê `GS_CSR` bit 13 (FIELD) em +0x80C e mostra o quadro por campo (campo 0 e estado 2 -> 1;
  campo 1 e estado 1 -> 0 e +0x808 = 1), se `FUN_004475e8(1,0)` (DMA) = 0; `FUN_003e3518` tira o quadro da fila
  (`FUN_00288e30`, +0xC--) quando +0x808 != 0. Os dois são chamados (probes) e o DMA devolve 0.
  O runtime alterna FIELD a cada VBlankStart (`EeScheduler::processEvent`).
- Relógio de áudio: `sceSdRemote(1, 0x8100, 0)` (status do BlockTrans AutoDMA do canal 0, ra 0x1EC9B0). No primeiro
  vídeo avança 0x400 por consulta; no vídeo da tela legal só alterna entre 0x164780 e 0x166780 (lado sempre 0).
- `project/main.cpp`: `VCS_PROBE_RETALL=1` imprime o retorno nas chamadas em potência de 2.
- Próximo: ver o estado do quadro (`*piVar6`) e +0x80C dentro de `FUN_003e3318` a cada chamada (probe com dump
  de `DAT_00486b08+0x800..0x810`) para saber qual condição nunca fecha; comparar com o STINGS.PSS, que toca inteiro.
- Registro dos handlers: `0x467380` = syscall 0x10 AddIntcHandler(2 = VBLANK_START, `FUN_003e3318`);
  `0x4673B0` = syscall 0x12 AddDmacHandler(2 = canal GIF, `FUN_003e3518`). A liberação do quadro vem no fim do DMA GIF.
- `VCS_PROBE=3e3318 VCS_PROBE_DEREF=486b08,800,2` (nova opção: dump em *(ptr)+off nas chamadas em potência de 2):
  +0x800=1, +0x804 avançando (0x16E, 0x56E, 0xD6E...), +0x808=1, +0x80C=1, +0x810=0 nas amostras (todas de mesma
  paridade de campo). A exibição parece rodar; ainda falta medir a fila (+0xC) e se o decodificador volta a entregar
  quadros. Também verificar a entrada do controle: o pulo do vídeo lê `FUN_002852a8(0)` +0x2A/+0x5A (no Sopranos o
  jogo lia o buffer DMA do pad direto e o runtime não o atualizava).
- Mais medidas (mesma sessão): `FUN_003e3518` roda tarde e zera +0x808 (libera); a fila alterna 1<->2. Mas o demux
  (`FUN_00456ce8`, ra 0x40DACC) devolve 0 consumido em todas as chamadas tardias com o anel cheio (a2=0x50000): o
  decodificador não consome a entrada. Hipótese a testar: a thread do decodificador (3) não ganha CPU porque a
  principal (prio 1) só cede com `RotateThreadReadyQueue(1)` (mesma prioridade); ver prioridades das threads
  (`PS2X_IDLE_TRACE`) e o que `FUN_00452e88` (GetPicture) devolve. A tela legal sai com X (+0x2A/+0x5A do pad,
  via scePadRead nativo) ou no fim dos dados (iVar6 <= 4).
- `GetPicture` nativo (0x452E88) é chamado ~50x/s e devolve 0 (sucesso): o vídeo gira, mas o demux devolve 0 com o
  anel cheio. Prioridades: threads 1, 3 (ociosa) e 4 (decodificador) todas em prio 1 (sem fome de CPU).
  Hipótese atual: o pacote de áudio (stream privado 0xBDFF...) não entra porque o anel de áudio do EE não esvazia,
  já que o relógio (status do BlockTrans) não progride.
- Worktree PS2Recomp (ps2xIOP `iop_memory.cpp`): DMA do SPU2 com AutoDMA ligado (ADMAS, core +0x19A) agora leva o
  tempo real (768 ciclos de IOP por palavra); `PS2X_IOP_ADMA_INSTANT=1` volta ao antigo. Resultado: o status passa a
  alternar também o bit de lado (0x1166B80 / 0x164B80), mas continua só nesses dois valores e o vídeo não sai
  da tela legal. Próximo: ler `FUN_001ecac0`/`FUN_001ecb68` (contabilidade do anel de áudio) e ver como o
  LIBSD calcula o BlockTransStatus (lado + MADR) para emular a posição dentro da metade.

## 05/11 (Claude)

- `FUN_001ec910` (estado 2 = tocando) lê `FUN_0045c298(1, 0x8100, 0)` (sceSdRemote BlockTransStatus) e usa
  `(status & 0xFFFFFF) - base` como posição de leitura do SPU2 dentro do anel (`FUN_001ecac0`: espaço livre em
  blocos de 0x400; `FUN_001ecb68`: cópias). Com o status parado no início de cada metade, a posição só pula de
  meia em meia volta.
- Worktree ps2xIOP: durante um bloco AutoDMA, ler o MADR do DMA do SPU2 (0x1F8010C0 / 0x1F801500) devolve
  `madr inicial + bytes já tocados` (192 ciclos de IOP por byte, relógio = totalCycles do emulador via
  `IopMemory::setCycleSource`). `PS2X_IOP_ADMA_INSTANT=1` desliga. Também: alocador do IOP first-fit (igual FO2).
- Build na fila `D:/CodexBuilds/bq.sh` (gtavcs). Próximo: smoke e ver se o vídeo sai da tela legal.

## 06/11 noite (Claude)
- Run vcs/x1/x2 (build 17:34, MADR do AutoDMA progressivo; X a cada 3 s): fica na tela legal com EE/IOP ociosos. Na
  verdade as threads morrem: t4 e t6 (vídeo, entry 0x31a6f0) retornam com ra=0 (fim normal -> dormant) e depois a
  **thread principal t1 morre em pc=0**: em FUN_001e4da8, `new(0x34)` (0x2BD148) devolve **0xF0000FC2** (fora da RAM),
  o objeto é montado ali, a vtable lida de volta é 0 e `jalr` em 0x1e50e8 vai para 0. Heap do jogo corrompido/esgotado
  depois do vídeo. Próximo: ver o alocador 0x2BD148 (lista livre? 0xF0000FC2 parece marcador) e quem estraga o heap
  (decodificador de vídeo / SetupHeap).
- 21:25 CAUSA e correção: o alocador grande (0x2BC7E8) esgotava e devolvia o sentinela 0xF0000FC2 (VCS_HEAPCHECK=1 em
  project/main.cpp mostra). O heap do jogo é 0x74DF54..0x1FE0000 (0x2BB6A0, ignora EndOfHeap) e o runtime punha a
  arena dele (0x1F00000..0x1FE0000: pacotes RPC/SIF, dados de callback) e as pilhas de interrupção/callback
  (0x1FC8000..0x1FE0000) dentro dessa faixa. Novo `PS2X_RUNTIME_ARENA=<base>:<fim>` (ps2_runtime.cpp, worktree):
  para o VCS `80000:100000` (abaixo do ELF, que começa em 0x100000) -> arena 0x80000-0xE0000, pilhas 0xE0000-0x100000.
- Resultado (run vcs/ar1): tela legal -> cena de abertura no quartel -> gameplay (Vic, HUD, radar). VU1 98%.
  Artefato: retângulos semitransparentes por cima da imagem (filtro de tela/realimentação de framebuffer). Próximo:
  ligar PS2X_RUNTIME_ARENA no launcher/smoke do VCS por padrão, VU1 nativo (captura + vu1n_gen), artefato.

## 07/11 madrugada (Claude) - artefato do céu (faixa de ~54 linhas no topo) e tira à direita
- Run art1 (hw, 180 s, X a cada 3 s): gameplay a 60 fps de apresentação, VU1 98%. Faixa de lixo (blocos ciano sobre
  amarelo) nas ~54 linhas de cima e coluna verde na borda direita. Com PS2X_GS=sw (run sw1) a faixa também aparece
  (mais os retângulos translúcidos, que são do backend sw).
- Encadeamento do quadro (GOW_GS_HW_DUMPAT=32 GOW_GS_HW_DUMPALL=1, run dump1; RTs: fbp 0, 140, 280, 384, 420, 452):
  cena em fbp 140 (640x448 CT32, z em 0x118); pós: #923 140->140 (tcc=0, alfa 0, ZTST GEQUAL z=0 = só onde não há
  geometria); fbp 420 (CT16, fbw 2, 128x128) <- 0x1180; fbp 452 (CT32 256x256) <- tbp 0x3480 CT32 tbw 4;
  #1138 140 += tex 0x3480 (CT32 256x256, tbw 4) * Ad (alpha 0x1000000068); 384 (512x512) <- 140; #1140/#1141 140 <- 384
  (desfoque); #1142 140 <- fbp 0 (quadro anterior, rastro); por fim #1 fbp 0 <- 140 (cópia de exibição; DISPFB = 0).
- Hipótese: tbp 0x3480 lido como CT32 256x256 tbw 4 cobre as páginas 420..451; só as 4 primeiras (= fbp 420, CT16
  128x128) têm dado do quadro -> 32 linhas de textura = ~54 linhas da tela. O resto vem de outra memória. Reinterpretar
  CT16->CT32 (findReinterpretSource) e o conteúdo das páginas 424..451 é onde procurar. GOW_GS_HW_SKIPTBP=3480 deixa a
  tela quase preta (esses passes compõem a imagem), então não dá para pular.
