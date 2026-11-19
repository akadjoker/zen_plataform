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

## O clip em vigor da zen_ui (ramo zen-ui-clip)

Desde o commit do grupo e da janela, `zui_listbox`, `zui_group_begin` e `zui_window_begin` cortam o clip que poem pelo que esta em vigor
(`ui_rect_within`) e `ui_hovered` pede que o rato esteja dentro de `ui->clip`: o que um painel esconde nao desenha nem apanha o rato.
A regra para quem escreve um widget: quem poe `ui->clip` por atribuicao (e nao por `ui_push_container`, que o guarda e o repoe) tem de o
repor antes de abrir um grupo, uma janela ou uma lista, senao estes cortam-se pelo clip velho. O dialogo de ficheiros fazia-o
(`ui->clip = sb`, `= hr`) e a lista de ficheiros saia vazia; corrigido (`ui->clip = panel` antes do grupo `dlg_files`) com o teste
`test_dialog_flow` (`text_visible`). O contorno e o fundo desse grupo passam a ver-se (antes pintavam-se com o clip velho).

- `zui_statusbar_begin` poe `ui->clip = bar` sem cortar pelo clip em vigor (a barra e do ecra todo, em baixo; dentro de um painel mais
  pequeno o conteudo sairia). Nao se mexeu: nao ha teste que o mostre e nenhum exemplo a poe dentro de um painel.
- Um widget que a janela esconde ja nao apanha o rato (por leitura do codigo, `ui_hovered`); o teste de "um widget rolado para fora de
  um grupo nao apanha o rato" nao existe. Os exemplos `demo_widgets_pixel` e `demo_dock_pixel`, guiados com cliques por um Xvfb
  privado (menus, combo, lista, arrastar janelas), ficam pixel a pixel iguais aos da main, com a unica excepcao do contorno do grupo
  do dialogo de ficheiros.

## Nucleo: o que as duas rondas de bordas e da comparacao com GLFW e SDL deixaram (ramos core-edge-bench e core-round2)

Fechado e retirado desta lista (provas nos testes e nas mensagens dos commits; `docs/COMPARACAO_CORE.md` tem os numeros): o GLX
tocado no `platform_init`, `window_set_vsync` numa janela de pixeis, tamanhos de janela fora do que o protocolo leva, a janela que
ficava no ecra depois de `window_destroy`, o ponteiro de janela `NULL` (contrato em `platform.h`, `tests/test_null_handles.c`), a
destruicao de uma janela dentro do gancho de eventos, do teste de acerto ou da frame do `app_run`, o titulo acima do pedido
maximo do servidor, a tabela de teclas, o `XOpenIM` com o servidor de metodo parado, e o que os fuzzers encontraram
(`tests/fuzz`, `tools/fuzz.sh`). O que fica:

- **O metodo de entrada interno do Xlib (a tabela de compose) paga-se no arranque quando `XMODIFIERS` esta vazio e nao ha servidor de
  metodo, e na primeira tecla quando `XMODIFIERS` aponta para um servidor que nao corre.** Com o servidor parado o arranque custa 5,0 M
  de instrucoes (era 52,5 M; GLFW 13,9 M) e a primeira tecla, de qualquer janela, 47,6 M (10 a 30 ms, uma vez): o custo mudou de sitio,
  nao desapareceu. Um jogo que nao leia teclas nao o paga; um que as leia tem um soluco de uma ou duas frames na primeira. Sem
  `XMODIFIERS` a biblioteca continua a abrir o metodo no arranque (53 M, igual ao GLFW e ao SDL2): o `XOpenIM` e quem decide se ha
  servidor e nao se pode pedir "so se houver servidor". Faltava ler a propriedade `XIM_SERVERS` da raiz e o dono de `@server=<nome>` (tres
  idas ao servidor) para saber se vale a pena abrir; nao se fez porque duplica a logica do Xlib e um servidor que se registe de outra
  forma ficava sem metodo de entrada.
- **Um `XCreateIC` por janela e metade de criar e destruir uma janela** (cerca de 56 mil das 111 mil instrucoes), a funcao do Xlib que
  prepara o contexto de entrada com a tabela de compose. E o preco das teclas mortas desde a primeira tecla. Com a abertura tardia
  do metodo ja nao se paga antes da primeira tecla quando o servidor esta parado; com o metodo aberto no arranque continua a pagar-se
  por janela. Criar o contexto so no `FocusIn` poupava as janelas que nunca o tem, com o risco de ainda nao existir quando chega a
  primeira tecla de uma janela com o foco do servidor.
- **A lista de eventos da frame (256) perde o que chega depois de cheia** (`FRAME_EVENT_MAX`): o estado (`key_down`, botoes, roda,
  posicao) e as filas de teclas (15) e de texto (31) ficam exactos, a lista fica com os primeiros 256 e so deixa entrar, cheia, um
  evento de ponteiro, tamanho ou posicao ao tirar outro destes. Um programa que siga so o fluxo de eventos (um editor de texto) e passe
  mais de 256 eventos numa frame perde as teclas do fim. Esta documentado e fixado em `tests/test_core_edge.c`.
- **Destruir a janela dentro de um gancho: so se provou no X11 e no Wayland.** A destruicao fica adiada ate `window_begin_frame` ou
  `app_run` voltar (`g_depth` em `core.c`); o ponteiro da janela deixa de valer nessa altura, por isso um programa que destrua a sua
  janela no gancho e depois chame `poll_event` na mesma frame le memoria libertada (o teste do Wayland tem de ter cuidado com isso).
  No Windows o gancho e a chamada de volta de "tempo real" correm dentro do ciclo do sistema, fora destes dois pontos: a destruicao
  fica para o proximo `window_begin_frame`, `window_create` ou `platform_shutdown`; nao se correu (sem wine). No web e no Android o
  `app_run` volta logo e as frames correm depois, fora do ambito: ai um `window_destroy` dentro da frame nao fica adiado.
- **Windows: so se compila.** Tudo o que mudou em `core.c`, `clipboard.c`, `png.c` e `gamepad.c` compila no MinGW sem avisos
  (`tools/check.sh mingw`); o que e do X11 e do Wayland nao se compilou no Windows e o resto nao se correu (nao ha wine aqui).
- **O dono morto do clipboard lembra-se pelo id da janela.** Um cliente novo pode receber o id de janela de um cliente que morreu
  (o servidor reaproveita a base dos ids); se o anterior estava marcado como morto, os pedidos ao novo falham durante 10 s. Um dono
  parado de verdade ainda tem os seus ids e o servidor larga a seleccao quando o cliente morre, por isso nao se viu fora de um teste
  que mata e recria clientes depressa. A defesa seria juntar o numero de serie da seleccao.
- **Wayland: nao se mediu contra o GLFW e o SDL2.** O GLFW 3.3.10 do pacote Ubuntu so tem X11 e nao ha compositor sem janela (o weston
  nao esta instalado); o SDL2 em Wayland nao se tentou sem um. As seccoes do X11 correm no Wayland contra o compositor de teste
  (`tests/test_core_edge_wayland.c`), que nao tem dispositivo de dados nem o resto do que um compositor a serio tem: o clipboard do
  Wayland, o arrastar e largar e as decoracoes do lado do compositor continuam sem teste de bordas.
- **Os fuzzers correram minutos, nao horas.** `fuzz_images` (PNG e BMP), `fuzz_paths`, `fuzz_x11_events` e `fuzz_clipboard_x11` correram de
  100 a 150 s cada, com semente fixa, depois de corrigidos os defeitos que acharam; o X11 de baixo nivel (XInput2, o metodo de entrada
  de um servidor ibus), o Wayland (o que um compositor hostil manda), o arrastar e largar fora do X11 e a copia do `stb_image` do `zen_ui`
  (fora do ambito, e com os mesmos problemas do PNG que se corrigiram em `png.c`) nao tem fuzzer. Para os fechar: correr
  `tools/fuzz.sh` com `ZP_FUZZ_SECONDS` de horas, e um alvo por protocolo que falta.
- **`process_spawn` apos `fork` usa `fprintf`.** Nao se reproduziu um bloqueio com o lock do `stderr` tomado por outro fio (o glibc 2.39
  reinicia os locks do stdio no filho); com outra libc podia haver. Trocar o `fprintf` por um `write` e trivial; nao se fez sem prova.
