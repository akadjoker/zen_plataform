# Comparacao do nucleo: zen_platform, GLFW 3 e SDL2

O que se compara e o nucleo (janela, teclado, rato, eventos, tempo) do backend X11, com as mesmas tarefas escritas uma vez
(`bench/compare/bench_main.c`) e ligadas a cada biblioteca por um ficheiro de dezenas de linhas (`bench_zp.c`, `bench_glfw.c`,
`bench_sdl2.c`), todas a correr no mesmo Xvfb invisivel. Repete-se com `tools/bench_compare.sh` (mais abaixo); os rotulos das tabelas
vem do script e estao em ingles. O SDL3 nao esta instalado nem em pacote nesta maquina e nao entrou.

Em resumo (numeros nas tabelas, medidos no fim da segunda ronda, ramo `core-round2`): em instrucoes, esperas ao servidor, memoria, poll
parado, ciclo de janela, movimentos do rato e teclas a zen_platform ganha ou empata com o GLFW e o SDL2, e o arranque com o servidor de
metodo de entrada parado (4,99 M de instrucoes contra 13,89 M do GLFW) deixou de ser uma perda. Perde em dois sitios, explicados em "Onde
perde": o primeiro uso da tabela de compose (o custo de 47 M de instrucoes foi do arranque para a primeira tecla quando `XMODIFIERS` aponta
para um servidor parado, e continua no arranque quando `XMODIFIERS` esta vazio, onde empata com o GLFW e o SDL2) e o tamanho da biblioteca
partilhada (345 kB contra 276 kB do GLFW; o programa minimo ligado em estatico fica em 130 kB). Uma tecla custa o mesmo que no GLFW
(3351 instrucoes por evento nos dois; 4035 antes desta ronda). O que a primeira ronda tinha corrigido (arranque sem GL, memoria, esperas ao
servidor) esta em "Antes e depois"; o que esta ronda mudou, em "Segunda ronda".

## Maquina e versoes

- AMD Ryzen 5 5500U (6 nucleos, 12 fios), Linux 7.0.0-43, gcc 13.3.0, `-O2 -DNDEBUG` na biblioteca e nos programas (os binarios do GLFW e
  do SDL2 sao os da distribuicao, com as opcoes dela). Xvfb 2:21.1.12-1ubuntu1.9 com `-screen 0 1280x800x24 -noreset`, GLX por llvmpipe (Mesa 25.2).
- GLFW 3.3.10 (pacote `libglfw3` do Ubuntu, so X11; desempacotado numa pasta propria com `dpkg-deb -x`, nada instalado), SDL2 2.30.0 (o do
  sistema). valgrind 3.22.0 do pacote do Ubuntu, tambem desempacotado numa pasta propria (o `perf` nao tem permissoes aqui:
  `perf_event_paranoid` = 4).
- A maquina estava partilhada com outras sessoes (carga 11 a 14 durante todas as medicoes): **os tempos oscilam** e a ordem entre duas
  bibliotecas muda de uma corrida do script para a outra quando a diferenca e menor do que cerca de 2x (um mesmo programa deu entre
  17 e 36 ms de arranque em corridas diferentes). Contam as instrucoes (deterministicas), as esperas de resposta ao servidor
  (deterministicas) e a memoria; os tempos ficam como indicacao.

## Metodo

Cada programa faz so o que a tabela diz, com a mesma janela (640x480, visivel, redimensionavel) e a mesma configuracao:

- **Arranque**: desde o `exec` (o `bench_run` guarda o relogio monotonico antes dele) ate a janela estar mapeada no servidor
  (`XGetWindowAttributes` com `map_state == IsViewable`, igual para as tres) e uma volta do ciclo de eventos feita. Instrucoes do
  processo inteiro (ld.so, bibliotecas e saida incluidos) contadas pelo `valgrind --tool=callgrind`; esperas de resposta = `poll` bloqueante
  no socket do X contado por `strace -f`.
- **Memoria**: RSS e PSS de `/proc/self/smaps_rollup` depois de iniciar, criar a janela e 1000 voltas paradas; bibliotecas `.so` distintas
  em `/proc/self/maps`; chamadas a `malloc` e bytes pedidos com um contador em `LD_PRELOAD` (`bench/compare/mcount.c`, vai ate a saida).
  O PSS depende do que mais esta a correr (as paginas partilhadas dividem-se por todos os processos que as mapeiam): o RSS e o que se
  compara.
- **Poll parado**: 1e6 voltas do ciclo de eventos sem evento (`window_begin_frame` e o ciclo de `poll_event` da zen_platform,
  `glfwPollEvents`, o ciclo de `SDL_PollEvent`); instrucoes por volta = (20 000 voltas - 0 voltas) / 20 000 sob o valgrind.
- **Ciclo de janela**: criar e destruir, 1000 ciclos (100 com GL, o contexto custa milissegundos); instrucoes entre os ciclos N+1 e 2N, para
  que a primeira janela (que carrega o driver) nao conte.
- **Eventos**: `bench_run` (o gerador, igual para as tres) mexe o ponteiro para dentro da janela com o XTest e envia N movimentos (100 000,
  o ponteiro alterna entre dois pontos) ou N teclas (20 000, premir e largar o "a"). Os eventos ja estao no socket quando o programa os
  le: ou todos de uma vez ("a frame que demorou") ou 100 de cada vez, uma volta por lote ("uma frame em que chegaram 100", que e um rato
  de 8000 Hz a 60 fps). Conta-se o CPU gasto nas voltas e as instrucoes por evento injectado ((N - 0) / N sob o valgrind).
- **Tamanhos**: `strip --strip-all` do programa minimo e da biblioteca (estatica e partilhada onde existe).

Trabalho igual, pontos que importam:

- A zen_platform chama ela propria `setlocale(LC_CTYPE, "")` (o metodo de entrada precisa dele); o GLFW e o SDL deixam-no a aplicacao. O
  `bench_main.c` chama-o para as tres (`BENCH_SETLOCALE=1`), como faria uma aplicacao que aceita texto. Uma segunda tabela de arranque corre
  com `LC_ALL=C` (nao ha tabela de compose para carregar).
- `XMODIFIERS` esta sem valor em tudo, menos na terceira tabela de arranque: sem servidor de metodo de entrada o Xlib usa o metodo interno
  (com a tabela de compose) e as tres bibliotecas abrem o mesmo. Com `XMODIFIERS=@im=ibus` e o ibus em baixo o GLFW e o SDL ficam sem
  metodo de entrada e a zen_platform recorre ao interno (ver "Onde perde").
- Sem gestor de janelas o Xvfb nao da o foco a janela; os programas dos eventos dao-no (`XSetInputFocus`) as tres, como um gestor faria.
- GL: contexto 3.3 core, sem vsync, as tres. Sem GL: a zen_platform usa `RENDER_PIXELS`, o GLFW `GLFW_NO_API`, o SDL nao pede `SDL_WINDOW_OPENGL`.
- O que cada biblioteca **entrega** nao e igual e as tabelas dizem-no: a zen_platform junta movimentos (a lista de eventos da frame tem
  256 entradas, o estado fica exacto; ver `tests/test_core_edge.c`), o SDL deixa cair eventos acima de 65535 por volta; o GLFW entrega todos.
- Uma volta do ciclo de eventos da zen_platform (`window_begin_frame`) faz mais do que um `glfwPollEvents`: le o estado dos comandos de
  jogo, os gestos e copia o estado do teclado e do rato para as arestas. E o trabalho que o programa precisa para ter `key_pressed`.

## Como repetir

```sh
tools/bench_compare.sh                 # tudo, as tres bibliotecas que houver, 11 corridas por tempo
tools/bench_compare.sh -n 5 startup    # so o arranque, 5 corridas
ZP_BENCH_SRC=<outra arvore> ZP_BENCH_LIBS=zp tools/bench_compare.sh   # a zen_platform de outro commit (a coluna "antes")
GLFW_ROOT=<pasta com usr/include/GLFW e usr/lib/x86_64-linux-gnu/libglfw.so> VALGRIND=<valgrind> VALGRIND_LIB=<libexec/valgrind> tools/bench_compare.sh
```

O script arranca o Xvfb por `tools/xvfb_run.sh` (invisivel, sem Wayland, `-noreset`), compila a arvore em `$ZP_BENCH_DIR` com
`-DPLATFORM_BUILD_COMPARE=ON` (a opcao esta desligada por defeito: a compilacao normal nao precisa de GLFW nem de SDL) e escreve as
tabelas em `results.md`. A coluna "antes" deste documento e o mesmo script com `ZP_BENCH_SRC` num checkout de `9a6c037` (a `main`) com
`bench/compare` e `tools/bench_compare.sh` copiados para la.

## Resultados

Instrucoes: valgrind 3.22 callgrind, espaco de utilizador, processo inteiro. As tabelas de arranque sao de uma corrida do script so com
a etapa `startup` (as outras de uma corrida completa); o numero de instrucoes e o mesmo entre corridas, os tempos nao.

### Start-up, nogl, locale pt_PT.UTF-8 (the input method loads its compose table)

| metric | zp | glfw | sdl2 |
|---|---:|---:|---:|
| ms to window viewable + first loop turn (median) | 22.419 | 28.660 | 43.984 |
|   of which init (ms) | 20.946 | 22.911 | 36.000 |
|   of which window create (ms) | 0.401 | 0.614 | 1.735 |
| instructions (whole process) | 52821109 | 61750399 | 57509554 |
| X requests that wait for a reply | 21 | 79 | 122 |
| syscalls | 673 | 1058 | 1582 |

### Start-up, gl, locale pt_PT.UTF-8 (the input method loads its compose table)

| metric | zp | glfw | sdl2 |
|---|---:|---:|---:|
| ms to window viewable + first loop turn (median) | 107.792 | 121.650 | 137.314 |
|   of which init (ms) | 15.239 | 21.045 | 28.732 |
|   of which window create (ms) | 79.489 | 92.714 | 95.612 |
| instructions (whole process) | 201347215 | 214694072 | 294718216 |
| X requests that wait for a reply | 39 | 103 | 150 |
| syscalls | 2701 | 10086 | 3725 |

### Start-up, nogl, locale C (no compose table to load)

| metric | zp | glfw | sdl2 |
|---|---:|---:|---:|
| ms to window viewable + first loop turn (median) | 7.997 | 11.840 | 25.295 |
|   of which init (ms) | 3.912 | 8.523 | 14.148 |
|   of which window create (ms) | 0.353 | 0.617 | 1.394 |
| instructions (whole process) | 10063186 | 18897638 | 14775524 |
| X requests that wait for a reply | 20 | 79 | 122 |
| syscalls | 555 | 931 | 1462 |

### Start-up, nogl, locale pt_PT.UTF-8, XMODIFIERS=@im=ibus with no server

| metric | zp | glfw | sdl2 |
|---|---:|---:|---:|
| ms to window viewable + first loop turn (median) | 6.938 | 10.846 | 38.026 |
|   of which init (ms) | 3.280 | 7.193 | 27.928 |
|   of which window create (ms) | 0.566 | 0.594 | 1.215 |
| instructions (whole process) | 4993485 | 13892303 | 57028737 |
| X requests that wait for a reply | 19 | 77 | 122 |
| syscalls | 497 | 879 | 1567 |

### Memory after init, a window and 1000 idle turns, nogl

| metric | zp | glfw | sdl2 |
|---|---:|---:|---:|
| RSS (kB) | 5616 | 5408 | 9096 |
| PSS (kB) | 1562 | 1585 | 3504 |
| PSS anonymous (kB) | 1212 | 1136 | 1612 |
| shared libraries mapped | 15 | 19 | 49 |
| heap: malloc calls | 11428 | 11746 | 12090 |
| heap: bytes asked for | 2080572 | 2242653 | 2189320 |
| heap: peak live bytes (approximate) | 998072 | 1013304 | 1081800 |

### Memory after init, a window and 1000 idle turns, gl

| metric | zp | glfw | sdl2 |
|---|---:|---:|---:|
| RSS (kB) | 79656 | 81476 | 89568 |
| PSS (kB) | 29356 | 30923 | 36977 |
| PSS anonymous (kB) | 22024 | 22176 | 28348 |
| shared libraries mapped | 47 | 50 | 77 |
| heap: malloc calls | 49407 | 50069 | 84106 |
| heap: bytes asked for | 53296525 | 53587463 | 103357008 |
| heap: peak live bytes (approximate) | 30125192 | 30322904 | 30204184 |

### Idle poll (an event loop turn with no event), no GL

| metric | zp | glfw | sdl2 |
|---|---:|---:|---:|
| ns per turn, 1e6 turns (median) | 2618.29 | 4257.90 | 3647.97 |
| instructions per turn | 1481 | 2243 | 2955 |

### Window create + destroy, nogl

| metric | zp | glfw | sdl2 |
|---|---:|---:|---:|
| us per cycle, 1000 cycles (median of 5) | 187.23 | 1815.91 | 2819.01 |
| CPU us per cycle | 204.09 | 1483.34 | 1873.86 |
| instructions per cycle | 109141 | 169104 | 260839 |

### Window create + destroy, gl

| metric | zp | glfw | sdl2 |
|---|---:|---:|---:|
| us per cycle, 100 cycles (median of 5) | 15909.33 | 24961.28 | 31826.56 |
| CPU us per cycle | 17098.29 | 26164.37 | 32302.74 |
| instructions per cycle | 64499374 | 70670025 | 127961561 |

### Window create + destroy, nogl, locale C (no compose table, so an input context costs less)

| metric | zp | glfw | sdl2 |
|---|---:|---:|---:|
| us per cycle, 1000 cycles (median of 5) | 250.35 | 2158.04 | 2048.38 |
| CPU us per cycle | 203.72 | 1682.96 | 1981.41 |
| instructions per cycle | 109428 | 183872 | 249217 |

### Events, motion, 100 waiting at each turn (a frame in which 100 arrived), no GL

| metric | zp | glfw | sdl2 |
|---|---:|---:|---:|
| events delivered to the application | 100000 | 100000 | 100000 |
| CPU ms in the turns, 100000 events (median of 3) | 214.673 | 230.049 | 590.830 |
| instructions per injected event | 1803 | 1938 | 7596 |

### Events, motion, all 100000 waiting at the first turn (a frame that took long), no GL

| metric | zp | glfw | sdl2 |
|---|---:|---:|---:|
| events delivered to the application | 768 | 100000 | 68836 |
| CPU ms in the turns, 100000 events (median of 3) | 53.109 | 55.543 | 371.902 |
| instructions per injected event | 2276 | 2422 | 9778 |

### Events, key, 100 waiting at each turn (a frame in which 100 arrived), no GL

| metric | zp | glfw | sdl2 |
|---|---:|---:|---:|
| events delivered to the application | 10000 | 10000 | 10000 |
| CPU ms in the turns, 20000 events (median of 3) | 46.026 | 49.774 | 77.773 |
| instructions per injected event | 3351 | 3351 | 4256 |

### Events, key, all 20000 waiting at the first turn (a frame that took long), no GL

| metric | zp | glfw | sdl2 |
|---|---:|---:|---:|
| events delivered to the application | 85 | 10000 | 10000 |
| CPU ms in the turns, 20000 events (median of 3) | 18.934 | 19.621 | 1138.534 |
| instructions per injected event | 3782 | 3845 | 59916 |

### Binary sizes (stripped, kB)

| file | kB |
|---|---:|
| minimal program, zen_platform linked statically | 130 |
| libplatform.a (archive, with its debug info stripped) | 1004 |
| minimal program, zen_platform shared (program alone) | 18 |
| libplatform.so (shared) | 345 |
| minimal program, GLFW (program alone) | 22 |
| libglfw.so (distribution, stripped) | 276 |
| minimal program, SDL2 (program alone) | 18 |
| libSDL2.so (distribution, stripped) | 1881 |

## Antes e depois (so a zen_platform)

"Antes" e a `main` (`9a6c037`), "depois" a ponta do ramo `core-edge-bench`; o mesmo script, as mesmas condicoes. Os tempos sao de
corridas diferentes numa maquina com carga: valem pela ordem de grandeza.

| | antes | depois |
|---|---:|---:|
| arranque sem GL, instrucoes (`pt_PT.UTF-8`, `XMODIFIERS` sem valor) | 132 952 280 | 52 951 314 |
| arranque sem GL, instrucoes (`LC_ALL=C`) | 90 042 823 | 10 040 677 |
| arranque sem GL, esperas de resposta ao servidor | 74 | 21 |
| arranque sem GL, chamadas ao sistema | 1488 | 674 |
| arranque com GL, instrucoes | 201 499 099 | 201 501 076 |
| arranque com GL, esperas de resposta ao servidor | 87 | 40 |
| sem GL, RSS depois de uma janela e 1000 voltas (kB) | 59 644 | 5 508 |
| sem GL, bibliotecas `.so` mapeadas | 47 | 15 |
| sem GL, chamadas a malloc / bytes pedidos | 15 289 / 3 272 338 | 11 424 / 2 078 876 |
| com GL, RSS (kB) | 79 624 | 79 828 |
| poll parado, instrucoes por volta | 1745 | 1746 |
| criar + destruir uma janela sem GL, instrucoes | 133 809 | 108 499 |
| criar + destruir uma janela sem GL, us (mediana de 5) | 375 | 124 |
| criar + destruir uma janela com GL, instrucoes | 64 533 215 | 64 506 473 |
| movimento, 100 por volta, instrucoes por evento | 1948 | 1947 |
| tecla, 100 por volta, instrucoes por evento | 4020 | 4023 |
| tecla, 20 000 numa volta (lista cheia), instrucoes por evento | 7843 | 4448 |
| `tests/test_core_edge.c` (uma frame com 8000 eventos), instrucoes | 20 707 278 | 3 761 883 |

O que mudou e porque (um commit por coisa, com a medicao na mensagem):

- **O GLX deixou de se tocar no arranque.** O primeiro `glXQueryExtensionsString` ou `glXGetProcAddressARB` faz o libGLX carregar o driver do
  fabricante (aqui o Mesa com o LLVM); `backend_init` chamava-os, por isso mesmo um programa que so abre janelas de pixeis ou Vulkan ficava
  com 47 bibliotecas e 59 MB de RSS. Agora os ponteiros do GLX procuram-se na primeira janela GL (o custo de arranque com GL e o mesmo:
  mudou de sitio).
- **Os 34 atomos pedem-se num so `XInternAtoms`** (eram 34 viagens ao servidor) e uma janela nova deixa de perguntar ao servidor o que ja
  se sabe: o `XSetWMProtocols` pedia o atomo `WM_PROTOCOLS` de cada vez e a procura do monitor (quatro pedidos do Xrandr por monitor) so serve
  a quem centra a janela ou vai para ecra inteiro.
- **A lista de eventos cheia deixou de ser percorrida** por cada tecla que nao cabe (256 comparacoes por evento).

## Segunda ronda: antes e depois (so a zen_platform)

"Antes" e a ponta da `main` no inicio da ronda (`fb9df47`), "depois" a ponta de `core-round2`; as duas contadas no mesmo Xvfb, com o mesmo
valgrind e os mesmos programas (`bench/compare`), numa maquina com carga 8 a 36 (os tempos nao se comparam; as instrucoes sim). Os numeros
do GLFW e do SDL2 sao os das tabelas acima.

| | antes | depois | GLFW | SDL2 |
|---|---:|---:|---:|---:|
| tecla, 100 por volta, instrucoes por evento | 4035 | 3351 | 3351 | 4256 |
| tecla, 20 000 numa volta (lista cheia), instrucoes por evento | 4450 | 3782 | 3845 | 59 916 |
| movimento, 100 por volta, instrucoes por evento | 1953 | 1803 | 1938 | 7596 |
| poll parado, instrucoes por volta | 1746 | 1481 | 2243 | 2955 |
| arranque sem GL com `XMODIFIERS=@im=ibus` e o servidor parado, instrucoes | 52 462 207 | 4 993 485 | 13 892 303 | 57 028 737 |
| arranque sem GL, `XMODIFIERS` vazio, instrucoes | 52 922 829 | 52 821 109 | 61 750 399 | 57 509 554 |
| arranque sem GL, `LC_ALL=C`, instrucoes | 10 055 982 | 10 063 186 | 18 897 638 | 14 775 524 |
| criar + destruir uma janela sem GL, instrucoes | 109 532 | 109 141 | 169 104 | 260 839 |
| criar + destruir uma janela com GL, instrucoes | 64 506 473 | 64 499 374 | 70 670 025 | 127 961 561 |
| primeira tecla com `XMODIFIERS=@im=ibus` e o servidor parado, instrucoes (so depois) | 0 | cerca de 47 600 000 | 0 | 0 |
| RSS sem GL, kB (tres corridas) | 5488 a 5612 | 5492 a 5580 | 5408 | 9096 |
| `libplatform.so`, kB | 340 | 345 | 276 | 1881 |

O que mudou e porque (um commit por coisa, com a medicao na mensagem):

- **Teclas.** O perfil da primeira ronda dizia que o `XFilterEvent` custava 1260 instrucoes por evento contra 175 no GLFW; medido de novo,
  como diferenca entre corridas de 0 e 10 000 eventos (`callgrind`, por funcao), custa 174 nos dois: o numero vinha de uma conta que
  misturava o arranque. O que sobrava era nosso: `im_copy` percorria as 64 entradas da tabela das teclas tomadas pelo metodo de
  entrada em todos os eventos (396 instrucoes; agora so se percorre quando ha uma entrada viva, e as entradas com mais de 10 s de
  idade do servidor saem), o `XLookupKeysym` (cerca de 190 com o que chama; agora uma tabela keycode -> tecla, invalidada por
  `MappingNotify`, `XkbMapNotify` e `XkbNewKeyboardNotify`) e um `XPending` por evento (160; o ciclo olha para a fila com `XQLength` e so
  chama o `XPending` quando ela esvazia). Com o servidor sem BIG-REQUESTS desligavel (ver abaixo) o teste do teclado muda o mapa a meio
  de duas maneiras (`XChangeKeyboardMapping` e `setxkbmap`) e confere que as teclas o seguem; desligando uma a uma as invalidacoes ve-se que
  o `MappingNotify` sozinho falha na troca de disposicao (o servidor nao o manda para `setxkbmap`) e que os eventos do XKB sozinhos chegam
  para as duas, por isso ficam os dois (o `MappingNotify` serve a servidores sem XKB).
- **Arranque com o servidor de metodo de entrada parado.** O metodo interno do Xlib (a tabela de compose, 47 M de instrucoes) abre-se
  na primeira tecla, antes de ela ser filtrada, e nao no arranque: o arranque desce de 52,5 M para 5,0 M; a primeira tecla morta
  continua a compor (teste com a tabela de compose do proprio teste, vigiada por `inotify`: nao e aberta no arranque, e aberta pela primeira
  tecla, e a primeira tecla do processo e a morta). **Nao e de graca:** os 47,6 M passam para a primeira tecla de qualquer janela
  (10 a 30 ms, uma vez). Alternativas medidas: abrir o metodo na primeira janela nao poupa nada a quem abre uma; abri-lo so quando uma tecla
  precisa de composicao (morta, Multi_key, fora do Latin-1) obrigava a um segundo caminho para o texto sem metodo de entrada
  (`XLookupString` mais uma tabela de keysyms) que o GLFW tem e nos nao, e a um soluco apenas para quem escreve acentos. Com `XMODIFIERS`
  vazio nao se mexeu: o `XOpenIM` abre o metodo interno de qualquer forma.
- **Poll parado.** 1746 -> 1481: o ciclo percorria as ranhuras de comandos e joysticks (cerca de 270 instrucoes) em todas as voltas; agora so
  quando algum liga ou desliga (`gamepad_internal_epoch`). Pagaram-se com isto as duas instrucoes do teste de `NULL` e os incrementos do contador de
  despacho (`g_depth`).
- **Janela `NULL`.** Duas instrucoes por chamada (`test` e salto), medidas em 600 000 chamadas de `key_down`, `mouse_x`,
  `mouse_button_down`, `window_should_close`, `mouse_wheel` e `key_pressed` (6 444 838 contra 7 644 865 instrucoes).

## Defeitos que a segunda ronda encontrou (cada um com o teste que falhava antes e passa depois)

| defeito | visto |
|---|---|
| `window_destroy(NULL)` aceitava-se, o resto da API (97 funcoes que levam uma janela) desreferenciava-a | `tests/test_null_handles.c`, uma linha por funcao, cada uma num processo filho, e le `platform.h` para nao deixar entrar uma funcao nova sem a sua linha: com o nucleo anterior rebentava (a primeira falha foi um `double free`), passa |
| `window_destroy` duas vezes, ou depois do `platform_shutdown`, libertava duas vezes; o shutdown deixava as janelas por destruir | mesmo teste, parte do ciclo de vida: passa (as janelas vivas ficam numa lista, o que nao esta nela ignora-se, o shutdown destroi as que sobram) |
| destruir a janela dentro do gancho de eventos, do teste de acerto ou da frame do `app_run` (a propria, outra, criar uma, `platform_shutdown` no gancho) | seccoes "destroy inside callbacks" e "shutdown inside a hook" de `test_core_edge_x11`: com o nucleo anterior o ASan da `heap-use-after-free` em `frame_event_add` e `SEGV` em `Xutf8LookupString`; agora a destruicao fica para quando a chamada volta (`g_depth`) |
| qualquer `ClientMessage` cujo primeiro campo fosse o atomo de `WM_DELETE_WINDOW` fechava a janela, tambem de outro tipo ou formato | seccao "events from a hostile client": falhava (o evento de fecho chegava), passa |
| uma resposta XDND (`SelectionNotify`) podia nomear qualquer propriedade da janela: lia-se e apagava-se (o titulo, por exemplo) | mesma seccao: com o codigo anterior o titulo desaparecia, passa |
| um `KeyPress` de keycode 0 enviado por outro cliente ia ao `Xutf8LookupString` e rebentava no Xlib (`strlen` num apontador que nao existe) | encontrado por `fuzz_x11_events`; a entrada que o reproduz esta em `tests/fuzz/corpus/x11_events` e corre como teste |
| `png_decode`: um PNG de 100 bytes cujo IDAT declarava 1,9 GB fazia o stb pedir 1,9 GB; outro de 70 bytes fazia o inflate encher a saida de zeros durante minutos e estourar um `int`; um IDAT inicial vazio e `NULL + 0` | `fuzz_images`; as tres entradas ficam em `tests/fuzz/corpus/images`; com o codigo anterior o teste de regressao falha sob o ASan (`allocation-size-too-big`), agora o decodificador recusa o que o ficheiro nao tem, imagens acima de 64 M de pixeis, e o stb nao cresce alem do que o IHDR pode precisar |
| uma transferencia INCR de um dono que manda um byte a cada 200 ms nunca acabava | `test_clipboard_x11`: com o codigo anterior fica preso; agora desiste aos 10,0 s e nao volta a perguntar de seguida |
| um titulo de 8 MB ficava guardado inteiro (num servidor sem BIG-REQUESTS e um `BadLength` e o Xlib acaba o processo) | seccao "titles": com o codigo anterior ficavam 8 MB e agora menos do que um pedido leva, cortado numa fronteira de caracter; **o `BadLength` em si nao se provou: o `-extension BIG-REQUESTS` do Xvfb nao tira a extensao (o `xdpyinfo` continua a listar-la)** |
| o servidor que desaparece a meio acabava o processo no Xlib (`X connection broken`, `exit(1)`) | seccao "the server goes away" (um Xvfb proprio, morto com `SIGKILL`): `XSetIOErrorHandler` e `XSetIOErrorExitHandler` (os dois: o de saida sozinho nao chega) marcam a ligacao perdida, cada janela recebe `EVENT_WINDOW_CLOSE` e as chamadas seguintes falham logo; o GLX deixou de ser chamado depois (inventava um erro X e o handler por omissao acabava o processo) |
| Wayland: uma janela acima de 16384 pixeis (a do pool do `wl_shm` e um `int`) partia a ligacao com o erro 71; um titulo acima de 4000 bytes, com o erro 7 (E2BIG) | `tests/test_core_edge_wayland.c`: com o codigo anterior a ligacao parte na seccao dos tamanhos, com este passam 1158 verificacoes sob o ASan |

Passaram sem defeito: `fuzz_paths` (348 500 execucoes em 121 s, caminhos vazios, de 6000 bytes, com NULs, UTF-8 invalido, `..` e laços de ligacoes
simbolicas, em `/dev/shm`), `test_process_hostile` (um argumento de 1 MB, 50 000 argumentos, um ambiente de 1 MB, directorio como programa,
listas com NULL no meio, filhos mortos por `SIGKILL`, `SIGSEGV` e `SIGTERM`, e um `exec` que falha com o lock do `stderr` tomado por outro fio: 48
verificacoes), o arrastar com o botao premido para fora da janela (coordenadas negativas e a milhares de pixeis, a libertacao la fora: 18
verificacoes), a varredura de todos os keycodes e de 20 327 keysyms (o intervalo das funcoes e do teclado numerico inteiro, num Xvfb proprio porque o servidor actua sobre alguns keysyms),
`fuzz_x11_events` (362 420 execucoes em 141 s com ClientMessage de qualquer tipo e formato, SelectionNotify com propriedades ausentes,
enormes ou de outro tipo, ConfigureNotify absurdos, eventos de qualquer tipo com bytes quaisquer e quedas de XDND com listas de URIs de qualquer
forma) e `fuzz_clipboard_x11` (57 126 execucoes em 141 s com um dono hostil: propriedades de qualquer tipo e formato, INCR de qualquer forma,
um PNG por cima, o que nunca responde).

## Onde a zen_platform perde e porque

1. **Arranque com `XMODIFIERS=@im=ibus` e o servidor em baixo** (52,5 M de instrucoes contra 13,9 M do GLFW). `backend_init` abre o metodo de
   entrada; o configurado falha e repete com `@im=none`, o metodo interno do Xlib, que le a tabela de compose (`_XimParseStringFile`: 47 M,
   o perfil do callgrind di-lo). O GLFW e o SDL, no mesmo caso, ficam sem metodo de entrada (nao ha teclas mortas) e nao pagam. Provado
   com a outra tabela: sem `XMODIFIERS` as tres abrem o mesmo metodo e a zen_platform fica com menos (53,0 M contra 61,8 M do GLFW e 57,5 M do
   SDL); com `LC_ALL=C` (sem compose) 10,0 M contra 18,9 M e 14,7 M. Num desktop com o ibus de pe o `XOpenIM` e barato e isto nao acontece.
   Nao se mexeu: abrir o metodo mais tarde perde a primeira tecla morta. Ver `docs/SUSPEITAS.md`.
2. **Uma tecla custa 16 a 20 % mais instrucoes do que no GLFW** (4023 contra 3350 por evento; o SDL 4260). Por evento, no perfil: o
   `XFilterEvent` custa 1260 instrucoes (1070 em `_XimLocalFilter`) contra 175 no GLFW, o `XLookupKeysym` 580, o `Xutf8LookupString` 285 (igual
   no GLFW). `ZEN_NO_IME=1` (contexto de entrada sem as chamadas de volta da pre-edicao) da o mesmo custo, por isso nao e a pre-edicao; nao
   se investigou porque o filtro do contexto da zen_platform custa mais do que o do GLFW. No tempo a diferenca e de 9 % (38,2 contra 35,0 ms
   para 20 000 teclas, dentro do ruido da maquina). Uma tabela keycode -> tecla poupava as 580 do `XLookupKeysym`; ver `docs/SUSPEITAS.md`.
3. **A biblioteca partilhada e maior** (`libplatform.so` 340 kB contra `libglfw.so` 276 kB; o SDL2 tem 1881 kB), porque a zen_platform
   inclui o que o GLFW nao tem (dialogos, PNG, o rasterizador por software, ficheiros, processos, gamepad evdev, fios). Em estatico, que e
   como se usa, o programa minimo fica em 126 kB (o do GLFW e 22 + 276 = 298 kB, o do SDL2 18 + 1881 kB).
4. **RSS sem GL**: 5508 kB contra 5376 kB do GLFW (2,5 %, empate: o PSS e menor e ha menos bibliotecas, 15 contra 19; o SDL2 tem 9228 kB).

## Onde ganha e porque

- **Esperas ao servidor no arranque**: 21 contra 78 (GLFW) e 122 (SDL2); uma janela nova custa cerca de 2 (120 esperas em 50 ciclos, com o arranque, contra 432 antes: os
  atomos em lote, sem a procura do monitor). E o que se ve nos tempos de criar e destruir uma janela (124 us contra 928 e 1002 na
  corrida completa), menos nos de arranque, que a maquina ocupada tapa.
- **Instrucoes**: arranque com e sem compose, poll parado (1746 contra 2243 e 2955 por volta, 22 % menos que o GLFW), criar e destruir uma
  janela (108 k contra 149 k e 263 k; com GL 64,5 M contra 70,7 M e 128 M).
- **Movimento do rato**: empate com o GLFW (1947 contra 1938 por evento; 167,5 contra 168,4 ms para 100 000), o SDL2 gasta 3,9x.
- **Memoria com GL**: igual (RSS 79,8 MB contra 81,4 e 89,7: e o Mesa que domina nas tres).

## Defeitos do nucleo que a ronda encontrou (todos em `tests/test_core_edge_x11.c`, cada seccao num processo filho)

| defeito | visto |
|---|---|
| o GLX era tocado em `platform_init`: um programa sem janela GL carregava o driver do fabricante | seccao "pixel window loads no GL driver" falhava com a `main` (driver mapeado), passa |
| `window_set_vsync` numa janela de pixeis chamava `glXSwapIntervalEXT` nela: `BadWindow` (GLX) e o Xlib acaba o processo | mesma seccao: o erro aparecia na `main` |
| `window_set_size` com 0, negativo ou acima de 65535, e `window_create` com 100000: `BadValue` em `X_ConfigureWindow` e o Xlib acaba o processo | seccao "sizes" falhava com a `main` (processo terminado), passa |
| `window_destroy` nao despejava a ligacao: a janela ficava no servidor ate algo a despejar | seccao "a thousand windows" falhava com a `main` (a arvore do servidor ainda tinha a janela), passa |
| uma lista de eventos cheia percorria-se por cada evento que nao cabia | `tests/test_core_edge.c` passa antes e depois (fixa o comportamento); 20,7 M contra 3,8 M de instrucoes |

Passaram nas duas, sem defeito: mil janelas seguidas (descritores e RSS iguais, 100 de aquecimento), titulos de 0 a 8 MB, vazios, nulos, com
UTF-8 invalido e cortados a meio de um caracter, uma tempestade de 600 mudancas de estado da janela (mostrar, esconder, minimizar,
maximizar, ecra inteiro, decoracao, opacidade, foco, posicao, tamanho), uma janela destruida com eventos por ler e dentro do seu proprio ciclo
`poll_event`, 96 janelas ao mesmo tempo destruidas fora de ordem, teclas e botao premidos ao destruir (a janela seguinte nao recebe
nada), 1020 entalhes da roda numa frame (a soma e exacta), 10 000 mudancas de forma do cursor com modos e agarres misturados e uma janela
destruida em captura (o ponteiro do servidor fica livre), clipboard de 0 bytes a 8 MB com NUL e bytes invalidos, relogios (monotonico em
200 000 leituras; `time_sleep(0)`, `(1)` e `(16)` nunca acabam antes do tempo).

## O que ficou por fazer

- Tudo o que esta em `docs/SUSPEITAS.md` na secao do nucleo: o ponteiro de janela `NULL`, destruir a janela dentro do seu gancho de eventos,
  titulos acima do pedido maximo do servidor, a lista de eventos de 256, o metodo de entrada no arranque, o `XCreateIC` por janela e a
  tabela de teclas.
- Nao se mediu o backend Wayland, o Windows, nem o SDL3; nem a latencia (do evento do servidor ate a aplicacao), so o custo de os tirar da ligacao.
- Os tempos pedem uma maquina sossegada (aqui a carga era 11 a 14); as instrucoes e as esperas ao servidor nao.
- Nao se testaram as teclas mortas e o compose na primeira tecla de uma janela com o foco do servidor, que e onde adiar o metodo de entrada
  podia falhar.
