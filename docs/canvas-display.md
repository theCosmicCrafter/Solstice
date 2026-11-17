# Canvas display and zoom quality

How Solstice renders your artwork to the screen, what "pixelated when
zoomed" means, and the controls that change it.

## Canvas backends

Solstice draws the canvas with one of two backends:

- **GPU canvas** (default) — hardware-accelerated, supports filter modes
  and 10-bit display surfaces. Selected automatically when OpenGL
  probing succeeds at startup; `OpenGLRenderer` in `kritadisplayrc`
  controls it (`none` forces the software path).
- **Software (QPainter) canvas** — fallback when GPU probing fails or
  OpenGL is disabled. Renders into a prescaled buffer sized in physical
  device pixels, so output stays sharp on high-DPI displays; honors the
  Nearest filter mode for pixel-exact magnification and the
  **Smooth Rendering at High Zoom** toggle for filtered magnification.

Which backend is active is logged at startup (`Use OpenGL:` in the
system info log).

## Zoom behavior

| Zoom level | Behavior (GPU canvas, High Quality filtering) |
| --- | --- |
| Below 50 % | `filterPureLinear8` — a 6-tap weighted LOD filter in the display shader |
| 50–100 % | Trilinear minification, plus contrast-adaptive sharpening (CAS) that fades out approaching 100 % — a 1:1 view is pixel-true |
| Exactly 100 % | Unfiltered, pixel-for-pixel |
| Above 100 % | 4-tap bicubic B-spline upscale when **Smooth Rendering at High Zoom** is on; plain bilinear otherwise; raw nearest pixels when Nearest mode is selected |

On the software canvas the same zones apply, expressed in *physical*
pixels (logical zoom × display device-pixel ratio) — so a 100 % document
zoom on a 150 %-scaled display is treated as the >100 % filtered case,
matching what the monitor actually shows.

## Smooth Rendering at High Zoom

**View → Smooth Rendering at High Zoom** (checkable) or
**Settings → Configure Solstice → Display → Miscellaneous →
"Smooth rendering at high zoom"**. On by default.

- **On:** the canvas is filtered at any zoom — artwork stays smooth
  instead of breaking into visible pixel blocks.
- **Off:** raw pixels at >200 % zoom, useful for pixel-level inspection
  and pixel art.

This is a **display-only** filter: it does not change your image data or
exports — the document still has the resolution it has. For genuinely
resolution-independent strokes see the brush stroke layer
(`brush-stroke-layer.md`) and vector layers.

## View-resolution vector layers

Vector (shape) layers are re-rendered on screen at the resolution you are
viewing, so their edges stay crisp at any zoom — including 400 % and
beyond — instead of being magnified from a fixed document-resolution
raster. On by default and active on both canvas backends.

It works by recompositing the affected part of the image for the screen:
stacking order, opacity, groups and pass-through semantics are preserved.
Cases it deliberately does *not* touch — vectors keep their normal
(blurry) rendering there:

- vector layers overlapping layers that use blend modes other than
  Normal, inherit alpha, layer styles, adjustment/filter layers, or
  restricted channel flags;
- vector layers inside mixed raster+vector groups (the group is treated
  as raster content, still correctly composited);
- wrap-around mode and very large images shown at reduced resolution;
- clone layers of vector layers.

To disable it entirely, set `viewResVectorRendering=false` in the
Solstice settings file or run with the environment variable
`KRITA_DISABLE_VIEW_RES_VECTORS` set (e.g. for debugging). Display-only —
documents, exports and thumbnails are unaffected.

## Tips for maximum image quality

- Author at 2–4x your intended output size; scale down on export.
- Print work: use a 300+ ppi document.
- The GPU canvas supports higher bit-depth display surfaces; the
  software canvas is 8-bit — on smooth gradients the software path can
  show banding the GPU path does not.
- `Image → Scale Image` offers Bicubic and Lanczos3 filters; Lanczos3
  gives the sharpest downscales.
