# SVG Export (vector layers → SVG)

Native impex export filter that writes the document's **vector layers**
to `image/svg+xml` — closing the gap where every export path flattened
vectors to pixels. Part of vector-quality item V2.4 (decided:
SVG only; PDF export remains absent).

## Behavior

- File → Export → `*.svg` produces a document with one `<g>` group per
  **visible** `KisShapeLayer`, preserving stacking order (bottom-first;
  Krita stores siblings top-first, the filter iterates
  `lastChild → prevSibling` — see `kis_svg_export.cc` comments).
- Inside each group `SvgWriter` emits native elements where a shape has
  one (`SvgShape::saveSvg` — paths, text, groups, z-sorted via
  `KoShape::compareShapeZIndex`) and embeds generic shapes as images.
- Raster layers are **not** represented; layer opacity/blending are not
  written per group (`LayerOpacityCheck` UNSUPPORTED).
- Documents with no vector layers fail with a clear error instead of
  writing an empty file.
- `MultiLayerCheck` SUPPORTED keeps Advanced Export from flattening the
  image (which would destroy the vectors).
- Page size comes from `image->bounds()` + `xRes/yRes` (px → pt).

## Source

- Filter: `plugins/impex/svg/kis_svg_export.{cc,h}`,
  `krita_svg_export.json` (`X-KDE-Export: image/svg+xml`, extension `svg`).
- Built as `kritasvgexport` from `plugins/impex/svg/CMakeLists.txt`,
  installed next to `kritasvgimport`.
- Writer: `libs/flake/svg/SvgWriter.{h,cpp}` (pre-existing, used by
  `KoDrag` for clipboard SVG; constructor takes `QList<KoShapeLayer*>`).

## Limitations / manual checks

- Nested group layers: vector layers inside group layers export as
  sibling `<g>` groups in stack order (group structure is flattened —
  acceptable for v1).
- Order correctness: paint something distinctive on a bottom vector
  layer, another on top, export, and confirm the top shape overlaps
  correctly when opened in a browser/Inkscape.
- Non-native shapes (e.g. complex brushes) arrive as embedded PNGs —
  expected, mirrors `KoDrag` behavior.
