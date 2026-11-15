# Suspeitas

Coisas que parecem erradas mas que nenhum teste mostrou. Uma correccao so entra com um teste que falha antes e passa
depois; o que nao se consegue fazer falhar fica aqui, com o que faltou para o provar e onde esta no codigo. Quando uma
suspeita se prova (ou se desfaz) sai desta lista e a razao fica na mensagem do commit.

Cada entrada diz: onde, o que se suspeita, porque nao esta provado, e o que seria preciso.

## Ficaram por provar na ronda 3

Windows nao corre aqui (nao ha wine): todas estas so se provam numa maquina Windows, ou passando a logica para uma funcao
pura que um teste de Linux possa exercer.

1. `src/fs_win32.c`, `enum_dir`: um nome de ficheiro com um surrogate sem par (o NTFS deixa) faz `win32_narrow` falhar e a
   listagem inteira da pasta e abandonada (`result = -1`), em vez de saltar so essa entrada. Falta: um ficheiro assim num
   volume NTFS, ou a conversao `win32_narrow` passada para uma funcao pura testavel em Linux.
2. `src/io_win32.c`, `io_platform_open_file`: testa `GetFileAttributesW(wide) & FILE_ATTRIBUTE_DIRECTORY` sem ver se o
   resultado foi `INVALID_FILE_ATTRIBUTES` (0xFFFFFFFF, que tem todos os bits ligados, por isso conta como pasta). Hoje
   isso so se alcanca se o `CreateFileW` acabou de abrir o ficheiro e este desapareceu a seguir; o resultado seria um
   "is a directory" falso. Falta: provocar a corrida, ou a correccao simples (comparar com `INVALID_FILE_ATTRIBUTES`).
3. `src/io_win32.c`: `h_flush` chama `FlushFileBuffers` num handle aberto so para leitura (da `ERROR_ACCESS_DENIED`, e um
   `io_flush` num ficheiro de leitura falha); `h_read` trata `ERROR_BROKEN_PIPE` como erro em vez de fim de ficheiro (um
   ficheiro que e um pipe nomeado). Falta: correr em Windows.
4. `src/backend_win32.c`, `handle_drop` (~1012) e `clip_hdrop_as_uris` (~2557): `DragQueryFileW` para um buffer de
   tamanho fixo, por isso um caminho que nao cabe fica cortado sem aviso. Falta: largar um ficheiro com um caminho de mais
   de 260 caracteres numa janela, em Windows.
5. `src/backend_wayland.c`, `keyboard_keymap` (~857): passa o keymap (um mmap do fd do compositor) a
   `xkb_keymap_new_from_string` como se tivesse um NUL no fim; o protocolo nao o garante (o tamanho inclui o NUL em todos
   os compositores vistos, mas nao esta escrito). Falta: um compositor que mande o keymap sem NUL (o compositor de teste
   da propria biblioteca pode ser feito para isso).
6. `time_nanos` antes de `platform_init` devolve o tempo desde o arranque da maquina, nao desde zero (a base so se fixa no
   `platform_init`). Ver o commit desta ronda se foi tratado.

As tres notas do rato capturado em X11 que a ronda 3 deixou (deteccao do eco do `XWarpPointer` so pela posicao, uma
janela capturada que ninguem bombeia nunca recebe o warp, um delta perdido quando o `reanchor` e armado durante a
compactacao) estao a ser estudadas na ronda 4, ver mais abaixo.

## Da ronda 4

### Windows: a limpeza dos temporarios de um `io_save_file` que morreu nao existe

Em POSIX o primeiro `io_save_file` numa pasta remove os `*.zsave-<hex>.tmp` cujo processo ja nao existe. Em Windows
`io_platform_clean_stale_temps` nao faz nada: um programa que morra a meio de uma gravacao deixa o temporario para sempre.
Falta uma implementacao com `FindFirstFileW` e `OpenProcess` que se possa correr numa maquina Windows; os identificadores de
processo do Windows sao reaproveitados depressa, por isso a limpeza la apanharia menos casos.

### POSIX: a limpeza dos temporarios nao distingue espacos de nomes de processos

O dono do temporario e visto com `kill(pid, 0)`. Dois contentores que partilham uma pasta (volume montado) e correm o
mesmo programa nao veem os pids um do outro: um pode apagar o temporario de uma gravacao do outro que ainda esta a decorrer
(essa gravacao falha com "cannot rename", o ficheiro antigo fica intacto). Nao se provou porque exige dois espacos de pids;
a defesa seria olhar tambem para a data de modificacao do temporario.
