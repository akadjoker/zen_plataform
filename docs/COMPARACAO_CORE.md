# Comparacao do nucleo: zen_platform, GLFW 3 e SDL2

O que se compara e o nucleo (janela, teclado, rato, eventos, tempo) do backend X11, com as mesmas tarefas escritas uma vez
(`bench/compare/bench_main.c`) e ligadas a cada biblioteca por um ficheiro de dezenas de linhas (`bench_zp.c`, `bench_glfw.c`,
`bench_sdl2.c`), todas a correr no mesmo Xvfb invisivel. Repete-se com `tools/bench_compare.sh` (mais abaixo); os rotulos das tabelas
vem do script e estao em ingles. O SDL3 nao esta instalado nem em pacote nesta maquina e nao entrou.

Em resumo (numeros nas tabelas, medidos no fim da terceira ronda, ramo `core-round3`): em instrucoes, esperas ao servidor, memoria, poll parado, ciclo de
janela, movimentos do rato, teclas (com o metodo de entrada aberto) e esperar por eventos (instrucoes por ciclo e latencia de acordar) a zen_platform
ganha ou empata com o GLFW e o SDL2. O arranque sem GL custa 4,9 M de instrucoes em qualquer configuracao (GLFW 13,9 a 61,8 M, SDL2 14,7 a 57,5 M): o
metodo de entrada interno do Xlib (a tabela de compose, 47,6 M) passou para a primeira tecla, onde se paga uma vez (10 a 30 ms), e por isso a tabela
de teclas do script mostra a zen_platform pior (8077 contra 3347 instrucoes por tecla; sem a abertura, 3317). A biblioteca partilhada (197 kB) ja nao e
maior do que a do GLFW (276 kB). O que se perde e medido: o tempo de relogio de um ciclo acordar + esperar num fio so contra o SDL2 (7,7 contra 4,6 us) e
a abertura do metodo na primeira tecla, explicados em "Terceira ronda" e em "Onde perde". O que a primeira ronda tinha corrigido (arranque sem GL,
memoria, esperas ao servidor) esta em "Antes e depois"; a segunda em "Segunda ronda"; a terceira (esperar por eventos, `key_mods`, o arranque, o
Windows lido) em "Terceira ronda"; a quarta (um teste intermitente que era do teste, o `process_kill_tree`, o fake e o Wayland contra o GLFW e o SDL2,
com `tools/bench_wayland.sh`) em "Quarta ronda".

## Maquina e versoes

- AMD Ryzen 5 5500U (6 nucleos, 12 fios), Linux 7.0.0-43, gcc 13.3.0, `-O2 -DNDEBUG` na biblioteca e nos programas (os binarios do GLFW e
  do SDL2 sao os da distribuicao, com as opcoes dela). Xvfb 2:21.1.12-1ubuntu1.9 com `-screen 0 1280x800x24 -noreset`, GLX por llvmpipe (Mesa 25.2).
- GLFW 3.3.10 (pacote `libglfw3` do Ubuntu, so X11; desempacotado numa pasta propria com `dpkg-deb -x`, nada instalado), SDL2 2.30.0 (o do
  sistema). valgrind 3.22.0 do pacote do Ubuntu, tambem desempacotado numa pasta propria (o `perf` nao tem permissoes aqui:
  `perf_event_paranoid` = 4).
- A maquina estava partilhada com outras sessoes (carga 11 a 14 durante as medicoes da primeira ronda, 8 a 36 na segunda e 9 a 57 na terceira): **os tempos oscilam** e a ordem entre duas
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
tools/bench_compare.sh -n 5 wait       # esperar por eventos: um ciclo acordar + esperar, a latencia de acordar, 10 s parado
ZP_BENCH_SRC=<outra arvore> ZP_BENCH_LIBS=zp tools/bench_compare.sh   # a zen_platform de outro commit (a coluna "antes")
ZP_BENCH_GL=gl tools/bench_compare.sh idle wait                      # os estagios idle e wait com janelas GL (as tabelas do Wayland so as tem assim)
GLFW_ROOT=<libglfw3-wayland e libglfw3-dev desempacotados> tools/bench_wayland.sh   # o Wayland: o compositor de teste sem cabeca, a zen_platform, o SDL2 e o GLFW
GLFW_ROOT=<pasta com usr/include/GLFW e usr/lib/x86_64-linux-gnu/libglfw.so> VALGRIND=<valgrind> VALGRIND_LIB=<libexec/valgrind> tools/bench_compare.sh
```

O script arranca o Xvfb por `tools/xvfb_run.sh` (invisivel, sem Wayland, `-noreset`), compila a arvore em `$ZP_BENCH_DIR` com
`-DPLATFORM_BUILD_COMPARE=ON` (a opcao esta desligada por defeito: a compilacao normal nao precisa de GLFW nem de SDL) e escreve as
tabelas em `results.md`. A coluna "antes" deste documento e o mesmo script com `ZP_BENCH_SRC` num checkout de `9a6c037` (a `main`) com
`bench/compare` e `tools/bench_compare.sh` copiados para la.

## Resultados

Instrucoes: valgrind 3.22 callgrind, espaco de utilizador, processo inteiro. Todas as tabelas sao da corrida final da terceira ronda
(`tools/bench_compare.sh -n 5`, a `main` de `core-round3` em `c1455df`; depois dela so mudaram o `png.c`, a copia do stb da zen_ui, testes e documentacao),
numa maquina com carga de 20 a 57 (outras sessoes a compilar e a correr fuzzers): os tempos oscilam muito e valem pela ordem de grandeza; as
instrucoes, as esperas ao servidor e a memoria nao oscilam. As tabelas da espera por eventos estao em "Terceira ronda".

### Start-up, nogl, locale pt_PT.UTF-8 (the input method loads its compose table)

| metric | zp | glfw | sdl2 |
|---|---:|---:|---:|
| ms to window viewable + first loop turn (median) | 9.097 | 42.533 | 75.023 |
|   of which init (ms) | 2.912 | 20.432 | 27.222 |
|   of which window create (ms) | 0.481 | 0.576 | 1.644 |
| instructions (whole process) | 4933890 | 61767189 | 57506486 |
| X requests that wait for a reply | 19 | 80 | 122 |
| syscalls | 492 | 1054 | 1587 |

### Start-up, gl, locale pt_PT.UTF-8 (the input method loads its compose table)

| metric | zp | glfw | sdl2 |
|---|---:|---:|---:|
| ms to window viewable + first loop turn (median) | 130.946 | 118.825 | 206.774 |
|   of which init (ms) | 2.600 | 21.521 | 34.626 |
|   of which window create (ms) | 109.744 | 158.585 | 165.082 |
| instructions (whole process) | 152999645 | 214561227 | 294614227 |
| X requests that wait for a reply | 38 | 104 | 151 |
| syscalls | 1977 | 9508 | 3212 |

### Start-up, nogl, locale C (no compose table to load)

| metric | zp | glfw | sdl2 |
|---|---:|---:|---:|
| ms to window viewable + first loop turn (median) | 8.643 | 19.415 | 34.783 |
|   of which init (ms) | 2.798 | 9.647 | 24.202 |
|   of which window create (ms) | 0.496 | 0.634 | 1.286 |
| instructions (whole process) | 4979655 | 18915453 | 14744634 |
| X requests that wait for a reply | 18 | 78 | 122 |
| syscalls | 503 | 931 | 1463 |

### Start-up, nogl, locale pt_PT.UTF-8, XMODIFIERS=@im=ibus with no server

| metric | zp | glfw | sdl2 |
|---|---:|---:|---:|
| ms to window viewable + first loop turn (median) | 5.962 | 38.503 | 42.231 |
|   of which init (ms) | 3.280 | 13.548 | 31.550 |
|   of which window create (ms) | 0.560 | 0.501 | 6.741 |
| instructions (whole process) | 4972283 | 13938391 | 57021177 |
| X requests that wait for a reply | 19 | 78 | 122 |
| syscalls | 502 | 877 | 1568 |

### Memory after init, a window and 1000 idle turns, nogl

| metric | zp | glfw | sdl2 |
|---|---:|---:|---:|
| RSS (kB) | 4908 | 5384 | 9212 |
| PSS (kB) | 872 | 1556 | 3471 |
| PSS anonymous (kB) | 632 | 1136 | 1608 |
| shared libraries mapped | 15 | 19 | 49 |
| heap: malloc calls | 1068 | 11746 | 12092 |
| heap: bytes asked for | 211588 | 2242877 | 2189819 |
| heap: peak live bytes (approximate) | 144048 | 1013416 | 1081912 |

### Memory after init, a window and 1000 idle turns, gl

| metric | zp | glfw | sdl2 |
|---|---:|---:|---:|
| RSS (kB) | 79104 | 81284 | 89432 |
| PSS (kB) | 25309 | 27377 | 33584 |
| PSS anonymous (kB) | 21568 | 22128 | 28112 |
| shared libraries mapped | 47 | 50 | 77 |
| heap: malloc calls | 39029 | 50050 | 84091 |
| heap: bytes asked for | 51424845 | 53585975 | 103357507 |
| heap: peak live bytes (approximate) | 29701216 | 30318296 | 30199120 |

### Idle poll (an event loop turn with no event), no GL

| metric | zp | glfw | sdl2 |
|---|---:|---:|---:|
| ns per turn, 1e6 turns (median) | 2925.41 | 5269.77 | 4491.12 |
| instructions per turn | 1480 | 2243 | 2955 |

### Window create + destroy, nogl

| metric | zp | glfw | sdl2 |
|---|---:|---:|---:|
| us per cycle, 1000 cycles (median of 5) | 102.88 | 1544.78 | 1618.50 |
| CPU us per cycle | 83.33 | 1806.34 | 2001.82 |
| instructions per cycle | 67131 | 150829 | 260903 |

### Window create + destroy, gl

| metric | zp | glfw | sdl2 |
|---|---:|---:|---:|
| us per cycle, 100 cycles (median of 5) | 15947.29 | 24126.08 | 29621.92 |
| CPU us per cycle | 14573.28 | 21462.70 | 25867.25 |
| instructions per cycle | 64476705 | 70727831 | 127981654 |

### Window create + destroy, nogl, locale C (no compose table, so an input context costs less)

| metric | zp | glfw | sdl2 |
|---|---:|---:|---:|
| us per cycle, 1000 cycles (median of 5) | 100.71 | 1697.80 | 1781.59 |
| CPU us per cycle | 84.50 | 1417.88 | 1495.06 |
| instructions per cycle | 68238 | 150717 | 249973 |

### Events, motion, 100 waiting at each turn (a frame in which 100 arrived), no GL

| metric | zp | glfw | sdl2 |
|---|---:|---:|---:|
| events delivered to the application | 100000 | 100000 | 100000 |
| CPU ms in the turns, 100000 events (median of 3) | 172.224 | 168.397 | 436.416 |
| instructions per injected event | 1790 | 1938 | 7630 |

### Events, motion, all 100000 waiting at the first turn (a frame that took long), no GL

| metric | zp | glfw | sdl2 |
|---|---:|---:|---:|
| events delivered to the application | 512 | 100000 | 65674 |
| CPU ms in the turns, 100000 events (median of 3) | 49.183 | 46.206 | 310.110 |
| instructions per injected event | 2264 | 2426 | 9777 |

### Events, key, 100 waiting at each turn (a frame in which 100 arrived), no GL

| metric | zp | glfw | sdl2 |
|---|---:|---:|---:|
| events delivered to the application | 10000 | 10000 | 10000 |
| CPU ms in the turns, 20000 events (median of 3) | 55.630 | 43.179 | 70.922 |
| instructions per injected event | 8077 | 3347 | 4261 |

### Events, key, all 20000 waiting at the first turn (a frame that took long), no GL

| metric | zp | glfw | sdl2 |
|---|---:|---:|---:|
| events delivered to the application | 85 | 10000 | 10000 |
| CPU ms in the turns, 20000 events (median of 3) | 32.296 | 17.530 | 929.721 |
| instructions per injected event | 8520 | 3846 | 59907 |

### Binary sizes (stripped, kB)

| file | kB |
|---|---:|
| minimal program, zen_platform linked statically | 94 |
| libplatform.a (archive, with its debug info stripped) | 1482 |
| minimal program, zen_platform shared (program alone) | 22 |
| libplatform.so (shared) | 197 |
| minimal program, GLFW (program alone) | 22 |
| libglfw.so (distribution, stripped) | 276 |
| minimal program, SDL2 (program alone) | 26 |
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
  chama o `XPending` quando ela esvazia). O teste do teclado muda o mapa a meio
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

Passaram sem defeito: `fuzz_paths` (348 500 execucoes em 121 s, caminhos vazios, de 6000 bytes, com NULs, UTF-8 invalido, `..` e lacos de ligacoes
simbolicas, em `/dev/shm`), `test_process_hostile` (um argumento de 1 MB, 50 000 argumentos, um ambiente de 1 MB, directorio como programa,
listas com NULL no meio, filhos mortos por `SIGKILL`, `SIGSEGV` e `SIGTERM`, e um `exec` que falha com o lock do `stderr` tomado por outro fio: 48
verificacoes), o arrastar com o botao premido para fora da janela (coordenadas negativas e a milhares de pixeis, a libertacao la fora: 18
verificacoes), a varredura de todos os keycodes e de 20 327 keysyms (o intervalo das funcoes e do teclado numerico inteiro, num Xvfb proprio porque o servidor actua sobre alguns keysyms),
`fuzz_x11_events` (362 420 execucoes em 141 s com ClientMessage de qualquer tipo e formato, SelectionNotify com propriedades ausentes,
enormes ou de outro tipo, ConfigureNotify absurdos, eventos de qualquer tipo com bytes quaisquer e quedas de XDND com listas de URIs de qualquer
forma) e `fuzz_clipboard_x11` (57 126 execucoes em 141 s com um dono hostil: propriedades de qualquer tipo e formato, INCR de qualquer forma,
um PNG por cima, o que nunca responde).

## Terceira ronda: esperar por eventos, arranque sem metodo de entrada, o que a leitura do Windows achou (ramo core-round3)

"Antes" e a ponta da `main` no inicio da ronda (`da1c7fc`), "depois" a ponta de `core-round3`; as duas contadas no mesmo Xvfb, com o mesmo
valgrind e os mesmos programas (`bench/compare`), numa maquina com carga de 9 a 30 (outras sessoes a compilar e a correr fuzzers): os tempos
nao se comparam, as instrucoes sim.

### Esperar por eventos: `platform_wait_events` / `platform_wake` contra `glfwWaitEventsTimeout` / `glfwPostEmptyEvent` e `SDL_WaitEventTimeout` / `SDL_PushEvent`

O que se mede (`bench/compare`, modos `wake`, `latency` e `idlecpu`; a etapa `wait` do `tools/bench_compare.sh`):

- **Um ciclo acordar + esperar + tratar**: um fio so, o acordar posto antes, a espera volta logo e trata o que veio (zen_platform: a espera e o
  `window_begin_frame` que a segue, que e o que um programa seu faz; GLFW: a `glfwWaitEventsTimeout` ja trata; SDL: o `SDL_WaitEventTimeout` e
  o que vem atras). Instrucoes pelo `valgrind` (20 000 ciclos menos 0).
- **Latencia de acordar**: outro fio poe o acordar 2 ms depois de o principal ter ido dormir sem limite; mediana, percentil 99 e pior de 1000
  (cinco corridas, mediana das cinco).
- **CPU parado**: 10 s a dormir na espera sem nada que a acorde; tempo de relogio e de CPU do processo.

### Sleeping between events: one wake + wait cycle on one thread, no GL

| metric | zp | glfw | sdl2 |
|---|---:|---:|---:|
| ns per cycle, 200000 cycles (median of 5) | 7696.32 | 10341.21 | 4551.39 |
| instructions per cycle | 3071 | 3566 | 3693 |

### Wake latency: another thread posts 2 ms after the main one went to sleep with no limit, 1000 times, no GL

| metric | zp | glfw | sdl2 |
|---|---:|---:|---:|
| median (us, median of 5 runs) | 41.8 | 54.1 | 564.3 |
| 99th percentile (us, median of 5 runs) | 2680.5 | 1921.8 | 20422.3 |
| worst of 1000 (us, median of 5 runs) | 31429.8 | 27112.2 | 38838.4 |

### Idle: 10 s in the wait with nothing to wake it, no GL

| metric | zp | glfw | sdl2 |
|---|---:|---:|---:|
| wall s | 10.049 | 10.054 | 10.052 |
| CPU ms | 0.056 | 0.087 | 0.048 |
| times the wait returned | 1 | 1 | 1 |


Onde cada um ganha e perde:

- **Instrucoes por ciclo: ganha** (3071 contra 3566 do GLFW e 3693 do SDL2). **Latencia de acordar: ganha na mediana** (41,8 us contra 54,1 do GLFW
  e 564,3 do SDL2; 29,7 / 39,4 / 135,4 numa corrida anterior da mesma maquina: a ordem mantem-se); o percentil 99 (2,7 ms, 1,9 ms, 20,4 ms) e o
  pior de mil (31 ms, 27 ms, 39 ms) sao os do escalonador de uma maquina com carga 30, parecidos nas tres, e o GLFW tem um percentil 99 mais baixo.
  **CPU parado: empata** (56 us, 87 us e 48 us em dez segundos, ruido de medida: o processo nao faz nada; uma chamada a `poll(2)` e uma volta).
- **Tempo de relogio por ciclo de um fio so: perde para o SDL2** (7,7 us contra 4,6 us; o GLFW fez 10,3 us). Um evento posto com `SDL_PushEvent` fica
  numa fila do proprio SDL e a espera volta sem tocar na ligacao X; a zen_platform, como o GLFW, olha primeiro para a ligacao (`XPending`: duas
  chamadas `recvmsg` com `EAGAIN`, para ver tambem os eventos que o xcb tenha na sua fila), faz o `poll`, le o descritor do acordar e o
  `window_begin_frame` volta a olhar: oito chamadas ao sistema por ciclo contra onze do GLFW. Nao se tirou a verificacao do `XPending` por causa
  de microssegundos: e ela que fecha o acordar perdido (o teste "events already in Xlib's queue" falha com um `poll` so no socket). Num
  programa que dorme entre eventos do sistema, o caso que conta, os dois tem a ligacao X para ler e o ciclo e o mesmo.

### Arranque, tecla, criar e destruir janela, `key_mods`

| | antes | depois | GLFW | SDL2 |
|---|---:|---:|---:|---:|
| arranque sem GL, `XMODIFIERS` vazio, instrucoes | 52 822 193 | 4 933 890 | 61 767 189 | 57 506 486 |
| arranque sem GL, `LC_ALL=C`, instrucoes | 10 059 687 | 4 979 655 | 18 915 453 | 14 744 634 |
| arranque sem GL com `XMODIFIERS=@im=ibus` e o servidor parado, instrucoes | 4 994 717 | 4 972 283 | 13 938 391 | 57 021 177 |
| arranque com GL, instrucoes | 201 162 946 | 152 999 645 | 214 561 227 | 294 614 227 |
| esperas de resposta ao servidor no arranque sem GL | 20 | 19 | 80 | 122 |
| RSS depois de uma janela e 1000 voltas, sem GL, kB | 5616 | 4908 | 5384 | 9212 |
| chamadas a `malloc` / bytes pedidos, sem GL | 11 428 / 2 080 572 | 1068 / 211 588 | 11 746 / 2 242 877 | 12 092 / 2 189 819 |
| criar + destruir uma janela sem GL, instrucoes | 109 325 | 67 131 | 150 829 | 260 903 |
| criar + destruir uma janela com GL, instrucoes | 64 501 842 | 64 476 705 | 70 727 831 | 127 981 654 |
| poll parado, instrucoes por volta | 1481 | 1480 | 2243 | 2955 |
| movimento, 100 por volta, instrucoes por evento | 1803 | 1790 | 1938 | 7630 |
| movimento, todos na primeira volta, instrucoes por evento | 2272 | 2264 | 2426 | 9777 |
| tecla, 100 por volta, instrucoes por evento, sem a abertura do metodo (ver abaixo) | 3351 | 3317 | 3347 | 4261 |
| tecla, 100 por volta, instrucoes por evento, como o script as conta (com a abertura) | 3351 | 8077 | 3347 | 4261 |
| a abertura do metodo de entrada interno (a tabela de compose), uma vez | no arranque | na primeira tecla, 47 600 000 | | |
| `key_mods`, instrucoes por chamada (antes tambem um pedido ao servidor por chamada) | 3534 | 43 | | |
| `libplatform.so` (stripped), kB | 345 | 197 | 276 | 1881 |
| programa minimo ligado em estatico (stripped), kB | 130 | 94 | 22 + 276 | 26 + 1881 |

"Antes" e a `main` no inicio da ronda medida com o mesmo script e as mesmas condicoes (para as celulas de arranque, de criar e destruir, de poll e de eventos), e as celulas de memoria e tamanho da segunda ronda; GLFW e SDL2 sao os da corrida final. O programa minimo do script inclui o `bench_main.c`, por isso e maior do que um `main` de dez linhas (78 kB com a mesma biblioteca).

O que mudou e porque (um commit por coisa, com a medicao na mensagem):

- **O metodo de entrada interno do Xlib abre-se na primeira tecla tambem com `XMODIFIERS` vazio.** O Xlib so procura um servidor de metodo (a
  propriedade `XIM_SERVERS` da raiz) para um nome dado com `@im=`: medido com um `ibus-daemon -x` a correr num Xvfb privado (sessao dbus
  propria), um programa sem `XMODIFIERS` compoe com a tabela interna antes e depois, e com `XMODIFIERS=@im=ibus` e o ibus em cima a tecla morta e composta pelo ibus
  (a com agudo) nos dois, e a tabela da biblioteca nem se le. Por isso a ideia da ronda anterior (ler `XIM_SERVERS` para decidir) nao era precisa. O custo (47,6 M)
  passa para a primeira tecla de qualquer janela, uma vez (10 a 30 ms); por tecla, depois, e o mesmo. A janela criada e destruida
  desce de 109 k para 67 k porque o `XCreateIC` (a tabela de compose por janela) so se paga a quem le teclas.
  Nota de medida: o numero "por evento" da tabela de teclas do `bench_compare` e a diferenca entre uma corrida de N teclas e uma de 0, e a abertura do metodo
  esta so na primeira: 8077 instrucoes por tecla em 10 000 teclas sao 3317 de tecla e 4760 da abertura repartida (47,6 M / 10 000); com o metodo aberto
  a tecla custa o mesmo que no GLFW (3317 contra 3347). Por isso a tabela de teclas do script mostra a zen_platform pior (8077 contra 3347 instrucoes e
  55,6 contra 43,2 ms de CPU nas voltas: esses 12 ms sao a abertura, uma vez, na primeira tecla). Medido como a diferenca entre 12 000 e 2 000 teclas, a
  `main` com o servidor parado (metodo aberto tarde, como agora) da 3312 e esta ronda 3334 instrucoes por tecla.
- **`key_mods`** perguntava ao servidor o estado de Caps Lock e Num Lock (`XkbGetState`: uma viagem e alocacoes) em cada chamada. Agora le o que os
  eventos `XkbStateNotify` (so dos modificadores trancados) disseram por ultimo; a primeira chamada pergunta uma vez, uma mudanca de mapa invalida e o
  `FocusIn` volta a perguntar.
- **A biblioteca partilhada desce de 356 para 192 kB** (o GLFW da distribuicao tem 276) e o programa minimo ligado em estatico de 130 para 78 kB:
  `-ffunction-sections -fdata-sections` na biblioteca e `--gc-sections` (PUBLIC, para que quem liga a estatica o leve). Os mesmos 250 simbolos
  exportados (`nm -D`, a mesma lista) e os testes iguais; a biblioteca estatica (o arquivo, antes de ligar) cresce de 1004 para 1482 kB porque tem mais seccoes, o programa ligado nao; so o GCC e o Clang em ELF e MinGW (o Apple tem `-dead_strip` e o MSVC `/OPT:REF`).

## Defeitos que a terceira ronda encontrou (cada um com o teste que falhava antes e passa depois)

| defeito | visto |
|---|---|
| nao havia espera: uma aplicacao parada gastava um nucleo (36 a 90 % medidos no iGUI) | `tests/test_wait_*`: a espera de 2 s gasta menos de 5 ms de CPU, o acordar de 4 fios sem nenhum perdido (10 000), o acordar antes da espera acaba a proxima uma vez, EINTR, mil `init`/`shutdown` sem descritores a mais |
| o acordar perdido: um `XSync` do programa deixa eventos na fila do Xlib com o socket calado, e um `poll` so no socket dorme ate ao fim do tempo | "events already in Xlib's queue" (`test_wait_x11`): com um `poll` so no socket falha (a espera de 2 s acaba sem eventos); com o `XPending` antes passa |
| sem nenhuma janela a espera voltava logo para sempre (os eventos das janelas que se foram nao tem quem os leia) | seccao "windows destroyed": falhava, passa (o que e do clipboard serve-se, o resto larga-se) |
| X11 com `XMODIFIERS` vazio: 47 M de instrucoes no arranque | "first dead key, no XMODIFIERS": a tabela de compose abria-se no `platform_init` (o inotify agora vigia desde antes); falha antes, passa; mais duas seccoes com um ibus a serio |
| `key_mods`: um pedido ao servidor por chamada | "lock state is cached": 5000 chamadas mandam 0 pedidos (antes 5000); Caps e Num Lock mudados sem foco vem-se no frame seguinte |
| o KeyPress de keycode 0 de outro cliente chegava ao `Xutf8LookupString` (SEGV no `strlen`, so sob sanitizador) | o teste interpoe o `Xutf8LookupString` e conta as chamadas: falha sem a proteccao (Release), passa |
| web: o `window_destroy` na frame nunca libertava a janela (o `app_run` segurava o ambito de um ciclo que nao volta) | `test_core_edge` (a sequencia do `main_tick` no fake): `core_window_count` durante e depois do ambito; o web nao se compilou |
| PNG de 68 bytes que declara 12799 x 2305 e tem 10 bytes de IDAT: o stb enchia 59 MB de zeros (20 s sob o ASan) | `fuzz_images`: com o codigo anterior acaba em `Illegal instruction` aos 38 s (o alvo aborta acima de 5 s); agora 0 s. Recusa-se o que o deflate (1032 para 1) nao pode ter dado |
| PNG RGBA de 4 bits: o stb calcula uma posicao antes do buffer | achado pelo `fuzz_images` (assert em `stb_image.h:4432`); so se aceitam as profundidades de cada tipo de cor |
| PNG truncado com um IDAT vazio sem CRC: o stb soma zero a um ponteiro nulo | achado pelo `fuzz_images` (UBSan, `stb_image.h:4903`); a copia do stb da zen_ui tinha os tres defeitos da ronda anterior e recebe o mesmo endurecimento |
| coordenada de um dedo NaN, infinita ou de 1e38: `(int)x` indefinido | `fuzz_touch` (UBSan, `core.c`); `test_core_edge` falha com o codigo anterior (`(int)inf` da INT_MIN) e passa |
| Wayland: o teste de acerto corria no dispatch da espera, fora do ambito de destruicao | por leitura; o `window_swap` ja despachava ao esperar a frame; `test_wait_wayland` e `test_input_wayland` passam |
| Windows (por leitura; compilados com o MinGW, nunca corridos): o teste de acerto e a chamada de tempo real sem ambito; tamanhos e posicoes sem limite (INT_MAX transborda); `window_set_vsync` numa janela de pixeis mudava o contexto de outra; os caminhos de um arrasto libertados antes de o `poll_event` os ler (uso depois de libertar em todos os arrastos); um HDROP que nao e um; texto da area de transferencia sem NUL; DIB maior do que o bloco; contagem de bytes do IME maior do que o buffer | `tests/test_win32_review.c` (um teste por correccao; o do IME nao tem: precisava de um IME) |

Passaram sem defeito (10 minutos cada, semente 1): `fuzz_x11_events` (999 578 execucoes), `fuzz_clipboard_x11` (65 196), `fuzz_paths` (413 787), e as
seccoes de bordas e de entrada do Wayland contra o compositor de teste com a espera nova (627 verificacoes sob o `test_wait_wayland`).

Os fuzzers, 10 minutos cada, semente 1 (`tools/fuzz.sh`, ASan + UBSan, sem corpus gerado no repositorio): `fuzz_x11_events` 999 578 execucoes, `fuzz_clipboard_x11`
65 196, `fuzz_paths` 413 787, `fuzz_images` 2 060 089 (depois de corrigidos tres defeitos que achou: o PNG de 68 bytes que enchia 59 MB de zeros, o RGBA de 4 bits
e o IDAT vazio truncado), `fuzz_touch` 1 633 118 (depois do `(int)inf`; o outro UB que deu era do proprio alvo) e `fuzz_wayland` 4802 (lento: cada passo
espera pelo compositor; uma entrada de 21 s numa maquina com carga 30 deu "timeout" numa corrida anterior e nao se repete sozinha: `ZP_FUZZ_TIMEOUT`).


## Quarta ronda: o teste das posicoes intermitente, `process_kill_tree`, o fake e o Wayland (ramo core-round4)

### `tests/test_process_slots.c` intermitente: era do teste, nao da tabela

Medido na maquina partilhada (carga 40 a 150), o teste antigo falhava em 14 de 40 execucoes seguidas (35 %), em 20 de 40 com os filhos atrasados (o teste a `nice 19` e 12 `yes`
a correr: 50 %) e em 110 de 200 com dois testes em paralelo e 12 `yes` (55 %): as linhas 171 (`failed == 0`: um `process_spawn` devolveu 0), 190 e 191, e as vezes
a 148 e a 150 (`test_never_polled`). Para saber de quem era, um programa de diagnostico no sitio da recusa (o `process_posix.c` incluido, como o teste faz) deu, nas execucoes
que falharam: `too many child processes`, com a tabela com 128 registos a correr e os 128 processos vivos (estado R/S em `/proc`, nenhum zombie, nenhum registo acabado).
Ou seja, o teste dormia 30 ms por cada 16 spawns e 300 ms no fim e supunha que as shells ja tinham acabado; com carga 50 uma shell demora mais de 200 ms, e uma tabela
com 128 filhos que correm de verdade recusa o 129o (e o contrato; a recusa com um registo acabado ou um filho por esperar nunca aconteceu).

Corrigido o teste e nao a biblioteca: nenhum passo supoe a velocidade de um filho. Os filhos que tem de viver ate um certo passo esperam num pipe que o teste fecha (`gate_*`), e o
que precisa de filhos acabados espera por eles (ate 30 s). Novos, deterministas: `test_full_table` (128 a correr: a 129a e recusada com "too many"; um `process_kill` faz sitio e
esquece-se esse registo; os que acabam sem ninguem perguntar ficam zombies e o spawn seguinte espera por eles, esquece o mais antigo e nao deixa zombie), `test_batches` (tres lotes de
64 que acabam antes do seguinte: ficam os 128 ultimos com o seu codigo, o primeiro lote esquece-se) e, em `test_more_children_than_slots`, a conferencia em cada recusa de que
os 128 registos da tabela estavam a correr (uma recusa com um registo acabado la seria o defeito da biblioteca que se procurava: nao houve nenhuma). Depois: **200 de 200** com dois
testes em paralelo e 12 `yes` (carga 110 a 150; o antigo, no mesmo cenario, 90 de 200) e 20 de 20 com `nice 19` e 12 `yes`. Uma execucao intermedia (4 em paralelo, `nice 10`, 8 `yes`) mostrou
um segundo defeito, meu e do mesmo tipo (esperava so pelos 128 filhos mais novos, e um mais velho ainda a correr contava como erro): 3 falhas em 41, corrigido.

### `process_kill_tree` (commit dbccc5b) revisto como codigo novo

| defeito | visto |
|---|---|
| o registo de um filho acabado ficava na tabela e o `kill_tree` mandava SIGKILL a `-pid` sempre: quando o ultimo membro do grupo sai o numero fica livre, e um registo antigo apontava para o grupo de outro processo se o sistema lho desse: SIGKILL a um estranho | `tests/test_process_pidreuse.c`, com a reutilizacao a serio (namespace de pids proprio, proximo pid posto a mao): antes, o estranho que lidera o grupo `leader` morria ("the stranger that got the number was signalled"); agora fica vivo. Mais `test_kill_tree_recycled_number` (o numero de um registo trocado pelo de um estranho) |
| `(pid_t)id` (e `(DWORD)id` no Windows): `id + 2^32` lia-se como o pid de um filho, e `process_kill` / `process_kill_tree` matavam-no | `process_contract` em `tests/test_null_handles.c` (antes "process id 4296594538 was not refused", 6 falhas; as quatro funcoes devolvem agora desconhecido para o que nao cabe num pid) |

O que o `process_kill_tree` ja fazia bem, agora fixado por testes (passam antes e depois): lider acabado e esperado com um neto vivo (o numero continua ocupado: medido com o pid
namespace, um fork a quem se oferece esse numero recebe outro; o POSIX diz-o: um id de processo nao se reutiliza enquanto existir um grupo com esse id), `process_kill` do lider e depois
`kill_tree` do neto, `kill_tree` enquanto o lider acaba (40 vezes, com um atraso de 0 a 540 us a seguir a saida; 24 a 36 vezes o lider acabou primeiro e 4 a 16 foi morto, 0 erradas), segunda chamada, o
numero de um registo que ja saiu da tabela (136 filhos depois), um processo do programa que nao e filho do `process_spawn`, o pid do proprio teste, do pai e o 1. O que escapa: um filho que faz `setsid` /
`setpgid` (`test_kill_tree_escaped`; documentado em `platform.h`). A janela que resta (o ultimo membro sai e o numero volta a um estranho entre duas chamadas) esta em `docs/SUSPEITAS.md`.
O custo: o registo tem um campo (`group`) que e lido quando o filho e esperado e relido em cada entrada para os registos que ainda o tem; o `reap_children` olha so para esses
(callgrind, `process_is_running` + `process_exit_code`: 1829 -> 1586 instrucoes com a tabela vazia e 3410 -> 2817 com 100 registos acabados: menos do que antes, porque um registo acabado ja nao paga duas
comparacoes).

Windows (a metade do job object, escrita sem compilar): lida pelas mesmas classes de defeito, em `src/process_win32.c`. Corrigido por leitura (MinGW compila sem avisos; `tests/test_win32_review.c`
`test_process`, compilado e **nunca corrido**): `ResumeThread` sem ver o resultado (o processo ficava suspenso para sempre), o `free_slot` que apagava um registo antes de o `CreateProcessW` ter corrido (um
spawn que falhava fazia a tabela esquecer um), o `exit_code = 0` inventado depois de `TerminateProcess` / `TerminateJobObject`, o `TerminateProcess` que falha com `ACCESS_DENIED` num processo que
acabou sozinho um instante antes, e o id acima de 32 bits. Sem alteracao por estar certo pela leitura: nenhum handle do job a vazar nos caminhos de falha do spawn nem na reutilizacao do registo, o
`AssignProcessToJobObject` que falha (Windows antes do 8, processo ja dentro de um job que nao aninha) deixa correr sem job, agora dito em `platform.h`.

### O fake: o que os eventos dos construtores levam

`fake_mouse_button` mandava um botao com x = y = 0 (o X11 poe a posicao do ponteiro, `ev->xbutton`), `fake_key` com `mods` e `scancode` a 0 (o X11 poe os modificadores do estado do servidor e o keycode),
`fake_mouse_move` sem `dx`/`dy` com o rato capturado (o X11 manda o passo), `fake_touch` sem pressao (os backends mandam 1 e 0 ao levantar) e `fake_resize` sem mudar `window_get_size`. Agora os
construtores deixam esses campos para o momento da entrega (`backend_pump_events`, na ordem dos eventos): `fake_mouse_button` leva a posicao do ponteiro (o ultimo `fake_mouse_move`);
**`fake_mouse_button_at(w, x, y, button, down)`** (novo) leva o ponteiro para `x, y` primeiro (um `EVENT_MOUSE_MOVE`, se ainda la nao estava) e o botao leva `x, y`; `fake_key` leva os modificadores premidos
**antes** da tecla (o Ctrl a descer nao esta nos seus `mods`, a subir esta: o estado do X11 no evento) e `scancode` = o numero da tecla; `fake_mouse_move` leva `dx`, `dy` com o rato capturado; `fake_touch` leva a
pressao; `fake_resize` / `fake_fb_resize` mudam o que `window_get_size` / `window_get_framebuffer_size` dizem depois de entregues. A roda nao leva posicao em nenhum backend (`data.wheel` e so x, y), por isso
nao ha o que mudar; `fake_inject_event` entrega o evento como esta. Teste: `test_fake_fidelity` em `tests/test_events.c` (com o fake anterior e um `fake_mouse_button_at` de mover + botao falham 19 verificacoes).

### Wayland contra o GLFW e o SDL2, e ao lado do X11

Para comparar sem instalar nada: o compositor pequeno de `tests/test_input_wayland.c` corre como programa proprio (`bench/compare/wl_compositor`, com um `wl_output`: sem ele o SDL2 recusa-se a arrancar, "The video driver did not add any
displays"), a mesma bateria de `bench/compare` compila-se sem X11 (`-DPLATFORM_LINUX_BACKEND=wayland`) e `tools/bench_wayland.sh` corre-a (compositor num `XDG_RUNTIME_DIR` proprio, sem `DISPLAY`, parado pelo PID guardado).
O GLFW e o `libglfw3-wayland` 3.3.10 do Ubuntu, desempacotado numa pasta propria (o `libglfw3`, usado ate aqui, e so X11), o SDL2 2.30.0 do sistema (com Wayland: `libwayland-client` ligado), o valgrind 3.22.0 do pacote, tambem desempacotado.
Nada instalado no sistema; nao ha weston, so o gnome-shell, que nao se arranca (partilharia o bus da sessao).

O que estas tabelas **nao** sao: um compositor a serio. O de teste nao tem dispositivo de dados, buffers partilhados (o Mesa renderiza por software e passa pelo `wl_shm`; `LIBGL_ALWAYS_SOFTWARE=1`), decoracoes
nem `xdg-decoration`: por isso o SDL2 e o GLFW caem no libdecor, tentam carregar o plugin GTK ("Could not get required globals") e ficam sem decoracoes, o que lhes custa tempo e bibliotecas (55 e 84 mapeadas contra 44) que a zen_platform nao tem (nao desenha
decoracoes de cliente); nao se separou quanto do arranque e disso. Todas as janelas levam um contexto GL, porque o backend Wayland da zen_platform so tem janelas `RENDER_GL` (`window_create` com `RENDER_PIXELS` falha: "the Wayland backend has only RENDER_GL
windows so far"); a "janela visivel" e a janela criada (as tres bibliotecas so a devolvem depois do primeiro `configure`). As medicoes do Wayland sairam com a carga da maquina a descer de 51 (no inicio) a 14 (no fim), as do X11 com 8 a 3 (o GLFW do X11, numa segunda corrida, com 12); os tempos oscilam, as instrucoes e as chamadas ao sistema nao.
Mesma maquina, mesmo compilador e opcoes das tabelas anteriores; X11 e o Xvfb do resto do documento, com GL (`ZP_BENCH_GL=gl`).

Colunas: o Wayland a esquerda (o compositor de teste, sem cabeca), o X11 a direita (Xvfb); os rotulos das metricas sao os do script, em ingles. O GLFW do X11 e o `libglfw3` 3.3.10 desempacotado numa pasta propria.

### Arranque, com GL

| metrica | zp Wayland | glfw Wayland | sdl2 Wayland | zp X11 | glfw X11 | sdl2 X11 |
|---|---:|---:|---:|---:|---:|---:|
| ms to window created (first configure seen) + first loop turn (median) | 154.899 | 207.987 | 246.967 | 57.664 | 95.753 | 97.853 |
| of which init (ms) | 104.421 | 111.692 | 91.855 | 2.032 | 14.287 | 17.040 |
| of which window create (ms) | 55.751 | 147.865 | 138.056 | 52.893 | 81.791 | 75.513 |
| instructions (whole process) | 158 828 004 | 312 240 876 | 235 138 063 | 153 114 606 | 214 627 790 | 294 810 901 |
| syscalls | 2 240 | 6 668 | 3 238 | 2 536 | 10 104 | 3 725 |

### Memoria depois do arranque, de uma janela e de 1000 voltas paradas, com GL

| metrica | zp Wayland | glfw Wayland | sdl2 Wayland | zp X11 | glfw X11 | sdl2 X11 |
|---|---:|---:|---:|---:|---:|---:|
| RSS (kB) | 77 780 | 82 104 | 83 752 | 78 792 | 80 828 | 89 324 |
| PSS (kB) | 27 240 | 29 812 | 29 155 | 28 932 | 30 249 | 36 964 |
| PSS anonymous (kB) | 21 724 | 23 388 | 21 992 | 21 688 | 22 332 | 28 368 |
| shared libraries mapped | 44 | 55 | 84 | 47 | 50 | 77 |
| heap: malloc calls | 48 612 | 53 996 | 49 792 | 39 048 | 50 067 | 84 104 |
| heap: peak live bytes (approximate) | 29 810 880 | 30 119 688 | 29 885 584 | 29 706 144 | 30 323 960 | 30 204 232 |

### Volta do ciclo de eventos sem eventos, com GL

| metrica | zp Wayland | glfw Wayland | sdl2 Wayland | zp X11 | glfw X11 | sdl2 X11 |
|---|---:|---:|---:|---:|---:|---:|
| ns per turn, 1e6 turns (median of 5) | 1705.08 | 2104.12 | 1707.94 | 2774.22 | 5085.50 | 3013.58 |
| instructions per turn | 1 115 | 802 | 1 905 | 1 622 | 2 534 | 3 152 |

### Criar e destruir uma janela, com GL (100 ciclos)

| metrica | zp Wayland | glfw Wayland | sdl2 Wayland | zp X11 | glfw X11 | sdl2 X11 |
|---|---:|---:|---:|---:|---:|---:|
| us per cycle (median of 5) | 14059.86 | 15798.83 | 34841.64 | 7763.69 | 16549.28 | 14196.01 |
| CPU us per cycle | 13765.69 | 14232.89 | 32186.47 | 7179.98 | 18881.32 | 14116.94 |
| instructions per cycle | 64 481 229 | 65 621 664 | 87 246 349 | 64 468 040 | 70 685 312 | 127 982 643 |

### Esperar e acordar num fio so, com GL

| metrica | zp Wayland | glfw Wayland | sdl2 Wayland | zp X11 | glfw X11 | sdl2 X11 |
|---|---:|---:|---:|---:|---:|---:|
| ns per cycle, 200000 cycles (median of 5) | 4810.81 | 18360.28 | 2059.86 | 4711.25 | 8674.04 | 2615.57 |
| instructions per cycle | 2 413 | 8 384 | 2 661 | 3 456 | 4 090 | 3 898 |

### Latencia de acordar (outro fio avisa 2 ms depois de o principal adormecer, 1000 vezes), com GL

| metrica | zp Wayland | glfw Wayland | sdl2 Wayland | zp X11 | glfw X11 | sdl2 X11 |
|---|---:|---:|---:|---:|---:|---:|
| median (us, median of 5 runs) | 30.8 | 100.1 | 102.9 | 46.3 | 46.1 | 129.8 |
| 99th percentile (us, median of 5 runs) | 1003.8 | 6030.0 | 5499.3 | 119.6 | 84.2 | 208.4 |
| worst of 1000 (us, median of 5 runs) | 5063.8 | 11581.2 | 4029.0 | 596.8 | 489.9 | 323.4 |

### Parado 10 s na espera, sem nada que acorde, com GL

| metrica | zp Wayland | glfw Wayland | sdl2 Wayland | zp X11 | glfw X11 | sdl2 X11 |
|---|---:|---:|---:|---:|---:|---:|
| wall s | 10.049 | 10.027 | 10.051 | 10.050 | 10.050 | 10.049 |
| CPU ms | 0.073 | 0.045 | 0.059 | 0.056 | 0.081 | 0.037 |
| times the wait returned | 1 | 1 | 1 | 1 | 1 | 1 |
Quem ganha e quem perde, no Wayland (instrucoes e chamadas ao sistema valem; os tempos oscilam):

- **Arranque**: 158,8 M de instrucoes contra 312,2 M (GLFW) e 235,1 M (SDL2), 2240 chamadas ao sistema contra 6668 e 3238, 155 ms contra 208 e 247. A `window_create` custa 56 ms contra 148 e 138; o `platform_init` da zen_platform
  custa 104 ms no Wayland (2,0 ms no X11), tanto como o dos outros dois (112 e 92 ms): o que o ocupa no Wayland (o keymap do xkbcommon, de que a "Terceira ronda" ja tinha 17 a 70 ms por `platform_init`, o EGL e as voltas ao compositor) nao se separou.
- **Memoria**: RSS 77,8 MB contra 82,1 e 83,8, PSS 27,2 contra 29,8 e 29,2; 44 bibliotecas mapeadas contra 55 e 84; o Mesa domina nas tres, como no X11 (RSS com GL 78,8 contra 80,8 e 89,3).
- **Criar e destruir uma janela**: 14,1 ms contra 15,8 e 34,8; 64,5 M de instrucoes contra 65,6 M e 87,2 M.
- **Acordar a partir de outro fio**: mediana 30,8 us contra 100,1 e 102,9; percentil 99 de 1,0 ms contra 6,0 e 5,5 ms (o pior de 1000: 5,1 ms contra 11,6 e 4,0: o SDL2 ganha no pior caso).
- **Parado 10 s na espera**: 0,07 ms de CPU, uma volta, como o GLFW (0,05) e o SDL2 (0,06): empate.
- **Onde perde**: (1) a volta do ciclo sem eventos custa 1115 instrucoes contra 802 do GLFW (e 1905 do SDL2); em tempo, 1,71 us contra 2,10 (GLFW) e 1,71 (SDL2): empata nos tempos e perde para o GLFW nas instrucoes. (2) Um ciclo acordar + esperar num fio so: 4,81 us contra 2,06 do SDL2
  (o GLFW 18,4), com menos instrucoes (2413 contra 2661): a diferenca esta no que o nucleo do sistema faz em cada volta, e e a do X11 (4,71 contra 2,62 us); nao se investigou o que se pode tirar. (3) O arranque total no Wayland e quase tres vezes o do X11 (155 contra 58 ms), como o do GLFW (208 contra 96) e o do SDL2 (247 contra 98).
- Nao se mediu o que depende de um compositor a serio: eventos de entrada (o XTest nao tem equivalente), a latencia desde o evento do compositor ate a aplicacao, nem as decoracoes.

## Onde a zen_platform perde e porque

1. **A tabela de compose do Xlib paga-se uma vez, na primeira tecla, sem `XMODIFIERS` e com o servidor de metodo parado.** O arranque custa 4,9 M de
   instrucoes (o GLFW 13,9 a 61,8 M, o SDL2 14,7 a 57,5 M) e a primeira tecla, de qualquer janela, 47,6 M (10 a 30 ms, uma vez): o custo mudou de sitio, nao
   desapareceu (a tabela de teclas do script, que conta a abertura, mostra 8077 contra 3347 instrucoes por tecla e 55,6 contra 43,2 ms; a tecla, depois,
   custa o mesmo). O GLFW e o SDL2 com o servidor parado ficam sem metodo de entrada (nao ha teclas mortas) e nao pagam nunca; a zen_platform tem teclas mortas
   desde a primeira tecla e paga o preco quando a primeira chega. Um jogo que nao le teclas nao o paga; um que as le tem um soluco de uma ou duas frames na
   primeira. Detalhes e o que faltou em `docs/SUSPEITAS.md`.
2. **Acordar e esperar num fio so: 7,7 us de relogio contra 4,6 do SDL2** (menos instrucoes: 3071 contra 3693; o GLFW fez 10,3 us). O SDL2 poe o evento
   numa fila propria e nao toca na ligacao X; a zen_platform olha para ela (`XPending`) para nao dormir com eventos que o Xlib ou o xcb ja leram.
   O percentil 99 da latencia de acordar (2,7 ms) e maior do que o do GLFW (1,9 ms) nesta maquina com carga 30; a mediana e a melhor (41,8 us).
3. **O arquivo da biblioteca estatica cresce** (1004 para 1482 kB, mais seccoes): o programa ligado nao (130 para 94 kB com o `bench_main`, 78 kB com um
   `main` minimo). A biblioteca partilhada (197 kB) ja e mais pequena do que a do GLFW (276 kB), e inclui o que o GLFW nao tem (dialogos, PNG, o rasterizador
   por software, ficheiros, processos, gamepad evdev, fios).

A tecla, que a primeira ronda dava como perdida (16 a 20 % acima do GLFW), esta empatada (3317 contra 3347 depois da abertura; ver "Terceira ronda"). O RSS sem
GL (4908 kB contra 5384 do GLFW) e a memoria (1068 chamadas a `malloc` contra 11 746) deixaram de ser uma perda: o metodo de entrada interno ja nao carrega a
tabela de compose no arranque.

## Onde ganha e porque

- **Esperas ao servidor no arranque**: 19 contra 80 (GLFW) e 122 (SDL2); uma janela nova custa cerca de 2.
- **Instrucoes**: arranque em qualquer configuracao (4,9 M contra 13,9 a 61,8 M do GLFW e 14,7 a 57,5 M do SDL2), poll parado (1480 contra 2243 e 2955 por
  volta), criar e destruir uma janela (67 k contra 151 k e 261 k; com GL 64,5 M contra 70,7 M e 128 M), movimento do rato (1790 contra 1938 e 7630 por
  evento; 2264 contra 2426 com a lista cheia), teclas depois da abertura do metodo (3317 contra 3347) e esperar por eventos (3071 instrucoes por ciclo contra
  3566 e 3693, latencia de acordar de 41,8 us contra 54,1 e 564,3).
- **Memoria**: sem GL 4908 kB de RSS contra 5384 e 9212 (PSS 872 contra 1556 e 3471; 1068 chamadas a `malloc` contra 11 746 e 12 092); com GL igual (RSS 79,1 MB contra 81,3 e 89,4:
  e o Mesa que domina nas tres).
- **CPU parado a dormir na espera**: empata (menos de 0,1 ms em dez segundos nas tres).
- Os tempos (ms) das tabelas sairam com carga de 20 a 57 e a ordem entre bibliotecas oscila: valem pela ordem de grandeza; as instrucoes e as esperas nao oscilam.

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

- Tudo o que esta em `docs/SUSPEITAS.md` na secao do nucleo: a espera por eventos em Wayland contra o GLFW e o SDL2 e corrida no Windows, no Android e no web, o
  que a espera nao espera (um metodo de entrada por socket proprio, outro fio a ler a ligacao X, o XInput), o acordar a cada 0,1 s com uma tecla premida, a lista
  de eventos de 256, a destruicao num gancho no Windows e no web (por leitura), o `tests/test_win32_review.c` por correr, o dono de clipboard morto lembrado
  pelo id da janela, e os fuzzers (10 minutos cada; faltam o servidor X hostil para o XInput 2, o ibus com bytes de fuzzer e o arrastar e largar fora do X11).
- Wayland: medido na quarta ronda contra um compositor de teste (sem cabeca, sem buffers partilhados, sem `xdg-decoration`), nao contra um compositor a serio; faltam a
  latencia dos eventos de entrada e as decoracoes. O backend Wayland so tem janelas `RENDER_GL`. Windows: so se compila
  (`tools/check.sh mingw`). Nem o SDL3, nem a latencia (do evento do servidor ate a aplicacao), so o custo de os tirar da ligacao e o de acordar.
- Os tempos pedem uma maquina sossegada; as instrucoes e as esperas ao servidor nao.
