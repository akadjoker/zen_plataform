#ifndef ZV_SCENE_H
#define ZV_SCENE_H

#include "platform.h"

#include <stddef.h>

/* A scene is plain text, one command per line, '#' at the start of a line is a
   comment. The same text is rendered by the reference (a browser canvas, through
   tools/ref_render.js) and by zen_vector, so the two can be compared.

   Supported now:
     size W H              canvas size in pixels; must come first
     fillStyle #rrggbb     opaque colour, default #000000
     fillRect X Y W H      integers, W and H may be negative (normalized)

   Any other command is an error: a scene never renders partially. */

/* Renders text into out (allocated here, transparent black before drawing).
   Release it with framebuffer_free. On failure returns false, fills err with a
   message that names the line, and leaves out untouched. */
bool zv_scene_run(const char *text, Framebuffer *out, char *err, size_t errcap);

#endif /* ZV_SCENE_H */
