# View-resolution vector layers (D7) — agent development notes

Fix for audit defect D7: `KisShapeLayer` rasterizes its `KoShape` content
into a document-resolution paint device
(`KisShapeLayerCanvas::m_projection`). The async merger composites that
device into the image projection and both canvas backends upscale the
result, so zooming past 100 % magnifies already-rasterized, antialiased
edges instead of re-rendering the vector geometry.

## Status and locations

- Implementation:
  - `libs/ui/canvas/kis_view_res_vector_compositor.{h,cpp}` —
    canvas-agnostic unit analysis, job grouping, backdrop recovery
    (source-over "peel"), raster unit patches. Exported via
    `KRITAUI_EXPORT`, unit-testable.
  - `libs/ui/canvas/kis_view_res_vector_decoration.{h,cpp}` —
    `KisCanvasDecoration` that owns the per-view caches and does the
    recomposited draw at view resolution.
- Hooks in upstream files:
  - `libs/ui/KisView.cpp` — creates the decoration and adds it to every
    view canvas (always present, paints only when it can improve the
    frame).
  - `libs/ui/kis_config.{h,cc}` — `viewResVectorRendering()` /
    `setViewResVectorRendering()`.
  - `libs/ui/CMakeLists.txt`, `libs/ui/tests/CMakeLists.txt` —
    build/test registration.
- Tests: `libs/ui/tests/kis_view_res_vector_compositor_test.{h,cpp}`
  (`kis_view_res_vector_compositor_test`, 6 cases).
- User document: `docs/canvas-display.md` ("View-resolution vector
  layers").

## Architecture

### Why not a plain overlay, and not a new projection

Two designs were rejected:

- **Pure overlay** (like assistant/selection decorations): paints the
  vector shapes at view resolution *above the whole image*. Crisp, but
  compositing-wrong whenever the vector layer is not the topmost simple
  layer — anything above it in the stack would be buried under the
  overlay repaint.
- **View-dependent projection** (re-rasterize vector layers into the
  layer stack at the current zoom): compositing-correct by construction,
  but the entire image pipeline (tiled devices, the async merger, masks,
  clones, undo extents, animation caches) assumes a fixed document
  resolution. Too invasive for this fix.

The implemented design is a **re-composition overlay**: for each vector
layer it rebuilds the exact patch of the *displayed* projection that the
vector layer participates in, at display resolution, and draws that patch
over the blurry canvas content. Stacking order is preserved inside the
patch; outside it the normal canvas image is untouched.

### Unit analysis (`KisViewResVectorCompositor::collectUnits`)

Walks the root projection leaf's children (`firstChild()`/
`nextSibling()`), which is the same flattened merge sequence the async
merger uses: pass-through groups disappear (their children show up as
units with merged effective opacity/channel flags), selection-mask
overlay leaves are skipped, hidden/dropped nodes report
`leaf->visible() == false`.

Each unit is classified:

- **recompositable** — can be un-blended from and re-composited onto the
  displayed image. Requires: the internal `KisLayerProjectionPlane`
  (rules out adjustment layers/filters — `dependsOnLowerNodes()` — and
  layer-style planes), `COMPOSITE_OVER`, no inherit-alpha/disabled
  channel, default channel flags, projection in the image color space.
- **isVector** — a recompositable unit that can additionally be
  re-rendered from live shapes: a `KisShapeLayer` with zero layer offset
  and no effect masks, or an isolated `KisGroupLayer` whose whole
  rendered subtree consists of such shape layers (nested isolated groups
  allowed, opacity accumulated per child).

### Job grouping (`computeJobs`)

For every vector unit, the pixel-space closure of units whose coverage
intersects it is taken — anything painting inside the same rect has to
be peeled and re-composited too, or z-order would break. Overlapping
closures merge into one job (iterate-until-stable). A closure that
touches a **non-recompositable** unit is dropped entirely: that vector
keeps its document-resolution rendering ("firewall" fallback — a
correctness-preserving bail-out, not a semantic change). Jobs are also
capped at `maxJobUnits = 64` units / `maxJobPixels = 16 MP` to bound
memory and peel cost.

### Backdrop recovery (`buildBackdrop`, `unblendSourceOver`)

For a job's union rect `U` the displayed projection is `D`. Working top
to bottom through the job units, each unit's effective contribution
`S = projection × unit.opacity` (display-space u8) is removed with the
inverse of premultiplied source-over:

```
D = S + B * (1 - a_s)   =>   B = (D - S) / (1 - a_s)
```

Fully covered pixels (`a_s = 255`) degenerate to `B = 0` — they are
repainted by the unit anyway. The result is the composite of everything
*below the whole job*. Re-compositing `B` plus every unit in order
reproduces `D` exactly (verified in the unit test).

### Decoration draw (`KisViewResVectorDecoration::drawDecoration`)

Runs for both canvas backends (decorations are painted after the image
in `KisCanvasWidgetBase::drawDecorations`, and after `paintCanvasOnly()`
in the OpenGL canvas). Priority `-10` so all UI decorations stay on top.

For each job intersecting the repaint area it draws, in order:

1. the backdrop `B` (document-space image, drawn under
   `imageToWidgetTransform` — identical to the canvas for those pixels);
2. raster units as their document-space patches (also pixel-identical to
   the original composite);
3. vector units as fresh `KoShapeManager::paint()` calls into a
   device-pixel-ratio-aware `QImage`, using
   `translate(-anchor) * documentToWidgetTransform` — the same
   document-space transform convention as
   `KisShapeLayerCanvas::repaint`, but mapping into *widget* pixels at
   the current zoom/rotation/DPR instead of image pixels.

Layer/group effective opacity is applied via `QPainter::setOpacity` on
the patch draw; isolated-group child opacity goes through an
intermediate image so internal child overlap keeps its semantics.
Antialiasing hints follow `KisShapeLayer::antialiased()`.

If any vector patch cannot be built (caps: 32 MP device pixels, 4096
shapes per job) `vectorComplete` stays false and the **whole job** is
skipped — the unmodified, correctly composited canvas pixels remain,
i.e. the vector renders blurry rather than wrong.

### Caching

- `collectUnits` + `unitsSignature` run per repaint; jobs and caches are
  rebuilt only on signature change (reorder/opacity/visibility/extent).
- `sigImageUpdated(QRect)` feeds a mutex-guarded `contentDirty` union
  (`Qt::DirectConnection`); the next repaint rebuilds the document-space
  caches of every intersecting job — including jobs outside the update
  rect, so a partial repaint never re-enters with a stale cache.
- Document-space caches (backdrop + raster patches) are transform-
  independent: panning and zooming reuse them. Vector patches are keyed
  by the linear part of `documentToWidgetTransform` plus DPR, so pure
  panning (translation only) reuses them too.

## Build, test, and install

- Build: `powershell -NoProfile -File C:/solstice-ci/ninja-env.ps1 kritaui`
- Test target: `kis_view_res_vector_compositor_test` (registered via
  `kis_add_tests`, links `kritaui kritalibkis kritatestsdk`).
- The decoration lives in `libkritaui.dll` — install
  `libs/ui/cmake_install.cmake` before interactive testing.

## Configuration and compatibility

- `kritarc` / Solstice settings key `viewResVectorRendering`
  (bool, default **true** — feature is on).
- Environment kill-switch `KRITA_DISABLE_VIEW_RES_VECTORS` (any value):
  forces the feature off regardless of the config entry. Use it when
  debugging canvas artifacts.
- Display-only: document data, exports, thumbnails and the stored
  projection are untouched. `KisShapeLayerCanvas` keeps rasterizing at
  document resolution — the overlay only re-renders for the screen.

## Invariants

- A skipped job must leave the canvas pixels *exactly* as the normal
  composite — never draw a partial recomposite (backdrop without all
  units, or units without a vector patch).
- Only `COMPOSITE_OVER` + default channel flags + internal projection
  plane units may be peeled/re-composited. Anything else is a firewall.
- UI decorations (grid, guides, assistants, selection, reference
  images) must stay above the recomposited patch — keep priority below
  every other decoration.
- The recomposite of a job must equal the displayed projection when
  vector units are painted at document resolution too (unit-tested
  invariant).

## Manual regression checklist

1. Vector layer alone: zoom 400 %+ — edges stay crisp.
2. Vector between raster layers: stacking preserved at any zoom.
3. Vector layer inside an isolated group, group opacity < 100 %.
4. Vector overlapping a layer with a non-over blend mode or inherit
   alpha: vector stays blurry (firewall), no artifacts.
5. Rotation/mirroring and 150 %+ display scaling (DPR).
6. Wrap-around mode ON: falls back to document-resolution rendering.
7. `KRITA_DISABLE_VIEW_RES_VECTORS=1`: feature fully off.
8. Animation playback and frame switching: vectors track the current
   frame, no stale patches after `sigImageUpdated`.

## Known limitations / improvement plan

- **Overlay internally.** Still a decoration, so it cannot run in
  export/thumbnail/filter pipelines — screen only, by design.
- **Wrap-around viewing** unsupported (each repeated tile would need
  its own recomposition): decoration bails, document-resolution
  rendering remains.
- **LOD > 0** (very large images, animation scrubbing at reduced
  resolution): bails — the peel arithmetic assumes full-resolution
  devices.
- **Firewalled vectors stay blurry**: vectors overlapping non-over blend
  modes, inherit-alpha, adjustment/filter layers, layer styles, or
  channel-flag-restricted layers keep document-resolution rendering.
  Clone layers of shape layers also stay raster (they have no live
  shape content).
- **Peel quantization + 8-bit patches**: the un-blend and all patches
  run in display-converted ARGB8. Residuals are bounded by display
  quantization and only matter where a recomposited unit is partially
  transparent — visually negligible on the standard path. On the GPU
  canvas's 10-bit display output, recomposited regions effectively
  render at 8-bit precision (same depth as the software canvas).
- **Peel quantization**: see above — bounded by display quantization.
- **Cost**: every repaint re-walks the leaf list and, on cache misses,
  re-reads `job.docRect` of the projection and each unit. Heavy stacks
  are bounded by `maxJobUnits`/`maxJobPixels`/device-pixel/shape caps,
  after which the feature self-disables per job.
- Possible follow-ups: async worker for backdrop/peel, incremental
  `contentDirty` propagation into `jobCaches`, wrap-around support,
  opt-out preferences checkbox (currently config-file/env only).
