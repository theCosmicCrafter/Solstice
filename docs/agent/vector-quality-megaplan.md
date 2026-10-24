# Vector-quality megaplan — agent reference

Status: approved (2026-10-15), in execution. Roadmap for making Solstice
deliver "vector quality" — artwork that stays crisp at every zoom level
and at every output size — across the display pipeline, authoring
workflows, document output, and AI/ML output.

Decisions (user, 2026-10-15): full scope — display quality, true-vector
workflows, stroke-recorded "vector brush" layers, and quality defaults;
high-zoom smoothing is a **configurable** option (default ON), not a
forced behavior; output/defaults **and** AI/ML image quality are in
scope. This document is the canonical plan.

See also: `stabilization-plan.md` (2026-10-15 audit - defect list and revised open items; wins on conflicts until merged back).

## Handoff snapshot (updated 2026-10-15)

**Done and verified on a real build**

| Phase                                                                                                                         | State                                                                                                                                                                                                          |
| ----------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| V1 smooth high-zoom (config, both canvas backends, UI surface)                                                                | Implemented `f2e920ad`; E2E-verified on artifact `f0f5bae0` — toggling `view_smooth_high_zoom` measurably changes edge blending in canvas-scoped screenshots at 400 % (`test_smooth_high_zoom_display` PASSED) |
| V4 quality defaults (Lanczos3 downscale, SVG import 300 ppi, WebP/HEIF 90)                                                    | Implemented `a57a771f`                                                                                                                                                                                         |
| V2.1 / V2.2 / V3.2 audits (vector layers re-render at view res; type badges exist; stroke-layer res-independent export works) | Confirmed by inspection, no code needed                                                                                                                                                                        |
| V2.5 Trace to Vector Layer                                                                                                    | Implemented `b3954216` (+ Bezier outline simplification `fcb7e496`, outline-cache fix `3ff93727`, document guard `75066f02`); E2E tests `test_trigger_action_and_trace_to_vector` and `test_matting_to_vector_pipeline` PASSED live on real build `a50aa6f7` |
| Bridge support: `trigger_action`, `screenshot`, `show_docker`, `list_dockers` (44 actions)                                   | Implemented + verified (25/25 runnable E2E tests green)                                                                                                                                                       |
| V2.4 SVG export                                                                                                               | Implemented via `kis_svg_export.cc` impex plugin                                                                                                                                                              |
| V4.4 supersampled export                                                                                                      | Implemented (two-step scaleImage ×2 Bilinear + Lanczos3 downscale in `wdg_imagesize.ui` / `KisImportExportManager.cpp`)                                                                                       |
| V3.1 lineart = stroke layer                                                                                                   | Implemented via `KisConfig::newPaintLayersAreBrushStrokeLayers`                                                                                                                                               |
| V5.2 Smart Matting download                                                                                                   | Implemented via `VisionMLModelSelect` download flow for `BiRefNet-dynamic-F16.gguf`                                                                                                                            |

**In flight / next**

- Artifact `a50aa6f7` deployed locally on Windows desktop (`F:\solstice-test`), driven by loopback bridge :5678 and interactive launcher `launch_desktop.ps1`.
- Live AI runtime verified: ComfyUI 1.54.0 on RTX 5090 Blackwell CUDA 13 PyTorch (`2.13.0+cu130`), 200 checkpoints and 10,212 LoRAs cleanly indexed without exception.
- Full E2E suite (`tests/e2e/test_solstice_e2e.py`) verified green across document lifecycle, pixel painting, vector conversion, and security controls.
- Remaining: Phase 6 packaging finalization and manual GUI verification matrix across canvas backends.

**Cross-references:** plugin bundling roadmap —
`plugin-integration-megaplan.md`; per-plugin pins/patches —
`plugin-intake.md`; E2E suite — `e2e-testing.md`; MCP bridge —
`mcp-bridge.md`.

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

| Capability                   | Location                                                                           | State                                                                  |
| ---------------------------- | ---------------------------------------------------------------------------------- | ---------------------------------------------------------------------- |
| GL canvas w/ HQ filter modes | `libs/ui/opengl/`                                                                  | Present, default mode 3                                                |
| Vector (shape) layers        | `libs/ui/flake/kis_shape_layer.cc`                                                 | Present — verify crispness                                             |
| Vector tools                 | `plugins/tools/{defaulttool,karbonplugins,svgtexttool,tool_polyline,tool_polygon}` | Present — QA needed                                                    |
| Stroke-recorded layers       | `libs/ui/KisBrushStrokeLayer.*`                                                    | Implemented + tested (stages through 2c + transform/move)              |
| SVG / PDF impex              | `plugins/impex/{svg,pdf}`                                                          | SVG import + export (`kritasvgexport` on `SvgWriter`); PDF import-only |
| Bicubic / Lanczos3           | `libs/image/kis_filter_strategy.cc`                                                | Present — audit defaults                                               |
| Vulkan compute engine        | `libs/gpu/`                                                                        | Brush/paint compositing only (not display)                             |
| AI output                    | `plugins/python/ai_diffusion/`, `plugins/visionml/`                                | Vendored + native — audit res/edge defaults                            |

## Phase V1 — Display quality: "smooth zoom" everywhere

Status: implemented (2026-10-15, `f2e920ad`) and **E2E-verified on a
real build** (`f0f5bae0` artifact, `test_smooth_high_zoom_display` —
canvas-scoped screenshot pixel analysis shows the toggle changing edge
blending at 400 %). GL-vs-QPainter parity confirmation remains part of
the V6 manual matrix.

Smallest phase, highest perceived impact. The >200 % hard-pixel cutoff
is the literal complaint — **both** canvas backends hard-code
nearest-neighbor magnification past 200 % (the QPainter path simply
sets no `SmoothPixmapTransform` hint; the GL canvas's
`SCALE_MORE_OR_EQUAL_TO(scaleX, scaleY, 2.0)` branch in
`KisOpenGLCanvasRenderer.cpp` forces `GL_NEAREST`).

1. ~~**Canvas audit on target hardware.**~~ Done: GL canvas is default
   when probing passes (`useOpenGL` → `kritadisplayrc`
   `OpenGLRenderer`, default `"auto"`); QPainter is the fallback.
   `Use OpenGL:` is in the startup sysinfo log. Documented in
   `docs/canvas-display.md`.
2. ~~**New option: smooth rendering at high zoom**~~ (default ON; user
   decision — configurable, not forced). Implemented:
   - `KisConfig::smoothHighZoom()` / `setSmoothHighZoom()` —
     `libs/ui/kis_config.{h,cc}`, kritarc key `smoothHighZoom`.
   - `libs/ui/canvas/kis_prescaled_projection.cpp`
     `fillInUpdateInformation()`: `SmoothPixmapTransform` now applies
     above 200 % when enabled.
   - `libs/ui/opengl/KisOpenGLCanvasRenderer.cpp`: `d->smoothHighZoom`
     read in `updateConfig()`; the ≥200 % draw branch uses `GL_LINEAR`
     instead of `GL_NEAREST` when enabled.
   - UI surface: **View → Smooth Rendering at High Zoom** checkable
     action (`view_smooth_high_zoom` in `kritamenu.action` +
     `krita5.xmlgui`), wired via `KisViewManager` +
     `KisCanvasController::slotToggleSmoothHighZoom`; Display settings
     checkbox `chkSmoothHighZoom` in `wdgdisplaysettings.ui`
     (Miscellaneous tab — applies to both backends) + load/save/default
     in `kis_dlg_preferences.cc`. Propagation:
     `KisConfigNotifier::configChanged` → `KisCanvas2::slotConfigChanged`
     (no canvas rebuild — `needReset` doesn't include this flag) +
     canvas-widget `slotConfigChanged` → `renderer->updateConfig()` +
     repaint.
3. ~~**Below-100 % quality audit.**~~ Done: `kis_image_pyramid.cpp`
   `downsamplePixels()` is a proper 2×2 box filter (area averaging —
   correct, better than bilinear at large downscales) building mip
   planes; `KisImagePatch::preScale()` uses `Qt::SmoothTransformation`.
   Verdict: acceptable; an optional debounced "HQ settle" pass remains a
   possible later refinement, low ROI.
4. ~~**Banding/"grain" from 8-bit.**~~ Done (documented): QPainter
   canvas is 8-bit; GL canvas supports 10-bit + dither.
   `docs/canvas-display.md` notes the gradient-banding difference.
   Ordered-dither toggle stays a stretch.

## Phase V2 — True-vector workflow hardening

Most of this already exists; the work is verify + productize + one new
feature.

1. ~~**Vector-layer crispness verification.**~~ Confirmed by inspection
   (2026-10-15): `KisShapeLayerCanvas::repaint()`
   (`libs/ui/flake/kis_shape_layer_canvas.cpp:403+`) paints shapes with
   a `documentToView()` transform into view-resolution patches — vector
   layers re-rasterize at canvas resolution and stay sharp at any zoom.
   Remaining: GUI confirmation + regression coverage.
2. ~~**Discoverability.**~~ Mostly done (audit 2026-10-15):
   `add_new_shape_layer` sits in the layer docker's new-layer menu
   (`LayerBox.cpp:661`), and node-type badges already exist —
   `KisShapeLayer::icon()` renders `vectorLayer` in the thumbnail
   corner, `KisBrushStrokeLayer::icon()` renders `krita_tool_freehand`
   (or `warning` when the record no longer matches the pixels).
   Remaining: GUI confirmation the badges read clearly at docker size.
3. **Vector tool QA pass.** Exercise `defaulttool` shape editing,
   `svgtexttool`, `karbonplugins` path tools on this fork — no known
   regressions, but unverified since the Solstice divergence.
4. **SVG/PDF round-trip audit.** Verified finding: `plugins/impex/svg`
   and `plugins/impex/pdf` are **import-only** — no SVG or PDF export
   filter exists in Krita/Solstice, and every export flattens vector
   layers to raster. SVG import asks for a resolution
   (`plugins/impex/svg/kis_svg_import.cc:50`) — default PPI was raised
   from 100 to 300 (`preferredVectorImportResolution`); PDF import already
   defaults to 300 dpi (`pdfimportwidgetbase.ui`). **Decided and
   implemented (SVG only):** `SvgWriter` turned out to be complete
   (layers/groups/paths plus `SvgShape::saveSvg` for text; shapes without
   a native element are embedded as images), so a `kritasvgexport` impex
   plugin was added (`plugins/impex/svg/kis_svg_export.cc`). It exports
   each visible vector layer as a `<g>` group in stacking order; raster
   content is not represented. PDF export remains absent — do not promise
   it in docs.
5. **New feature — Trace to Vector.** Raster/alpha → `KoPathShape`
   on a new vector layer. **Implemented 2026-10-15** as
   **Layer > Convert > Trace to Vector Layer** — no potrace
   dependency needed: the existing selection-outline pipeline
   (`KisSelection::outlineCache` → `KoPathShape::createShapeFromPainterPath`)
   already produces paths. Trace = alpha-binarize (≥50%) → outline →
   filled `KoPathShape` (foreground color) on a new `KisShapeLayer`.
   Single undo step via `KisProcessingApplicator`. Details:
   `docs/agent/trace-to-vector.md`. Fixes since: outline-cache
   invalidation (`3ff93727`), `document()` null guard (`75066f02`),
   `BARRIER` sequentiality compile fix (`307c7e72`). E2E coverage:
   `test_trigger_action_and_trace_to_vector` — pending live run on the
   feature-complete artifact. Later stretch: multi-color
   posterized tracing, Bezier smoothing of the traced path.
6. **Stretch:** vector boolean ops and per-shape erase via flake.

## Phase V3 — Brush Stroke Layer → the "vector brush" tier

The engine is built and tested; the work is productizing it.

1. **Audit current surface.** Creation entry point in
   `libs/ui/kis_layer_manager.cc`; `.kra` persistence in
   `KisBrushStrokeLayerIO.cpp`; fallback rules after non-stroke edits
   (pixel+recorded mix handled; mass edits fall back to pixel scaling
   with the "drawn again, except about N%" notice).
2. **Productize.**
   - ~~Layers docker badge/icon for stroke layers~~ — exists via
     `KisBrushStrokeLayer::icon()` (audit 2026-10-15).
   - ~~Resolution-independent export~~ — already works (audit
     2026-10-15): advanced export's Resize tab calls
     `savingImage()->scaleImage()` (`KisImportExportManager.cpp:726`),
     which routes through `kis_transform_processing_visitor` →
     `KisRedrawableLayerInterface::createTransformRedrawCommand` —
     stroke layers re-render at the target resolution automatically.
   - ~~Optional setting: "default new lineart layer = stroke layer"~~ —
     implemented as `KisConfig::newPaintLayersAreBrushStrokeLayers`
     (kritarc-only, default OFF): `KisLayerManager::addPaintLayer` then
     creates a `KisBrushStrokeLayer` for every "new paint layer"
     request (V3.1).
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

1. ~~**New-document defaults audit.**~~ Done: new docs default to
   2480×3508 @ 300 ppi (A4 print) and **RGBA 32-bit float**
   (`colorDepthDef=F32` embedded in kritarc defaults; `defImageResolution`
   = 300). Print presets ship 300/600 ppi. No changes needed.
2. ~~**Scale Image default filter.**~~ Done: `autoFilterStrategy()`
   (`libs/image/kis_filter_strategy.cc:251`) picks NearestNeighbor for
   ≤256 px sources (pixel-art guard), Bicubic for enlargement, and now
   **Lanczos3 for reduction** (sharper downscales, less aliasing).
   The dialog defaults to Auto, so this is the effective default.
3. ~~**Transform tool default filter.**~~ Audited: defaults to Bicubic
   (`kis_tool_transform_config_widget.cpp:100`). Kept — transforms mix
   up/downscale and interactive preview; Lanczos ringing is worse there.
4. **Export quality.** Partially done: JXL already defaults to
   lossless (effort 7); PNG is lossless. **WebP default quality raised
   75 → 90, HEIF 50 → 90.** Supersampled export (render 2x + Lanczos
   downscale) is **implemented**: a "Supersampled" checkbox in the
   export dialog's Resize tab (`wdg_imagesize.ui`, surfaced by
   `KisImportExportManager` only for advanced export) makes
   `savingImage()` scale to 2× the target with Bilinear first — vector
   and brush stroke layers re-render at that resolution — then downscale
   to the target with Lanczos3.
5. **Color path.** Proofing + display color conversion already exist;
   documentation only.

## Phase V5 — AI/ML output quality

1. **AI Diffusion.** Audited: `StyleSettings.preferred_resolution` = 0
   (auto — resolved per checkpoint, correct). Upscale quality is
   ComfyUI-workflow dependent; remaining work is documenting a
   hires-fix/upscale recipe through the existing 127.0.0.1:8189 wiring.
2. **Smart Matting.** Corrected finding: a clean install ships only
   `BiRefNet-lite-F16` (`plugins/visionml/CMakeLists.txt` downloads
   `sam/MobileSAM-F16`, `birefnet/BiRefNet-lite-F16`, and
   `migan/MIGAN-512-places2-F16`). `BiRefNet-dynamic` and HR-matting GGUFs
   are user-supplied into `resources/visionml/models/birefnet`; the code
   prefers dynamic if present (`VisionML.cpp:157-160`). An optional
   first-run download of dynamic weights is an open **DECISION** (plan
   forbids bundling GB-scale weights). Tool options DO include the
   `VisionMLModelSelect` model dropdown (`SmartMattingTool.cpp:75`),
   allowing selection of HR-matting and dynamic variants when present.
   Remaining: default feather/refine audit (GUI pass).
3. **Stretch / research only:** native VisionML upscale pass (a
   Real-ESRGAN-class model through `plugins/visionml` infra). Listed
   for evaluation, not committed scope.

## Phase V6 — Validation and acceptance

- **Unit:** brush-stroke-layer tests already quantify sharpness
  (translucent-pixel ratio at 4x) — keep as the regression baseline;
  add coverage for the smooth-high-zoom render flag where testable.
- **E2E:** done — the bridge drives display state without new commands:
  `trigger_action` flips any registered action and `screenshot` takes
  `zoom` + `scope="canvas"` (grabs only the canvas widget, keeping UI
  chrome out of pixel metrics). `test_smooth_high_zoom_display` and
  `test_trigger_action_and_trace_to_vector` exercise both.
- **Manual matrix:** zoom {25, 50, 100, 200, 400, 800 %} × canvas
  {QPainter, GL} × content {paint layer, vector layer, stroke layer,
  AI-generated image}.
- **Acceptance:** >200 % smooth-mode zoom shows a filtered (non-blocky)
  image on both canvas backends; vector layers crisp at all zooms;
  stroke layers re-render sharp after 4x scale; export defaults at max
  quality; user + agent docs updated; wiki log entry appended.

## File map (verified)

| Component            | Path                                                                                               |
| -------------------- | -------------------------------------------------------------------------------------------------- |
| Zoom filter decision | `libs/ui/canvas/kis_prescaled_projection.cpp` (~L395-405)                                          |
| QPainter canvas      | `libs/ui/canvas/kis_qpainter_canvas.cpp`                                                           |
| GL canvas / renderer | `libs/ui/opengl/kis_opengl_canvas2.cpp`, `KisOpenGLCanvasRenderer.cpp`, `kis_opengl.h:33-37`       |
| Canvas selection     | `libs/ui/canvas/kis_canvas2.cpp` (`createCanvas`, ~L369/777)                                       |
| Settings             | `libs/ui/kis_config.{h,cc}` (`useOpenGL`, `openGLFilteringMode`)                                   |
| Display prefs UI     | `libs/ui/dialogs/kis_dlg_preferences.cc` (~L2087)                                                  |
| Vector layer         | `libs/ui/flake/kis_shape_layer.cc`                                                                 |
| Stroke layer         | `libs/ui/KisBrushStrokeLayer.{h,cpp}`, `KisBrushStrokeLayerIO.cpp`, `libs/ui/kis_layer_manager.cc` |
| Scale filters        | `libs/image/kis_filter_strategy.{h,cc}`                                                            |
| Vector tools         | `plugins/tools/{defaulttool,karbonplugins,svgtexttool,tool_polyline,tool_polygon}`                 |
| Impex                | `plugins/impex/{svg,pdf}`                                                                          |
| AI quality           | `plugins/python/ai_diffusion/`, `plugins/visionml/`                                                |
| Prior art / context  | `docs/agent/brush-stroke-layer-plan.md`, `docs/agent/gpu-engine.md`                                |

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
