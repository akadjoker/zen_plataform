# Fase 3: rasterizador

Estado: concluída. Testes a passar (331 verificações em `test_raster`, 431 em `test_geom`), também com ASan e UBSan, comparação com a referência dentro da tolerância medida, benchmark registado.

## Fase 2 (geometria), fechada no mesmo ciclo

`include/zv_geom.h`, `src/geom.c`:

| Peça | Função |
|---|---|
| `ZvMatrix` | afim 2x3 com a convenção do Canvas (`x' = a x + c y + e`); `concat`, `invert` (devolve `false` em singular), `translate`/`scale`/`rotate` ao estilo do Canvas (a nova operação aplica-se primeiro), `max_scale` (maior valor singular, para converter tolerâncias em pixels), bounds transformados |
| `ZvPath` | verbos move/line/quad/cubic/close com vários subpaths; regras do Canvas: um segmento sem ponto atual começa um subpath no seu próprio ponto; depois de `close` o segmento seguinte começa no início do subpath fechado; memória pelo alocador, falha devolve `false` e deixa o path como estava |
| `zv_path_bounds` | bounds justos (extremos das curvas pelas raízes da derivada), além dos bounds dos pontos de controlo |
| `zv_path_flatten` | achata o path já transformado em `ZvPolyline` (um contorno por subpath, com `closed`); o número de segmentos por curva vem da fórmula de Wang, `n = ceil(sqrt(k · max|Δ²P| / tol))` com `k = 0,25` (quadráticas) e `0,75` (cúbicas), que garante o erro de corda, não o estima |

Testes de tabela: 6 matrizes com ida e volta pela inversa; 18 curvas (degeneradas, retas, laços, minúsculas, enormes) em 4 tolerâncias com o erro máximo medido por amostragem densa (2001 pontos) contra a polilinha: nunca acima da tolerância mais 2 % de folga de vírgula flutuante.

## O rasterizador (`include/zv_raster.h`, `src/raster.c`)

- Acumulação de área com sinal em células esparsas (uma célula por pixel que uma aresta toca), ponto fixo 24.8, a técnica do AGG: `cover` (altura com sinal) e `area` por célula; a varredura por scanline soma o `cover` da esquerda para a direita e dá cobertura exata para uma aresta por pixel.
- As duas regras de preenchimento saem das mesmas células: nonzero satura em 1, even-odd dobra em dente de serra.
- Cobertura arredondada ao mais próximo (`(a·255 + meio) / cheio`), não truncada como no AGG.
- Clipping antes das células: arestas fora das linhas do clip são descartadas; em x, as partes à esquerda do clip são encostadas ao lado esquerdo (mantêm o efeito no winding) e as da direita ao lado direito (fecham as tiras). O custo segue as arestas visíveis.
- Saída por spans: tiras de cobertura constante (caminho rápido: opaco é um preenchimento) e máscaras por pixel só onde há células. `zv_fill_polyline_solid` liga isto aos spans da Fase 1.

### Limite conhecido

Onde duas arestas de sentido oposto atravessam o mesmo pixel, as áreas com sinal cancelam-se em vez de se combinarem por sub-região. É a aproximação de todos os rasterizadores de acumulação; aparece só nos pixels de cruzamento (2 pixels em 4096 na estrela even-odd).

## Como se sabe que está certo

Referência independente nos testes: a área exata do polígono dentro de cada pixel, por recorte de Sutherland-Hodgman ao quadrado do pixel e fórmula do sapateiro. Com os pontos arredondados a 1/256 (o que o rasterizador recebe):

| Caso | erro máximo (em 255) |
|---|---|
| retângulos fracionários | 1 |
| triângulo, triângulo invertido, nonzero e even-odd | 1 |
| lasca mais fina que um pixel | 1 |
| L (côncavo), polígono com espigões | 0 e 1 |
| 40 polígonos convexos aleatórios | 2 |
| 40 triângulos finos aleatórios | 1 |

O 2 vem da aritmética inteira: a altura de cada aresta é distribuída pelas células em unidades de 1/256 e dois arredondamentos no mesmo pixel somam-se. A soma da cobertura iguala a área do polígono dentro do arredondamento de cada pixel de aresta.

Mais: regras de preenchimento (estrela, quadrados sobrepostos com a mesma e com orientação oposta, retângulos adjacentes num só path sem costura), clipping (forma a atravessar os quatro lados do clip igual ao desenho sem clip dentro da caixa, a 1 de diferença pela quantização do ponto de corte; formas inteiramente fora não tocam em nada; forma de 10^6 px cobre tudo; clip vazio), ordem e validade dos spans, total da cobertura de um quadrado rodado igual à área, degenerados (sem arestas, só horizontais, ponto, NaN, infinito, clip invertido), alocador a falhar.

## Contra o Chromium

| Cena | diferença máxima por canal | pixels acima de 8 (‰) |
|---|---|---|
| rect_opaque | 0 | 0 |
| rect_subpixel | 1 | 0 |
| tri_subpixel | 7 | 0 |
| overlap_edges | 5 | 0 |
| sliver | 23 | 15 |
| star_nonzero | 41 | 21 |
| star_evenodd | 104 (2 pixels de cruzamento), 32 nos restantes | 24 |
| circle_cubic | 35 | 15 |
| quad_blob | 31 | 16 |
| translucent | 4 | 0 |
| offscreen | 25 | 4 |

**O browser não é a cobertura exata.** Medido contra a referência analítica: no triângulo o Chromium erra até 6 e o zen_vector 2; na lasca o Chromium erra até 23 e o zen_vector 1. O padrão das diferenças (uma aresta horizontal em `y = 23,348` dá no Chromium a cobertura 0,75 em vez de 0,652) mostra sobreamostragem vertical em 4 sub-linhas no Skia para estes paths. Por isso a tolerância contra o browser é larga nas arestas e a exatidão é verificada contra a referência analítica.

Tolerâncias fixadas nos testes, a partir destes dados: retângulos 0 e 1; polígonos retos 32 sem exceções; curvas e estrelas 48, com 2 pixels de exceção na estrela even-odd; no máximo 30 ‰ dos pixels acima de 8.

### Tolerância de achatamento: 0,25 px rejeitado, 0,05 px adotado

Medido no círculo de cúbicas e no blob de quadráticas (diferença máxima contra o Chromium): 0,25 px dá 75 e 53; 0,1 dá 45 e 42; 0,05 dá 35 e 31; 0,03 dá 31 e 33; 0,02 e 0,01 não melhoram. As cordas ficam do lado de dentro das curvas convexas e com 0,25 px encolhem o círculo em até um quarto de pixel, visível no topo (cobertura 0,53 onde o exato é 0,74). Abaixo de 0,05 o que resta é a diferença de anti-aliasing do próprio browser. Custo: o número de segmentos cresce com `1/sqrt(tol)`, 0,25 para 0,05 é 2,2 vezes mais segmentos.

## Benchmark

`bench_raster`, cenas equivalentes às do `bench_draw2d` (1280x720, mínimo e mediana de 9 medidas, neste contentor):

| Cena | zen_vector mín (ms) | mediana | draw2d mín (ms) |
|---|---|---|---|
| fill_rect_opaque (4000 retângulos) | 27,4 | 31,1 | 8,5 |
| fill_rect_alpha (4000, alpha 0x80) | 54,7 | 63,0 | 61,7 |
| fill_circle_alpha (1000 círculos) | 15,6 | 16,0 | 11,1 |
| stars_alpha (500 estrelas, só zen_vector) | 9,5 | 9,9 | — |
| full_screen_alpha (20 ecrãs inteiros) | 19,2 | 20,6 | — |

Leitura honesta: o `draw2d` desenha retângulos e círculos inteiros, sem anti-aliasing, com coordenadas inteiras; o zen_vector passa tudo pelo rasterizador com anti-aliasing e coordenadas fracionárias. Nos retângulos opacos o draw2d é 3,2 vezes mais rápido (é um preenchimento de memória); com alpha o zen_vector já empata, porque o blend da Fase 1 é mais rápido. O custo fixo por forma (clip, células, ordenação, varredura) e o caminho dos retângulos alinhados são alvos da Fase 9; a Fase 7 pode ainda desviar `fillRect` sem rotação do rasterizador.

Correr: `build-vector/bench_raster` (ou `--csv`).
