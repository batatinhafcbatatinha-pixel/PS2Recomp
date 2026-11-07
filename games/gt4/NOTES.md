# NOTES - Gran Turismo 4 (USA) v2.00, native port

Registro tecnico do que foi **observado** (nao testado em runtime). Nada aqui foi
validado executando o jogo: a regra da equipe proibe rodar jogo, runner ou launcher.

## Midia e extracao

- ISO: `D:\ISO\Gran Turismo 4 (USA) (v2.00)\Gran Turismo 4 (USA) (v2.00).iso`,
  5.314.478.080 bytes, aberta somente para leitura.
- DVD9 com **dois volumes ISO9660**: camada 1 no setor 16, camada 2 com setor 0 em
  1.317.040 (descritores em 1.317.056).
- `scripts/extract_game_files.py` extraiu 137 arquivos (5.270.980.724 bytes) para
  `D:\CodexBuilds\gt4\game_files`, SHA-256 conferido contra os extents relidos
  (`analysis/game-files-verification.json`).

## Boot

`SYSTEM.CNF`: `BOOT2 = cdrom0:\SCUS_973.28;1`, `VER = 2.00`, `VMODE = NTSC`.

`SCUS_973.28` (273.020 bytes, ELF32 LE MIPS, entry `0x01000008`, SHA-256
`f8f10823...8019fa`) e **so o launcher**: strings `core.gt4`, `cdrom0:\IRX\`,
`cdrom0:\GAMEDATA`, `cdrom0:\IOPRP300.IMG;1`, classes `ImageLoader`/`DiskSource`/
`HostSource`/`MCardSource`. Ele carrega `CORE.GT4` e os dados do `GT4.VOL`.

## GT4.VOL - formato RoFS (decodificado nesta etapa)

O comeco do arquivo e uma **tabela de conteudo falsa no estilo GT3** (magic
`AC B9 90 AD` + versao `0x00020002`). A ToC real fica em pagina de 0x800, na
**pagina 0x2159** (offset `0x10AC800`).

Cabecalho real (0x40 bytes):

| offset | campo |
| --- | --- |
| 0x00 | magic `0xAD90B9AC` (big-endian; valor negado = `RoFS`) |
| 0x04 | versao `0x00030001` |
| 0x08 | tamanho da ToC comprimida (`0x59436`) |
| 0x0C | numero de paginas do cabecalho+ToC (179) |
| 0x10 | tamanho da pagina (`0x800`) |
| 0x12 | numero de entradas/paginas da ToC (407) |

- Tabela de offsets em `0x40`, `entry_count + 1` inteiros. Cada valor e
  `valor ^ (0x14AC327A * (i + 1))` (32 bits).
- Cada pagina: deflate cru (`zlib.decompressobj(-15)`), depois XOR `0x55`.
- Cabecalho de pagina: `short flag` (0 = pagina de entradas, != 0 = pagina de
  indice), `short entry_count` (numero real = `entry_count / 2`), `int next`,
  `int prev`.
- Tabela de entradas no fim da pagina, lida de tras para frente em blocos de 8
  bytes: `short entry_offset`, `short entry_length`, `short meta_offset`,
  `short meta_length`.
- No registro: `int parent` (big-endian), nome (`entry_length - 4` bytes). No
  metadado: `byte tipo` (0 diretorio, 1 arquivo, 2 arquivo comprimido), depois
  `int node_id` (diretorio) ou `int page_offset, int mtime, int size`
  (arquivo) ou `int page_offset, int mtime, int compressed_size, int real_size`
  (comprimido).
- Offset do arquivo = `data_offset + page_offset * 0x800`, com
  `data_offset = base + page_count * 0x800 = 0x1106000`.
- Arquivo comprimido comeca com `C5 EE F7 FF` + tamanho negativo; o payload e
  deflate cru a partir de `+8`.

Resultado (`scripts/index_vol.py`, `analysis/gt4-vol-index.json`):

- **18.562 nos**: 210 diretorios e **18.352 arquivos**;
- 4.092 arquivos crus e **14.260 comprimidos** (PS2Zip);
- 3.795.832.691 bytes reais, 2.420.417.876 bytes empacotados;
- **nenhum ELF** dentro do VOL: nao ha overlays de codigo EE no volume.

`GT4L1.VOL` (2.595.657.728 bytes) e um segundo volume RoFS minusculo (2 paginas,
3 entradas de ToC) que contem apenas `./mpeg/main/*.pss` (filmes MPEG). Nao tem
codigo. Ver `analysis/gt4-vol-l1-index.json`.

## CORE.GT4 - executavel real do jogo (decodificado nesta etapa)

`CORE.GT4` (2.020.861 bytes) = camada de compressao do formato CORE:

| offset | campo |
| --- | --- |
| 0x00 | `ushort` boot load flags = `0x0101` |
| 0x02 | `uint` tamanho descomprimido = 6.119.116 |
| 0x06 | deflate cru ate o fim do arquivo |

O conteudo descomprimido e o container do executavel:

| campo | valor observado |
| --- | --- |
| tamanho do hash 1 / hash | 128 / SHA-512 |
| tamanho do hash 2 / hash | 128 / SHA-512 |
| numero de secoes | 3 |
| entrypoint | `0x00100008` |
| secao 0 | `0x006179FC`, 0x18 bytes (24 bytes, ultimos 4 = `0x006DDDF0`) |
| secao 1 | `0x00100000`, `0x517A14` bytes (codigo + rodata) |
| secao 2 | `0x00617A80`, `0x0BE37C` bytes (dados gravaveis) |

- O carregamento cobre `0x00100000`-`0x006D5DFC`; o codigo de inicializacao
  limpa BSS de `0x006D5E00` ate `0x008A215C` (lui/addiu vistos em `0x100110`),
  batendo com o ponteiro `0x006DDDF0` da secao 0.
- `scripts/build_core_elf.py` reconstroi um **ELF32 LE MIPS** normal
  (`D:\CodexBuilds\gt4\decompressed\CORE.ELF`, SHA-256
  `169c705c...5202603`) com dois PT_LOAD (RX em `0x00100000`, RW em
  `0x00617A80`), entry `0x00100008`.
- Observacao em aberto: os primeiros `0x90` bytes de `0x00100000` parecem uma
  tabela de ponteiros para a memoria da VU1 (`0x70000C28`, `0x70001428`, ... de
  0x800 em 0x800) e nao codigo; o codigo de CRT (limpeza de BSS, `mtc1` em
  sequencia) comeca por volta de `0x1000A0`-`0x100110`. Confirmar no
  desmontador do Ghidra antes de tratar esse trecho como funcao.

## REA

- `rea.open_binary` em `SCUS_973.28` (e em qualquer ELF MIPS) e **recusado**:
  `target_format_unsupported` - "Windows Ghidra P0 accepts native PE and
  explicitly selected raw 6502 targets only". Hopper nao roda em win32, IDA e
  Kuna nao configurados e sem lane MIPS. Registrado conforme a regra da equipe.
- Analise de codigo segue com Ghidra R5900 (`r5900:LE:32:default`), extensao
  `ghidra-emotionengine-reloaded`.

## Ferramentas

- `ps2_recomp.exe` / `ps2_analyzer.exe`: build compartilhado do `codwaw-ff`
  (`work/codwaw-ff/build/ps2recomp/ps2xRecomp`), localizado por
  `scripts/local_tools.py`.
- Ghidra: `C:\Users\dudua\Downloads\ghidra_12.1.4_PUBLIC_20260921\ghidra_12.1.4_PUBLIC`.

## 03/11 - travamento no handshake do MSIFRPC

- Depois de carregar os 22 IRX, a thread principal do EE fica em `FUN_00590978`
  (= `sceSifMInitRpc` do libmrpc): registra o handler 0x80000018, manda
  `SET_SREG(1,1)` ao IOP (`sceSifSendCmd 0x80000001`) e espera `sceSifGetSreg(1) != 0`.
- O lado IOP e o `MSIFRPC.IRX` export #4 (offset 0x38): registra 0x80000019/1A/1D,
  manda `SET_SREG(1,1)` ao EE e espera o sreg 1 do IOP. Quem chama e uma thread
  (prio 0x40) criada no start do `RTINETB.IRX` (0x9bc -> msifrpc#4 e #17).
- Antes, o runtime nao entregava comandos EE->IOP e o IOP nao tinha sregs; as mudancas
  em `third_party/PS2Recomp` (`sendIopSifCommand`, `IopRpcBridge::receiveEeCommand`,
  sifcmd #6/#7/#10/#11) fazem os dois lados. Rebuild completo iniciado 03/11 a tarde.
- Teste boot9 (03/11 noite): o handshake passou. O jogo segue com RPCs de CD (`PCDV`), som (`PUPS`)
  e memory card. Novos bloqueios:
  - O IOP nao tinha `thmsgbx` (o MSIFRPC usa CreateMbx/DeleteMbx/iSendMbx/ReceiveMbx). Implementado no
    `IopKernel` (`dispatchMailboxImport`, estado `Mailbox`).
  - A thread 11 do EE comeca em `0x4B0210`, que o Ghidra juntou com `FUN_004b01f8` (essa termina numa chamada
    que nao retorna + nop). `gen_gap_functions.py` agora cria entradas para prologos depois de
    `jal/j/jr`+delay+nop dentro de funcoes do Ghidra, e tambem para ponteiros de dados que caem num prologo
    (20 entradas novas). Sem `analysis/analyzer.log` (apagado), ele usa os inicios do `gap-functions.csv` anterior.
- Para o build do GT4: o objeto da copia local `gt4-runtime/ps2_runtime.cpp` pode ficar velho quando um header
  muda de acesso (`private` -> `public`); apague esse .obj se o link reclamar de `sendIopSifCommand`.

## 04/11 - thread 2 sleeping in the DMA wait (game_fps=0)

- boot10 (after the libfs handlers in `project/main.cpp`): the IRX modules load, the RPCs (PCDV, PUPS, PBGM, LGDEV
  0x046D046D = Logitech wheel, answers "no device") go back and forth, and the main thread polls the memory card
  through `SceKernelDelayThread` (`FUN_005aed18(2000)`). No game frame.
- `PS2X_IDLE_TRACE=1`: thread 2 is in `SleepThread` inside `FUN_004a0f68` (wait for DMA channel 1/2: flag
  `0x7000206d + ch*0xc`, cleared by the DMAC handler `0x4ab6d8` registered in `FUN_004a0d98`).
  `GT4_PROBE=4ab6d8` -> the handler is **never** called.
- Cause: the DMA kicks (`FUN_004aba50` chain / `FUN_004a1140` normal) write scratchpad addresses as
  `0x80000000 | offset` (bit 31 = SPR, real hardware). The runtime only recognized `0x70000000+`, read the tag from RAM
  at 0 and could leave the channel without a transfer -> no completion -> no interrupt.
- Fix in `third_party/PS2Recomp` (`ps2DmaAddress` in ps2_memory.h; MADR/TADR/ASR/tag ADDR in `writeIORegister`)
  + a kick with nothing to send completes the channel (STR off + DMAC cause).
- Ghidra decompile from the command line: headless `-process CORE.ELF -noanalysis -readOnly` with a postScript that
  calls `DecompInterface` (~30 s per call).

## 05/11 - EE preso no IOP emulado

- boot17: game_fps 1 -> 0, EE sem ociosidade. Handler de VBLANK `FUN_004ab430` (INTC 2) roda e acorda a thread 2
  (`FUN_004a21f0` espera o contador 0x70002050): esse lado está certo. Thread de jogo amostrada no cdb dentro do cache de
  setores de CD (`FUN_00548a80` -> `FUN_00548640` -> RPC PCDV fn 3).
- Perfil (`PS2X_PROFILE=1 PS2X_PROFILE_AT=40`, `ps2-perf-tools/prof.py gt4 prof1 --map ...`): a thread game passa ~100%
  dentro do IOP emulado (read32/executeInstruction/decode) com `IopMemory::allocate` 6% e sysmem 1,5%: algum módulo
  IOP chama AllocSysMemory em laço.
- Causa provável (mesma do FlatOut 2): o heap do IOP era bump (0x120000-0x1F0000, blocos liberados nunca voltavam).
  `third_party/PS2Recomp` (compartilhado, ainda sem commit) agora usa arena first-fit única 0x10000-0x1F0000 para
  módulos e sysmem, Query 7/8 = maior lacuna / soma. Build na fila (`D:/CodexBuilds/bq.sh`). Próximo: smoke de 90 s.
- Ferramentas novas: `scripts/DecompAt.java`, `scripts/RefsTo.java` (projeto `D:\CodexBuilds\gt4\ghidra` GT4CORE, CORE.ELF).

## 06/11 (Claude)
- MC: runtime compartilhado agora devolve -1 (cartão novo) no 1º GetInfo de cada porta (`PS2X_MC_FIRST_CHANGED=0`
  desliga); análise do Codex em analysis/ASK-mcloop.md. Cache do jogo passa a 2/8192/1.
- Todos os 20 IRX do GT4 (inclusive pdi* da Polyphony) rodam recompilados estaticamente (project/iop_aot,
  `ps2-perf-tools/irx_aot_setup.py gt4 D:/CodexBuilds/gt4/game_files/IRX`).
- Ainda game_fps=0, ee_idle ~85%: threads 3/5/7/10 esperam semáforos em 0x578540 (espera de RPC), t1 em DelayThread
  (0x5aedc0); RPCs PCDV fn3/fn1, SPUP fn4, LGDEV, PBGM fn8 repetem periodicamente. Nenhum quadro do GS.
  Próximo: descobrir o que a thread principal espera antes do 1º quadro (tarefa para o Codex).

## 07/11 02:15 (Claude)
- AOT dos 20 IRX regenerado com o gerador corrigido (HI16/LO16 pela ordem da tabela, causa do GTA LCS); build 02:08.
  Igual: game_fps=0, ee_idle 93%, polling de GetInfo do cartão nas duas portas; PS2X_IOP_AOT=0 dá o mesmo (não é o AOT).
  Log: "[IOP] unhandled import modload:16" duas vezes (ver qual módulo chama e o que espera do retorno).

## 07/11 manhã (Claude)
- O laço da thread principal: `FUN_00549130` (procura arquivo no volume) → `FUN_00548e20` lê o setor 16+ do disco
  procurando "CD001" via RPC PCDV fn3 (`FUN_00548660`: send {lsn,count,dest}; resposta 0 = ok) → falha → espera
  2 ms → repete. O polling do cartão (`FUN_005620a8`/`FUN_00549d98`) é só o monitor de fundo, normal.
- O PCDV responde 0 mas **nenhum sceCdRead chega ao IOP** (`[ff-iop-cd]` nunca aparece; também com
  PS2X_CD_IMAGE=ISO). O handler (PDICDVD 0x1040, fn3) despacha a leitura por tarefas da LIBPDI (libpdi#32/#45...).
- Causa provável: `thbase` SetAlarm/CancelAlarm/iSetAlarm/iCancelAlarm (35-38) eram no-op no ps2xIOP; a LIBPDI
  usa alarmes. Implementados em `ps2xIOP/src/emulator/imports/iop_timrman.cpp` (`dispatchAlarmImport`, serviço em
  `serviceDue`, `nextEventCycle`) e roteados em iop_emulator.cpp. Build na fila (bq gt4).
- `decomp/Para estudo/gt4-decomp` (v1.01, 98% casado) tem os mesmos endereços do nosso CORE.ELF nessa área
  (ex.: src/network/unit_00494578/func_00548660.cpp) — usar para nomes/estruturas.
- PDIUSB.IRX tenta sceSifBindRpc no servidor EE 'PUSB' em laço (153 mil vezes em 30 s) — ver depois.
