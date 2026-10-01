#!/bin/sh
# Regenerates every reference image in refs/ from scenes/ with the browser.
set -e
here="$(cd "$(dirname "$0")/.." && pwd)"
args=""
for scene in "$here"/scenes/*.scene; do
    name="$(basename "$scene" .scene)"
    args="$args $scene $here/refs/$name.bmp"
done
ZV_SCENE_FONT="$here/fonts/DejaVuSans.ttf" node "$here/tools/ref_render.js" $args
