# Boku no Natsuyasumi 2 — Adventure by the Sea

Mídia original em `D:\ISO\Boku no Natsuyasumi 2 - Adventure by the Sea (Japan) (T-En by Hilltop v1.2) (n).chd`, preservada (1.585.186.559 bytes).

Em 26/10/2026, o DVD do CHD v5 foi descompactado e os **926 arquivos, somando 3.177.992.698 bytes**, foram extraídos para `game_files/`. A imagem intermediária de 3.576.135.680 bytes conferiu com o SHA-1 de dados do cabeçalho CHD. Todos os arquivos extraídos foram comparados com seus extents e relidos para verificar SHA-256; ver `analysis/game-files-verification.json`.

A plataforma foi confirmada pelos arquivos: `SYSTEM.CNF` contém `BOOT2 = cdrom0:\SCPS_150.26;1`, e `SCPS_150.26` é ELF32 little-endian MIPS, entrypoint `0x100008`. Há módulos IOP, dados, vídeos e áudio. O nome do CHD menciona tradução Hilltop v1.2; a versão e o idioma do patch ainda não foram validados.

A cópia temporária do CHD e a ISO intermediária foram removidas após a verificação. O CHD original continua disponível porque preserva a mídia completa e ainda não há runtime do Boku2 neste projeto. Não houve recompilação nem execução do jogo.

A conversão usou `chdman` 0.289, extraído do pacote oficial MAME com SHA-256 conferido contra o digest da release. A ferramenta e sua origem estão em `../third_party/chdman-0.289/`; veja a [documentação oficial de extração de CHD](https://docs.mamedev.org/tools/chdman.html#extractdvd).


Em 26/10/2026 foi criado o projeto AOT com Ghidra R5900 local e PS2Recomp: 5.329 funções mapeadas, fontes gerados, compilação nativa e inicialização de GPU/áudio. `generate.cmd` reproduz a geração; `build-native.cmd` compila; `Play-Boku2.cmd` usa `build-location.txt`. O build atual fica em `D:\CodexBuilds\native-ports\boku2`.

A entrada omitida em `0x20a920` foi recuperada do ELF, com nove endereços de retomada. A adaptação local `project/runtime_memory.cmake` separa o heap (fim `0x01e00000`), a arena do runtime, as pilhas de callbacks e a pilha principal de 1 MiB. Não modifica os fontes compartilhados. O teste de 25 segundos ultrapassou o erro de pilha e criou threads do jogo, mas permaneceu travado em `0x2338dc`, após `iChangeThreadPriority`, sem cena validada. Não é um port jogável concluído. Evidências: `analysis/runtime-smoke.json`, `runs/smoke/runtime.log`, `analysis/recovered-entry.json`.
