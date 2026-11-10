# STATUS - Port nativo de The Sopranos: Road to Respect (PS2, USA, SLUS_213.88 v1.01)

Iniciado em 03/11/2026 (Claude), no modelo do GTA LCS/GT4/Ben 10 (PS2Recomp). Runtime: worktree própria
`third_party/PS2Recomp` (branch `sopranos-port`, a partir de d8e5a9d).

## Disco

- ISO `E:\ISO\Sopranos, The - Road to Respect (USA)\...iso` (4.363.223.040 bytes), só leitura, mantida.
- `scripts/extract_game_files.py`: 56 arquivos, 3.082.660.732 bytes em `game_files/`, SHA-256 em
  `analysis/game-files-verification.json`.
- `SYSTEM.CNF`: `BOOT2 = cdrom0:\SLUS_213.88;1`, `VER = 1.01`, NTSC.
- ELF 3.908.440 bytes, SHA-256 `3248f63e...15e958`, entry `0x100008`, um PT_LOAD em `0x100000`
  (filesz 0x3A9898, memsz 0x1BA5356). `.text` 0x100000-0x3C7348, `.vutext` 0x3C7350, overlays VU (DVP),
  `.data` 0x3D3E80, `.bss` até 0x1CA4356. Sem overlays de código EE.
- Dados em pacotes `DATA/*_P.FS` (DATA_P 1 GB, AUDIO_P 1,6 GB, POKER_P, SLOTS_P), vídeos Bink (`DATA/MOVIES/NTSC/*.BIK`,
  decodificados no EE). Módulos IOP: IOPRP271.IMG, STREAM.IRX (próprio), LIBSD, SDRDRV, MCMAN/MCSERV, PADMAN, SIO2MAN.

## Pipeline

1. `analysis/run-ghidra.cmd` (destacado) -> `ghidra-export.cmd` -> `config/ghidra.toml`, labels e funções.
2. `generate.cmd` (analyzer + prepare_config + geração + chunks + gap functions + SDK handlers + roteamento).
3. Build fora do Claude Code: `ps2-perf-tools/sopranos-detached.cmd` (status em `analysis/build-status.txt`).
4. `Play-Sopranos.cmd`; teste de boot: `python scripts/smoke.py --seconds 60 --name boot1`.

## 04/11 - primeiro boot

- Build de 03/11 roda: janela, GS hw e SetupHeap sobem, depois o EE fica preso (`[stall] pc=0x100008 cycle=0`, snapshot velho).
- cdb (escolher o PID pelo caminho do exe: outros ports rodam `ps2EntryRunner.exe` ao mesmo tempo): o laço é
  `0x3C4050` do SDK (procura da tabela de syscalls): `SetSyscall(0x83, 0x3C3FC8)`, `SetSyscall(0x5A, 0x3C3F90)` e depois
  `FindAddress` (syscall 0x83) em 0x80000000-0x80080000 até `hit83-0x20C == hit5A-0x168`. Os handlers 0x3C3F90/0x3C3FC8
  não eram funções recompiladas -> `dispatchSyscallOverride` devolvia KE_ERROR -> laço infinito.
- Causa: `scripts/gen_gap_functions.py` tinha os limites de seção do LCS (`text_end` 0x3865E4). Corrigido para
  `.text` até 0x3C7348, `.data` 0x3D3E80, fim da imagem 0x4A9898: gap functions 1792 -> 4675 (com os dois handlers).
- Worktree do PS2Recomp: correção do LCS no `EeScheduler::createThread` (aceita prioridade 0, como o kernel do EE).

## 04/11 tarde - pacotes .FS e primeira imagem

- `project/main.cpp`: `sceDopen/sceDread/sceDclose` nativos (0x3C13C0/0x3C15F0/0x3C1488; a versão do jogo usa um cliente
  fileio que o `sceFsInit` nativo nunca liga -> RPC sid=0 fn=9). O `sceDread` devolve nomes de arquivo do cdrom0 com `;1`
  (como o cdvdman): a varredura dos pacotes (0x1CFA80) procura `strlwr(nome)` terminando em `"_p.fs;1"`.
- `config/sdk-handlers-extra.txt`: `sceCdSearchFile@0x179ED8` (a interna 0x179BC0 girava em "Libcdvd bind err CdSearchFile").
  Com isso os quatro `DATA/*_P.FS` são achados pelos LSNs originais (`.disc/layout.tsv`) e o STREAM.IRX lê o disco.
- `scripts/route_sdk_tailcalls.py` também considera os handlers `Sop*` do main.cpp.
- Probe: `SOP_PROBE=<addr>` agora imprime também a string em a0 (`a0str`).
- Resultado: logo "The Sopranos / Road to Respect" na tela (runs/boot7). Depois: EE parado (`cycle=0`), IOP ~100%,
  e o processo sai sozinho com código 1 (16-77 s, sem mensagem; não reproduz sob o cdb). IOP avisa
  `SOUND_AllocateStreamBuffer: Memory allocation failed (0x18000)` em uma das corridas.

## 04/11 noite - menu principal (Claude)

- Worktree PS2Recomp (ps2xIOP): `IopMemory::allocate` agora faz first-fit a partir de `HeapBase` (reaproveita blocos
  liberados abaixo do cursor, como o sysmem do IOP) e `maxFreeMemory` devolve o maior buco livre
  (QueryMaxFreeMemSize). `AllocSysMemory` que falha loga `[IOP sysmem] ... maxFree=`. Levar ao compartilhado.
- Antes (prof2): o driver de som do jogo trocava de cena, liberava e realocava os pools e falhava em
  `SOUND_AllocateStreamBuffer 0x18000` -> tela preta, game_fps=0.
- Depois (runs/heap2-4, 90 s, `PS2X_PAD_FILE` alternando Start/X a partir de 20 s): sem erro de alocação,
  game_fps ~30 contínuo, VU1 ~27%, **Menu Principal desenhado** (New Game / Load Game / Options / Bonus Material).
- O travamento antigo (EE `cycle=0`, IOP ~100% logo no início) ainda aparece às vezes: 1 de 4 corridas (heap1).
  Intermitente, não investigado ainda (anexar `cdb -pv` depois de ~40 s quando aparecer).
- Próximo: entrar em New Game pelo menu (o X alternado não saiu do menu em 90 s) e ver a próxima tela.
- **Controle**: o jogo não chama `scePadRead`; abre as 8 portas/slots com buffer DMA em `0x19980C0 + n*0x100`
  (`FUN_001e98a0`) e lê direto dele em `FUN_001eab20` (+1 modo, +2/+3 botões) depois de `scePadGetState`.
  O `scePadPortOpen` nativo só zerava o buffer. Worktree PS2Recomp (`Stubs/Pad.cpp`): `scePadGetState` estável agora
  reescreve o buffer DMA da porta com `readPadPortData` (o padman faz isso a cada quadro). Levar ao compartilhado.
  Resultado (runs/pad1): o direcional move o cursor e o X entra em "Bonus Material" (Concept Art/Movies/Credits).
- New Game (runs/newgame1): X no menu -> RPCs do STREAM (sid 0x12345), driver de som libera e realoca pools
  (`3e01 80c 18000 ...`) -> tela preta, EE ~99,7% ocioso, IOP ocioso, `[stall] thread=1 pc=0x121384` (dentro de
  `FUN_001212e0`, recursiva, chamada de `FUN_00120888`/`FUN_00121280`; o pc do snapshot pode estar velho).
  Próximo: ver o que a thread principal espera depois do último RPC 0x3e01 (`PS2X_IDLE_TRACE=1`, `SOP_PROBE`).
- Ferramenta: `scripts/DecompAt.java` (headless, `-process SLUS_213.88 -noanalysis -readOnly`): decompila a função
  que contém cada endereço e lista os chamadores.
- **Deadlock do mutex do stream** (New Game): o STREAM.IRX avisa o EE pelo servidor RPC do lado EE 0x332588
  (sid 0x12344321, thread 2 `0x337268` no PS2); o handler chama o callback 0x329EC8 (registrado por `FUN_003377f0` em
  `FUN_00328280`), que faz `WaitSema` no mutex `DAT_01ca3a80`. O runtime despacha esse servidor dentro da thread EE
  que estiver rodando; quando é a thread 1 dentro de um dono do mutex (0x328550/0x328978/0x328bc8/0x32ad68/0x32af00),
  ela esperava a si mesma (`[idle] t1 why=2 obj=3 pc=0x329f1c`). `FUN_0032ab00` (troca de cena) apaga e recria o mutex.
- `project/main.cpp`: `SopInstallStreamGuards` envolve os 5 donos (profundidade por thread) e o callback; se o callback
  chega na thread que segura o mutex, ele "empresta" o mutex (count 0 -> 1 durante a chamada; `[sop] stream callback
  re-entered`). `SOP_NO_STREAM_GUARD=1` desliga. Primeira tentativa (adiar o callback devolvendo 0 ao servidor)
  travava o IOP no boot: descartada (o código de adiamento `SopRunDeferred` ficou sem uso).
- Resultado (runs/newgame5): New Game -> **tela de carregamento** (pistola). Depois EE ~100% e IOP ~100% por 2 min,
  game_fps=0, `[stall] thread=1 pc=0x318264` / `0x2a3e38`. Próximo: pilha do EE sob cdb nesse ponto (laço de espera
  pelo IOP?) e o que o IOP gira.
- Tela de carregamento: o IOP girava na thread 7 do STREAM.IRX (entrada 0x5063C) dentro do `sceSdGetAddr` do LIBSD
  (0x48CF0, lê NAX das vozes; `PS2X_IOP_PCSAMPLE=2000`): o SPU2 do runtime é só memória e o NAX nunca anda.
  Worktree PS2Recomp (`ps2xIOP/src/emulator/core/iop_memory.{h,cpp}`): modelo mínimo de endereços do SPU2 —
  DMA SPU (RAM->SPU) copiado para 2 MB de RAM de som em TSA, KON/KOFF (gatilhos, palavra limpa depois), NAX das vozes
  ligadas avança em tempo real pelo pitch através dos blocos ADPCM (flags loop start/end/repeat, ENDX).
  `PS2X_IOP_SPU_NAX_OFF=1` desliga. Compila; sem som (não é um mixer).
- Resultado (runs/spu1/spu2): a thread 7 sai do `sceSdGetAddr` e passa a rodar o laço de vozes 0x582E8 (lê ENDX),
  esperando seu semáforo a cada volta. A tela de carregamento continua: depois do último callback do stream não
  há mais RPC, leitura de disco nem syscall; EE 100% em código (pcs variados 0x22F9AC/0x3B2BF8/0x318264).
  Hipóteses: (a) o EE espera um aviso do STREAM.IRX (servidor 0x12344321) que não chega porque falta algo no modelo
  do SPU2 (ENDX não é limpo no KON; IRQ do SPU2 não existe); (b) a falta de som/IRQ trava o carregamento.
  Próximo: cdb no GameThread com várias amostras para achar o laço do EE; ver que avisos o STREAM.IRX manda ao EE
  (`[ff-rpc]` do servidor 0x12344321) e qual condição dispara cada um.

## 05/11 (Claude)

- Worktree: alocador do IOP first-fit (o heap do runtime era bump, sem reaproveitar blocos; FO2/GT4 travavam por isso).
  O EE/IOP a 100% na tela de carregamento pode ser o mesmo. Build na fila `D:/CodexBuilds/bq.sh` (sopranos).
- Hipótese do lagent (não verificada, `analysis/ASK-loading.md`): `FUN_001fc258` (estado do recurso em
  0x1C9B400 + idx*0x60 +0x58; flag 0x3FEA9C) seria o laço; os pcs amostrados não batem com ele. Conferir com cdb
  depois do rebuild.

## 06/11 (Claude) — IOP e VU1 nativos

- Menu principal funciona (30 fps). New Game -> carregamento -> 1ª cena 3D; travava porque o thread 7 do STREAM.IRX
  (vozes, acordado por hard timer) consumia o IOP interpretado e a thread do jogo ficava sem tempo (iop 100%).
- Novo: recompilação estática dos IRX (ps2xIOP/tools/irx_aot.py + include/ps2x/iop/iop_aot.h; `ps2-perf-tools/irx_aot_setup.py
  <jogo> <pasta IRX>` gera project/iop_aot e o bloco CMake). Log "[IOP] AOT <nome> ... native|interpreted";
  `PS2X_IOP_AOT=0` volta ao interpretador; `PS2X_IOP_AOT_DUMP=<dir>` salva IRX interpretados (buffers) para gerar.
- VU1 nativo: captura `GOW_VU1_CAPTURA=E:/CodexBuilds/sopranos/vu1cap`, `codwaw-ff/scripts/vu1n_gen.py project/vu1n <cap>`;
  build_native.py passa -DPS2X_VU1N_DIR se project/vu1n existe.
- Resultado (run sopranos/vu1): cena de abertura a 30 fps, mas geometria esticada/sem textura e a legenda não avança
  (com IOP nativo e VU1 interpretado a 2 fps ela avançava). Próximo: (1) rebuild com MCMAN/PADMAN/SIO2MAN também
  nativos (iop ainda 40%), (2) geometria: comparar VIF unpack/VU1 com captura do GS, (3) avanço da cena ligado ao
  stream de áudio (SPU2 sem mixer).
- 06/11 17:10: bug do gerador (último case de cada bloco caía no `default:`) corrigido -> com todos os IRX nativos e VU1
  nativo, a cena de abertura renderiza CORRETAMENTE a 30 fps (run sopranos/fix1: letreiro, carro, céu). A geometria
  quebrada de antes era esse bug. Mas a cena fica parada na 1ª legenda; IOP 65%, 100% das amostras no thread 7
  (STREAM.IRX, laço de vozes, chamadas sceSdGetParam no LIBSD). O avanço da cena depende do stream de áudio (posição
  NAX/ENDX/IRQ do SPU2); o runtime não tem mixer nem IRQ do SPU2. Próximo: SPU2 nativo (vozes ADPCM, AutoDMA, IRQ,
  saída de áudio) no ps2xIOP compartilhado — serve a Sopranos, MotorStorm, VCS (relógio do BlockTrans) e FO2.

## 06/11 (noite) - SPU2 nativo recebendo dados (Claude)
- Build quebrava: `#include "ps2x/iop/iop_audio.h"` estava dentro do `#if __linux__` em ps2_runtime.cpp. Corrigido.
- `PS2X_SPU2_LOG=1` (iop_spu2.cpp): uma linha por segundo emulado com key-on/off, IRQ, leituras/escritas, DMA, vozes ativas,
  ENDX, ATTR, ADMAS, IRQA, 1ª voz ativa (nax/lsa/pitch/env) e os 3 registradores mais lidos/escritos.
- Causa da legenda parada: `dma=0` — nenhuma DMA chegava ao SPU2 novo; iop_memory.cpp só copiava se o modelo antigo
  (`spuModelEnabled()`, desligado quando o IopSpu2 está ativo) estivesse ligado. Corrigido; AutoDMA (ADMA) agora termina
  no ritmo do áudio (256 amostras por bloco de 1 KB).
- Resultado (runs spu3, cb1): a cena passa da 1ª legenda, câmera desce pelo letreiro, 2ª fala ("You gotta learn to respect
  women...") aparece; depois a cena anda muito devagar e a legenda não muda até 200 s.
- O STREAM.IRX é a MultiStream V7.2 (Jason Page/SCEE): faz polling do ENDX (0x340/0x342/0x740) ~44 mil vezes/s e limpa
  ~8,8 mil/s. ENDX agora limpa só a metade escrita (antes limpava os 24 bits em qualquer escrita) — build na fila.
- Callback de stream do EE (0x329EC8, via RPC IOP->EE 0x12344321) roda dentro de qualquer thread do EE; o contorno em
  project/main.cpp "empresta" o mutex quando cai na thread que o segura (o comentário fala em adiar, o código empresta).
  O callback não tem argumentos, então adiar é possível; suspeito de evento de fim de fala perdido. A investigar.
- 19:40 run spu5 (ENDX por metade): igual — 6 vozes ligadas no s79 tocam em loop até o fim (230 s), nenhuma key-on nova,
  câmera se move devagar sobre o letreiro, legenda da 2ª fala fixa. O SPU2 roda ~em tempo real (243 s emulados em 230 s).
  Próximo: descobrir o que o EE espera para pedir a próxima fala (RPC de status da MultiStream / callback 0x329EC8).
- 07/11 02:00 (Claude): AOT regenerado com o gerador corrigido (HI16/LO16 pela ordem da tabela; era a causa do GTA LCS).
  Run aot2 (scripts/drive.py novo, X a cada 3 s): menu principal -> New Game -> cena de abertura, mas continua parada na
  2ª fala por 200 s, IOP 66% (STREAM.IRX). Não era o AOT. Segue o próximo passo de antes (o que o EE espera da MultiStream).

## 08/11 (Claude) — pipeline genérico
- Rodar sempre com o perfil: `--port .local/ports/sopranos-prof --work .local/measure/sopranos` (o port.toml/handlers só
  são lidos da pasta do port; com `--port E:/CodexBuilds/sopranos` o perfil não entra e o jogo fica no laço do
  sceGsSyncV do próprio jogo em 0x1dbbd0).
- Com o perfil: para em "Checking memory card" (fps 0). MC ok (sem save: Chdir -4). EE faz 4 pedidos SIF cid 0xb ao
  módulo RAD_IOP_SIDE (IRX de 7 KB carregado de buffer, id 8, leitor de arquivos Bink/RAD; respostas tipo 1 = handle,
  tipo 0 = 4 KB lidos com palavra 5 = erro 0, tipo 4 = tique do alarme). Depois de 2 blocos de 4 KB o EE não pede mais;
  laço principal fica na máquina de estados de leitura 0x16d4xx (FUN_0016d1e8). Handler EE dos comandos = 0x169740.
- Testes sem efeito: excluir sceSifGet/SetSreg, sceSifSearchModuleByName/Stop/UnloadModule; entregar a 1ª invocação
  entregável em vez da da frente; PS2X_VIF1_IRQ=0; PS2X_INTC_STAT_PLAIN=1.
- O pipeline antigo (games/sopranos/project) chegava ao menu e à cena de abertura; a diferença está nas ligações
  SDK/runtime. Ferramenta nova: PS2X_PCSAMPLE=<s> (histograma de pc/ra do EE).

## 08-09/11 (Claude) — pipeline genérico, de "Checking memory card" até a jogabilidade
- Boot: IOP sysclib _wmemset/_wmemcopy em BYTES (apagavam o próprio RAD); isceSifSendCmd ligado; contexto de interrupção
  do IOP (QueryIntrContext). Vídeos Bink: família MMI de HI/LO 128 bits, PINTH/PINTEH, embaralhamentos, PADDUH/PSUBUH.
  Personagens nas cenas: VU0 macro VLQI/VSQI/VLQD/VSQD/VILWR/VISWR na memória do VU0 (0x11004000) e VSQI/VSQD com
  campos it/fs certos (os ossos vão por VSQI vf24,(vi12++) antes do VCALLMS 0x90). VDIV/VSQRT/VRSQRT como no VU.
- port.toml: libm (sin/cos/atan/atan2/pow/sqrt/floor/fabs/__kernel_*) não ligada aos stubs do runtime (liam floats;
  as do jogo são double em GPRs).
- ABERTO: na jogabilidade o mundo e os personagens não são desenhados (GS só recebe HUD, VU1 <1%, nenhuma thread
  bloqueada; cenas no motor funcionam). Referência PCSX2 (C:\Users\dudua\Downloads\pcsx2-v2.9.166-windows-x64-Qt,
  portable, BIOS 0230a) mostra o beco iluminado com os personagens. Comparar RAM: .p2s é zip com zstd (eeMemory.bin);
  heap/pools do jogo (0x1514d00-0x1915d00, 0x1a8xxxx) mudam de ordem, só globais comparam.
- LEMBRETE: `ps2port tools` só reconstrói o recompilador com --force.

## 09/11 (Claude) — mundo da jogabilidade aparece (backend gpu)
- A lógica do jogo estava certa (RAM igual ao PCSX2, só diferenças de 1 ULP) e o VIF1 recebia cadeias completas.
  Trace do GS (PS2X_GS=gpu, PS2X_GS_TRACE): o quadro inteiro chega ao GS (mundo, personagens, HUD). O jogo alterna
  os buffers de exibição fbp 0/112 (512x448x32, Z em 224), desenha sombras num buffer de 16 bits (fbp 336, psm 0xa)
  e faz bloom com sprites em tiras que leem o próprio quadro (fbp 336/368) e o Z (tbp 7168) como textura.
- O backend **hw** perde o mundo nesses passes (fica só HUD + "blocos laranja"); o backend **gpu** mostra o beco e
  os personagens como o PCSX2 a 30 fps cheios. port.toml: [env] PS2X_GS = "gpu" (também na cópia .local/ports).
- ABERTO: (1) faixas longas saindo dos personagens (membros esticados até a câmera; triângulos com vértice atrás da
  câmera, z negativo, que o ADC deveria descartar); (2) bug do hw nos passes render-to-texture; (3) blocos escuros no
  fundo do menu principal.
- VOPMSUB do recompilador NÃO tinha bug (idioma OPMULA ACC,a,b / OPMSUB d,b,a; mesmas lanes do OPMULA). A troca
  feita em 4411458 foi revertida em cad901d. ps2port generate: troca da linha output por regex (configs antigos).
- Diagnósticos novos: PS2X_DMA_STATS=1 (+ analysis/dma-log.trigger percorre as tags das cadeias), GOW_VU1_KICKPRIM=800
  (resumo por segundo de vértices desenhados/ADC nos pacotes do VU1).
- Faixas (membros esticados), investigação 09/11 03:00: o vértice que vira faixa tem X de tela > 16 bits (dá a volta
  no GS) e Z negativo, e sai com ADC=0 porque a matriz de clip por osso explode junto com a de desenho (escala
  ~1e2..1e5 nos ossos 0-3/6-7 de um personagem; os índices de osso nos pesos estão certos). As matrizes chegam ao
  VU1 por UNPACK relativo ao TOPS (área 0x84-0x143 sobreposta aos buffers 0x64/0xfd); nenhuma execução do VU1 as
  escreve; não estão na RAM como floats (pacote temporário). Descartado: VU0 nativo (GOW_VU0_NATIVE_OFF=1 igual),
  VU1 nativo (verify igual ao interpretador), paridade DBF (PS2X_VIF_DBF_INVERT=1 igual), OPMSUB (correto).
  Próximo: achar no EE quem monta esses ossos (pacote 0x15xxxxx/0x17xxxxx) e comparar com o PCSX2 na mesma cena.
- Diagnósticos: PS2X_VIF_SEQLOG=<s>:<n> (sequência BASE/OFFSET/UNPACK/MSCAL com TOPS/DBF), PS2X_VIF_UNPACKLOG +
  PS2X_VIF_UNPACK_ADDR=<hex> (só UNPACKs que cobrem esse endereço, com src na RAM), PS2X_VU_DUMP_N=<execuções>,
  PS2X_VIF_DBF_INVERT=1, GOW_VU0_NATIVE_OFF=1, dump vu-negz (GOW_VU1_KICKPRIM + GOW_VU1_NEGZ_AFTER).
- **Faixas corrigidas (09/11 04:10)**: o VCALLMS do VU0 tinha teto de 4096 ciclos no runtime
  (executeVU0Microprogram). O microprograma 0x738 converte o quatérnio de cada osso em matriz numa chamada só
  (~89 ciclos/osso); com 57 ossos ele era cortado no osso ~46, e os ossos seguintes ficavam como quatérnio cru no
  scratchpad -> multiplicados pela matriz do pai viravam faixas. Teto agora igual ao do VU1 (1<<22). Caminho do
  diagnóstico: vu-negz -> pacote 0x152b920 -> watch (FUN_001c1690 copia de 0x70000000+i*0x40) -> watch no scratchpad
  (PS2X_WATCH=7000xxxx agora vale para o scratchpad) -> FUN_00149af8/00156604/00156a38 (Ghidra) -> VCALLMS cortado.
- Blocos claros/escuros no fundo do menu principal: NÃO é defeito — o PCSX2 mostra os mesmos painéis animados.
- Backend hw depois da correção do VU0: mundo e personagens aparecem (resolução maior), mas o painel do tutorial
  some (só o texto) e aparecem glifos laranja soltos (texturas 8 bits com CLUT do HUD). O perfil segue com
  PS2X_GS="gpu" (correto, 30 fps); o hw fica como pendência do renderizador.

## 60 fps (2026-11-09)
- The game runs at 30 fps by design: the frame swap (0x1DD3F4) waits two vsyncs while the flag 0x3F8020 is set
  (set to 1 at boot by 0x1F4D68; 0x1F6E50 toggles it, no direct caller = debug option). With the flag 0 it waits one.
- `patches/60fps.pnach` (patch=1, EE 0x3F8020 = 0 every frame) -> menus, pause menu, Bink screens at 58-60 fps.
- Video-mode init 0x1DB5F8 stores field time 1/60 (0x3F8028) and frame time 1/30 (0x3F802C) (PAL 1/50, 1/25); no
  direct loads of either were found, so whether game logic is tied to 30 fps is UNVERIFIED (check movement speed
  once 3D runs above 30).
- 3D (cut-scenes, gameplay) stays ~29 fps with or without the patch: EE waits for VIF/VU1 (ee_sync 30-40%, VU1
  blocks partly interpreted) and the IOP's STREAM.IRX MultiStream thread (entry 0x5523C, signals its own semaphore
  and polls sceSdGetAddr(NAX) for 48 voices) uses ~40% of the game thread. Turning that poll into a 500 us delay cut
  IOP time to ~11% but LOWERED fps (EE waits for stream answers) -> reverted. Import-stub decode is now cached.
