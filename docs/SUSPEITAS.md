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

A ronda 4 leu tambem a metade do job object do `process_kill_tree` (`src/process_win32.c`, vinda de um patch escrito sem compilar) e
corrigiu o que a leitura mostra errado (compila com o MinGW sem avisos; `tests/test_win32_review.c`, `test_process`, compilado e nunca corrido):

- `ResumeThread` sem ver o resultado: se falhasse, o processo ficava suspenso para sempre e lia-se como a correr; agora termina-o, fecha os
  handles e devolve 0.
- `free_slot` fechava e apagava o registo acabado mais antigo antes de o `CreateProcessW` ter corrido: um spawn que falhava fazia a tabela
  esquecer um registo; agora so sai quando o processo novo existe.
- `exit_code = 0` inventado depois de `TerminateProcess` / `TerminateJobObject`: um processo que acabou sozinho um instante antes perdia o
  codigo; agora espera e le o codigo que ele tem. E um `TerminateProcess` num processo que acabou sozinho entre o `refresh` e a chamada falha com
  `ACCESS_DENIED` e o `process_kill` devolvia false; agora vee-se se ja acabou.
- um id acima de 32 bits lia-se como o `DWORD` da esquerda (`id + 2^32` acabava o filho), como no POSIX.

Lido e deixado, por estar certo pela leitura: o job so e criado depois do `CreateProcessW` (nenhum handle a vazar se este falha) e fecha-se se o
`AssignProcessToJobObject` falha (Windows antes do 8, ou um processo ja dentro de um job que nao aninha: corre sem job e o `process_kill_tree`
so acaba o processo, agora dito em `platform.h`); o PID nao se reutiliza enquanto o registo guarda o handle do processo. Falta numa maquina
Windows: correr `test_process` (o `cmd` e o `ping` que ele corre acabam juntos; um `ping` lancado com `start /b` de um `cmd` que ja acabou tambem), e ver um
processo que e lancado de dentro de um job que nao aninha (Windows 7) a correr sem job.

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

## `process_kill_tree` (POSIX): o que resta depois da revisao da ronda 4

Provado por `tests/test_process_pidreuse.c` (um namespace de pids proprio, onde o proximo pid se poe a mao; salta onde o sistema nao deixa criar namespaces
sem privilegios): com o lider acabado e esperado e um neto vivo o numero continua ocupado (o POSIX: um id de processo nao se reutiliza enquanto
existir um grupo com esse id), e com o grupo vazio o numero volta a quem o pedir, que pode liderar um grupo dele. O registo sabe agora se o grupo tinha gente
(`group`, lido quando o filho e esperado e relido em cada entrada na tabela), e um registo acabado com o grupo vazio nao sinaliza nada. Resta:

- **A janela entre duas chamadas.** O ultimo membro do grupo sai, o sistema da o numero a um estranho que lidera o grupo dele, e o programa chama
  `process_kill_tree` antes de qualquer outra funcao de processos ter relido o registo: o registo ainda diz "tem gente" e o `kill(-pid)` apanha o estranho. So
  acontece com a volta dos pids (ate `pid_max`, 4 M neste sistema) dentro dessa janela; o unico remedio completo era nao esperar pelo lider enquanto o grupo tiver
  membros (zombies enquanto durar), o que e pior. Documentado em `platform.h`.
- **Quem sai do grupo escapa** (`setsid`, `setpgid`: um daemon que se desliga, uma shell com controlo de tarefas): fica fora do alcance. Testado
  (`test_kill_tree_escaped`) e documentado; uma arvore verdadeira no Linux pedia um cgroup (nao ha privilegios para isso aqui) ou o `PR_SET_CHILD_SUBREAPER` mais
  uma varredura de `/proc`, que nao e portavel.
- **Outro `waitpid` do programa** (um `SIGCHLD` ignorado, um `waitpid(-1)`): o filho acaba e o numero fica livre logo; o registo diz `-1` (ja era assim) e o grupo
  olha-se na hora; se o programa esperar pelos filhos por conta propria a janela acima abre-se mais depressa.
- O `process_kill_tree` espera pelo lider (como o `process_kill`) com o fecho da tabela; um lider em estado `D` (disco morto) bloqueia as outras funcoes.

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
  janela) mas a velocidade a que um rato de verdade a faz acontecer so se sabe com um rato.

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

## Nucleo: o que as tres rondas de bordas e da comparacao com GLFW e SDL deixaram (ramos core-edge-bench, core-round2 e core-round3)

Fechado e retirado desta lista (provas nos testes e nas mensagens dos commits; `docs/COMPARACAO_CORE.md` tem os numeros): o GLX
tocado no `platform_init`, `window_set_vsync` numa janela de pixeis, tamanhos de janela fora do que o protocolo leva, a janela que
ficava no ecra depois de `window_destroy`, o ponteiro de janela `NULL` (contrato em `platform.h`, `tests/test_null_handles.c`), a
destruicao de uma janela dentro do gancho de eventos, do teste de acerto ou da frame do `app_run` (agora tambem no web, onde cada
frame do navegador e um ambito, no Wayland dentro da espera e do `window_swap`, e no Windows dentro do teste de acerto e da chamada
de tempo real), o titulo acima do pedido maximo do servidor, a tabela de teclas, o `XOpenIM` com o servidor de metodo parado e com
`XMODIFIERS` vazio (abre-se na primeira tecla), a copia do `stb_image` da zen_ui, o KeyPress de keycode 0 de outro cliente (teste
dirigido que nao depende do sanitizador), o `key_mods` que ia ao servidor em cada chamada, a espera por eventos que nao havia
(`platform_wait_events`, `platform_wake`, `app_run_events`), e o que os fuzzers encontraram (`tests/fuzz`, `tools/fuzz.sh`). O que fica:

- **A espera por eventos foi medida em Wayland contra o GLFW e o SDL2 na ronda 4 (ver abaixo), mas nao corrida no Windows, no Android nem no web.** X11 (Xvfb) e
  Wayland (o compositor de teste, em-processo) correm os mesmos testes (`tests/wait_common.h`); o fake corre-os sob o TSan. O Windows
  (`MsgWaitForMultipleObjectsEx`) compila com o MinGW e nunca correu; o Android (`ALooper_pollOnce`) nao se compilou (sem NDK) e o corpo
  foi verificado contra declaracoes das tres funcoes; o web devolve logo. Para fechar: uma maquina Windows, o NDK e o Emscripten.
- **O que a espera nao espera, por escolha ou porque nao da** (`platform.h` diz-o): um servidor de metodo de entrada que fale por um
  socket proprio e nao pela ligacao X (raro; o GLFW e o SDL ignoram-no tambem); outro fio do programa que leia a ligacao X por conta
  propria (um driver GL ou Vulkan com o xcb) e deixe eventos numa fila com o socket calado (o `backend_gl_worker_*` acorda a espera
  depois de cada chamada sua; para o resto a aplicacao chama `platform_wake` ou espera com tempo); o XInput e o winmm (acorda-se a cada
  8 ms com um comando ligado e a cada segundo sem nenhum); e um comando de jogo que mande eventos sem parar (um joystick com ruido no
  eixo) acorda a espera sem parar: nao ha como ligar os comandos sem os ler.
- **A espera acorda a cada 0,1 s enquanto uma tecla esta premida numa janela com o foco** (a verificacao do `sync_stuck_keys`, um
  `XQueryKeymap` e um `XkbGetState`, para o caso de o servidor nunca ter mandado a libertacao). Um evento nao a substitui (e o evento que
  se perdeu); o que a evitaria era confiar que o servidor manda sempre a libertacao, o que o codigo ja deixou de fazer por causa de
  gestores de janelas e metodos de entrada que a engolem.
- **O `window_begin_frame` le o descritor do inotify dos comandos em cada volta** (um `read` que da `EAGAIN`: uma chamada ao sistema por
  frame, sem custo medido em instrucoes). Com a espera, a leitura so seria precisa depois de o `poll` o dar pronto; nao se mexeu.
- **O metodo de entrada interno do Xlib (a tabela de compose) paga-se na primeira tecla** (47,6 M de instrucoes, 10 a 30 ms, uma vez)
  quando `XMODIFIERS` esta vazio ou aponta para um servidor que nao corre; o arranque custa 5,0 M (era 52,8 M) e uma janela criada e
  destruida 66,6 k (era 109,3 k). Um programa que nao leia teclas nao o paga; um que as leia tem um soluco de uma ou duas frames na
  primeira. O custo por tecla depois disso e o mesmo (3352 contra 3351 instrucoes). O `XCreateIC` por janela custa o que custava, agora
  na primeira tecla.
- **A lista de eventos da frame (256) perde o que chega depois de cheia** (`FRAME_EVENT_MAX`): o estado (`key_down`, botoes, roda,
  posicao) e as filas de teclas (15) e de texto (31) ficam exactos, a lista fica com os primeiros 256 e so deixa entrar, cheia, um
  evento de ponteiro, tamanho ou posicao ao tirar outro destes. Um programa que siga so o fluxo de eventos (um editor de texto) e passe
  mais de 256 eventos numa frame perde as teclas do fim. Esta documentado e fixado em `tests/test_core_edge.c`.
- **Destruir a janela num gancho no Windows, no web e no Android: so se provou por leitura (e no fake, a sequencia do ambito).** O
  teste de acerto e a chamada de tempo real do Windows correm entre `core_scope_enter` e `core_scope_leave` (`tests/test_win32_review.c`,
  compilado, nunca corrido); o web abre um ambito por frame (`main_tick`, nao compilado: sem Emscripten); o Android corre o mesmo codigo
  da secretaria.
- **Windows: so se compila, e agora com um teste por cada correccao da leitura** (`tests/test_win32_review.c`: o teste de acerto e a
  chamada de tempo real que destroem a janela, tamanhos e posicoes de INT_MAX, o vsync de uma janela de pixeis, os caminhos de um
  arrasto que viviam menos do que o evento, um HDROP que nao e um, a area de transferencia com texto sem NUL, um DIB maior do que o
  bloco e uma lista de ficheiros que nao acaba). Nenhum se correu: sem Windows nem wine. O IME (`imm_composition`) so se corrigiu pela
  documentacao do `ImmGetCompositionString`, sem teste (precisava de um IME). Falta correr o ficheiro numa maquina Windows.
- **O dono morto do clipboard lembra-se pelo id da janela.** Um cliente novo pode receber o id de janela de um cliente que morreu
  (o servidor reaproveita a base dos ids); se o anterior estava marcado como morto, os pedidos ao novo falham durante 10 s. Um dono
  parado de verdade ainda tem os seus ids e o servidor larga a seleccao quando o cliente morre, por isso nao se viu fora de um teste
  que mata e recria clientes depressa. A defesa seria juntar o numero de serie da seleccao.
- **Wayland: medido contra o GLFW e o SDL2 num compositor de teste (ronda 4), com o que isso nao diz.** `docs/COMPARACAO_CORE.md`, "Quarta ronda". O
  compositor e o de `tests/test_input_wayland.c` como programa proprio (`bench/compare/wl_compositor`, mais um `wl_output`), nao um compositor a serio: nao tem
  dispositivo de dados, buffers partilhados (o Mesa renderiza por software e passa pelo `wl_shm`) nem decoracoes; os tempos de um compositor real (que compoe e
  pede frames) nao sao estes. As seccoes do X11 correm no Wayland contra ele (`tests/test_core_edge_wayland.c`); o clipboard do Wayland, o arrastar e largar e
  as decoracoes do lado do compositor continuam sem teste de bordas. O backend Wayland so tem janelas `RENDER_GL` (`window_create` com `RENDER_PIXELS` falha:
  "the Wayland backend has only RENDER_GL windows so far"), por isso as tabelas Wayland sao todas com GL. Mil `platform_init` / `platform_shutdown` demoram 17 s a 70 s
  (compilar o keymap do xkbcommon em cada um).
- **Os fuzzers correram 10 minutos cada, com semente fixa, nao horas.** `fuzz_images` (PNG e BMP, e a copia do stb da zen_ui),
  `fuzz_paths`, `fuzz_x11_events`, `fuzz_clipboard_x11`, `fuzz_touch` (o que o XInput 2, o WM_TOUCH e os backends tacteis entregam ao
  nucleo) e `fuzz_wayland` (um compositor hostil). Os eventos XInput 2 em si nao se alcancam por outro cliente (o `XSendEvent` recusa
  eventos genericos e o Xvfb nao tem dispositivo tactil): so um servidor hostil os manda, e para isso faltava um servidor X falso a
  falar o protocolo; o `fuzz_wayland` manda mensagens legais de significado absurdo, porque a libwayland do cliente larga a ligacao
  que ouve ou manda uma mal formada. Faltam o arrastar e largar fora do X11, o ibus a falar com a biblioteca com bytes de
  fuzzer, e correr tudo `ZP_FUZZ_SECONDS` de horas.
- **Em `libX11` a recuperacao de um erro de E/S (o servidor morto) e acusada pelo TSan** (`unlock of an unlocked mutex` dentro do
  `XPending` da ligacao partida): a seccao "the server goes away while waiting" de `tests/test_wait_x11.c` salta-se sob o TSan; corre
  nas compilacoes Release e ASan.
- **`platform_wake` depois de um `fork`**: o filho herda o descritor e acorda o pai. Um programa que faca `fork` sem `exec` e chame
  `platform_wake` no filho vai acordar a espera do pai; nao se tratou (`pthread_atfork`).
- **`process_spawn` apos `fork` usa `fprintf`.** Nao se reproduziu um bloqueio com o lock do `stderr` tomado por outro fio (o glibc 2.39
  reinicia os locks do stdio no filho); com outra libc podia haver. Trocar o `fprintf` por um `write` e trivial; nao se fez sem prova.
