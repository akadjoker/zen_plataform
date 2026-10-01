# zen_vector

Rasterizador vetorial sobre o `Framebuffer` do zen_platform, com APIs ao estilo do Canvas 2D e do Flash. Está em construção: ver `docs/PLAN.md` para as fases e `docs/FASE0.md` para o estado e os números.

Ainda não há API Canvas nem Flash. Existe o núcleo de pixel (`zv_pixel.h`), a geometria (`zv_geom.h`: matrizes, paths, achatamento) e o rasterizador com anti-aliasing analítico (`zv_raster.h`), além do harness de validação (`tools/`, imagens de referência em `refs/`). O interpretador de cenas (`tools/scene.c`) já desenha paths com o zen_vector.
