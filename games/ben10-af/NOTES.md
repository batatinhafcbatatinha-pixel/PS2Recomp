# Notas — 29/10/2026 (Codex)

## Estado atual
Build finalizado: ps2EntryRunner.exe ligado. Boot real (drive.py) atinge:
- Autosave → ✕ → `/Movies/legal.pss` em LBN 0x383e6 (correto).
- sceMpeg decodifica 8 frames à frente; vídeo de intro (LBN 0x383e6) renderiza.
- Mas o jogo fica no pré-carregamento: FUN_001cd700 → FUN_001cce68 (mesmo buffer 0x4e1e40
  demuxado repetidas vezes) e **nunca** chama FUN_001cdb28 (que pega a imagem).

## Hipóteses (por prioridade)
1. **Player espera relógio de áudio (IOP/libsd interpretado).** Suspeita do checkpoint.
   Teste: desligar wall clock (`PS2X_EE_WALL_CLOCK=0`) OU forçar clock de áudio no EE via
   `PS2X_EE_CLOCK_SOURCE=audio`. Se o EE estava travado no interpretador de áudio, o relógio
   real resolve. Mas o checkpoint diz que o relógio real já estava ligado no drive.
2. **FUN_001cd700 é um loop sem saída** (espera uma condição que nunca se dá porque o SDK de
   áudio do IOP não entrega samples). Precisa de HLE de áudio (sceSd) ou do callback de stream.
3. **FUN_001cdb28 (que pega a imagem) só é chamado se o buffer de áudio estiver cheio** — depende
   de `IopKernel::signalSema` no callback de DMA de áudio, que talvez não esteja disparando.

## Próximo passo
Experimentar: compilar com wall clock OFF e clock de áudio no EE, rodar drive, ver se o player
sai do pré-carregamento. Se não, REA o FUN_001cd700 para entender a condição de saída.
