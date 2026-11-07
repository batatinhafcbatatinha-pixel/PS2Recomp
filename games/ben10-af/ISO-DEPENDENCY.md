# Ben 10: Alien Force — remoção da dependência de ISO

## Causa

A imagem era usada como solução de compatibilidade para dois acessos que precisam concordar sobre o mesmo endereço de setor:

1. O IOMGR do jogo abre arquivos internos de `DATA.PAK` e obtém setores pela interface CDVD do IOP.
2. O player MPEG do EE lê os mesmos dados através de `sceCdStStart`/`sceCdStRead` nativos.

O virtual-disc antigo do IOP e o leitor EE não compartilhavam o mesmo layout. O PAK começava em LBN `0x98F` na ISO original, mas o layout virtual antigo o colocava em `0x18`. A correção anterior foi procurar setores na ISO real, reintroduzindo a imagem como dependência de execução. Não há uma necessidade intrínseca de ISO: são leituras de arquivos e offsets dentro do PAK.

## Evidências

- Exportação Ghidra já existente do ELF original, `FUN_001cd700_0x1cd700.cpp`: em `0x1CD7D0` o jogo carrega o setor inicial de `[s0+8]`; em `0x1CD7E0` chama `sceCdStStart@0x2BFF68`.
- `FUN_001cce68_0x1cce68.cpp`: em `0x1CCED0` chama `sceCdStRead@0x2BFFE8` com oito setores, destino `0x4E1E40`, modo zero; converte a contagem retornada de setores para bytes com deslocamento de 11 bits.
- A análise original MIPS pelo REA não foi suportada pelo provider Windows configurado. A limitação deve ser mantida explícita; as instruções acima são da exportação local de Ghidra, não uma observação REA de gameplay.
- `analysis/extracted-files-mapping.json`: leitura offline somente de `game_files`, sem abrir ISO. No layout sintético novo `DATA.PAK` começa em LBN26. `legal.pss` no offset relativo `0x37A57` resolve para `DATA.PAK` no byte466794496, assinatura `000001BA` (PSS). A loading-screen no setor relativo `0x11B92` resolve para o byte148672512. Esse cálculo mantém os offsets internos do PAK quando seu setor-base muda.

## Mudanças

- Uma única tabela `.disc/layout.tsv`, gerada dos arquivos extraídos, serve os acessos do EE e do IOP pelo adaptador compartilhado.
- `Play-Ben10.cmd`, `run-linux.sh`, `scripts/drive.py` e `scripts/smoke.py` não passam nem procuram ISO. Limpam `PS2X_CD_IMAGE` para impedir que um caminho herdado do ambiente mascare o teste.
- `drive.py` agora aceita diretório de binário e pasta de resultados independentes, registra hash do executável, ausência de imagem e motivo de término.
- O manifesto tinha três entradas de save em `mc0/` indevidamente misturadas aos dados do disco. Removidas de `expected_files`: uma ISO limpa tem12 arquivos e não deve precisar de saves previamente criados.
- A preparação de índices pertence ao instalador compartilhado; nenhum gerador duplicado foi mantido no projeto do jogo.

## Validação

A compilação isolada está em `build/extracted-files`, usando a biblioteca AOT já existente de `build/native`. O checkout do runtime compartilhado não é modificado. Logs em `analysis/extracted-files-configure.log` e `analysis/extracted-files-build.log`.

Teste de 120 s concluído sem argumento de ISO e com PS2X_CD_IMAGE vazio. Leituras de vídeo foram registradas por [extracted-disc], com assinaturas PSS válidas no DATA.PAK; capturas mostraram carregamento. A captura final apresentou artefatos gráficos. Não foi validada uma partida nem áudio/controles completos. O código de saída 1 foi produzido pelo encerramento imposto no limite do teste, não comprova crash. Relatório privado: runs/folder-only-final/report.json.
