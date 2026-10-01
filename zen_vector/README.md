# zen_vector

Rasterizador vetorial sobre o `Framebuffer` do zen_platform, com APIs ao estilo do Canvas 2D e do Flash. Está em construção: ver `docs/PLAN.md` para as fases e `docs/FASE0.md` para o estado e os números.

Ainda não há API de desenho. Existe o núcleo de pixel da Fase 1 (`include/zv_pixel.h`: blend source-over premultiplicado, spans, superfícies e conversão de e para o Framebuffer) e o harness de validação da Fase 0 (`tools/`, imagens de referência em `refs/`).
