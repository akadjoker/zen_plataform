# Suspeitas

Coisas que parecem erradas mas que nenhum teste mostrou, ou que se corrigiram sem as poder correr. Uma correccao so entra
com um teste que falha antes e passa depois; o que nao se consegue fazer falhar fica aqui, com o que faltou para o provar
e onde esta no codigo. Quando uma suspeita se prova (ou se desfaz) sai desta lista e a razao fica na mensagem do commit.

Cada entrada diz: onde, o que se suspeita, porque nao esta provado, e o que seria preciso.

## Corrigido sem correr em Windows (a confirmar numa maquina Windows)

Windows nao corre aqui (nao ha wine). Estas quatro vinham da lista da ronda 3 e a ronda 4 corrigiu-as pela documentacao da
API, em correccoes simples e locais que o MinGW compila com `-Wall -Wextra` sem avisos, mas nenhuma foi corrida:

- `src/io_win32.c`, `h_read`: `ERROR_BROKEN_PIPE` e fim de ficheiro, nao erro.
- `src/io_win32.c`, `io_platform_open_file`: `INVALID_FILE_ATTRIBUTES` ja nao conta como pasta (um pipe com nome ou um
  dispositivo abertos pelo `CreateFileW`).
- `src/io_win32.c`, `h_flush`: um fluxo aberto so para leitura nao chama `FlushFileBuffers` (da `ACCESS_DENIED`).
- `src/backend_win32.c`, `handle_drop` e `clip_hdrop_as_uris`: o comprimento do caminho pergunta-se primeiro e os buffers
  alocam-se desse tamanho (antes cortavam acima de 1039 e de 519 caracteres).
- `src/core.c`, `time_base` (o relogio de `time_nanos`): o ramo `InterlockedCompareExchange64` nunca correu.

O que falta para as dar por provadas: largar um ficheiro com um caminho de mais de 260 caracteres numa janela, ler de um pipe
com nome cujo escritor fecha, abrir `\\.\pipe\x`, e chamar `io_flush` num fluxo `"rb"`.

## Ficou por provar

1. `src/fs_win32.c`, `enum_dir`: um nome de ficheiro com um surrogate sem par (o NTFS deixa). A suspeita da ronda 3 era que
   `win32_narrow` falha e a listagem inteira e abandonada. Pela documentacao do `WideCharToMultiByte`, sem
   `WC_ERR_INVALID_CHARS` (que `win32_narrow` nao passa) o UTF-8 troca o surrogate por U+FFFD e a chamada **nao falha**, por
   isso a listagem continua; o que acontece e o caminho dessa entrada trazer U+FFFD e nao abrir o ficheiro. Nao se mudou nada;
   falta uma maquina Windows com um ficheiro assim para ver.
2. `src/ui/stb_truetype.h` e `ui_font.c`: o stb_truetype nao valida o que esta dentro de uma tabela (offsets de subtabelas,
   contornos), por isso uma fonte feita para o rebentar rebenta-o. A ronda 4 recusa os ficheiros que nao sao fontes e as fontes
   cortadas (as tabelas do directorio passam do fim dos dados), que e o caso comum; uma fonte de uma origem que ninguem
   confia continua a nao dever carregar-se. Falta um fuzzer de fontes (o stb nao se salva sem a validacao toda).
3. `src/ui/ui_dialog.c`: o dialogo de ficheiros assume caminhos com `/`: a barra de caminho parte por `/`, o botao "Raiz" vai
   para `/`, "Home" usa `getenv("HOME")`, e `dlg_confirm` junta com `/`. Num caminho do Windows (`C:\Users\x`) a barra mostra
   um segmento so e clicar nele leva a `/C:\Users\x`. Falta correr o dialogo em Windows.
4. `src/ui/ui_dialog.c`, `fmt_date`: com uma data fora do que `localtime_r` converte (acima de 6,7e16 segundos) `lt` fica por
   inicializar e o `strftime` escreve lixo. Nao ha ficheiro com essa data; nao se mudou.
5. `src/ui/ui_layout.c`: com mais de 7 contentores aninhados (`UI_MAX_CONTAINERS` 8) `ui_push_container` nao empurra e o
   `ui_pop_container` correspondente tira o contentor de fora; e um `zui_*_end` a mais tira a raiz e o `ui_top` seguinte indexa
   `containers[-1]`. So acontece com uso errado (pares desequilibrados) ou aninhamento absurdo.

## draw2d: linhas e rectangulos com coordenadas muito grandes

- `src/draw2d.c`, `draw_line`: percorre todos os passos da recta, tambem os de fora do framebuffer. Medido (Release, um CPU):
  uma linha de 1280 px visiveis demora 8 us, uma de 200 milhoes de px com 1280 visiveis demora 539 ms (um soluco de meio
  segundo numa frame) e uma diagonal de 2 milhoes de px 7 ms. `draw_rect` e `draw_round_rect` (contorno) usam-na. Um programa
  que projecte linhas em perspectiva ou desenhe guias "ate ao infinito" apanha-o. A correccao e cortar a recta ao
  framebuffer e ao recorte antes do ciclo mantendo os mesmos pixeis (saltar para o primeiro passo visivel com a formula do
  erro de Bresenham e parar quando sai, que uma recta nao volta a entrar num rectangulo), validada contra a versao actual em
  milhoes de rectas aleatorias; nao se fez porque a igualdade bit a bit e delicada e ninguem desenha essas rectas hoje.
- `draw_line` (`x1 - x0`) e `draw_round_rect` (`x + w`) tem overflow de inteiro com sinal (UB) para coordenadas perto de
  INT_MIN e INT_MAX; `draw_fill_rect` e o recorte ja foram arranjados na ronda 2 para a largura INT_MAX. O teste
  `test_render_any_geometry` fica nos +-2^20, onde nada rebenta.

## Rato capturado em X11 (`MOUSE_MODE_CAPTURED`)

A ronda 4 mediu-o com `examples/bench_input_x11` (rato de 125 Hz a 8000 Hz: a soma dos deltas bate certo com o movimento
real, sem saltos), mas ha uma perda que se prova com o mesmo programa:

- **Um arranco rapido perde movimento.** O rato fica confinado a janela (`XGrabPointer` com `confine_to`) e a biblioteca so o
  manda de volta para o centro uma vez por frame; num arranco que passe de meia largura da janela numa frame o ponteiro bate na
  parede e o resto do movimento nao chega. Medido (`captured flick 8 px at 8000 Hz`, janela de 600 px, 60 fps, 1066 px por
  frame): 17 940 de 64 000 px chegam, 72 % perdidos. Numa janela de 1920 px a 60 fps a perda comecaria por volta de 57 600
  contagens por segundo (conta, nao medido). A correccao e usar o movimento em bruto do XInput2 (`XI_RawMotion`, `valuators` aceleradas e sem
  limite, sem depender do ponteiro) em vez de contar a diferenca de posicoes. Nao se fez porque: (a) o XTest (e dispositivos
  de entrada absolutos, tablets e maquinas virtuais) manda valores absolutos nos eventos em bruto, por isso era preciso
  distinguir dispositivos relativos de absolutos e manter o caminho antigo para eles; (b) os testes de captura
  (`test_mouse_captured*` em `test_input_x11` e `test_mouse_x11`) injectam posicoes absolutas e teriam de passar a injectar
  movimento relativo. A deteccao do eco do aviso so pela posicao, a janela capturada que ninguem bombeia nunca avisada e o
  delta perdido quando o `reanchor` e armado durante a compactacao (as tres notas da ronda 3) deixam de existir com o
  movimento em bruto.
- O Xvfb nao tem rato real, compositor nem vsync: a perda acima e determinista (o servidor corta o movimento no limite da
  janela) mas a velocidade a que um rato a sério a faz acontecer so se sabe com um rato.

## Limpeza dos temporarios de `io_save_file`

- Windows: `io_platform_clean_stale_temps` nao faz nada, por isso um programa que morra a meio de uma gravacao deixa o
  temporario (`caminho.zsave-<hex>.tmp`) para sempre. Falta uma implementacao com `FindFirstFileW` e `OpenProcess` que se possa
  correr; os identificadores de processo do Windows sao reaproveitados depressa, por isso a limpeza apanharia menos.
- POSIX: o dono do temporario e visto com `kill(pid, 0)`. Dois contentores que partilham uma pasta (volume montado) e correm o
  mesmo programa nao veem os pids um do outro: um pode apagar o temporario de uma gravacao do outro que ainda decorre (essa
  gravacao falha com "cannot rename", o ficheiro antigo fica intacto). Nao se provou porque exige dois espacos de pids; a
  defesa seria olhar tambem para a data de modificacao do temporario.
- A pasta le-se no maximo 256 entradas (`STALE_SCAN_MAX`, o "." e o ".." contam), para que a primeira gravacao numa pasta
  gigante nao a leia toda: numa pasta com mais de 256 entradas so se limpam os temporarios que caiam nas primeiras 256 pela
  ordem em que o sistema de ficheiros lista, que numa pasta com indice por hash nao e a ordem de criacao; os outros ficam.
  Falta uma forma de continuar a leitura nas gravacoes seguintes (um DIR* ou um cookie por pasta) se isso importar.
- Os temporarios do nome da ronda 3 (`caminho.<hex>.tmp`, sem a marca `.zsave-`) nao se limpam: nao se distinguem de um
  ficheiro de outro programa.
