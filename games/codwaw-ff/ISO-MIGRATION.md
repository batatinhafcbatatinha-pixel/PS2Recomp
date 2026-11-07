# Leitura dos arquivos extraídos — CoD Final Fronts

O port usa `game_files/SLUS_217.46` e a pasta completa `game_files/` como origem dos dados. A execução não recebe uma ISO e o launcher limpa `PS2X_CD_IMAGE` herdado.

## Por que o caminho antigo precisava da ISO

O código original usa leitura de setores, além de nomes de arquivos. O SDK identificado no ELF inclui `sceCdRead` (0x00427D00), `sceCdStStart` (0x00428200) e `sceCdStRead` (0x00428280). O runtime precisa converter os LBNs recebidos nessas chamadas para posições dentro dos arquivos extraídos. Não bastava retirar o argumento da ISO: o caminho nativo do EE e o serviço CD do IOP precisam concordar sobre os mesmos setores.

O launcher anterior fornecia a ISO como terceiro argumento do runner e o roteiro de campanha repetia um caminho absoluto privado; ambos usavam `disc-root/` como raiz do ELF. O runtime abria essa imagem para leituras por LBN. O histórico local de diagnóstico registra, por exemplo, a leitura de `GRAPHICS/TILES/CODTILES.ASR` e sua transferência IOP → EE.

## Substituição

O adaptador compartilhado de `tools/port-installer/runtime` traduz essas leituras para os arquivos comuns em `game_files/`. `game_files/.disc/` guarda apenas o índice de extensões e pequenos metadados de diretório; não contém uma cópia da ISO nem um pacote com o conteúdo do jogo.

`build-game.cmd` inclui o adaptador depois de criar o runtime e fornece `PORT_GAME_ROOT` para preparar a pasta. `run-game.cmd` prepara o índice quando ele estiver ausente. O instalador compartilhado também o produz ao importar a ISO ou uma pasta já extraída.

`analysis/extracted-layout-verification.json` é um registro privado da comparação SHA-256 dos 231 arquivos extraídos com o relatório anterior. O mapa original foi gerado desse inventário sem abrir a ISO. Registros de execução e capturas ficam em `analysis/`; não são distribuídos.

O ELF MIPS original não é aceito pelo provider Ghidra do REA disponível nesta instalação Windows. Os endereços do SDK acima vêm do analisador local já existente, não de uma nova decompilação pelo REA. A investigação REA da camada nativa PE é registrada separadamente no relatório compartilhado. Resultado de compilação ou chegada ao menu não deve ser anunciado como validação completa de campanha.

## Validação concluída

Os231 arquivos extraídos foram comparados por SHA-256 com o inventário anterior. O leitor C++ comparou768 amostras de ranges com os arquivos comuns sem abrir ISO. A compilação final com fallback de imagem desativado concluiu.

Foram executados dois testes do mesmo binário, sem argumento de ISO e com PS2X_CD_IMAGE vazio: um de127s com o índice de setores original, outro de98s com o índice recriado exclusivamente da pasta via prepare_folder.py --rebuild-layout. Ambos chegaram ao título e avançaram ao carregamento; foram registrados dois binds MPEG. Nenhum registrou unresolved LBN, open failed ou imagem configurada. O segundo usou os novos LBNs sintéticos, sem depender do inventário antigo.

A captura posterior ao carregamento ficou preta e o log inclui um diagnóstico de thread que chegou a PC0. A campanha não foi validada, nem a causa desse problema atribuída à remoção de ISO. O exitCode1 dos relatórios foi causado pelo encerramento imposto pelo teste no limite de tempo. Registros privados: analysis/report-folder-final.json e analysis/report-folder-synthetic-final.json. Nenhuma captura, exportação original ou arquivo de jogo é distribuído neste repositório.
