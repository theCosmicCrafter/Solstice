# Lane E merge report — `plugins/` (tools, filters, paintops, impex, visionml, metadata)

Date: 2026-11-15. Merge driver: `build-tools/rebase/rebase.py merge --lane e`
(derived base = `git show HEAD:<path>`; current = dirty worktree; other =
`Solstice-1.1.1` snapshot). Report JSON: `lane-e-merge.json`.

## Outcome summary

- 95 manifest `merge` files processed, 0 merge errors.
- `git merge-file` produced real conflict markers in exactly 1 file
  (`SegmentationToolHelper.cpp`, 2 blocks) — resolved by hand.
- **Silent-drop hazard found and fixed:** because the snapshot is an *older-
  shaped* version of some visionml files, `git merge-file` cleanly applied
  upstream "deletions" of committed fork work wherever the working tree
  matched base. 6 files needed manual restoration (all in visionml +
  whitespace in HeifExport). For every other file the working tree equalled
  base (`cur == base`), so `merged == upstream` byte-for-byte — correct
  wherever upstream's file is a functional superset, which it was everywhere
  else (see verification below).
- Final state: **89/95 files byte-identical to upstream**; 6 differ
  intentionally (5 visionml hand-merges + `HeifExport.cpp` whitespace strip).
- `scan --lane e`: **0 files with conflict markers.**
- `git diff --check` on all 95 files: **0 problems** (after stripping 5
  upstream trailing-whitespace lines in `HeifExport.cpp`).
- No `.py` files in the lane — `compileall` N/A.
- `keep` files (7) untouched: `impex/svg/CMakeLists.txt`,
  `visionml/CMakeLists.txt`, `icons/kritavisionml.qrc`, `VisionML.h`,
  `VisionMLPlugin.cpp`, `vision_filters.action`, `vision_tools.action`.

## VisionML — dirty_merge files (uncommitted SmartMatting work preserved)

### `plugins/visionml/src/segmentation/SegmentationToolHelper.cpp` (316−/12+)

The upstream file is the pre-SmartMatting shape (~300 lines smaller): no
`applyMattingMask`, no color-labeled-layer handling, no
`cancelStroke`-on-empty-undo pattern, `VisionMLImage image` instead of the
fork's `img` rename, stack `KisCursorOverrideLock` instead of the
stroke-lifetime `shared_ptr`. The merge produced:

- 2 real conflict blocks → resolved to the **cur (working tree) side**: the
  in-flight version is a strict superset (upstream's `VisionMLImage` decl +
  null check is contained inside the fork's larger block).
- 8 clean-applied upstream *reversions* → **all reverted back to fork
  content**: deleted includes (`SegmentationMatteUtils.h`,
  `canvas/kis_canvas2.h`, `canvas/kis_coordinates_converter.h`, `QPainter`,
  `QPolygonF`, `QThread`, `memory`), `shared_ptr` cursorLock, `inputImage`
  null guard + `isColorLabeledMode` decl + `m_bounds` ternary,
  `applicator.cancel()` vs `.end()`, `strokeId`/`image` decls, lambda
  captures (`image`, `strokeId`, `isColorLabeledMode`, `cursorLock`), and
  the C3 cursorLock finalizer command at `applyMattingMask`'s tail.
- 1 clean-applied upstream rename (`img.view` → `image.view`) fixed back to
  `img.view` (upstream's `image` would shadow `KisImageSP image`).

Uncommitted user work verified present after merge: `klocalizedstring.h`
include, C1 `effectiveBounds` logic in the fast-mode lambda, `rect.isEmpty`
guards, C6 view-rect `tryDeselectCurrentSelection`, C4 redesign comment,
`downscaleFactor`/`scaledSizeFor`/`kMaxInferencePixels`/`kMinInferenceDim`
downscaled-inference block, `matteImg` upsample + full-region compositing
loop. Remaining diff vs HEAD = exactly the dirty-work deltas (7 hunks).
`applyMattingMask` decl/def/call chain intact (`.h:54`, `.cpp`,
`SmartMattingTool.cpp:67`).

### `plugins/visionml/src/VisionML.cpp` (127−/7+)

Merge kept all uncommitted DepthMap/ESRGAN work (task enums, `configureModel`
calls, `estimateDepth()`/`restore()`, unload/addModels cases, model dirs).
Restored 4 clean-applied upstream deletions of committed fork work:
`KisRemoteFileFetcher.h`/`KoColorProfile.h`/`KoColorSpaceRegistry.h`/`QFile`
includes, the sRGB-profile fast-path check in `VisionMLImage::prepare`, the
BiRefNet-Dynamic download button block, and `downloadModel()` itself.
Upstream's genuine adds kept: 2 × `i18n()`-wrapped `std::runtime_error`
messages. Consistency with kept `VisionML.h` verified (declares
`m_downloadButton`, `downloadModel`, `estimateDepth`, `restore`,
`m_depthany`, `m_esrgan`).

### `plugins/visionml/src/CMakeLists.txt` (5−/2+)

Restored `segmentation/SegmentationMatteUtils.h` and
`smartselect/SmartMattingTool.cpp` to `kritavisionml_SOURCES` (upstream
deleted them cleanly). Dirty-work additions `filters/DepthMapFilter.cpp` +
`filters/ESRGANFilter.cpp` survived. Upstream's genuine fix kept:
`-Wno-missing-prototypes` now behind a non-GNU-CXX generator expression.

### Other visionml merges

- `SegmentationToolHelper.h` — restored `QPolygonF` include,
  `applyMattingMask` declaration, trailing newline → now identical to HEAD
  base (upstream contributed only deletions).
- `README.md` — restored the "## Tools" section + "lasso matting" intro
  (union of docs; upstream only deleted fork docs) → identical to base.
- `inpaint/InpaintTool.cpp` — upstream `i18n()` wrap of the mask-colorspace
  error applied cleanly. Done.

## Puppet Warp / tool_transform2 (15 files claimed, 8 in lane)

All merged files came out `== upstream`, verified as upstream *supersets* of
the fork feature, not regressions:

- `tool_transform_args.cc` — upstream adds `puppetOrders` vector, preserves
  `PuppetClickAction` on copy (KConfig pref, not transform state), NaN-
  rotation sanitize on load, `puppetExpansion` bound, mesh-validity in
  `isUnchanging` (legacy no-mesh transforms don't count as changed).
- `kis_warp_transform_strategy.cpp` — `puppetWorkerOrders` added to the
  worker cache key, rotation ring 2.4→3.0×handleRadius matching the 24px
  drawn ring, sampler-modifier (Ctrl+Alt) delete semantics.
- `kis_tool_transform.cc` — `flushPendingRecalculation()` before commit;
  mesh build no longer commits mid-gesture (undo-step splitting fix).
- `kis_tool_transform_config_widget.cpp` — `objectName`s on pin-order
  buttons/combo (scenario tests), Ctrl+Alt doc fixes, `setPuppetRotation=0`
  in unlock (Apply-still-rotated fix). Fork's 4 order buttons intact.
- `strokes/inplace_transform_stroke_strategy.{cpp,h}` — fork's
  `redrawRecordedStrokes` extended with `initialArgs` (cancelled continued
  transforms) + `transformMaskCacheHash` sources.
- `strokes/transform_stroke_strategy.cpp` — doc-pointer rename only.
- `kis_transform_utils.cpp` — upstream added `cachedPuppetWorker` (4-entry
  ARAP-solve cache) reusing the fork's `createPuppetWorker`/`usesPuppetMesh`
  machinery — preview/final-render consistency machinery preserved.

## basictools / defaulttool / enclose_and_fill

- `kis_tool_move.{cc,h}` — upstream is a superset: fork's
  `startStrokeImpl(mode, pos)` + `startAction` kept and extended with
  `duplicate` (Ctrl-drag duplicate-selection, upstream's own feature list);
  `m_currentlyDuplicating`, no-drag Ctrl-click cancels, `!m_strokeId` guards
  kept.
- `strokes/move_selection_stroke_strategy.{cpp,h}` — fork's stroke-carry
  (`KisBrushStrokeLayer::createSelectionMoveCommand`) kept + upstream added
  `createSelectionDuplicateCommand` for duplicates and a
  `currentLevelOfDetail()` guard for Instant Preview.
- `defaulttool/DefaultTool.cpp` — `hasSelection` rewritten to on-demand
  `koSelection()` lookup (equivalent semantics, avoids stale QPointer).
- `tool_enclose_and_fill/CMakeLists.txt` — union: fork's sources +
  upstream's `KisToolRemoveDust.cpp`/`KisToolRemoveDust.action` (files exist
  on disk).
- Through-layer-eraser registration already present in
  `basictools/CMakeLists.txt` + `default_tools.cc` (other lane's files — not
  touched here).

## paintops (incl. libpaintop shared model)

- `applyMinimumDabSize(...)` (upstream "At least 1 pixel",
  `KisMinimumDabSize`) wraps every `m_sizeOption.apply(info)` call site —
  including inside the fork's `lodToScale` multiplications, which all
  survived (`applyMinimumDabSize(...) * KisLodTransform::lodToScale(...)`).
  `KisBrushMinimumOnePixelTest` + `KisThroughLayerEraserTest` +
  `KisGpuProjectionStressTest` registered in brush tests CMakeLists (files
  on disk); fork's `KisBrushStrokeLayerTransformTest` registration kept and
  its link line extended with `kritadefaulttools_static` + basictools
  include dir.
- Shared model intact: `lager::cursor<KisBrushTipOptionData>`,
  `KisPaintOpOptionsModel`, `KisBrushTipOptionState` usages unchanged.
  libpaintop drops were doc-pointer renames
  (`brush-option-shared-model-plan.md` → `brush-option-shared-model.md`).
- `kis_clipboard_brush_widget.cpp` — upstream replaced the
  `clip(QRect(0,0,0,0))` hack with `clipImage(false)` +
  `KisToolUtils::bakeDefaultPixel` (real fix).
- `KisBrushStrokeLayer{,Transform}Test.cpp` — upstream massively extended
  (mirror param + `testMirroredStrokeRecorded`, duplicate/instant-preview/
  damage-rejection cases); all fork coverage supersets.
- Remaining ~25 files: drops are doc-pointer renames and superseded lines;
  merged == upstream verified superset each.

## impex / metadata

- `libkra/kis_kra_tags.h` — **both tag sets present**: fork's
  `BRUSH_STROKE_LAYER`/`DOT_BRUSH_STROKES` (already upstream, doc link
  renamed `brush-stroke-layer.md`) + upstream's `CLIPPED` +
  `SOLSTICE_FAST_EXPORT_PATH`. CLIPPED round-trip wiring
  (`kis_kra_loader.cpp`, `kis_kra_savexml_visitor.cpp`) already in worktree
  via other lanes — not my files.
- `libkra/kis_kra_saver.cpp` — upstream `saveSolsticeFastExport` +
  compositions-always-saved with `newLayersVisible` attr. Stroke-layer
  `.brushstrokes` payload saving lives in `kis_kra_save_visitor.cpp` /
  `savexml` (present, other lanes).
- `HeifExport.cpp` — upstream quality default 90→50 + HDR option changes;
  stripped 5 upstream trailing-whitespace lines.
- `kis_png_export.cc` — upstream `forceSRGB` default → true.
- `kis_wdg_options_webp.cpp` — upstream quality default 90→75.
- `kis_exif_io.cpp` — upstream `dvalue` null check replaces `Q_ASSERT`
  (crash fix).

## filters

- `blur/kis_lens_blur_filter.cpp` + `kis_wdg_lens_blur.cpp` — upstream's
  lens-blur highlights feature (`KisLensBlurHighlights.{h,cpp}` on disk,
  `irisRoundness`/`highlightStrength` properties, FFT even-kernel padding
  fix, non-RGBA notice label, `QSignalBlocker`); legacy path preserved when
  both new controls are 0. `tests/KisLensBlurFilterTest.cpp` present (copy
  lane).
- `filters/CMakeLists.txt` already contains `solsticeeffects` (other lane).

## Conflicts resolved

| File | Blocks | Resolution |
| --- | --- | --- |
| `SegmentationToolHelper.cpp` | 2 markers | Took cur side (strict superset) both times; additionally reverted 8 clean upstream deletions + 1 rename — see above. |

## Verification commands

- `PYTHONUTF8=1 python build-tools/rebase/rebase.py merge --lane e` →
  95 files, 2 markers, 0 errors (`reports/lane-e-merge.json`).
- `rebase.py scan --lane e` (and direct `has_markers` loop) → 0.
- `git diff --check -- <95 files>` → 0 problems.
- Merged-vs-snapshot byte compare → only the 6 intentional files differ.
- Diff merged-vs-`HEAD:` blob per file → fork-work audit (dropped-line
  analysis, ~200 lines reviewed individually; all either upstream-superseded
  or restored).
- Symbol greps: `applyMattingMask` chain, `applyMinimumDabSize`,
  `lager::cursor`, `KisPuppetTransformWorker`/`puppetOrders`,
  `BRUSH_STROKE_LAYER`/`DOT_BRUSH_STROKES`/`CLIPPED`,
  `KisToolThroughLayerEraser` registration, `KisToolRemoveDust` sources.

## Open questions / notes for the integrator

1. **merge-tool limitation worth noting:** `git merge-file` with no real
   common ancestor applies upstream *deletions* cleanly wherever
   `cur == base`. For any other lane file where the fork committed work that
   1.1.1 lacks, upstream's smaller file silently erases it. My per-file
   base-line drop audit caught this lane's cases; other lanes with
   upstream-smaller files (e.g. quickaccess monoliths, KisBrushStrokeLayer)
   need the same audit.
2. `plugins/tools/tool_transform2/tests/scenarios/` — 9 upstream scenario
   JSONs present on disk; the scenario *runner* registration lives outside
   this lane (test infra / another lane). Confirm a lane owns it.
3. Upstream changed user-visible defaults: PNG `forceSRGB` false→true, WebP
   quality 90→75, HEIF quality 90→50. Taken as upstream decisions; flag if
   the fork had a deliberate policy.
4. `DefaultTool::hasSelection` is now an on-demand lookup (upstream) rather
   than the fork's cached `QPointer` — equivalent, but noting the behavior
   change.
5. `visionml/src/CMakeLists.txt`: upstream's `-Wno-missing-prototypes`
   genex guard kept; the `ggml*` targets it guards are thirdparty (EOL-only
   diffs per inventory) — no further action.
6. `SelectSegmentFrom{Point,Rect}Tool.{cpp,h}` are in no lane because they
   need none: EOL-normalized, fork HEAD blobs are byte-identical to the
   snapshot (the "both-modified" impression was CRLF noise; worktree files
   are unmodified). Verified 2026-11-15, zero content diff on all four.
7. Stray `nul` file (0 bytes) appeared at the repo root at merge time —
   an artifact of the batch merge invocation (NUL redirect in Git Bash
   creates a literal file). Not a merge output; left in place per dirty-
   tree rules — safe to delete.
