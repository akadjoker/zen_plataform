# Plano: zen_vector (rasterizador vetorial com APIs Canvas 2D e Flash)

Estado: todas as fases concluídas (ver `FASE0.md`, `FASE1.md`, `FASE3.md`, `FASE4.md`, `FASE5-11.md`).

Objetivo: biblioteca que desenha gráficos vetoriais com qualidade ao nível do Canvas do browser sobre um `Framebuffer` do zen_platform, sem dependências de terceiros. Serve de base à ferramenta de animação estilo Flash.

## 1. Princípios

- Depende só do `Framebuffer` do zen_platform. O zen_platform não muda (regra "só entra o que os engines usam").
- Formato interno RGBA8 premultiplicado; conversão explícita na entrada e na saída.
- Coordenadas em float na API, rasterização em ponto fixo.
- Sem alocações escondidas: paths e buffers crescem por um alocador que o utilizador passa (ou um por omissão, explícito).
- Cada fase fecha com: testes verdes (também com ASan e UBSan), comparação com imagens de referência dentro da tolerância definida, e benchmark registado. Sem fechar uma fase não se começa a seguinte.

## 2. Fases

### Fase 0: Medição e infraestrutura de validação
- Correr o benchmark do draw2d na tua máquina (as cinco cenas) e registar os números como linha de base.
- Gerador de imagens de referência: cenas de teste descritas num formato simples, renderizadas pela referência escolhida (ver secção 3) e guardadas como PNG ou BMP.
- Ferramenta de comparação: diferença máxima por canal, número de pixels fora da tolerância e imagem de diferença para inspeção.
- Fecho: benchmark registado, harness a comparar uma imagem trivial (retângulo opaco) com 0 diferenças.

### Fase 1: Núcleo de pixel
- Conversão Framebuffer ↔ premultiplicado.
- Blend source-over que preserva o alpha do destino (a versão medida: 66,5 ms → 31,8 ms).
- Funções de span: preencher uma tira horizontal com cor sólida, com caminho rápido para opaco e para cobertura total.
- Fecho: testes exatos de blending (tabelas de casos, incluindo alpha 0, 255 e destino transparente).

### Fase 2: Geometria
- Matriz afim 2x3 (multiplicar, inverter, aplicar).
- Path: `move_to`, `line_to`, `quad_to`, `cubic_to`, `close`, com vários subpaths.
- Achatamento adaptativo de béziers depois da transformação, com tolerância em pixels (ponto de partida 0,25 px, a validar na Fase 3). Validado: 0,25 rejeitado, 0,05 adotado com dados em `FASE3.md`.
- Bounds e transformação de paths.
- Fecho: testes de tabela para matrizes e para o erro máximo do achatamento.

### Fase 3: Rasterizador
- Cobertura analítica por acumulação de área com sinal (técnica AGG / font-rs), em células esparsas por scanline.
- Fill rules nonzero e even-odd.
- Clipping ao framebuffer e a um retângulo.
- Fecho: polígonos, círculos, estrelas auto-intersetadas e béziers dentro da tolerância contra a referência; benchmark comparado com a linha de base da Fase 0.

### Fase 4: Pinturas
- Cor sólida.
- Gradiente linear e radial (o radial do Canvas é de dois círculos; o do Flash usa matriz e `focalPointRatio`).
- Modos de repetição: pad, reflect, repeat. Dithering para evitar banding.
- Padrão de bitmap com transformação, nearest e bilinear.
- Fecho: gradientes e bitmaps dentro da tolerância contra a referência.

### Fase 5: Stroker
- Conversão de path em contorno: joins miter, round, bevel com `miterLimit`; caps butt, round, square.
- Tracejado (`setLineDash`, `lineDashOffset`).
- Linhas finas abaixo de 1 px (cobertura proporcional à espessura).
- Fecho: bateria de strokes contra a referência, incluindo curvas apertadas, segmentos degenerados e espessuras de 0,1 a 50 px. É a fase com mais casos limite; não se encurta.

### Fase 6: Clipping, estado e camadas
- Pilha `save`/`restore`.
- Clip por path (com anti-aliasing).
- Camadas fora do ecrã para alpha de grupo e máscaras.
- Fecho: testes de clip encaixado e de camadas contra a referência.

### Fase 7: API Canvas 2D
- `fillRect`, `strokeRect`, `clearRect`, `beginPath`, `rect`, `roundRect`, `arc`, `arcTo`, `ellipse`, `quadraticCurveTo`, `bezierCurveTo`, `fill(rule)`, `stroke`, `clip`.
- `translate`, `rotate`, `scale`, `transform`, `setTransform`, `resetTransform`.
- `fillStyle`, `strokeStyle`, `lineWidth`, `lineCap`, `lineJoin`, `miterLimit`, `globalAlpha`.
- `globalCompositeOperation`: subconjunto a definir (ver secção 3).
- `drawImage`, `getImageData`, `putImageData`.
- Fora desta fase: texto, sombras, filtros.
- Fecho: cenas de teste equivalentes às do browser dentro da tolerância.

### Fase 8: API Flash e display list
- `Graphics` retido: grava comandos (`beginFill`, `beginGradientFill`, `beginBitmapFill`, `lineStyle`, `moveTo`, `lineTo`, `curveTo`, `cubicCurveTo`, `drawRect`, `drawCircle`, `drawEllipse`, `drawRoundRect`, `drawPath`, `drawTriangles`, `endFill`, `clear`) e traduz para o núcleo.
- Semântica Flash: even-odd por omissão, fecho automático do fill, `lineStyle` com `scaleMode` e `pixelHinting`.
- `DisplayObject`/`Sprite`: matriz, alpha, visível, filhos, ordem de desenho.
- `cacheAsBitmap`, `blendMode` (subconjunto), máscaras.
- Fecho: cenas equivalentes a exemplos do NME/OpenFL dentro da tolerância.

### Fase 9: Desempenho
- SIMD (SSE2 e NEON, com caminho escalar sempre disponível; WASM SIMD para a Web).
- Tiles para paralelizar.
- Dirty rectangles na display list.
- Fecho: metas de desempenho definidas a partir dos números da Fase 0 e da Fase 3; nenhuma regressão de qualidade (as imagens de referência continuam a passar).

### Fase 10: Texto
- Leitura de TrueType, contornos de glifos como paths (reutiliza o rasterizador), cache de glifos.
- `fillText`, `strokeText`, `measureText` no Canvas; `TextField` básico no Flash.

### Fase 11: Sombras e filtros
- Desfoque (três box blurs como aproximação de gaussiano, a validar contra a referência).
- `shadowBlur`, `shadowColor`, `shadowOffsetX/Y` no Canvas; `DropShadowFilter`, `GlowFilter`, `BlurFilter` no Flash.

Fora deste plano (pertencem à ferramenta de animação): timeline, MCP, export de vídeo, voz.

## 3. Precisa de mais investigação

- **Linguagem:** decidido na Fase 1: C, como o zen_platform. Razões em `FASE1.md`. Uma camada C++ fina por cima fica em aberto para as fases 7 e 8, se for útil.
- **Referência para as imagens:** Chrome headless (o Canvas real), Skia ou Cairo. A especificação do Canvas não define o anti-aliasing ao pixel, por isso nunca haverá igualdade exata com o Chrome; a tolerância tem de ser definida com dados na Fase 0, não à partida.
- **Testes públicos do Canvas:** os web-platform-tests têm testes de canvas 2D; precisamos de verificar quais se podem usar fora do browser e em que licença.
- **Threads:** decidido na Fase 9: sem threads nem tiles (ver `FASE5-11.md`).
- **Semântica exata do Flash:** `focalPointRatio`, `scaleMode` dos traços, `pixelHinting`, interpolação `linearRGB` dos gradientes. Fontes a confirmar: a referência de ActionScript 3 da Adobe (verificar se ainda está acessível) e o código do NME e do OpenFL.
- **Licenças:** se algum código do NME, OpenFL ou AGG for portado e não apenas estudado, verificar as licenças de cada um antes.
- **Subconjunto de `globalCompositeOperation` e `blendMode`:** definido na Fase 7 (15 operações) e na Fase 8 (9 modos).
- **Texto complexo:** shaping (árabe, ligaduras, etc.) normalmente exige HarfBuzz, que é uma dependência. Fica fora até haver decisão.
