# Vector-quality megaplan — agent reference

Status: approved (2026-10-18). Roadmap for making Solstice deliver
"vector quality" — artwork that stays crisp at every zoom level and at
every output size — across the display pipeline, authoring workflows,
document output, and AI/ML output.

Decisions (user, 2026-10-18): full scope — display quality, true-vector
workflows, stroke-recorded "vector brush" layers, and quality defaults;
high-zoom smoothing is a **configurable** option (default ON), not a
forced behavior; output/defaults **and** AI/ML image quality are in
scope. This document is the canonical plan.

## What "grainy / pixelated" actually is — verified diagnosis

Five distinct problems hide inside "looks grainy when zooming". Each has
a different fix, so conflating them produces bad plans. Verified in the
source:

1. **Zoom > 200 % shows raw pixels by design.**
   `libs/ui/canvas/kis_prescaled_projection.cpp` (~L395-405): the
   QPainter canvas applies `QPainter::SmoothPixmapTransform` only below
   100 % zoom and between 100–200 %. Above 200 % it draws unfiltered —
   nearest-neighbor hard pixels. This is intentional raster behavior
   (pixel artists rely on it), not a bug — but it is exactly the
   "pixelated" users see.
2. **The canvas backend matters.** `KisCanvas2::createCanvas()`
   (`libs/ui/canvas/kis_canvas2.cpp:369,777`) picks `KisQPainterCanvas`
   or `KisOpenGLCanvas2` from `KisConfig::useOpenGL()`
   (`OpenGLRenderer` defaults to `"auto"`). The GL canvas has four
   filter modes (`libs/ui/opengl/kis_opengl.h:33-37`): Nearest /
   Bilinear / Trilinear / **HighQualityFiltering** (mipmaps + custom
   shader, default mode 3). If GL probing fails on a machine, it falls
   back to the weaker QPainter path silently.
3. **True vector layers already exist and stay crisp.** `KisShapeLayer`
   (`libs/ui/flake/kis_shape_layer.cc`) paints flake shapes into the
   canvas projection, which re-rasterizes at canvas resolution — vector
   content should remain sharp at any zoom. Confirm, do not rebuild.
4. **Resolution-independent raster already exists.**
   `KisBrushStrokeLayer` (`libs/ui/KisBrushStrokeLayer.{h,cpp}`,
   `KisBrushStrokeLayerIO.cpp`) records every stroke (post-smoothing
   `KisPaintInformation` jobs + fixed random seeds + presets) and
   re-renders them at the new resolution on image scale / transform /
   move — Clip Studio-style "vector brush" semantics on a raster layer.
   `KisBrushStrokeLayerTest` (19 tests) + `KisBrushStrokeReplayTest`
   confirm redrawn strokes are ~5–6x sharper (translucent-pixel ratio)
   than pixel-scaled art at 4x. See `brush-stroke-layer-plan.md`.
5. **HQ scaling filters exist.** `KisFilterStrategyRegistry`
   (`libs/image/kis_filter_strategy.cc:210-214`) already ships Bicubic
   and Lanczos3 for image/layer scaling; the defaults and export paths
   may not use the best one.

"Grain" can also come from: 8-bit banding without dithering (the GL
canvas supports 10-bit surfaces; the QPainter path is 8-bit), low
document resolution/DPI defaults, low-res brush tips, and AI-generated
imagery produced below print resolution.

## What already exists to build on

| Capability | Location | State |
| --- | --- | --- |
| GL canvas w/ HQ filter modes | `libs/ui/opengl/` | Present, default mode 3 |
| Vector (shape) layers | `libs/ui/flake/kis_shape_layer.cc` | Present — verify crispness |
| Vector tools | `plugins/tools/{defaulttool,karbonplugins,svgtexttool,tool_polyline,tool_polygon}` | Present — QA needed |
| Stroke-recorded layers | `libs/ui/KisBrushStrokeLayer.*` | Implemented + tested (stages through 2c + transform/move) |
| SVG / PDF impex | `plugins/impex/{svg,pdf}` | Present — audit round-trip |
| Bicubic / Lanczos3 | `libs/image/kis_filter_strategy.cc` | Present — audit defaults |
| Vulkan compute engine | `libs/gpu/` | Brush/paint compositing only (not display) |
| AI output | `plugins/python/ai_diffusion/`, `plugins/visionml/` | Vendored + native — audit res/edge defaults |

## Phase V1 — Display quality: "smooth zoom" everywhere

Smallest phase, highest perceived impact. The >200 % hard-pixel cutoff
is the literal complaint.

1. **Canvas audit on target hardware.** Confirm which canvas is active
   (KisUsageLogger already logs `Use OpenGL: ...`); document the
   QPainter-fallback conditions in user docs so support can tell the two
   apart.
2. **New option: smooth rendering at high zoom** (default ON; user
   decision — configurable, not forced).
   - `KisConfig::smoothHighZoom()` / `setSmoothHighZoom()` in
     `libs/ui/kis_config.{h,cc}`, persisted in kritarc.
   - `libs/ui/canvas/kis_prescaled_projection.cpp`: extend the
     `SCALE_MORE_OR_EQUAL_TO(scale,1)` branch so `SmoothPixmapTransform`
     also applies above 200 % when enabled (today only the 100–200 %
     sub-branch gets it).
   - `libs/ui/opengl/kis_opengl_canvas2.cpp` +
     `KisOpenGLCanvasRenderer.cpp`: audit the magnification sampler;
     switch to LINEAR magnification under the toggle. Shader-level
     change — verify on real GPU *and* the QPainter fallback.
   - UI surface: checkbox on the Display settings page
     (`libs/ui/dialogs/kis_dlg_preferences.cc`, near
     `grpOpenGL`/`cmbFilterMode` ~L2087) **and** a View-menu toggle so
     painters can flip it mid-session without opening Preferences.
   - Naming: surface it as "Smooth rendering at high zoom" — be honest
     in docs that it filters the display; it does not add detail.
3. **Below-100 % quality audit.** Check `kis_image_pyramid.cpp`
   filtering (box-filter mip levels). If grain is visible at 25–75 %
   zoom, add a deferred "HQ settle" pass that re-scales the visible
   region with a better kernel after zoom stops (debounced; interactive
   speed first, quality second).
4. **Banding/"grain" from 8-bit.** Document that QPainter canvas is
   8-bit while the GL canvas supports 10-bit + dither. Optional stretch:
   ordered-dither toggle on the QPainter path.

## Phase V2 — True-vector workflow hardening

Most of this already exists; the work is verify + productize + one new
feature.

1. **Vector-layer crispness verification.** Create a vector layer, draw
   shapes/text, zoom through {25…800 %}; confirm re-rasterization at
   canvas resolution. Add regression coverage; fix only if doc-res
   rasterization is found.
2. **Discoverability.** Layer-type badge/icon in the Layers docker so
   paint / vector / stroke layers are visually distinct at a glance;
   ensure "New Vector Layer" is one click away.
3. **Vector tool QA pass.** Exercise `defaulttool` shape editing,
   `svgtexttool`, `karbonplugins` path tools on this fork — no known
   regressions, but unverified since the Solstice divergence.
4. **SVG/PDF round-trip audit.** `plugins/impex/{svg,pdf}`: vector
   layers must export as real vectors, not flattened raster. Document
   the exact round-trip fidelity (gradients, text, masks).
5. **New feature — Trace to Vector.** Raster/alpha → `KoPathShape`(s)
   on a new vector layer (potrace-style outline tracing). Initial
   scope: monochrome/alpha line art — potrace-class tracing is
   monochrome; multi-color tracing is a later stretch. Gives raster
   artists a real path to vector output. Implementation home: new tool
   or Layer-menu action; evaluate embedding a potrace-compatible
   implementation vs writing a Qt/alpha-edge tracer.
6. **Stretch:** vector boolean ops and per-shape erase via flake.

## Phase V3 — Brush Stroke Layer → the "vector brush" tier

The engine is built and tested; the work is productizing it.

1. **Audit current surface.** Creation entry point in
   `libs/ui/kis_layer_manager.cc`; `.kra` persistence in
   `KisBrushStrokeLayerIO.cpp`; fallback rules after non-stroke edits
   (pixel+recorded mix handled; mass edits fall back to pixel scaling
   with the "drawn again, except about N%" notice).
2. **Productize.**
   - Layers docker badge/icon for stroke layers.
   - **Resolution-independent export:** on Scale Image / export at a
     different resolution, stroke layers re-render at the target
     resolution instead of pixel-scaling — the mechanism already exists
     through `transform()`; wire it into export paths.
   - Optional setting: "default new lineart layer = stroke layer" for
     ink-first workflows.
   - User doc (`docs/brush-stroke-layer.md` if absent): what it is,
     when redraws happen, honest limits (fills/filters/large edits fall
     back to pixel scaling; only recorded strokes re-render).
3. **Scope decision with user:** per-stroke select/move/delete and
   stroke-level erase (CSP-style erase-to-node). Stage 3+ of
   `brush-stroke-layer-plan.md` — significant work, not assumed in
   scope.
4. **Determinism regression check.** Replay relies on recorded jobs +
   fixed seeds through `FreehandStrokeStrategy`; confirm GPU-brush
   output still matches replay (was verified at implementation — add to
   the regression matrix, do not re-verify by hand every release).

## Phase V4 — Quality defaults & output pipeline

1. **New-document defaults audit.** `wdgnewimage` + templates: DPI
   default, size presets, bit depth. Proposal: keep 8-bit default
   (memory), ensure print-oriented presets default to 300 DPI;
   document "draw at 2–4x final size" guidance.
2. **Scale Image default filter.** Registry default is Bicubic
   (`kis_filter_strategy.cc:264`); evaluate Lanczos3 as the downscale
   default — test, then decide. Record the decision here.
3. **Transform tool default filter.** Audit `KisTransformWorker`'s
   default filter strategy; align with V4.2.
4. **Export quality.** Audit PNG/JXL/HEIF quality defaults in
   `plugins/impex/`; evaluate a **supersampled export** option (render
   at 2x, Lanczos downscale) — cheap, large visible gain on textured
   or noisy art.
5. **Color path.** Proofing + display color conversion already exist;
   documentation only.

## Phase V5 — AI/ML output quality

1. **AI Diffusion.** Audit vendored generation/upscale resolution
   defaults; document a hires-fix/upscale workflow through the existing
   ComfyUI wiring (127.0.0.1:8189); ensure output lands at print-viable
   resolution.
2. **Smart Matting.** Evaluate making the HR-matting variant the
   default model (the model selector shipped in `8cdeebdc`); audit
   default feather/refine values for clean mask edges.
3. **Stretch / research only:** native VisionML upscale pass (a
   Real-ESRGAN-class model through `plugins/visionml` infra). Listed
   for evaluation, not committed scope.

## Phase V6 — Validation and acceptance

- **Unit:** brush-stroke-layer tests already quantify sharpness
  (translucent-pixel ratio at 4x) — keep as the regression baseline;
  add coverage for the smooth-high-zoom render flag where testable.
- **E2E:** extend `tests/e2e` if the bridge can drive zoom/canvas state
  (kritamcp may need a `set_zoom` / canvas-info action — small bridge
  addition if missing). Pixel-readback assertions exist to reuse.
- **Manual matrix:** zoom {25, 50, 100, 200, 400, 800 %} × canvas
  {QPainter, GL} × content {paint layer, vector layer, stroke layer,
  AI-generated image}.
- **Acceptance:** >200 % smooth-mode zoom shows a filtered (non-blocky)
  image on both canvas backends; vector layers crisp at all zooms;
  stroke layers re-render sharp after 4x scale; export defaults at max
  quality; user + agent docs updated; wiki log entry appended.

## File map (verified)

| Component | Path |
| --- | --- |
| Zoom filter decision | `libs/ui/canvas/kis_prescaled_projection.cpp` (~L395-405) |
| QPainter canvas | `libs/ui/canvas/kis_qpainter_canvas.cpp` |
| GL canvas / renderer | `libs/ui/opengl/kis_opengl_canvas2.cpp`, `KisOpenGLCanvasRenderer.cpp`, `kis_opengl.h:33-37` |
| Canvas selection | `libs/ui/canvas/kis_canvas2.cpp` (`createCanvas`, ~L369/777) |
| Settings | `libs/ui/kis_config.{h,cc}` (`useOpenGL`, `openGLFilteringMode`) |
| Display prefs UI | `libs/ui/dialogs/kis_dlg_preferences.cc` (~L2087) |
| Vector layer | `libs/ui/flake/kis_shape_layer.cc` |
| Stroke layer | `libs/ui/KisBrushStrokeLayer.{h,cpp}`, `KisBrushStrokeLayerIO.cpp`, `libs/ui/kis_layer_manager.cc` |
| Scale filters | `libs/image/kis_filter_strategy.{h,cc}` |
| Vector tools | `plugins/tools/{defaulttool,karbonplugins,svgtexttool,tool_polyline,tool_polygon}` |
| Impex | `plugins/impex/{svg,pdf}` |
| AI quality | `plugins/python/ai_diffusion/`, `plugins/visionml/` |
| Prior art / context | `docs/agent/brush-stroke-layer-plan.md`, `docs/agent/gpu-engine.md` |

## Risks

- **Smooth zoom masks true pixel data** — artists may be surprised on
  export. Mitigate: quick View-menu toggle + docs explicitly saying the
  filter is display-only.
- **GL magnify-filter change touches shaders** — needs real-GPU
  verification plus parity with the QPainter fallback path.
- **Trace-to-Vector expectations** — potrace-class tracing is
  monochrome; set scope/docs honestly (line art first, color later).
- **Stroke-layer limits are real** — fills/filters/large edits fall
  back to pixel scaling; document, don't overclaim "vector".
- **Stroke-layer + selection/filter edits** degrade gracefully already;
  new features must not regress the mismatch-detection notice.
