# Comparacao do nucleo: zen_platform, GLFW 3 e SDL2

O que se compara e o nucleo (janela, teclado, rato, eventos, tempo) do backend X11, com as mesmas tarefas escritas uma vez
(`bench/compare/bench_main.c`) e ligadas a cada biblioteca por um ficheiro de dezenas de linhas (`bench_zp.c`, `bench_glfw.c`,
`bench_sdl2.c`), todas a correr no mesmo Xvfb invisivel. Repete-se com `tools/bench_compare.sh` (mais abaixo); os rotulos das tabelas
vem do script e estao em ingles. O SDL3 nao esta instalado nem em pacote nesta maquina e nao entrou.

Em resumo (numeros nas tabelas): em instrucoes, esperas ao servidor, memoria, poll parado, ciclo de janela e movimentos do rato a
zen_platform ganha ou empata com o GLFW e o SDL2. Perde em tres sitios, explicados em "Onde perde": o arranque quando o servidor de metodo
de entrada configurado nao esta la (faz mais: tem teclas mortas, e o GLFW e o SDL ficam sem metodo de entrada), uma tecla por evento
(16 a 20 % mais instrucoes do que o GLFW, o SDL fica a meio) e a biblioteca partilhada (340 kB contra 276 kB do GLFW; o programa minimo
ligado em estatico fica em 126 kB). Antes deste ramo perdia tambem, e muito, o arranque sem GL (133 M de instrucoes; agora 53 M, e 10 M sem compose), a
memoria (59 MB de RSS sem abrir GL; agora 5,5 MB, o GLFW tem 5,4) e as esperas ao servidor (74; agora 21, o GLFW tem 78); ver "Antes e
depois".

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
| ms to window viewable + first loop turn (median) | 35.846 | 33.085 | 87.013 |
|   of which init (ms) | 26.429 | 32.291 | 64.025 |
|   of which window create (ms) | 0.853 | 0.930 | 4.963 |
| instructions (whole process) | 52951314 | 61771913 | 57506435 |
| X requests that wait for a reply | 21 | 78 | 122 |
| syscalls | 674 | 1054 | 1590 |

### Start-up, gl, locale pt_PT.UTF-8 (the input method loads its compose table)

| metric | zp | glfw | sdl2 |
|---|---:|---:|---:|
| ms to window viewable + first loop turn (median) | 159.799 | 191.183 | 299.524 |
|   of which init (ms) | 28.323 | 33.706 | 56.583 |
|   of which window create (ms) | 149.917 | 152.106 | 227.227 |
| instructions (whole process) | 201501076 | 214624115 | 294779182 |
| X requests that wait for a reply | 40 | 103 | 151 |
| syscalls | 2608 | 10028 | 3631 |

### Start-up, nogl, locale C (no compose table to load)

| metric | zp | glfw | sdl2 |
|---|---:|---:|---:|
| ms to window viewable + first loop turn (median) | 15.872 | 38.639 | 62.566 |
|   of which init (ms) | 5.394 | 10.964 | 30.049 |
|   of which window create (ms) | 1.304 | 0.926 | 1.641 |
| instructions (whole process) | 10040677 | 18907627 | 14737924 |
| X requests that wait for a reply | 20 | 79 | 123 |
| syscalls | 556 | 937 | 1464 |

### Start-up, nogl, locale pt_PT.UTF-8, XMODIFIERS=@im=ibus with no server

| metric | zp | glfw | sdl2 |
|---|---:|---:|---:|
| ms to window viewable + first loop turn (median) | 47.665 | 16.603 | 63.411 |
|   of which init (ms) | 18.593 | 18.440 | 38.132 |
|   of which window create (ms) | 0.982 | 1.077 | 4.459 |
| instructions (whole process) | 52472748 | 13902251 | 57010575 |
| X requests that wait for a reply | 22 | 77 | 125 |
| syscalls | 663 | 882 | 1574 |

### Memory after init, a window and 1000 idle turns, nogl

| metric | zp | glfw | sdl2 |
|---|---:|---:|---:|
| RSS (kB) | 5508 | 5376 | 9228 |
| PSS (kB) | 1514 | 1568 | 3904 |
| PSS anonymous (kB) | 1204 | 1140 | 1596 |
| shared libraries mapped | 15 | 19 | 49 |
| heap: malloc calls | 11424 | 11745 | 12092 |
| heap: bytes asked for | 2078876 | 2242821 | 2189846 |
| heap: peak live bytes (approximate) | 995784 | 1013384 | 1081896 |

### Memory after init, a window and 1000 idle turns, gl

| metric | zp | glfw | sdl2 |
|---|---:|---:|---:|
| RSS (kB) | 79828 | 81376 | 89680 |
| PSS (kB) | 30768 | 32278 | 38997 |
| PSS anonymous (kB) | 22192 | 22228 | 28188 |
| shared libraries mapped | 47 | 50 | 77 |
| heap: malloc calls | 49410 | 50072 | 84115 |
| heap: bytes asked for | 53299401 | 53590643 | 103362598 |
| heap: peak live bytes (approximate) | 30125088 | 30322792 | 30204248 |

### Idle poll (an event loop turn with no event), no GL

| metric | zp | glfw | sdl2 |
|---|---:|---:|---:|
| ns per turn, 1e6 turns (median) | 1688.70 | 4057.99 | 3956.81 |
| instructions per turn | 1746 | 2243 | 2955 |

### Window create + destroy, nogl

| metric | zp | glfw | sdl2 |
|---|---:|---:|---:|
| us per cycle, 1000 cycles (median of 5) | 124.24 | 927.80 | 1002.24 |
| CPU us per cycle | 114.96 | 931.58 | 1504.16 |
| instructions per cycle | 108499 | 148960 | 262944 |

### Window create + destroy, gl

| metric | zp | glfw | sdl2 |
|---|---:|---:|---:|
| us per cycle, 100 cycles (median of 5) | 12759.77 | 18822.26 | 20743.05 |
| CPU us per cycle | 8712.20 | 18019.89 | 14802.57 |
| instructions per cycle | 64506473 | 70708742 | 127959908 |

### Window create + destroy, nogl, locale C (no compose table, so an input context costs less)

| metric | zp | glfw | sdl2 |
|---|---:|---:|---:|
| us per cycle, 1000 cycles (median of 5) | 170.89 | 1254.93 | 1308.98 |
| CPU us per cycle | 165.52 | 1212.67 | 1375.03 |
| instructions per cycle | 106553 | 152460 | 250178 |

### Events, motion, 100 waiting at each turn (a frame in which 100 arrived), no GL

| metric | zp | glfw | sdl2 |
|---|---:|---:|---:|
| events delivered to the application | 100000 | 100000 | 100000 |
| CPU ms in the turns, 100000 events (median of 3) | 167.542 | 168.398 | 420.106 |
| instructions per injected event | 1947 | 1938 | 7595 |

### Events, motion, all 100000 waiting at the first turn (a frame that took long), no GL

| metric | zp | glfw | sdl2 |
|---|---:|---:|---:|
| events delivered to the application | 256 | 100000 | 65674 |
| CPU ms in the turns, 100000 events (median of 3) | 46.350 | 42.291 | 305.729 |
| instructions per injected event | 2411 | 2423 | 9777 |

### Events, key, 100 waiting at each turn (a frame in which 100 arrived), no GL

| metric | zp | glfw | sdl2 |
|---|---:|---:|---:|
| events delivered to the application | 10000 | 10000 | 10000 |
| CPU ms in the turns, 20000 events (median of 3) | 38.244 | 34.998 | 59.698 |
| instructions per injected event | 4023 | 3350 | 4260 |

### Events, key, all 20000 waiting at the first turn (a frame that took long), no GL

| metric | zp | glfw | sdl2 |
|---|---:|---:|---:|
| events delivered to the application | 85 | 10000 | 10000 |
| CPU ms in the turns, 20000 events (median of 3) | 21.620 | 19.061 | 843.472 |
| instructions per injected event | 4448 | 3844 | 59915 |

### Binary sizes (stripped, kB)

| file | kB |
|---|---:|
| minimal program, zen_platform linked statically | 126 |
| libplatform.a (archive, with its debug info stripped) | 993 |
| minimal program, zen_platform shared (program alone) | 18 |
| libplatform.so (shared) | 340 |
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
