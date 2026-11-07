# Boku 2 — leitura exclusiva de game_files

O jogo usa setores porque seu código de disco procura o LBN do arquivo e chama sceCdRead com a quantidade de setores. Ghidra direto, projeto existente em modo somente leitura, linguagem r5900:LE:32:default e SHA-256 original ecdac5741e092099e08834ea16b785cdfd70bc750855caa6c4dd28fc689cb6b1: FUN_00128190 busca via0x21d960 e lê via0x21e688; FUN_00128578 lê o setor-base somado ao progresso.

O REA Windows6.7.0 recusa ELF/MIPS; a tentativa no executávelPE nativo atingiu o timeout de330s. Essas limitações não são apresentadas como análise bem-sucedida REA. A consulta original foi realizada diretamente pelo GhidraR5900.

A tabela e os pequenos metadados .disc servem tanto o leitorEE como oIOP. Os caminhos de fallback para imagem foram removidos pelo adaptador. Launcher e diagnósticos não fornecem imagem e limpam PS2X_CD_IMAGE. Uma pasta copiada manualmente pode ser preparada com prepare_folder.py, sem ISO. O instalador já cria seu índice.

Validação: build native-opt concluída; teste85s semISO chegou visualmente ao menu New Game/Continue/Reminisce/Settings. Log registrou PVD e setores dos arquivos via [extracted-disc]. O leitor C++ comparou2808 amostras com os arquivos comuns. Isso não valida campanha, saves ou gameplay completo. Capturas e exportação Ghidra permanecem privadas em analysis/ e runs/.
