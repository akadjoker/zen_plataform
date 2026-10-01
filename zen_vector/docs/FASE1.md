# Fase 1: núcleo de pixel

Estado: concluída. Testes exatos a passar (73 verificações), também com ASan e UBSan, sem avisos com `-Wall -Wextra -Wpedantic`, e benchmark registado.

## Decisão: linguagem C

Escolhida C, no mesmo padrão do zen_platform (C11, só com o que o MSVC também aceita). Razões:

- é a linguagem do zen_platform, por isso a biblioteca compila inteira em Linux, Windows, Web e Android, e os engines em C++ chamam a API em C diretamente;
- os ciclos quentes (spans, cobertura, SIMD na Fase 9) ficam mais simples sem classes nem alocações escondidas, que é um princípio do plano;
- handles opacos dão uma ABI estável.

Se as camadas Canvas e Flash pedirem classes, acrescenta-se um cabeçalho C++ fino por cima, sem reescrever o núcleo.

## O que existe (`include/zv_pixel.h`, `src/pixel.c`)

| Peça | Função |
|---|---|
| `ZvPixel` | pixel RGBA8 premultiplicado, com o layout `0xAARRGGBB` do Framebuffer; um pixel é válido se cada canal de cor é menor ou igual ao alpha |
| `zv_div255` | `round(x / 255)` exato para `0 <= x <= 65025` |
| `zv_blend_over` | source-over de dois pixels premultiplicados, os quatro canais incluindo o alpha, dois canais por multiplicação; sobre um destino opaco o resultado continua opaco |
| `zv_scale` | multiplica os quatro canais por `k / 255`; é como uma cobertura se aplica a uma cor |
| `zv_premultiply`, `zv_unpremultiply` | conversão explícita de e para alpha direto; alpha 0 dá 0 |
| `zv_span_solid` | tira com uma cor; opaca é um preenchimento, transparente não toca em nada |
| `zv_span_solid_cover` | uma cobertura para a tira toda |
| `zv_span_cover` | uma cobertura por pixel, a scanline do rasterizador da Fase 3 |
| `ZvAllocator`, `ZvSurface` | memória sempre pedida pelo utilizador a um alocador; `NULL` significa o alocador por omissão, escrito por extenso (`zv_default_allocator`) |
| `zv_surface_load`, `zv_surface_store` | conversão entre Framebuffer e superfície do mesmo tamanho, com strides diferentes |

## Pré-condição

Todas as funções de blending assumem pixels premultiplicados válidos. Com entradas inválidas (canal maior que o alpha) um canal pode passar de 255 e dar a volta. Não há verificação em tempo de execução, para não custar nada no ciclo quente; os testes só usam entradas válidas.

## Como se sabe que é exato

A referência dos testes não usa a fórmula do código: faz a divisão inteira `(x + 127) / 255` canal a canal.

- `zv_div255` é comparada com a referência para todos os valores de 0 a 65025.
- `zv_premultiply` é comparada para todas as 65536 combinações de cor e alpha.
- O blend é testado para cada valor de canal do destino contra cada alpha da origem (256 x 256), em cada uma das quatro posições, com vizinhos aleatórios. Um transporte entre canais apareceria aqui.
- 4 milhões de pares de pixels válidos aleatórios, mais uma tabela de casos de borda (alpha 0, 1, 2, 127, 128, 129, 253, 254, 255 com cores 0, alpha e alpha/2), comparados com a referência e verificados como válidos à saída.
- Propriedades: destino opaco continua opaco para qualquer alpha da origem; destino transparente recebe a origem sem alteração; origem opaca substitui; origem transparente não altera.
- Ida e volta alpha direto, premultiplicado e alpha direto: exata para alpha 255 e, para os outros, um canal move-se no máximo `255 / (2 * alpha) + 0,5` (verificado para todas as combinações).
- Spans: tamanhos de 0 a 1001 (incluindo não múltiplos de 4 e 8), seis cores de origem, sete coberturas, cobertura por pixel aleatória, e bytes de guarda de cada lado a provar que nada é escrito fora da tira.

Limite do que isto prova: a ausência de transporte entre canais está provada pelo argumento aritmético (cada canal usa 16 bits e o máximo é `255 * 255 + 128 = 65153 < 65536`) e verificada por amostragem aleatória e pelos testes por posição, não por enumeração de todos os 2^64 pares de pixels.

Verificação dos próprios testes: com mutações no código (arredondamento errado, máscara errada, `div255` sem correção, atalho de cobertura errado, `unpremultiply` sem arredondar) os testes falham em todas. Uma sexta mutação (retirar o atalho "origem transparente não faz nada") passou, e é legítimo: com uma origem premultiplicada válida e alpha 0 todos os canais são 0, por isso o resultado é o mesmo com ou sem o atalho.

## Benchmark

Custo, por pixel, de misturar uma cor translúcida (alpha 0x80) sobre um destino opaco de 1280x720 (921 600 pixels), mínimo de 15 medidas. Neste contentor (Intel Xeon 2,1 GHz, GCC 13.3, `-O2`, uma thread, VM partilhada):

| Caso | mínimo (ms) | ns por pixel |
|---|---|---|
| versão ingénua, divisão por canal | 1,47 | 1,60 |
| `zv_blend_over` em ciclo | 0,47 | 0,51 |
| `zv_span_solid`, cor translúcida | 0,77 | 0,84 |
| `zv_span_solid`, cor opaca | 0,13 | 0,14 |
| `zv_span_cover`, cobertura por pixel (1/4 a 0, 1/4 a 255, 1/2 a 128) | 1,91 | 2,07 |
| `draw2d`, `draw_fill_rect` com `BLEND_ALPHA` (linha de base da Fase 0) | 1,28 | 1,39 |

Como ler:
- O número honesto para a Fase 3 é o do span (0,84 ns/pixel), não o do ciclo em linha (0,51). No benchmark a cor do ciclo é uma constante de compilação, e o compilador pré-calcula os multiplicadores; o span recebe a cor em tempo de execução, que é o caso real.
- Contra a versão ingénua o ciclo é 3,1 vezes mais rápido; o span é 1,9 vezes mais rápido; contra o `draw2d` o span é 1,7 vezes mais rápido. O `draw2d` só mistura canais de cor sobre um destino opaco, o span mistura os quatro canais, alpha incluído.
- Estas razões não são as do plano (66,5 ms para 31,8 ms, ou seja 2,1 vezes): vêm de outra medida, noutra máquina e outra cena. Não as consigo reproduzir sem a tua cena.
- `zv_span_cover` é o caso lento (2,07 ns/pixel) porque escala a cor e mistura por pixel. É o que a Fase 3 vai usar nas arestas, e é o principal alvo da Fase 9 (SIMD).
- Três execuções seguidas variam: `zv_blend_over` 0,51 a 0,56, `zv_span_solid` translúcido 0,75 a 0,83, `zv_span_cover` 1,91 a 2,62, `draw2d` 1,48 a 1,54 ns/pixel. Qualquer meta tem de ficar acima desse ruído.

Correr: `build-vector/bench_pixel` (ou `--csv`).
