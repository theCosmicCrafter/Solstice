# Trace to Vector Layer

Solstice feature, added 2026-10-14 (Phase V2 of `vector-quality-megaplan.md`).

Converts the active layer's painted pixels into a filled `KoPathShape` on a
new vector layer, so the result stays crisp at any zoom and survives
document scaling. Menu: **Layer > Convert > Trace to Vector Layer**
(action id `layer_to_vector`).

## How it works

`KisLayerManager::traceLayerToVector()` in `libs/ui/kis_layer_manager.cc`:

1. Takes the active node's projection (falls back to `paintDevice()` /
   `original()`), so group layers and layers with masks/filter layers
   trace their *visible* result.
2. Builds a binarized `KisPixelSelection` from pixel alpha
   (`opacityU8 >= 128`) — same iteration pattern as
   `KisSelectionManager::selectOpaqueOnNode`, but hard-thresholded so
   faint residue does not inflate the outline.
3. Runs `KisUpdateOutlineJob` and reads `KisSelection::outlineCache()`
   for a `QPainterPath`, then maps it image→document space and creates
   a `KoPathShape` — identical to `KisSelectionToVectorActionFactory`.
4. Fills the shape with the current **foreground color** (no stroke),
   adds a new `KisShapeLayer` ("Traced Layer") above the source layer via
   `addLayerCommon`, and parents the shape into it with
   `shapeController()->addShapeDirect(shape, layer.data())`.
5. Both commands run inside one `KisProcessingApplicator` — a single
   "Trace to Vector Layer" undo step.

## Touch points

- `libs/ui/kis_layer_manager.{h,cc}` — slot + implementation + includes.
- `libs/ui/kis_layer_manager.cc::setup()` — `layer_to_vector` action
  creation/connect.
- `krita/krita.action` — action definition (icon `vectorLayer`).
- `krita/krita5.xmlgui` — `LayerConvert` submenu entry; **version + hash
  bumped in `krita/CMakeLists.txt`** (required for every xmlgui edit).

## Limitations

- Single-color trace: the whole outline gets the FG color. Multi-color
  posterized tracing (per-color paths) is not implemented.
- Outline fidelity is the selection-outline algorithm's — pixel-accurate
  polygon outlines, not smoothed Bezier fitting. For clean results, trace
  at high resolution or simplify the resulting path with the shape tool.
- Soft antialiased edges become a hard 50% boundary — expected for vector
  output.
- Source layer is preserved (not a destructive convert).

## Manual check

1. New document → paint black strokes on a paint layer →
   Layer > Convert > Trace to Vector Layer.
2. A "Traced Layer" appears above the source with a FG-colored filled
   shape matching the stroke silhouette; hide the source layer to verify.
3. Zoom to 800% — edges stay crisp (vector re-render, not scaled pixels).
4. Ctrl+Z — one undo removes the layer and shape.
5. Empty layer → friendly floating message, no empty layer created.
