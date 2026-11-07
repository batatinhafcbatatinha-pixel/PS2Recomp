# Call of Duty: World at War — Final Fronts (USA, Canada) — projeto de port para PC

Projeto separado dos ports Xbox/Xbox 360. **Plataforma diferente: PlayStation 2 (EE MIPS R5900 + VU0/VU1 + GS + IOP).** Nenhuma ferramenta ou código gerado dos outros projetos se aplica diretamente. **Ainda não existe port jogável nem código traduzido.**

## Estado inicial — 21 de outubro de 2026

- ISO: `D:\ISO\Call of Duty - World at War - Final Fronts (USA, Canada) (En,Fr)\...iso`, 2.786.852.864 bytes, verificada pela Redump (MD5 `1cda43af4beb779a1429fa9f1473235a`). ISO9660, volume `COD5`, 231 arquivos.
- `SYSTEM.CNF`: `BOOT2 = cdrom0:\SLUS_217.46;1`, `VMODE = NTSC`.
- `SLUS_217.46`: 4.121.756 bytes, SHA-256 `e191d4275760546a7a0f3a6ee3d7b8bc2c5205ec30a9e6bdb61436237e83b4c3`.
  - ELF32 MIPS LE, entrada `0x00100008`, um segmento LOAD em `0x00100000` (filesz `0x3D9C98`, memsz `0x49AF4C`).
  - Sem tabela de símbolos, mas **com cabeçalhos de seção**: `.text` 0x36D320, `.vutext` 0x12FF0 (microcódigo VU), 47 overlays `.DVP.overlay.*`, `.data`, `.rodata`, `.sdata`, `.bss` etc.
  - Estimativa grosseira: ~12.000 prólogos `addiu sp,sp,-N` e ~10.350 alvos `jal` distintos. Não é contagem formal de funções.
- SDK Sony 3.1.0 (`PsIIlib* 3100`: cdvd, dma, graph, kernl, mc, pad, scf, sdr, vu0). Motor **Asura (Rebellion)**; há nomes C++ mangled em strings (`CdSearchFile__18Asura_PS2_TOCCacheP10sceCdlFILEPCc`), úteis para nomear funções.
- IOP: `IOPRP310.IMG` + módulos `CDVDSTM, LIBSD, SDRDRV, STREAM, PADMAN, MCMAN, MCSERV, SIO2MAN, LGKBM`. Áudio via libsdr/SPU2, streaming `.MIC`; vídeo `.PSS` (MPEG-2/IPU, com `MISC\MPEG_DLL.ASR`).
- Recursos: níveis `ENVS\SP\*.PS2` + pacotes de idioma `.AM`/`.FR`; formatos `.ASR`, `.GUI`, `.TM2`.
- Extraídos para `analysis/disc/` somente `SLUS_217.46`, `SYSTEM.CNF`, `IRX\*` e `MISC\MPEG_DLL.ASR`. Esses arquivos são privados (não publicar).

## Arquitetura proposta

1. **EE**: recompilação estática MIPS R5900 → C++ (avaliar PS2Recomp como base; instruções MMI de 128 bits, COP1 sem IEEE completo, COP2/VU0 macro).
2. **VU1/VIF/GS**: o ponto mais pesado. O jogo desenha via microcódigo VU1 (`.vutext` + overlays DVP). Opções: interpretar/recompilar o microcódigo e reimplementar o GS sobre D3D11/Vulkan, ou HLE dos programas VU conhecidos.
3. **IOP**: HLE dos serviços RPC (cdvd/stream de arquivos da ISO, libsd/sdr para áudio, pad, memory card) em vez de executar os IRX.
4. **IPU/MPEG**: decodificar `.PSS` com biblioteca nativa.

## Cadeia de ferramentas (21/10/2026)

- **PS2Recomp** `ran-j/PS2Recomp` @ `eb8688b`, em `../third_party/PS2Recomp`. Patch local em `ps2xRuntime/CMakeLists.txt`: opção `PS2X_RUNNER_DIR` (fontes geradas fora da árvore) e include desse diretório. `build-tools.cmd` compila o analisador/recompilador/runtime; testes: 444/445 (falha: "VU0 macro mappings cover all S1/S2 enums").
- **Ghidra 12.1.4** + extensão `ghidra-emotionengine-reloaded` v2.1.38 (linguagem `r5900:LE:32:default`). `ghidra-export.cmd` faz import/análise headless e roda `ExportPS2Functions.java` → `config/codwaw-ff.toml` + `config/ghidra-functions.csv` (14.367 funções, 67.499 rótulos executáveis).
- `ps2_analyzer` (banco de símbolos SCE embutido) → `config/analyzer-heuristic.toml`: 132 funções do SDK identificadas (pad, cdvd, mc, SIF/RPC, vu0, gs, fileio) + 147 entry points. `scripts/merge_config.py` junta as duas fontes em `config/recomp.toml` (179 stubs).
- `ps2_recomp config/recomp.toml` → `project/runner/` (81.866 arquivos, 642 MB, privado): 81.680 funções recompiladas, 178 stubs, 0 instruções não tratadas, 0 erros; 7.527 JR/JALR indiretos resolvidos por fallback (log em `analysis/recomp.log`).
- `scripts/scan_syscalls.py` → `analysis/ee-syscalls.json`: o jogo linka a tabela completa de stubs do libkernl (`0x00436344`–`0x00436BD4`); uso real só se verifica em execução.
- `build-game.cmd` compila `ps2EntryRunner` com as fontes geradas (22/10, ainda não testado):
  - `scripts/make_chunks.py` agrupa as ~81 mil fontes em ~2.560 pacotes (`project/runner-chunks/`, 32 fontes por pacote). O CMake vê só os pacotes, e a geração do build deixa de levar ~17 min.
  - O código do jogo vira a biblioteca `ps2_game_code.lib` (opções `PS2X_SPLIT_GAME_CODE`, `PS2X_GAME_CHUNK_DIR`, `PS2X_GAME_LIB_PREBUILT` no CMakeLists do PS2Recomp). `build-game.cmd` sem argumento compila só o runtime e refaz a ligação. `build-game.cmd full` recompila o jogo; use-o depois de regenerar `project/runner` ou de mudar o layout de dados das classes do runtime.
  - `chunk_register.cpp` (tabela de funções, ~400 mil linhas) é compilado com `/Od`. A ligação usa `/OPT:NOICF` (`PS2X_FAST_LINK`).
  - Não editar o `.cmd` enquanto ele roda. O runner usa a pasta do ELF como raiz `cdrom0:`; o disco completo está extraído em `disc-root/` (privado).
- IOP: o ps2xIOP executa os IRX originais num interpretador R3000; `IOPRP310.IMG` ainda precisa ser verificado em execução.

## Validação de boot — 23 de outubro de 2026

### Continuação — 24 de outubro de 2026

- Teste `test-iop.cmd` passou, incluindo resposta de RPC reverso com bloqueio e retomada da thread IOP.
- Funções ausentes foram completadas até `0x1c90b0`. `analysis/boot-1c90b0.log` não repetiu o erro de destino ausente durante a coleta; a janela ainda apresentou tela preta, não menu ou jogabilidade.
- `scripts/make_function_elf.py` permite recompilar um intervalo original isolado. Os suplementos `0x1c93f0` e `0x1c90b0` foram gerados sem instruções não suportadas; o último também registra sua continuação `0x1c9364`. Isso evita reconstruir a biblioteca inteira de código do jogo para cada função nova.
- `analysis/boot-frame-wait.log` confirma avanço de Count e sucessivas esperas de frame de 0,0333333 s emulados. A lentidão nesta fase não é Count congelado. A sonda `[ff-frame-wait]` é limitada a potências de dois.
- Experimento opcional `PS2X_FAST_FRAME_WAIT=1` acrescenta tempo emulado em blocos de 1 ms EE na espera original identificada por fingerprint. Permanece desligado por padrão: é ferramenta de diagnóstico, não prova de temporização correta.
- `analysis/boot-4496b8.log`: as tabelas virtuais com rotinas `0x4553e8`, `0x455410`, `0x455418`, `0x455420`, `0x455428` e a rotina de remoção `0x4496b8` passaram no boot acelerado. Conteúdo de tela foi observado, mas a fonte está corrompida; a tecla X produziu mudança de tela e apareceu nos logs de pad.
- Depois dessas telas o jogo carregou código externo ao ELF: `MISC/MPEG_DLL.ASR`, formato `SNR2`, observado em `0x9e7480` com inicialização em `0x9e9aa0`. `analysis/boot-mpeg-dump.log` e a captura privada `analysis/missing-code-9e0000.bin` registram esse estado.
- A faixa de código MPEG observada é `0x9e7500..0x9f6740` (fim exclusivo); o restante contém dados, não instruções. A recompilação dessa faixa produziu 140 funções, 2.644 entradas, zero instruções não suportadas e 28 avisos de resolução de fluxo indireto. Integração e reprodução ainda em validação. O mapeamento atual usa o endereço de carga observado; não é suporte geral a relocação dinâmica.
- Continuação MPEG: a primeira integração revelou que o runtime recusava entradas fora da tabela densa. Adicionada tabela suplementar por instância, sem alteração da ABI pública, com limpeza na carga de ELF e destruição. `boot-mpeg-sparse.log` confirma execução da inicialização; `boot-mpeg-ctor.log` confirma passagem do construtor suplementar `0x9e9688`.
- A tentativa seguinte mudou a base de carga de `0x9e7480` para `0x9e7300`. O mapeamento fixo acima foi substituído por associação na inicialização do módulo, verificando cabeçalho `SNR2` e assinatura `PsIIlibmpeg 3100`. `scripts/relocate_supplemental.py` adapta PCs, endereços absolutos otimizados e operandos imediatos lidos das instruções relocacionadas. Ainda pendente de validação em execução; não declarar vídeo funcional.
- Quatro testes em `python scripts/test_supplemental.py` passaram, incluindo diferenças entre duas cargas (659 instruções) e cobertura dos 187 operandos imediatos/endereço afetados. Testes de imports IOP continuam passando.
- `scripts/split_supplemental.py` separa as 140 funções em 18 objetos paralelos e um include de ligações. MSVC usa `/Ob1` apenas nesses objetos para limitar expansão por inlining; o restante do runtime mantém suas opções. Evita incluir todo o módulo num único objeto de runtime.
- O carregador também altera chamadas `JAL 0` do executável principal. O dispatcher passou a consultar a instrução atual em RAM nesses placeholders antes de transferir execução. Validação dinâmica dessa correção ainda pendente, pois a mudança de base interrompeu o teste anterior antes desse ponto.
- `boot-mpeg-relocatable.log` confirmou `[mpeg:bind] base=0x9e7300 ... delta=-384` e a resolução de `JAL 0` para a rotina externa. A entrada de reprodução era uma das ausentes; `find_overlay_entries.py` gerou 57 candidatos suplementares sem erros de tradução, com registro apenas onde não havia entrada existente.
- `boot-mpeg-extra.log` passou das chamadas de reprodução e apresentou a tela de título "CALL OF DUTY WORLD AT WAR FINAL FRONTS", confirmada em `analysis/game-frame.png`. Isso não confirma reprodução correta das introduções nem jogabilidade. O teste usou `PS2X_FAST_FRAME_WAIT=1`; há artefatos claros em fontes/texturas.
- `PS2X_FRAME_DUMP=analysis/game-frame.png` exporta somente o framebuffer emulado, sem outras janelas do desktop. `PS2X_FONT_TRACE=1` revelou valor `0x50000008` na paleta da fonte, compatível com vazamento de comando VIF. A sonda limitada `PS2X_VIF_TRACE=1` está em investigação; a causa visual não foi corrigida ainda.
- Corrigida paginação `sceMcGetDir` (modo 0 inicia, modo 1 continua até retornar zero). O teste do jogo coletado até aqui usou modo 0 na consulta inicial; portanto não é evidência de que essa falha de paginação causava aquela tela.
- `analysis/runtime-tests.log`: 445 testes do runtime passaram, incluindo páginas e fim da enumeração de memory card. Executar o binário de testes a partir da raiz de `third_party/PS2Recomp`, pois um teste lê `instructions.h` por caminho relativo.
- Corrupção visual identificada e corrigida: a continuação GIF IMAGE pelo PATH2 consumia o cabeçalho VIF `NOP + DIRECT` como pixels. A captura mostrou os oito bytes de controle na paleta e nas texturas. O interpretador agora exclui esse cabeçalho quando a contagem corresponde à imagem pendente, preservando continuação raw compatível. Novo teste reproduz o pacote; 446/446 testes passaram.
- `boot-vif-fix.log` e `analysis/title-vif-fixed.png` confirmam logo e fontes corretos, título e leitura de Start. Depois de Start foi observada a mensagem legível de ausência de save, com indicação de continuar por X; não era travamento de consulta do memory card. A paginação permanece uma correção independente validada por testes.
- `scripts/integrate_supplemental.py` gera um include privado com declarações antecipadas e registro de todas as entradas do suplemento. `make_function_elf.py` aceita um quinto argumento opcional com a base de uma captura bruta, além de ELF de origem.
- `PS2X_DUMP_MISSING_CODE=1` habilita captura privada limitada de memória quando falta um destino executável. Deixar desligado no uso normal e não publicar as capturas/código original gerado.

- Continuação: a sonda de buffers tinha endereço-base incorreto (`0x4f9188`); corrigido para `0x4e9188`. Os logs antigos de buffers zerados não são evidência válida; o campo `available` da sonda representa tamanho total, não bytes prontos.
- `scripts/locate_sector.py` confirmou que a espera era em `GRAPHICS/TILES/CODTILES.ASR` (265.700 bytes). Logs `boot-iop-read.log` e `boot-iop-dma.log` mostram leituras da ISO e transferências IOP -> EE.
- Causa encontrada: imports IOP `sifcmd` 15/16 (BindRpc/CallRpc) eram no-ops. Implementado bind aos servidores EE registrados e despacho reverso, com resposta e bloqueio/desbloqueio da thread IOP para esta chamada síncrona. Ainda não cobre callbacks IOP assíncronos completos.
- `boot-reverse-rpc-sync.log` confirma que o carregamento passou da espera e alcançou chamada indireta não registrada `0x447328`. Implementada tradução da rotina leaf original, condicionada ao fingerprint das instruções no ELF.
- `boot-leaf-447328.log` mostra carregamento posterior e apresentação de quadros 512x448; a thread principal para no próximo destino não registrado, `0x1d03f0`. Tradução leaf adicionada; validação em andamento. Menu/jogabilidade não confirmados.
- Rastreamento por função desligado por padrão em execução para evitar flush de arquivo a cada chamada. Reativar com `PS2X_FUNCTION_TRACE=1`; sondas limitadas permanecem no stderr.

- Histórico: Count inicialmente não avançava. Patch no `EeScheduler::accountCycles` faz CP0 Count avançar a cada dois ciclos EE, incluindo contextos suspensos. A interpretação antiga de buffers zerados foi invalidada pela correção de endereço descrita acima. O patch não implementa interrupção de Compare nem sincronização global de escritas MTC0 Count entre contextos.

- A compilação completa produziu `build/game/ps2xRuntime/ps2_game_code.lib` (961.453.340 bytes); a ligação de `ps2EntryRunner.exe` terminou com código 0.
- `run-game.cmd` inicia o ELF extraído e passa a ISO original como segundo argumento para leituras por LBN. Aceita outro caminho de ISO como primeiro argumento.
- Boot registrado em `analysis/boot-current.log`: inicialização OpenGL/áudio, oito módulos IRX carregados, registro RPC `0x12344321` e comandos GIF/GS executados.
- O rastreamento de funções mostra repetição de `0x244A08`, `0x244A50`, `0x244A60`, `0x244C88` e `0x269D18`. A última rotina consulta estado de leitura/buffers; a causa da espera ainda não foi determinada. Menu e jogabilidade ainda não foram confirmados.
- O teste foi encerrado para limitar o crescimento do rastreamento `ps2_log.txt` (~427 MB). A hipótese anterior de travamento em interrupções não foi confirmada neste boot.
- A ausência anterior da biblioteca intermediária não prova que o executável antigo fosse incompleto; ele apenas antecedia a reconstrução atual.

## Próximos passos

## Ciclo incremental de desenvolvimento

- `build-incremental.cmd`: após editar funções já existentes ou o runtime, compila os pacotes afetados e liga o executável, sem solicitar uma configuração completa a cada chamada.
- `build-incremental.cmd runtime`: recompila só o runtime e liga com a biblioteca existente. Não usar após mudar layout/ABI dos headers compartilhados com o código gerado.
- O segundo argumento permite ajustar paralelismo, por exemplo `build-incremental.cmd game 8`.
- `build-game.cmd full`: reservar para primeira configuração, regeneração que adiciona/remove fontes ou mudanças nas opções de build. Não apaga objetos, mas força a configuração do CMake.
- A troca observada de objetos `Unity/unity_*.obj` para objetos `runner-chunks/chunk_*.obj` exige uma recompilação completa de transição. Manter o layout atual evita repetir esse custo; não alternar unity/chunks entre builds.
- O ciclo incremental foi validado em builds sucessivos: alterações no runtime recompilam apenas os objetos afetados e religam o executável, reutilizando a biblioteca do jogo.

- Sonda limitada adicionada a `FUN_00269d18_0x269d18.cpp`: prefixo `[ff-read-poll]`, registra slot, CP0 Count, flags, busy, consumed e available nas primeiras oito consultas e potências de dois. Build em `analysis/build-read-probe.log`; validação da sonda em execução ainda pendente. A inspeção do runtime não encontrou atualização de `cop0_count`; investigar temporização sem presumir que explique toda a espera.

- Investigar a espera de leitura em `0x269D18` e a entrega dos buffers/retornos RPC; executar com `run-game.cmd`.
- Inventariar os programas VU1 (overlays DVP) para decidir entre interpretação e HLE.

### Continuação do port — teste de campanha (24/10/2026)

- Último bloqueio reproduzido no log anterior: `JALR 0x1d3fd0 -> 0x139fb0`. Extraído do ELF o intervalo `0x139fb0..0x139fe0` e recompilado em `project/supplemental-139fb0`: uma função, uma entrada, zero avisos ou instruções não suportadas. Registro condicionado à assinatura do código original.
- Build incremental passou (`analysis/build-139fb0.log`). Os 446 testes do runtime e os quatro testes Python de suplementos passaram (`analysis/runtime-tests-139fb0.log`).
- Execução `analysis/boot-139fb0.log` + `analysis/boot-139fb0-out.log`, com `PS2X_FAST_FRAME_WAIT=1` e entrada de pad por arquivo, permitiu navegar por título, aviso de save ausente, perfil A, campanha, New Game e dificuldade Regular. Capturas `analysis/profile-139fb0.png` e `analysis/campaign-menu-139fb0.png` mostram menus legíveis.
- Ao iniciar a campanha, o módulo MPEG foi associado em outra base (`0x95c800`, delta `-568448`). O próximo bloqueio é uma chamada indireta `0x126e84 -> 0x159fc8`, com captura privada em `analysis/missing-code-150000.bin`. A falha anterior em `0x139fb0` não reapareceu durante esse percurso. Ainda não há confirmação de missão renderizada ou jogabilidade.
- Próximo passo concreto: delimitar e recompilar a rotina original em `0x159fc8`, integrar como suplemento e repetir o início da campanha. O processo de teste desta rodada foi encerrado após registrar o novo bloqueio.

### Controle e opções de vídeo — 24/10/2026

- Entrada do host capturada na thread da janela e compartilhada por snapshot protegido com mutex; a thread EE não consulta mais diretamente o estado raylib de teclado/gamepad. Teclado permanece ativo com gamepad conectado; analógicos com zona morta de 18% e valores limitados. WASD e IJKL também controlam os analógicos; Escape é Círculo, sem fechar o processo. Conexão/desconexão de gamepads consultada a cada quadro.
- Saída da janela em 1920x1080, redimensionável. F9 restaura janela 1080p; F10 alterna sem bordas na resolução do desktop; F11/Alt+Enter alterna tela cheia solicitando 1080p. F8 mostra ajuda. Modo salvo por atalho em `video-settings.cfg`. A resolução interna do GS permanece original.
- `jogar-janela.cmd`, `jogar-tela-cheia.cmd`, `jogar-sem-bordas.cmd` e documentação `CONTROLES.md` adicionados. `run-game.cmd` aceita modo no segundo argumento.
- Teste real de troca de modos no executável: janela 1920x1080 -> sem bordas 1360x768 (desktop desta máquina) -> tela cheia 1920x1080 -> janela 1920x1080; quatro PASS em `analysis/boot-video-input.log`. Corrigida restauração que inicialmente retornava janela com altura 1061. Diagnóstico opcional `PS2X_VIDEO_SELF_TEST=1` usa as mesmas funções dos atalhos.
- Build concluído em `analysis/build-video-input.log`; 446/446 testes em `analysis/runtime-tests-video-input.log`. Nenhum gamepad físico foi detectado (`[pad:host] connected=0`); a resposta física dos eixos/botões e hotplug ainda precisa ser validada com hardware conectado.
- Novo boot com pad de teste, `PS2X_FAST_FRAME_WAIT=1`, navegou por título, perfil A, campanha, New Game e dificuldade Regular. Capturas privadas `analysis/title-video-input.png` e `analysis/menu-video-input.png`. Ao carregar missão repetiu exatamente o bloqueio anterior `0x126e84 -> 0x159fc8`; não houve confirmação de jogabilidade. Processo de teste encerrado após a coleta.

### Continuação — função 0x159fc8 e preferência de janela

- Implementada por recompilação original a rotina `0x159fc8..0x15a220` (600 bytes): uma função e 21 entradas suplementares, zero avisos, zero instruções não suportadas. Registro condicionado ao fingerprint do ELF. Build em `analysis/build-159fc8.log`; quatro testes Python de suplementos passaram.
- Preferência `video-settings.cfg` definida para `windowed`, conforme pedido do usuário. Novo teste confirma `[video] mode=windowed client=1920x1080`, sem executar o ciclo de testes de tela cheia.
- `analysis/boot-159fc8.log` e `analysis/boot-159fc8-out.log`: navegação até New Game/Regular passou do bloqueio anterior. O próximo destino ausente é `0x4532e0`, chamado em `0x2033e8`; a tela permanece preta nessa parada. Ainda sem confirmação de cena jogável.
- Nesta rodada o processo foi mantido aberto, em janela, após registrar o bloqueio. As rodadas anteriores foram encerradas deliberadamente pelo agente após coleta; não eram evidência de crash espontâneo.

## Sessão 25/10/2026 — código 100% recompilado, GS na GPU, missão renderizando

### Código EE sem lacunas
- `scripts/find_code_gaps.py` + `scripts/find_unregistered_targets.py` + `scripts/make_gaps_elf.py`: todos os trechos de `.text` fora do mapa do Ghidra (1.083 trechos, ~27,5 KB) e os 246 ponteiros de tabelas/vtables sem entrada registrada viram um suplemento único (`project/supplemental-gaps`): 1.412 funções, 2.678 entradas, 0 instruções não tratadas. Compilado em 177 objetos paralelos `codwaw_gaps_chunk_*.cpp`. Eliminou a sequência de bloqueios `0x4532e0`, `0x28e628` etc.

### Correções de runtime
- VIF1 como fluxo contínuo: comando incompleto (MPG/UNPACK/DIRECT/STROW/...) é guardado e completado na transferência seguinte, como o hardware.
- **UNPACK com `STCYCL CL=0, WL>0`** (escrita de preenchimento) não consome dados. O código antigo tratava CL=0 como 1, consumia 44 vetores inexistentes e dessincronizava todo o fluxo VIF1 na missão → contadores de VU1 lixo, microprogramas em laço infinito, pacotes GIF inválidos e tela branca. Era a causa raiz da missão não renderizar.
- `UNPACK V4-5` expande RGB<<3 e A<<7. GIFtag `FLG=3` opera como IMAGE (XGKICK, parser GS e árbitro).
- VU1: microprograma roda até o bit E (antes era abandonado após 65.536 ciclos). `commitReadyPipelines` com caminho rápido por `m_nextCommitCycle`.
- CP0 Count segue o relógio do host (294,912 MHz) durante a execução; IOP/timers/eventos mantêm a cadência estimada (ligar tudo ao relógio real travava as leituras de CD). `PS2X_FAST_FRAME_WAIT` não é mais necessário.
- Rastreamento por função (`PS2_FUNCTION_LOG_TRACKER`) virou opção `PS2X_FUNCTION_LOG_TRACKER` (OFF): custava ~25% da thread do jogo mesmo desligado.
- Entrada: `PS2X_PAD_FILE` soma-se ao teclado/controle em vez de substituí-los.
- Janela: modo janela cabe em 90% do monitor; imagem em 4:3 (F7 estica); título mostra FPS.

### GS na GPU (`gs_gpu_backend.cpp`, `gs_gpu_shaders.inl`)
- OpenGL 4.6 + `GL_ARB_fragment_shader_interlock`, numa thread própria de GS com fila de comandos. VRAM de 4 MB num SSBO com o swizzle real (tabelas exportadas por `GSMem::PageTableEntry`); desenho por fragment shader portando `WritePixel`/`SampleTexture`/sprite/triângulo/ponto; upload, cópia local e CLUT em compute shaders. Apresentação lê a VRAM de volta e reaproveita o `Present` de CPU.
- Padrão: GPU. `PS2X_GS=cpu` volta ao rasterizador de software.
- Resultado: menus a ~30 FPS (antes 10–12); missão "Basic Training" renderizando a ~4 FPS (antes 1 FPS) — gargalo restante é o interpretador VU1.

### Diagnóstico
- `PS2X_PROFILE=1` + `scripts/profile_report.py` (perfil por amostragem com map do linker). `[perf]` no log/título (FPS e % de VU1/GS/IOP). `[stall]` registra threads do EE quando o jogo para de trocar quadros. `PS2X_VIF_CMD_TRACE`, `PS2X_VU_DUMP`, `PS2X_GS_VRAM_DUMP`, `PS2X_IOP_TRACE`, `analysis/gs-trace.trigger`.
- `scripts/drive_campaign.py TAG [s] [--keep]`: boot em janela com roteiro de botões até a missão.

### Build rápido
- O código do jogo compila contra uma cópia congelada dos headers (`build/game-abi`, mantida por `scripts/sync_game_abi.py`); só mudanças em headers de ABI do jogo (`ps2_runtime.h`, macros, syscalls/stubs, `ps2_log.h`) recompilam os ~2.560 blocos (~40 min, 12 jobs). Mudanças no runtime: < 1 min. `build-incremental.cmd` escolhe os jobs pela memória livre (0,5 GB/job, até os núcleos).

### Trava na missão (em investigação)
- Em cerca de metade das rodadas a missão para de trocar quadros: `FUN_001dd3b8` percorre uma grade com limites de uma caixa delimitadora contendo ~2,5e18. Rastreado com `PS2X_WATCH=0xADDR:LEN` (watchpoint por proteção de página: registra o PC do EE e despeja registradores): a posição do objeto (`0x1ea8ee8`) é gravada por `FUN_001e9ff8` a partir de um vetor calculado em `FUN_002a5008` (`func_2A1540` × `*(s0+0x130)`). Velocidade e `dt` (`0x584380` = 0,02) estão normais; origem do valor ainda em análise.
- CP0 Count agora é um contador global único espelhado em todos os contextos (antes cada thread tinha sua cópia e podia divergir); isso não resolveu esta trava.

### Pendências
- Causa raiz da posição corrompida (acima).
- VU1 nativo (recompilação do microcódigo; protótipo do Codex em scratchpad): na missão o VU1 interpretado consome ~45% da thread do jogo (~6 FPS).
- A missão renderiza toda branca mesmo com o VU1 rodando; capturar primitivas com `analysis/gs-trace.trigger` (agora também no backend de GPU).

### Tela branca na missão — causa e correção (26/10/2026)
- O microprograma VU0 `0x1c0` (produto de matrizes 3×3, chamado por `FUN_00241700`) recebia entradas ~1e38. O EE lê as linhas de 12 bytes com o idioma desalinhado `MTSAB` + dois `LQ` + `QFSRV`; o runtime lia o `LQ` no endereço desalinhado e o `QFSRV` deslocava de novo.
- Correção: `Ps2FastRead128/Write128` e `Load128/Store128` ignoram os 4 bits baixos, como `LQ/SQ/LQC2/SQC2` no R5900 (são as únicas instruções do código gerado que usam esses acessos). Teste novo; 450/450. Exige `build-incremental.cmd game` (muda header de ABI).
- `analysis/boot-lqfix2.log` / `frame-lqfix2-*.png`: a missão mostra cena 3D texturizada (antes branca). Ainda ~1 FPS (VU1 ~90%).
- `PS2X_VU0_TRACE=1` registra entrada/saída das chamadas de microprograma VU0 e grava `analysis/vu0-code.bin`. sin/cos (`0x210`) e acos (`0x0`) conferem com a libm.
- Gerador: `VRSQRT` passa a calcular fs/√ft (não usado por este jogo). Pendente: `VLQI/VSQI/VLQD/VSQD/VILWR/VISWR` acessam a RAM do EE em vez da memória do VU0 (também não usados).
- Intermitente e antigo (não causado por esta correção): às vezes as threads do EE terminam em PC 0 logo após o primeiro `[mpeg:bind]` (rodadas `count1`, `count2`, `vud1`, `lqfix`).

## VU1 nativo (AOT) — 26/10/2026
- Camada VU1 do port nativo de God of War (Waffle565, ramo `runtime`, GPL-3) integrada em `ps2xRuntime/src/lib/vu` + `ps2_vu1_native.h`/`ps2_perf.h`: motor exato, escritas diretas de VF/VI/ACC e blocos nativos AOT. Cópia do VU1 anterior em `analysis/vu1-backup-20261026/`.
- Fluxo AOT: `GOW_VU1_CAPTURA=analysis/vu1-capture` grava as micromemórias (dados do jogo, privados) → `python -I scripts/vu1n_gen.py build/vu1n analysis/vu1-capture --parts 6` → `cmake build/game -DPS2X_VU1N_DIR=<build/vu1n>` + `build-incremental.cmd runtime`. 9 imagens → 792 blocos nativos (20 sem suporte caem no motor exato).
- `analysis/boot-vu1n.log`: VU1 de ~140 para 10–18 ns/ciclo; VU1 caiu de ~90% para ~10% da thread; missão 3D a ~3,5–4,8 FPS (antes 1). `GOW_VU1N_VERIFY=300`: nenhuma divergência relatada. Pela primeira vez aparece o tutorial com HUD (bússola, objetivo).
- Pendências: (1) após ~1 min na missão a thread principal fica em `pc=0xf7ffffff` e o jogo para — falta A/B com `GOW_VU1_NATIVE_OFF=1`; (2) GS agora é o gargalo (~70%); (3) letras estreitas "l"/"i" somem no texto do HUD.
- `PS2X_VU1=ref` volta às filas do modelo de referência; `GOW_VU1_NATIVE_OFF=1` desliga os blocos; `GOW_VU1_STATS=1` mostra ns/ciclo.

## GS: diagnóstico e renderizador nativo (em andamento) — 26/10/2026
- Contadores `[gs:sync]` (esperas síncronas do backend GPU por motivo; `lock=` espera do lock do produtor) e fila do thread de GS limitada a 2048 tarefas (`queue-full`).
- Diagnóstico (`analysis/boot-gssync.log`): ~74% da thread do jogo bloqueada. Cada `Present` espera ~460 ms o thread de GS drenar a fila e ler 4 MB de VRAM; a thread do jogo espera `m_backendLifetimeMutex` nesse período. Present assíncrono foi testado e piorou (fila cheia, <1 FPS): o limite é a vazão da emulação do GS na GPU, não a sincronização. Revertido.
- Renderizador nativo do port de God of War (origem: port de SotC, GPL-3) integrado como opção: `PS2X_GS=hw` → `GSThreadedBackend(GSHwBackend)` (`src/lib/gs/gpu/`, render targets reais, cache de texturas, escala 2x). Na missão: ~7–9,5 FPS (contra ~4 com o GS emulado). Imagem ainda incorreta: texto ladrilhado, fundos listrados; o atlas de fonte é decodificado corretamente (`PS2X_GS_HW_IDXDUMP=n`), o erro está na amostragem/composição. O jogo desenha em muitos render targets na área de texturas (fbp 341–392).
- O front end do GoW (com semântica de ST empacotado etc.) foi testado e quebrou também o caminho padrão; revertido para o nosso. Padrão continua `backend=gpu`. Backups em `analysis/vu1-backup-20261026/`.
- Mesma trava após ~1 min de missão em todos os backends (thread principal em `pc=0xf7ffffff`/0).

### Renderizador nativo (PS2X_GS=hw): correções de imagem e perfil — 26/10/2026 (continuação)
- **Causa do "texto ladrilhado / fundo listrado" nos menus**: `copyLatchedHostPresentationFrame` (`gs_frontend.cpp`) copiava as linhas com passo fixo de 640 px, mas o hw entrega quadros de 1024x896 (2x). Cada linha começava em 640·y → cópias deslocadas de 64 px PS2 num ciclo de 8 linhas. Passo agora é `max(largura, 640)` (mesma correção do GoW). `UploadFrame` (`ps2_runtime.cpp`) também cortava em 640x512: a textura da janela agora cresce até o tamanho do quadro. Menus em hw ficam corretos (`analysis/frame-hwfix1-menu4.png`).
- **Passes de pós-processo página a página**: o CoD processa um buffer 256x224 em fbp 336 (fbw 4) com 28 desenhos, um por página (fbp 336..363, T8 via CLUT 0x3c00, azul mascarado). `target()` criava um alvo por fbp. Agora um desenho cujo fbp é página interna de um alvo de mesma largura (e cuja extensão cabe nas linhas já desenhadas) cai nesse alvo com deslocamento da página.
- **Mesmo fbp com fbw diferente** (336: 8 e 4; 392: 4 e 1; 420: 2 e 1, todo quadro): um alvo por largura, e as páginas do mais recente são copiadas (`copyPages`, `glCopyImageSubData` por trechos) para o layout da outra largura. `findTarget` devolve o mais recente. Amostragem direta de alvo exige `tbw == fbw`; senão usa o caminho de reinterpretação por endereço.
- **Shuffles CT16 ignorados** (~24/quadro, `view why=2`): `TEST=0x31001` (alpha NEVER + AFAIL=FB_ONLY, Z mascarado) equivale a ALWAYS; agora aceitos (0 ignorados).
- Resultado (`analysis/boot-hwfix2.log`): cartão "Basic Training", HUD, bússola e textos do tutorial corretos a 2x; letras "l"/"i" aparecem no hw (somem no GS padrão). A geometria esticada do início também aparece no GS padrão (`frame-gsdef3-*`), logo vem de antes do GS.
- **Perfil na missão (hw, ~5–8 FPS)**: `PS2X_GS_WAITSTATS=1` (novo) mostra a thread do jogo bloqueada ~85% do tempo: ~2,2 s/5 s esperando Present e ~2,1 s/5 s com a fila do GS cheia. `PS2X_PROFILE=1` agora também amostra a thread `gs`: ~90% dentro do driver (ntdll 44%, win32u 39%, nvoglv64 7%); código próprio < 10%. `[gs:present]`: `glFinish` espera **~191 ms por quadro** na missão (≈3 ms nos menus); readback ~1 ms; readbacks de CLUT 0. O limite é a execução/serialização do quadro na GPU/driver.
- Próximo passo (sem novos testes nesta rodada): medir por tipo de desenho o custo na GPU (timer queries) — suspeitos: `TextureBarrier` por desenho, blend em shader triângulo a triângulo, passes de reinterpretação (T8 lendo alvo), muitos lotes pequenos; e apresentação compartilhada (sem readback) que o GoW já tem.
- Backups: `analysis/vu1-backup-20261026/{gs_hw_backend.before-pagealias,ps2_runtime.before-hwstride,gs_frontend.before-hwstride,gs_threaded_backend.before-waitstats}.cpp`.
- **Custo de GPU por tipo de desenho** (`PS2X_GS_GPUPROF=1`, novo): timestamps + contagem de fragmentos (SAMPLES_PASSED) por lote, somados por quadro a cada 60 presents (`[gs:gpuprof]`). Na missão (`analysis/boot-gpuprof4.log`): a GPU fica ocupada ~145–430 ms por quadro (gpu-span ≈ wall), CPU da thread do GS só 14–58 ms. Quase tudo vem de desenhos texturizados comuns: ~360 M a ~980 M fragmentos/quadro (~400–1000x a área de 1024x896); 57 lotes `tex+upload` sozinhos geram 624 M. Barreiras, blend no shader, DATE, alias de Z, reinterpretação, shuffles e cópias de página somam < 10 ms.
- Conclusão: o FPS da missão é limitado pelo overdraw dos triângulos esticados gigantes (mesmo defeito do GS padrão, já presente antes do VU1 nativo em `frame-lqfix2-*`). Corrigir essa geometria (VU1/EE) resolve imagem e FPS juntos; otimizar o renderizador agora não ajuda.

### Travamento intermitente (thread principal morre em vídeos) — 26/10/2026
- Sintoma: em ~metade das rodadas a thread principal morre durante um vídeo MPEG (logos iniciais ou briefing da missão) e o jogo fica a 0 FPS. Assinaturas variadas: chamada virtual com vtable zerada (`ra=0x1b5428`, objeto com `+0xD0 = 0`), `ra=0`, saltos para endereços de dados/pilha (`0x1c92d80`, `0x1e7bce0`, `0x1fffac0`), sempre com objetos do heap do jogo corrompidos.
- Causa: o heap interno do runtime (`guestMalloc`: pacotes de RPC do IOP e SIF, dados de callback do MPEG, pacotes de fonte/GS, pilhas de thread) começava na base passada a `SetupHeap` (`0x59af50`, fim do `.bss`), a mesma de onde o `sbrk` recompilado do jogo cresce até `EndOfHeap` (`0x1F00000`). Os dois entregavam a mesma memória; o streaming de vídeo (muito RPC de CD) sobrescrevia objetos do jogo.
- Correção: arena própria do runtime em `0x1F00000–0x1FE0000`, acima do limite do jogo; pilhas de interrupção/callback acima dela (até a pilha principal em `0x1FF8000`). `SetupHeap` devolve ao jogo a base dele e não reconfigura mais a arena; `EndOfHeap` devolve o limite do jogo. Teste do heap atualizado (448/450; as 2 falhas restantes são do motor VU1 do GoW: visibilidade de VI no JR e latência do EFU).
- Também: `CVT.W.S` agora trunca e satura (R5900) em vez de arredondar (`nearbyintf`); 1064 usos no jogo (índices de grade em `FUN_001dd3b8` etc.). Exige `build-incremental.cmd game`.
- `sceMpegGetPicture` limita a escrita ao `mbcount` do buffer (`[mpeg:getpicture] ... clipped` se cortar).
- Diagnóstico novo: `[ee:pc0]` a cada morte de thread (PC 0 ou função inexistente): registradores, objeto/vtable em s0/a0, pilha em torno de sp, trilha de despacho e os últimos 48 despachos com sp/ra. `[ee:setup-thread]`, `[ee:invocation-stack]`, `[SetupHeap]` mostram o layout de memória.
- `PS2X_VU1_DATADUMP=<s>:<n>`: grava a memória de dados do VU1 (+ registradores) antes/depois de n execuções a partir de s segundos do primeiro uso do VU1 (`analysis/vu1d_*`).
- `PS2X_GS_GPUPROF=1` (renderizador hw): tempo de GPU, CPU e fragmentos por tipo de desenho.
- Backups: `analysis/vu1-backup-20261026/{ps2_runtime.before-arena,System.before-arena,MPEG.before-mbcount,ps2_runtime_macros.before-cvtw}.*`.

### Geometria esticada e travamentos — causas encontradas (26/10/2026, continuação)
- **Geometria esticada (causa raiz):** o stub HLE `sceVu0MulMatrix` (`0x426950`) multiplicava na ordem invertida. A função original carrega `a1` em vf4..vf7 e passa cada linha de `a2` por ela (`out[i] = Σk a2[i][k]·a1[k]`); o stub fazia `a1·a2`. Toda matriz composta (projeção × visão × mundo) saía invertida: a câmera chegava ao VU1 como V·P (linha w = (0,0,1,0), eixo Z encolhido ×−0,03), w ≈ z do objeto, Q negativo, vértices explodindo. Corrigido (`mulVuMatrix(m1, m0)`), teste atualizado. Resultado: missão com cenário (celeiro, soldados, árvores, céu), fragmentos/quadro de ~980 M para ~28 M, FPS da missão de ~5–7 para ~10–11 (também no GS padrão, antes ~4).
- **`SQRT.S` lia o registrador errado:** o R5900 usa o campo ft (fs = 0); o tradutor gerava `sqrt(f[fs])` = `sqrt(f0)`. 199 instruções do jogo (274 com cópias de delay slot) calculavam a raiz de f0. Corrigido no `fpu_translator.cpp` e nos fontes gerados (`scripts/fix_sqrt_operand.py`, sem regerar tudo). Causava, p.ex., a morte em `FUN_001054c8` (matriz→quatérnio escrevendo −1,0 sobre o `ra`: `pc=0xbf800000`).
- **Fatia de tempo inexistente no PS2:** o escalonador alternava threads de mesma prioridade a cada 65536 ciclos; o kernel do EE não faz isso. Desligado (`PS2X_EE_TIMESLICE=1` reativa).
- `sceVu0ScaleVector`: com escala 0.0 o stub lia `a2` como float; agora usa sempre f12, como a original.
- Conferidos contra o código original (desassemblado): `ApplyMatrix`, `InversMatrix`, `SubVector`, `ViewScreenMatrix`, microprogramas VU0 `0x1c0` (mat3), `0xe0` (asin), `0x210` (sin/cos).
- Ferramentas: `PS2X_VU0_FULL=n` (registradores VU0 antes/depois), `PS2X_VIF_UNPACKLOG=s:n` (comandos UNPACK do VIF1), `PS2X_VU1_DATADUMP` agora grava também o código (`*_code.bin`); `scratchpad/vudis.py`, `eedis.py` (desassembladores mínimos VU/EE).
- Pendências: (1) ainda há morte ocasional da thread principal durante vídeos, no sistema de widgets `0x1b4xxx–0x1b7xxx` (quadro de `FUN_001b5820` sobrescrito por outro quadro salvo): ~1 em 11 rodadas (antes ~60%); (2) restam algumas faixas grandes na cena (provavelmente recorte/ADC de vértices atrás da câmera ou outra matemática); (3) caminho `reinterp` do renderizador hw caro (~110 ms/quadro com perfilador).
