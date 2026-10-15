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
  OpenGL is disabled. Uses an internal image pyramid for downscaling.

Which backend is active is logged at startup (`Use OpenGL:` in the
system info log).

## Zoom behavior

| Zoom level | Behavior |
| --- | --- |
| Below 100 % | Downscaled through a filtered image pyramid — smooth |
| 100–200 % | Smooth filtering applied |
| Above 200 % | **Smooth Rendering at High Zoom** decides: filtered (default, looks like an image viewer) or raw nearest-neighbor pixels |

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

## Tips for maximum image quality

- Author at 2–4x your intended output size; scale down on export.
- Print work: use a 300+ ppi document.
- The GPU canvas supports higher bit-depth display surfaces; the
  software canvas is 8-bit — on smooth gradients the software path can
  show banding the GPU path does not.
- `Image → Scale Image` offers Bicubic and Lanczos3 filters; Lanczos3
  gives the sharpest downscales.
