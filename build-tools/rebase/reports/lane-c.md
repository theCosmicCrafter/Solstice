# Lane C merge report — `libs/ui/`

Selective intake of `Solstice-1.1.1` into the dirty Solstice fork.
Upstream snapshot: `C:\Users\richk\Downloads\Solstice-1.1.1\Solstice-1.1.1`
(reference `ac468d9`, no Git ancestry — `git merge-file` with `HEAD` as
derived base, then manual judgment).

## Gates

| Gate | Result |
| --- | --- |
| `rebase.py merge --lane c` | 67 files merged; 7 files with conflict markers |
| Conflict resolution | All 7 resolved by hand |
| `rebase.py scan --lane c` | `{"files_with_markers": [], "count": 0}` |
| `git diff --check -- libs/ui/` | Clean (only CRLF autocrlf warnings) |
| Stroke actions spot-check | `rerender_brush_strokes` / `forget_brush_strokes` actions + `createRerenderCommand()` / `createForgetStrokesCommand()` present |
| View-res compositor spot-check | Sources listed in `CMakeLists.txt:25-26`, decoration installed in `KisView.cpp:249-251`, `KisConfig::viewResVectorRendering` kill-switch present |
| Build / run | Not attempted (per lane rules) |

The literal grep the task suggested (`RerenderStrokes|reRender|forgetRecorded`)
matches nothing because the feature's actual identifiers are
`rerenderBrushStrokes`, `forgetBrushStrokes`, `rerender_brush_strokes` and
`forget_brush_strokes` — all present (see table below).

## Why marker removal was not enough

`git merge-file` with `HEAD` as base treats every *committed* fork change
upstream does not contain as "ours removed, theirs kept" wherever the two
regions don't textually collide. A clean merge can therefore silently drop
committed fork features. After resolving the 7 marker files I ran a
semantic audit: for every lane file, each `HEAD` line containing a fork
feature term (`stroke|viewRes|smoothHigh|trace|lazy|assetlib|supersampl|…`)
was checked for presence in the merged output. Findings below; raw dump in
`lane-c-absent-audit.txt`.

## Fork deltas silently dropped by clean merges — all restored

1. **"New paint layer = brush stroke layer" preference** — full chain:
   - `libs/ui/forms/wdggeneralsettings.ui`: `chkNewLayersAreStrokeLayers`
     checkbox re-added (row 16, col 1, same grid as `chkIgnoreHighFunctionKeys`).
   - `libs/ui/kis_config.h`: `newPaintLayersAreBrushStrokeLayers()` /
     `setNewPaintLayersAreBrushStrokeLayers()` declarations restored.
   - `libs/ui/kis_config.cc`: getter/setter restored
     (key `newPaintLayersAreBrushStrokeLayers`, default `false`).
   - `libs/ui/dialogs/kis_dlg_preferences.cc`: load, default-reset and
     save wiring + `GeneralTab::newPaintLayersAreBrushStrokeLayers()`
     accessor restored (declaration survived in the `keep` header).
   - `libs/ui/kis_layer_manager.cc`: `addPaintLayer()` conditional that
     instantiates `KisBrushStrokeLayer` when the setting is on, restored.
   - `libs/ui/kis_layer_manager.cc`: `layer_to_vector` action creation +
     `SLOT(traceLayerToVector())` connect restored — the whole
     Trace-to-Vector implementation was present but unreachable.
2. **Smooth high zoom (D-series)**:
   - `libs/ui/forms/wdgdisplaysettings.ui`: `chkSmoothHighZoom` checkbox
     re-added (row 6, col 1 after `chkMoving`).
   - `libs/ui/dialogs/kis_dlg_preferences.cc`: load, default-reset and
     `cfg.setSmoothHighZoom(...)` save restored.
   - `libs/ui/canvas/kis_prescaled_projection.cpp`:
     `smoothHighZoom()` getter and `setSmoothHighZoom()` (with `preScale()`
     + `sigSettingsChanged()`) restored — header still declared them, so
     this was a latent link break.
   - `libs/ui/opengl/KisOpenGLCanvasRenderer.cpp`: `d->smoothHighZoom`
     member and `d->smoothHighZoom = cfg.smoothHighZoom()` load restored —
     upstream kept `d->smoothHighZoom` *referenced* (line ~832) but the
     member and its load were dropped, a latent compile break.
   - `libs/ui/KisViewManager.cpp` / `.h`: `view_smooth_high_zoom` action,
     `slotUpdateSmoothHighZoomAction()`, config-change and
     canvas-controller connections restored.
   - `libs/ui/canvas/kis_canvas_controller.cpp`:
     `slotToggleSmoothHighZoom(bool)` restored (writes config, fires
     `KisConfigNotifier::notifyConfigChanged()`).
   - `libs/ui/tests/kis_prescaled_projection_test.cpp`:
     `testSmoothHighZoomToggle()` impl restored — header still declared it.
3. **Supersampled export** (`KisImportExportManager.cpp`):
   `setSupersamplingAvailable(true)`, the
   `supersamplingRequested() && desiredSize != savingImage()->size()`
   branch (2× Bilinear then Lanczos3 downscale) and the
   `kis_filter_strategy.h` include restored.

## Restructure decisions

| Fork file | Upstream layout | Decision |
| --- | --- | --- |
| `KisBrushStrokeLayer.cpp` (800+ line monolith) | thin `.cpp` + `brushstrokelayer/{Monitor,Replay,Transform}.cpp` | Took upstream's split. Ported only genuinely fork-unique code: `createRerenderCommand()` and `createForgetStrokesCommand()` into `Transform.cpp` (:513, :545); empty-record → `RecordState::Unknown` into `Monitor.cpp`. Upstream's `createTransformRedrawCommand()` already contains stronger partial-mismatch blending and HDR-native-channel handling — kept upstream, no port. Move-Tool carry (`createSelectionMoveCommand`/`createSelectionDuplicateCommand`) and background record check are upstream-present in the split files. |
| `KisSolsticeLazyTools.cpp` (monolith) | thin file + `lazytools/*` | Took upstream's split. Fork dialog/settings survived inside `lazytools/`: `Solstice/FastExport*` keys in `KisLazyToolsFastExport.cpp:56-60`, `Solstice/RenameDialog*`/`RenameGridColumns` in `KisLazyToolsRenameDialog.cpp`, `lazy_tools/config/name_color_list.txt` in `KisLazyToolsRenamePresets.cpp`. `Solstice/ColorPickFromAnywhere` kept in the thin file (:96). |
| `KisWelcomeAssetLibraryWidget.cpp` | `assetlibrary/*` split | Merged; `Private`/`q` back-pointer intact. |
| `freehand_stroke.cpp` recording hooks | upstream evolved | Upstream's `recordedChangeLayer` / `beginRecordedChange` / `KisRecordedChangeScopeCommand` tracking is a strict superset of the fork's `recordingLayer` + `KisRecordBrushStrokeCommand` hookup — kept upstream. `deviceOffset` still captured from `recordingLayer->paintDevice()` in `initStrokeCallback`. |
| `kis_painter_based_stroke_strategy.*` | `setAdditionalUndoCommand(cmd, closingCmd)` | Upstream signature superset (added `closingCommand`, default `nullptr`) — fork call sites still valid. |

## Verified upstream supersets — deliberately not "restored"

- `KisGpuEngineUi.cpp`: upstream evolved fork GPU conversion into
  `Convert16`/`Convert` policy + two conversion buttons.
- `KisDocument.cpp`, `kis_custom_image_widget.*`, `KisDlgCreateNewDocument.cpp`,
  `kis_image_from_clipboard_widget.*`: upstream's per-dialog-type config
  APIs supersede the fork's global-default calls; old write-backs not
  reintroduced.
- `kis_prescaled_projection.cpp`: upstream renamed `info->scaleX/scaleY` →
  `physicalScaleX/Y` in the smooth-high-zoom predicate; same semantics.
  Only real loss: a `dbgRender << "smoothHighZoom"` trace line (not
  restored; cosmetic).
- `KisOpenGLCanvasRenderer.cpp`: upstream lifted the fork's inline
  filter-mode conditional into named `smoothUpscale`; identical condition.
- `libs/ui/tests/kis_clipboard_test.*`: upstream test file is a superset
  of the fork's.

## Per-file status

Dirty = part of the 17 `dirty_merge` in-flight files. All files: merged,
markers zero.

| File | Status |
| --- | --- |
| `CMakeLists.txt` (dirty) | Kept upstream split dirs (`brushstrokelayer/`, `assetlibrary/`, `lazytools/`), reference-image filters, through-layer eraser, file-explorer helper + fork view-res sources (`canvas/kis_view_res_vector_*`) |
| `KisApplication.cpp` | Clean merge; Solstice style + window titles verified present |
| `KisBrushStrokeLayer.cpp` (dirty) | **Restructured** — thin upstream impl; fork commands ported to `brushstrokelayer/` |
| `KisBrushStrokeLayer.h` (dirty) | **Restructured** — full record/transform API intact (`createSelectionMove/DuplicateCommand`, `createRerender/ForgetStrokesCommand`, `strokesMatchPixels`, `requestRecordCheck`, `setRecordState`) |
| `KisBrushStrokeLayerIO.cpp` | Clean merge; `.kra` stroke serialization intact |
| `KisDocument.cpp` | Clean merge; clone/sync-wrapper strokes + Solstice titles present |
| `KisGpuEngineUi.cpp` | Upstream superset kept |
| `KisImportExportManager.cpp` | Clean merge + **supersampled export restored** |
| `KisLayerThumbnailCache.cpp` | Clean merge; `ThumbnailsStroke`/`KisIdleTaskStrokeStrategy` intact |
| `KisMainWindow.cpp` | Clean merge; docker locks, `KisSolsticeStyle`, `updateSolsticeDockLocks` verified |
| `KisPaintOpOptionsModel.h` | Clean merge |
| `KisPart.h` | Clean merge |
| `KisReferenceImage.cpp` | Clean merge |
| `KisRemoteFileFetcher.h` | Clean merge |
| `KisSolsticeLazyTools.cpp` | **Restructured** — thin upstream; fork behavior survives in `lazytools/` |
| `KisView.cpp` (dirty) | Clean merge; `KisViewResVectorDecoration` installed at :249-251, stroke-cancellation wiring intact |
| `KisViewManager.cpp` | Clean merge + **smooth-high-zoom action restored** |
| `KisViewManager.h` | Clean merge + slot declaration restored |
| `KisWelcomeAssetLibraryWidget.cpp` | Clean merge |
| `actions/KisPasteActionFactories.cpp` | Clean merge |
| `actions/kis_selection_action_factories.cpp` | Clean merge |
| `canvas/KisCanvasState.cpp` (dirty) | `devicePixelRatio` member/fromConverter/equality intact |
| `canvas/kis_canvas_controller.cpp` (dirty) | Clean merge + **`slotToggleSmoothHighZoom` restored** |
| `canvas/kis_prescaled_projection.cpp` (dirty) | Conflict-resolved (4): fork V1 smooth-zoom/filter logic kept; **getter/setter impls restored** |
| `canvas/kis_prescaled_projection.h` | Conflict-adjacent keep: decls intact |
| `canvas/kis_qpainter_canvas.cpp` (dirty) | Clean merge; upstream `devicePixelRatio` blit path |
| `dialogs/KisDlgCreateNewDocument.cpp` | Upstream per-dialog API kept |
| `dialogs/kis_dlg_preferences.cc` | **Restored**: stroke-layer + smooth-zoom checkbox wiring (7 points); RestNote/ColorPick/foreground-slot wiring intact |
| `flake/kis_shape_controller.cpp` | Clean merge |
| `flake/kis_shape_layer.cc` | Clean merge; `homogenizeZIndexesLazy`, `rerenderAfterBeingInvisible` present |
| `forms/wdgdisplaysettings.ui` | **`chkSmoothHighZoom` restored** |
| `forms/wdggeneralsettings.ui` | **`chkNewLayersAreStrokeLayers` restored**; RestNote/start-session widgets merged |
| `kis_clipboard.cc` | Clean merge; Solstice dialog title + fork mime paths present |
| `kis_clipboard.h` | Clean merge |
| `kis_config.cc` (dirty) | Conflict-resolved; `smoothHighZoom`, `viewResVectorRendering` kept; **`newPaintLayersAreBrushStrokeLayers` impl restored** |
| `kis_config.h` (dirty) | Conflict-resolved; same three APIs + decls restored |
| `kis_layer_manager.cc` (dirty) | Conflict-resolved (3): upstream clip actions + fork stroke/trace actions coexist; **`addPaintLayer` conditional and `layer_to_vector` action restored** |
| `kis_layer_manager.h` (dirty) | Conflict-resolved (1): `traceLayerToVector` + `traceDeviceToPathShape` (`KRITAUI_EXPORT`) decls present |
| `kis_mimedata.cpp` | Clean merge; "nothing to rasterize" fork comments/paths present |
| `kis_mimedata.h` | Clean merge |
| `kis_node_manager.cpp` (dirty) | Clip actions + `rerender_brush_strokes`/`forget_brush_strokes` actions coexist (:320-324, :1308-1390); uncommitted context-menu work preserved |
| `kis_node_manager.h` (dirty) | Slot decls :229-235 preserved |
| `kis_node_model.cpp` | Clean merge |
| `kis_paintop_box.cc` | Clean merge |
| `kis_paintop_settings_widget.cpp/.h` | Clean merge |
| `opengl/KisOpenGLCanvasRenderer.cpp` (dirty) | **`smoothHighZoom` member + config load restored** (latent compile break) |
| `opengl/kis_opengl_shader_loader.cpp/.h` (dirty) | `SmoothUpscale` shader enum intact |
| `operations/kis_operation.cpp/.h` | Clean merge |
| `tests/CMakeLists.txt` | Conflict-resolved: both fork tests kept (`kis_view_res_vector_compositor_test`, `KisLayerManagerTest`) |
| `tests/kis_clipboard_test.cpp/.h` | Upstream superset kept |
| `tests/kis_prescaled_projection_test.cpp` | **`testSmoothHighZoomToggle` impl restored** |
| `tool/kis_tool.h` | Clean merge |
| `tool/strokes/freehand_stroke.cpp` | Recording hooks + upstream `recordedChange` superset intact |
| `tool/strokes/freehand_stroke.h` | Clean merge |
| `tool/strokes/kis_painter_based_stroke_strategy.cpp/.h` | `SequentialRunnableJobs`, `setAdditionalUndoCommand` superset intact |
| `tool/strokes/move_stroke_strategy.cpp` | Clean merge (carry commands live in `brushstrokelayer/`) |
| `widgets/KisBrushStrokePreviewCache.cpp` | Clean merge |
| `widgets/kis_custom_image_widget.cc/.h` | Upstream per-dialog API kept |
| `widgets/kis_filter_selector_widget.cc` | Clean merge |
| `widgets/kis_image_from_clipboard_widget.cpp/.h` | Upstream per-dialog API kept |

`keep` entries (untouched, fork wins): `canvas/KisCanvasState.h`,
`canvas/kis_canvas_controller.h`, `dialogs/kis_dlg_preferences.h`,
`imagesize/wdg_imagesize.{cc,h,ui}`, `kis_action.h`,
`kis_action_manager.cpp`, `tests/kis_prescaled_projection_test.h`.

## Behavior deltas

None intended. Every restored block was taken verbatim from fork `HEAD`
(or the fork's uncommitted worktree content where applicable) and placed
into upstream's evolved structure; upstream-evolved implementations were
preferred wherever they were strict supersets.

## Open questions / residual risk

1. `KisOpenGLCanvasRenderer.cpp` and `kis_prescaled_projection_test.cpp`
   each contained a **latent compile break** from the clean merge
   (use-without-decl / decl-without-impl). Fixed, but other lanes may have
   the same failure mode — recommend a `libs/ui` compile check once all
   lanes land.
2. `dbgRender << "smoothHighZoom"` trace line not restored (cosmetic).
3. `newPaintLayersAreBrushStrokeLayers` UI row numbering: the merged
   general-settings grid has different row indices than the fork's
   (upstream renumbered); the checkbox is placed at fork's row 16 —
   visually last, no overlap, but worth a glance at runtime.
4. `KisView.cpp` generative-fill / stroke helpers: upstream file kept;
   no fork-unique lines were flagged beyond whitespace, but this file is
   dense — low residual risk only.
