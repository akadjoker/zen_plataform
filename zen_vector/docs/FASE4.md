# Fase 4: pinturas

Estado: concluída. 154 verificações em `test_paint`, também com ASan e UBSan; todas as cenas de gradientes e padrões dentro de 4/255 do Chromium.

## O que existe (`include/zv_paint.h`, `src/paint.c`, `include/zv_fill.h`, `src/fill.c`)

| Peça | Função |
|---|---|
| `ZvPaint` | descrição: cor sólida, gradiente linear, gradiente radial de dois círculos, padrão de bitmap; `alpha` global, `matrix` (espaço da pintura para o espaço do utilizador), até 16 stops ordenados por offset (inserção estável) |
| `ZvPaintContext` | pintura preparada para uma transformação: inversa de dispositivo para pintura, tabela de 1024 cores premultiplicadas, coeficientes incrementais; `zv_paint_span` dá as cores de uma tira de pixels do dispositivo |
| Gradiente linear | `t` é afim nas coordenadas do dispositivo, um incremento por pixel |
| Gradiente radial | o do Canvas: maior `t` com `|p - c(t)| = r(t)` e `r(t) >= 0`; caso concêntrico reduzido à distância; fora do cone não pinta nada (`NaN` dá transparente) |
| Radial do Flash | é o mesmo: `focalPointRatio f` é o círculo interior de raio 0 em `(f, 0)` da caixa do gradiente, com a matriz da pintura; a Fase 8 só constrói a pintura |
| Spread | pad, reflect, repeat |
| Dithering | opcional: a tabela guarda 8 bits extra por canal, premultiplicados, e a leitura soma ruído ordenado (Bayer 4x4, média zero, até meio nível) antes de arredondar; os pixels ficam premultiplicados válidos (cor limitada ao alpha) |
| Padrão | bitmap premultiplicado com matriz própria, nearest ou bilinear (16 passos), repetição independente em x e y; fora do bitmap sem repetição é transparente |
| `zv_fill_polyline` | cobertura do rasterizador vezes cores da pintura, em tiras de até 256 pixels; uma pintura que se reduz a uma cor vai pelo caminho sólido da Fase 3 |

Decisão: os gradientes interpolam em alpha direto e premultiplicam depois, como o browser. Confirmado pela cena `grad_linear_alpha` (vermelho opaco para azul transparente): diferença máxima 2.

Degenerados: sem stops ou comprimento zero pinta nada (Canvas); um stop é cor sólida; transformação singular pinta nada; padrão sem bitmap falha em `zv_paint_prepare`.

## Como se sabe que está certo

- Linear: 64 pixels contra a interpolação direta feita no teste, canal a canal: erro máximo 1. Pad nas pontas; repeat e reflect verificados por pares de pixels equivalentes; a escala 2x do utilizador faz `t` andar a metade; gradiente vertical e horizontal dão o mesmo valor pela projeção.
- Radial: concêntrico contra a fórmula da distância (erro 1), simetria em x e y, anel com raio interior (dentro é a primeira cor, fora a última), focal: no foco `t = 0`, no círculo `t = 1`, raiz maior escolhida, ponto fora pad; cone sem contenção dá transparente fora.
- Dithering: passos entre vizinhos no máximo 1, mais de 20 mudanças em 64 pixels onde sem dithering haveria 5, média igual ao valor sem dithering a 1,5, pixels válidos num gradiente de alpha 0 a 255.
- Padrão: periodicidade em x e y, `no-repeat` transparente fora, `repeat-x` só, bilinear nos centros devolve o pixel e a meio devolve a média, escala 3x nearest, alpha global.
- Preenchimento: as cores seguem a pintura e a cobertura multiplica-as nas arestas; pintura sólida dá exatamente o mesmo que `zv_fill_polyline_solid`; tira de 1000 pixels (mais do que o buffer de 256).

## Contra o Chromium

| Cena | diferença máxima por canal |
|---|---|
| grad_linear (3 stops, diagonal) | 1 |
| grad_linear_alpha (opaco para transparente sobre branco) | 2 |
| grad_radial (dois círculos descentrados, 3 stops, num losango) | 1 |
| grad_focal (raio interior 0 no foco) | 1 |
| grad_alpha_global (globalAlpha 0,5) | 1 |
| pattern_repeat (16x16, deslocado 3,5) | 0 |
| pattern_scaled (3x bilinear, sem repetição) | 2 |
| pattern_nearest (4x sem suavização, repeat-x) | 0 |
| pattern_rotated (30 graus, repeat) | 4 |

O browser não faz dithering nos gradientes (as diferenças de 1 são o arredondamento); por isso o dithering fica desligado por omissão e é uma opção da pintura.

A imagem de teste dos padrões é construída pela mesma fórmula nos dois lados (`make_pattern_image` em `tools/scene.c` e `makePatternImage` em `tools/ref_render.js`): vermelho cresce com x, verde com y, azul em xadrez 4x4, o quarto superior esquerdo com alpha 128.
