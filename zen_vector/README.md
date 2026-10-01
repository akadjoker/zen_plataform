# zen_vector

Rasterizador vetorial em C sobre o `Framebuffer` do zen_platform, com API Canvas 2D e API Flash (graphics retido e display list). Sem dependências além do zen_platform.

![demo](demo_canvas.png)

| Cabeçalho | O que dá |
|---|---|
| `zv_pixel.h` | pixel RGBA8 premultiplicado, blend exato, spans (SSE2/NEON), superfícies, conversão de e para o Framebuffer |
| `zv_geom.h` | matriz afim, paths, achatamento de curvas, bounds |
| `zv_raster.h` | rasterizador com anti-aliasing analítico, nonzero e even-odd, clipping |
| `zv_paint.h`, `zv_fill.h` | cor, gradientes linear e radial, padrões de bitmap, preenchimento com pintura |
| `zv_stroke.h` | joins, caps, tracejado |
| `zv_canvas.h` | Canvas 2D: estado, transformações, paths, clip, composição, imagens, texto, sombras, camadas |
| `zv_font.h` | TrueType para paths, cache de glifos |
| `zv_filter.h` | desfoque gaussiano aproximado |
| `zv_flash.h` | `Graphics`, `Sprite`, `Stage` com dirty rectangles |

Uso mínimo:

```c
ZvSurface surface;
zv_surface_init(&surface, NULL, 640, 400);
ZvCanvas c;
zv_canvas_init(&c, &surface, NULL);
zv_canvas_set_fill_color(&c, 0xFF3366CC);
zv_canvas_begin_path(&c);
zv_canvas_arc(&c, 320, 200, 100, 0, 6.2831853f, false);
zv_canvas_fill(&c, ZV_FILL_NONZERO);
zv_surface_store(&surface, &framebuffer); /* o Framebuffer de window_lock_pixels */
```

Compilar e testar: `cmake -S zen_vector -B build-vector && cmake --build build-vector && ctest --test-dir build-vector`. Plano em `docs/PLAN.md`, resultados por fase em `docs/FASE*.md`, cenas de referência em `scenes/` e `refs/` (geradas pelo Chromium com `tools/make_refs.sh`).
