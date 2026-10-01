# Fases 5 a 11: stroker, clipping e camadas, Canvas 2D, Flash, desempenho, texto, sombras

Estado: concluídas num só ciclo. 7 suites de testes (1247 verificações) a passar em Release e com ASan/UBSan; 32 cenas comparadas com o Chromium.

## Fase 5: stroker (`zv_stroke.h`, `src/stroke.c`)

- Um contorno fechado por subpath aberto (lado esquerdo para a frente, cap final, lado direito para trás, cap inicial) e dois por subpath fechado (as duas margens em sentidos opostos), preenchidos com nonzero. Joins miter (com `miterLimit`, razão `sqrt(2 / (1 + n0·n1))`), round (arco com a tolerância do achatamento) e bevel, só no lado exterior de cada curva; o lado interior passa pelo vértice e o nonzero remove a sobreposição, como o Skia.
- Caps butt, round e square; subpath de um ponto ou de pontos iguais dá um ponto com round e square e nada com butt; inversão de 180 graus tratada como join exterior.
- Tracejado com offset, padrão ímpar repetido, subpath fechado cujo tracejado atravessa o início junta o último traço ao primeiro.
- Linhas finas: geometria exata, a cobertura do rasterizador já é proporcional à espessura (a cena `stroke_caps` vai de 0,1 a 50 px).
- O traço é construído no espaço do utilizador e transformado depois: uma escala não uniforme dá traços não uniformes (`stroke_transform`, 0 pixels acima de 48).

## Fase 6: clipping, estado e camadas (`zv_canvas.h`)

- Pilha `save`/`restore` de 32 níveis; o clip é uma caixa mais uma máscara de cobertura opcional por pixel (um retângulo alinhado em pixels inteiros fica caixa); clips encaixados multiplicam-se.
- Camadas: `zv_canvas_begin_layer`/`end_layer` com alpha de grupo, operação de composição e máscara.

## Fase 7: Canvas 2D (`zv_canvas.h`, `src/canvas.c`)

`fillRect`, `strokeRect`, `clearRect`, `beginPath`, `moveTo`, `lineTo`, `quadraticCurveTo`, `bezierCurveTo`, `closePath`, `rect`, `roundRect`, `arc`, `arcTo`, `ellipse`, `fill(rule)`, `stroke`, `clip`, `isPointInPath`, `translate`/`rotate`/`scale`/`transform`/`setTransform`/`resetTransform`, estilos e linha, `globalAlpha`, `globalCompositeOperation` (source-over/in/out/atop, destination-over/in/out/atop, lighter, copy, xor, multiply, screen, darken, lighten), `imageSmoothingEnabled`, `drawImage` (9 argumentos, com vista do retângulo de origem), `getImageData`/`putImageData`, sombras, texto. Os pontos do path são transformados ao entrar, como no browser.

Operações que não são source-over passam por uma camada com a cor por pixel e a cobertura à parte, e o resultado é `lerp(dst, op(dst, src), cobertura)`, a semântica do Skia; as operações "sem limite" (source-in, source-out, destination-in, destination-atop, copy) limpam o que está fora da forma dentro do clip.

Caminho rápido: `fillRect` com transformação alinhada aos eixos, cor sólida, source-over, clip retangular e sem sombra calcula a cobertura separável sem rasterizador (testado contra o rasterizador: igual a 1).

## Fase 8: Flash (`zv_flash.h`, `src/flash.c`)

- `ZvGraphics` retido: `beginFill`, `beginGradientFill` (caixa de 1638,4 unidades com matriz, `focalPointRatio` como círculo interior de raio 0), `beginBitmapFill`, `lineStyle` (`pixelHinting`, `scaleMode` normal/none/horizontal/vertical, caps, joins, miter), `lineGradientStyle`, `moveTo`, `lineTo`, `curveTo`, `cubicCurveTo`, `drawRect`, `drawRoundRect`, `drawCircle`, `drawEllipse` (8 quadráticas, como o Flash), `drawPath`, `drawTriangles`, `endFill`, `clear`. Preenchimento even-odd e fecho automático; a linha vai por cima do preenchimento do mesmo bloco.
- `ZvSprite`: x, y, escala, rotação em graus ou matriz própria, alpha, visível, `blendMode` (normal, layer, multiply, screen, lighten, darken, add, erase, alpha), máscara por outro sprite, filhos ordenados, `cacheAsBitmap` (desenha uma vez para uma superfície própria e reutiliza enquanto a matriz do mundo não muda).
- `ZvStage` com dirty rectangles: redesenha só a união dos bounds antigos e novos dos sprites alterados (e dos descendentes de quem mudou); o teste move um sprite e verifica que o retângulo redesenhado é o esperado e que nada é redesenhado quando nada muda.

## Fase 9: desempenho

- SSE2 (e NEON em ARM) em `zv_span_solid` e `zv_span_cover`, com o ciclo escalar sempre presente para o resto. Medido (`bench_pixel`, ns por pixel): `zv_span_solid` translúcido 0,84 para 0,38; `zv_span_cover` 2,07 para 1,50.
- `bench_raster` (1280x720, mínimo em ms): fill_rect_alpha 54,7 para 39,2; fill_circle_alpha 15,6 para 12,7; full_screen_alpha 19,2 para 7,7; fill_rect_opaque 27,4 para 23,0 (ainda pelo rasterizador; o Canvas usa o caminho rápido, que não passa aqui).
- Dirty rectangles na display list (Fase 8).
- Threads e tiles: não. O zen_platform não tem threads e a regra é não acrescentar o que os engines não usam; na Web exigiriam SharedArrayBuffer. Fica documentado como decisão, não como falta.

## Fase 10: texto (`zv_font.h`, `src/font.c`)

Leitor de TrueType: `head`, `hhea`, `hmtx`, `maxp`, `cmap` (formatos 4 e 12), `loca`, `glyf` com glifos compostos, `kern` formato 0. Contornos como paths (quadráticas com pontos implícitos), cache de 256 glifos, UTF-8. `fillText`, `strokeText`, `measureText`, `textAlign`, `textBaseline`. A fonte de teste (DejaVu Sans, licença em `fonts/LICENSE`) vai no repositório para o CI; o browser carrega a mesma por `FontFace`. Sem hinting nem shaping: a cena `canvas_text` fica a 7,8 % dos pixels acima de 32 contra o Chromium, que faz hinting e posicionamento próprio dos glifos; a forma é a mesma (ver `demo_canvas.png`).

`TextField` do Flash: não feito como classe; o texto usa `zv_canvas_fill_text` com a mesma fonte, e a ferramenta de animação compõe por cima.

## Fase 11: sombras e filtros (`zv_filter.h`, `src/filter.c`)

Desfoque por três box blurs (larguras de Kovesi), em superfícies e em máscaras de cobertura. `shadowColor`, `shadowBlur` (sigma = blur / 2, medido: 0,5 é o fator que mais se aproxima do Chromium, 21 ‰ dos pixels acima de 8; 0,6 dá 27 ‰, 0,7 dá 80 ‰), `shadowOffsetX/Y` (deslocamento transformado pela matriz), em fills e strokes. `DropShadowFilter`, `GlowFilter` e `BlurFilter` do Flash: o blur e a sombra existem como funções; a ligação ao sprite fica para a ferramenta.

## Contra o Chromium (diferença máxima por canal e pixels acima de 32)

| Cena | máx | acima de 32 | nota |
|---|---|---|---|
| stroke_joins | 23 | 0 | |
| stroke_caps | 63 | 30 | linha de 0,1 px e pontas |
| stroke_dash | 111 | 23 | traços que atravessam cantos |
| stroke_curves | 107 | 137 | laço auto-intersetado com miter |
| stroke_degenerate | 94 | 4 | |
| stroke_transform | 22 | 0 | |
| canvas_arcs | 50 | 18 | |
| canvas_clip | 57 | 75 | arestas do clip rodado |
| canvas_composite | 255 | 175 | arestas das operações: o browser aplica algumas por camada completa |
| canvas_composite_in | 255 | 85 | idem |
| canvas_shadow | 61 | 0 | |
| canvas_image | 255 | 539 | amostragem bilinear nas bordas sem repetição |
| canvas_text | 223 | 2053 (7,8 %) | hinting do browser |
| canvas_transform | 17 | 0 | |

Correr tudo: `cmake -S zen_vector -B build-vector && cmake --build build-vector && ctest --test-dir build-vector`.
