
## 06/11 noite (Claude)
- Aplicados do VCS: PS2X_RUNTIME_ARENA=80000:100000 e PS2X_GS=hw como padrão (project/main.cpp), SPU2 nativo
  (ps2-perf-tools/patches/spu2_merge.py), getenv("PS2X_IOP_TRACE") em static (era metade da CPU no laço do CD:
  patches/iop_trace_static.py), atraso de DVD também no callback de leitura do cdvdman (iop_emulator.cpp).
- sceSifFreeIopHeap (0x382C00) não era reconhecido -> RPC sem servidor, memória do IOP nunca voltava (cada vídeo
  reserva 0x6000+0x800+0x28010). Agora nativo (kLcsLocalHandlers). Frees conferidos com LCS_PROBE=382c00.
- AINDA PARADO na tela legal: o EE acha \AUDIO\SET0\SFX0_PS.SDT (lsn 0xFABE8, 0xEAC bytes), manda RPC sid 0x65686579
  fn 0xA0000 com parâmetros 0 0 0 0, e o CDSTREAM passa a ler 2 setores para o endereço IOP 0 em laço (~1 milhão de
  vezes; sceCdRead falha em ownsRamRange). Nenhuma AllocSysMemory do IOP falha (log novo em iop_sysmem.cpp).
  Próximo: de onde o CDSTREAM tira o buffer do banco de SFX (variável estática? RPC anterior que não chegou?) e
  comparar com o VCS, que passa. Logs: PS2X_IOP_TRACE=1 mostra [ff-rpc-send] (palavras antes da chamada).

## 07/11 madrugada (Claude) - CDSTREAM, leitura do banco de SFX para o endereço 0
- Resposta do opencode/nemotron (analysis/ASK-sfxbank.md) NÃO é confiável (EE inventado, "RPC 0xA0001" sem evidência).
- Servidor 0x65686579 = handler 0x5210 (dispatch por fn & 0xFFFF00, fn & 0xFF = argumento): 0xA0000 -> 0x35D4 (init sem
  argumentos: cria semáforos/threads 0x46B0 e 0x51E4, depois 0x3704 = init de áudio: AllocSysMemory(0, 0x20800) -> 0x6B44).
  0xA0200 -> 0x3C40, 0xA0A00 -> 0x3CD0(fn&0xff), 0xA0B00 -> 0x3D1C(w0), 0xA0C00 -> 0x3D64(w0), 0xA0D00 -> 0x4074,
  0xA0F00 -> 0x3CA8, 0xB0000 -> 0x461C(pkt), 0xB0100 -> 0x4304, 0xB0200 -> 0x3E34, 0xB0300 -> 0x3DA0.
- Leitura com repetição: 0xA4C(lsn, setores, buf, modo, [sp+0x10]) chama sceCdRead (0x56D4) até dar certo; em erro chama
  o monitor 0x8A0. Dois chamadores: 0xCC8 (streaming: buf = [0x69A8] + bloco*0x8000, depois SIF DMA para o EE) e 0xD74
  (leitura direta: lsn/setores/buf = palavras 0/1/2 do pacote). O banco de SFX (2 setores) com buf 0 deve ser o 0xD74:
  o EE manda um endereço IOP 0 no pacote. Próximo: PS2X_IOP_TRACE=1 para ver qual fn chega antes do laço e de onde o
  EE tira o endereço IOP (sceSifAllocIopHeap? retorno de um RPC anterior, p. ex. o 0xA0000 devolvendo 0x6B44).
- 07/11 01:40 CAUSA E CORREÇÃO: bug do gerador AOT de IRX (ps2xIOP/tools/irx_aot.py, hi_lo_values): pareava HI16 com o
  LO16 seguinte por ENDEREÇO (sorted(relocs)); a ABI/carregador pareia pela ORDEM DA TABELA. No CDSTREAM o `lui s3` de
  0x4508 recebeu o -0x6780 de 0x4524 em vez do 0x6B44 de 0x452C -> s3 64 KB abaixo, [0x6B44] lido como 0 -> banco de SFX
  lido para o IOP 0 em laço. Com PS2X_IOP_AOT_SKIP=CDSTREAM já passava; corrigido em todas as cópias do gerador
  (shared + worktrees fo2/lcs/vcs/msae/sopranos) e AOT do LCS regenerado (irx_aot_setup.py gta-lcs game_files/MODULES).
- Resultado (run d1, scripts/drive.py novo, X a cada 3 s): créditos -> Loading -> GAMEPLAY (1ª missão "Get in the car",
  HUD, radar). VU1 92-95%: renderização pesada (próximo: VU1 nativo como no VCS/Sopranos).
