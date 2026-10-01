# Fase 0: medição e infraestrutura de validação

Estado: concluída. Fecho cumprido: o harness compara a cena do retângulo opaco com 0 diferenças, também com ASan e UBSan.

## O que existe

| Peça | Ficheiro | Função |
|---|---|---|
| Formato de cena | `tools/scene.h` | texto, um comando por linha; a mesma cena é renderizada pela referência e pelo zen_vector |
| Interpretador de cenas | `tools/scene.c` | renderiza com o `draw2d` do zen_platform; hoje aceita `size`, `fillStyle #rrggbb` e `fillRect` com inteiros; qualquer outro comando é um erro, para uma cena nunca ser renderizada a meio |
| Referência | `tools/ref_render.js` | corre a cena num canvas real do Chromium headless e grava um BMP de 32 bits; reencaminha cada comando para o contexto 2D, por isso as fases seguintes não precisam de o mudar |
| Comparação | `tools/bmpcmp.c`, `tools/vcmp.c` | diferença máxima por canal, número de pixels acima da tolerância e imagem de diferença (preto igual, verde dentro da tolerância, vermelho fora) |
| Benchmark | `tools/bench_draw2d.c` | cinco cenas fixas do `draw2d` |
| Segunda referência | `tools/cairo_ref.c` | polígonos com o Cairo, só para medir a tolerância |

Correr (a partir da raiz do repositório):

```sh
cmake -S zen_vector -B build-vector -DCMAKE_BUILD_TYPE=Release
cmake --build build-vector
ctest --test-dir build-vector --output-on-failure
zen_vector/tools/make_refs.sh             # regenera refs/ com o Chromium (precisa de playwright)
build-vector/vcmp a.bmp b.bmp --tol 8 --diff diff.bmp
build-vector/bench_draw2d
```

## Pressupostos que fiz (confirma ou corrige)

1. **O benchmark do plano não existia no repositório.** Não conheço as tuas "cinco cenas", nem a máquina onde mediste 66,5 ms e 31,8 ms. Escrevi cinco cenas minhas (abaixo). Os meus números não são comparáveis com os teus até correres o mesmo programa na tua máquina.
2. A referência é o Chromium 141 headless, a única que é o Canvas real. O Cairo serviu só para medir desacordo entre motores.
3. Comparação em alpha direto (não premultiplicado). As cenas da Fase 0 são opacas sobre fundo opaco ou transparente; a regra para cenas translúcidas fica para a Fase 1.
4. O zen_vector vive em `zen_vector/` no repositório do zen_platform, com CMake independente que faz `add_subdirectory(..)`. O zen_platform não foi alterado nas suas funções. Para o mover para um repositório próprio basta trocar essa linha.

## Linha de base do draw2d

Cenas (1280x720, semente fixa, uma passagem de aquecimento e 9 medidas; mínimo e mediana):

1. `fill_rect_opaque`: 4000 retângulos, `BLEND_NONE`.
2. `fill_rect_alpha`: 4000 retângulos com alpha 0x80, `BLEND_ALPHA`.
3. `lines_alpha`: 8000 linhas, `BLEND_ALPHA`.
4. `fill_circle_alpha`: 1000 círculos, `BLEND_ALPHA`.
5. `blit_scaled`: 10 blits bilineares de ecrã inteiro e 2000 blits de 128x128 para 96x96 com alpha.

Medido neste contentor (Intel Xeon 2,1 GHz, GCC 13.3, `-O2`, uma thread, VM partilhada):

| Cena | mínimo (ms) | mediana (ms) |
|---|---|---|
| fill_rect_opaque | 8,3 | 8,7 |
| fill_rect_alpha | 60,5 | 62,4 |
| lines_alpha | 28,1 | 33,4 |
| fill_circle_alpha | 10,0 | 10,5 |
| blit_scaled | 262,0 | 273,8 |

Três execuções seguidas variam entre 5% e 10% (por exemplo `blit_scaled`: 252,7 / 274,8 / 266,7 ms de mínimo). Qualquer meta de desempenho tem de ficar acima desse ruído.

## Dados para a tolerância

**O Chromium é determinístico:** três renderizações das sete cenas dão ficheiros idênticos byte a byte.

**Chromium contra Cairo** (mesmas cenas, alpha direto, `vcmp --tol N`; as colunas são os pixels cuja diferença máxima por canal excede N):

| Cena | pixels | diferença máxima | > 0 | > 4 | > 8 | > 16 | > 32 |
|---|---|---|---|---|---|---|---|
| rect_opaque | 3072 | 0 | 0 | 0 | 0 | 0 | 0 |
| rect_subpixel | 1536 | 0 | 0 | 0 | 0 | 0 | 0 |
| tri_subpixel | 1024 | 13 | 79 | 10 | 5 | 0 | 0 |
| overlap_edges | 2304 | 14 | 151 | 26 | 11 | 0 | 0 |
| sliver | 2048 | 24 | 94 | 60 | 33 | 8 | 0 |
| star_nonzero | 4096 | 29 | 228 | 133 | 85 | 48 | 0 |
| star_evenodd | 4096 | 29 | 295 | 156 | 100 | 60 | 0 |

Leitura:
- Retângulos alinhados, mesmo com arestas fracionárias: os dois motores coincidem exatamente.
- Polígonos: só os pixels de aresta discordam, até 29/255 (cerca de 11%) no pior caso, e em nenhuma cena passa de 32.
- Na estrela, 60 pixels em 4096 (1,5%) discordam mais de 16.

**Tolerância provisória para as fases seguintes:** diferença máxima por canal de 32 em qualquer pixel. Para cenas sem curvas o critério pode ser mais apertado, até 0 nos retângulos. A Fase 3 tem de a rever com a medida do próprio rasterizador, porque aqui só comparei dois motores entre si: não sei qual deles está mais perto da cobertura exata. Um cálculo analítico da cobertura de um triângulo dá essa resposta e fica por fazer.

## Limitações do harness

- `cairo_ref` só suporta polígonos e preenchimento; não serve para gradientes, traços nem clip.
- `ref_render.js` precisa de Playwright com Chromium; os testes do CI não o usam (as imagens de referência estão no repositório).
- A comparação é pixel a pixel num tamanho fixo; não há comparação perceptual.
