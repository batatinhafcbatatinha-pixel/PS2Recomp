# STATUS - Port nativo de GTA Liberty City Stories (PS2, USA, SLUS_214.23 v1.01)

Iniciado em 03/11/2026 (Claude), no modelo do GT4/Ben 10 (PS2Recomp, runtime compartilhado em `../third_party/PS2Recomp`).

## Disco

- ISO `D:\ISO\Grand Theft Auto - Liberty City Stories (USA)\...iso` (4.010.016.768 bytes), lida só para leitura.
- `scripts/extract_game_files.py`: 238 arquivos, 1.940.334.696 bytes em `game_files/`, SHA-256 de cada um em
  `analysis/game-files-verification.json`. A ISO foi mantida.
- `SYSTEM.CNF`: `BOOT2 = cdrom0:\SLUS_214.23;1`, `VER = 1.01`, NTSC.
- ELF: 2.994.708 bytes, SHA-256 `65bc2ec3...ec53b2`, entry `0x1C6A60`, um único PT_LOAD em `0x100000`
  (filesz 0x2D93C4, memsz 0x573168). `.text` 0x100000-0x3865E4, `.vutext` 0x3865F0 (microcódigo VU1),
  `.data` 0x387900, `.bss` até 0x673168. Sem overlays de código EE.
- Módulos IOP próprios da Rockstar em `MODULES/`: CDSTREAM.IRX, CDVDSTM.IRX, SDRDRV.IRX (rodam no IOP interpretado,
  como todo módulo fora de `rom0:`), mais IOPRP300.IMG, LIBSD, MCMAN/MCSERV, PADMAN, SIO2MAN.
- O jogo lê `MODELS/*.IMG` e o áudio por setor: `scripts/prepare_layout.py` escreve `game_files/.disc` com os
  **LBNs originais** do disco (o layout do instalador reposiciona os arquivos).

## Pipeline

1. `ghidra-export.cmd` -> `config/ghidra.toml`, `config/ghidra-labels.csv`, `config/ghidra-functions-only.csv`.
2. `generate.cmd` (analyzer + `prepare_config` + geração + chunks + gap functions + SDK handlers + roteamento).
3. Build fora do Claude Code: `ps2-perf-tools/lcs-detached.cmd` (buildsup.py, status em `analysis/build-status.txt`).
4. `Play-LCS.cmd`; teste de boot: `python scripts/smoke.py --seconds 60 --name boot1`.

`recompiler/` tem cópias do ps2_recomp/ps2_analyzer do GT4 (fonte de 03/11, sem o bug do SQRT.S): o
`build-recompiler.cmd` falhou ao clonar o toml11 a partir de um processo destacado.

## 03/11 18:10 - primeiro boot

- Build completo (`project/main.cpp` precisava incluir `ps2_syscalls.h`/`ps2_stubs.h`, como no GT4).
- `smoke.py --seconds 60 --env PS2X_GS=hw`: janela e GS hw sobem, `SifInitRpc` inicializa, depois o EE para:
  `[stall] thread=1 pc=0x1c6a60 (entry) cycle=0`, game_fps=0, IOP ~4%. Nada desenhado.
- Próximo: descobrir onde o entry `0x1C6A60` fica preso logo após o SifInitRpc (provável espera de RPC/SIF
  com os módulos Rockstar CDSTREAM/SDRDRV, ou laço que não avança o contador de ciclos); cdb/PDB como nos outros ports.

## 03/11 noite - varredura do disco e vídeo de abertura

- Handlers próprios em `project/main.cpp`: `sceSifSearchModuleByName` (0x382E70), `sceDopen/sceDread/sceDclose`
  (0x382750/0x382980/0x382818, lista a pasta do host); `config/sdk-handlers-extra.txt`: `sceCdSearchFile@0x365758`.
- `InitThread` (0x37E120) fica com o código do jogo (`sdk-handlers-exclude.txt`): cria a thread "top" com prioridade 0
  e baixa a main para 1. O laço do vídeo (0x35A0C8) cede com `RotateThreadReadyQueue(1)`; com a main em 0 as threads 2/3
  (prio 1, decodificação) nunca rodam -> tela preta com o stream do vídeo parado.

## 04/11 - prioridade 0

- O `CreateThread` do runtime recusava prioridade 0 (-403), então o `InitThread` do jogo falhava (probe `LCS_PROBE=37d460`).
- **O LCS compila a própria worktree `gta-lcs/third_party/PS2Recomp`** (detached em d8e5a9d), não `work/third_party/PS2Recomp`.
  A correção de 03/11 tinha ido para a árvore compartilhada; movida para a worktree do LCS em 04/11.
- Com a main em prio 1 o vídeo decodifica (512x448, MPEG nativo), mas a tela ficava preta: a cadeia DMA de upload do
  quadro (montada uma vez em 0x1FC588 com a lib sceGifPk) tinha o primeiro CNT com 6 qw em vez de 3, engolindo os tags
  REF da imagem. Causa: só `sceGifPkAddGsAD`/`sceGifPkCloseGifTag` eram stubs nativos; o construtor nativo mantém o qwc
  do tag DMA aberto atualizado, e o `sceGifPkCnt` do jogo soma o tamanho de novo. Os dois agora rodam como código do jogo.
  (Vale para outros ports: não misturar stubs nativos e código do jogo da mesma família sceGifPk/sceVifPk.)
- Ferramentas: `LCS_PROBE=<addr>` + `LCS_RAMDUMP=<arquivo>` `LCS_RAMDUMP_N=<n>` salva os 32 MB da RAM na n-ésima chamada.

## 04/11 noite - nomes com ";1" e tela de carregamento (Claude)

- Depois dos vídeos o EE ficava em `CDSTREAM` RPC sid 0x65686577 fn 2 com `0xFE`: o fn 1 (`CdStream_Read`,
  0x1F6CA0) recebia lsn 0 e 0 setores. Causa: o open de stream (0x2E6208) normaliza o nome com 0x2E6048
  (maiúsculas, `\`, acrescenta `;1`) e procura no diretório montado pela varredura de boot (0x2E5EB8), cujos nomes
  vêm do `sceDread` nativo sem a versão ISO9660 -> nenhum arquivo de stream abria (`CHK\PS2\GAME.DTZ`, `MODELS\*.IMG`).
  `LcsDopen` agora acrescenta `;1` aos arquivos de `cdrom0:` (como o cdvdman e o Sopranos); `sceCdSearchFile` tira.
- Resultado (`runs/boot-semi1`, `PS2X_PAD_FILE` com Start/X): passa dos vídeos e mostra a **tela de carregamento do
  LCS com a barra de progresso**; o IOP lê do disco. Ferramenta: CDSTREAM.IRX desmontado com capstone (handler RPC
  0xDE4, tabela em .rodata 0x5860; fn 1 = 0x528 pedido, fn 2 = 0x630 status, códigos 0xFB..0xFE ficam gravados).
- `cdvdman:14` (`sceCdTrayReq`) não tratado no IOP: inofensivo aqui (o CDSTREAM só pula a checagem de bandeja).
- Depois do `;1`: laço eterno relendo `GTA3PS2.IMG` lsn 0x129B58 (5 setores). O laço do jogo em 0x18F788 reenvia o
  `CdStream_Read` canal 0 até ele devolver 0 (= canal ocupado com o pedido); a leitura do IOP terminava na hora,
  o canal nunca ficava ocupado e o fn 1 sempre aceitava de novo. Worktree PS2Recomp (ps2xIOP):
  `sceCdSync(0)` depois de leituras agora segura a thread do IOP por 2 ms + 0,15 ms/setor
  (`iop_emulator.cpp`, `IopCdvd::takePendingReadSectors`; `PS2X_IOP_CD_INSTANT=1` desliga). Levar ao compartilhado.
- Resultado (`runs/boot-lat1`): a leitura avança (0x129B58, +5, +21), 88 leituras, depois o EE fica 100% ocupado
  sem syscalls e sem novas leituras, ainda na tela de carregamento. Próximo: pilha do EE sob cdb.

## 05/11 madrugada (Claude)

- Teste com o build atual (runs/c1, Start/X a partir de 25 s): mesma tela de carregamento; cdb no GameThread
  (3 amostras): a thread do jogo fica em `FUN_0031e820` (percorre uma lista ligada estilo RenderWare "ForAll" até
  voltar à cabeça; só é infinito se a lista estiver corrompida/circular). IOP ~7%, EE 100%.
- `LCS_PROBE=31e820` (só vê chamadas pela tabela de despacho; chamadas diretas não passam): a única chamada vem de
  `ra=0x104B68` (`FUN_00104b48`) com **a0=0xB8EE** (lista inválida, desalinhada). Cadeia: `FUN_002b96b8` /
  `FUN_001d0890` copiam campos de um recurso (`param_2`) e chamam `FUN_0026cdf0(obj, param_2[0|1])` ->
  `FUN_00104b48(valor cru)` -> `FUN_0031e820`. Os outros campos passam por `FUN_00191d00(&campo, 2, 1)`.
  Hipótese: o recurso carregado (GAME.DTZ / IMG) não foi realocado (ponteiros ainda são deslocamentos), ou
  `FUN_00191d00` é a realocação e a ordem difere. Próximo: `LCS_RAMDUMP` na chamada 1 e ver o recurso em
  `param_2` (deslocamentos x ponteiros), e quem deveria realocar (tabela de relocação do chunk).
- `project/main.cpp`: `LCS_PROBE_STUCK=<arquivo>` (watchdog do probe: chamada que não volta em 5 s -> ra/args e
  dump da RAM). Nesta corrida não disparou (a chamada travada deve ser direta, fora da tabela).
- `scripts/DecompAt.java` (copiado do Sopranos): decompila e lista chamadores em modo headless somente leitura.
- Dump da RAM na chamada (`LCS_PROBE=1d0890` + `LCS_RAMDUMP`, ~vsync 1527): `FUN_001d0890(obj=0x16A0284,
  res=0x1B20D60, 0x1B20D40)`; o recurso em 0x1B20D40 tem `25c058ac 0 b20e0d2c ca09 / ca0c b7 ca08 10000 /
  0d40b6c0 b8ee 0 0 / 8 0 1 b720 / 7 7 be7c aaaaaaaa` e floats depois; sem cabeçalho de chunk antes (0x1B20B40..D20
  zerado). O GAME.DTZ (`GTAG` em 0x11C1F40, relocTab 0x531018, 0x22058 relocações) e os dicionários `xet` em
  0x1AFE040/0x1B1E500 estão realocados (ponteiros absolutos). Então ou esse bloco não é um chunk realocável
  (0xB8EE seria um índice/ID usado como ponteiro por outra razão), ou veio de leitura de setor errado do
  GTA3PS2.IMG. Próximo: descobrir quem preenche 0x1B20D40 (probe no fn 1 do CDSTREAM com lsn/destino, comparar
  com o arquivo no IMG) e o que `FUN_0018dee8` (ra 0x18DFE8) espera nesse registro.

## 05/11 (Claude)

- Watchpoint (`PS2X_WATCH=0x1B20D40:48`, decimal length) com Start/X: 0x1B20D40 é um bloco do heap do jogo (cabeçalho em
  -8/-4 escrito pelo alocador 0x14fb84/0x14fbb4); o conteúdo chega por leitura de stream (escritas do host com o EE em
  0x1f6eec, laço de espera do CdStream): +0 = 0x006d646c ("ldm\0" = arquivo .mdl do motor da Leeds), +0x20 = 0xb6c0.
  Depois, 0x22c590 grava ponteiros em +0x20/+0x24. Os arquivos da Leeds são blocos realocáveis (cabeçalho com
  relocTab/numRelocs; cada entrada soma a base). 0xB8EE no registro = deslocamento não realocado.
- Hipótese principal: a leitura do stream não traz o arquivo inteiro (tabela de relocação no fim do arquivo vem
  zerada/velha, ex. leitura cortada em blocos do SIF/CDSTREAM) ou vem do setor errado. Próximo: logar (lsn, setores,
  destino) das leituras do CdStream e comparar o bloco em RAM com o arquivo dentro do GTA3PS2.IMG.
- Worktree: alocador do IOP first-fit (igual FO2). Build na fila `D:/CodexBuilds/bq.sh` (lcs).
