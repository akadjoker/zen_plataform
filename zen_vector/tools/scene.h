#ifndef ZV_SCENE_H
#define ZV_SCENE_H

#include "platform.h"

#include <stddef.h>

/* A scene is plain text, one command per line, '#' at the start of a line is a
   comment. The same text is rendered by the reference (a browser canvas, through
   tools/ref_render.js) and by zen_vector through its Canvas API, so the two can
   be compared. Commands are the Canvas 2D ones with the same arguments:

     size W H                             must come first
     fillStyle / strokeStyle #rrggbb[aa]
     lineWidth N, lineCap butt|round|square, lineJoin miter|round|bevel, miterLimit N
     setLineDash A B ..., lineDashOffset N
     globalAlpha A, globalCompositeOperation NAME, imageSmoothingEnabled true|false
     shadowColor #rrggbb[aa], shadowBlur N, shadowOffsetX N, shadowOffsetY N
     translate X Y, rotate R, scale X Y, transform a b c d e f, setTransform ..., resetTransform
     save, restore
     fillRect / strokeRect / clearRect X Y W H
     beginPath, closePath, moveTo, lineTo, quadraticCurveTo, bezierCurveTo,
     rect X Y W H, roundRect X Y W H R, arc X Y R A0 A1 [true], arcTo X1 Y1 X2 Y2 R,
     ellipse X Y RX RY ROT A0 A1 [true]
     fill [nonzero|evenodd], stroke, clip [nonzero|evenodd]
     gradientLinear X0 Y0 X1 Y1, gradientRadial X0 Y0 R0 X1 Y1 R1, gradientStop T #c,
     fillGradient, strokeGradient
     patternImage W H, fillPattern REP, patternTransform a b c d e f
     drawImage SX SY SW SH DX DY DW DH          (the pattern image)
     font SIZE                                   (the test font, DejaVu Sans)
     textAlign start|end|left|right|center, textBaseline alphabetic|top|middle|bottom|hanging|ideographic
     fillText X Y TEXT..., strokeText X Y TEXT...

   Any other command is an error: a scene never renders partially. */

/* Renders text into out (allocated here, transparent black before drawing).
   Release it with framebuffer_free. On failure returns false, fills err with a
   message that names the line, and leaves out untouched. */
bool zv_scene_run(const char *text, Framebuffer *out, char *err, size_t errcap);

/* The font file used by the "font" command; NULL means the default,
   ZV_SCENE_FONT if defined at build time. */
void zv_scene_set_font_path(const char *path);

#endif /* ZV_SCENE_H */
