# Lane B merge report — `libs/` (excluding `libs/ui`)

Scope: `build-tools/rebase/lanes/lane-b.json` — 61 merge files, 9 keep files, 4 dirty_merge files.
Ran `python build-tools/rebase/rebase.py merge --lane b`: 61 files merged, 1 textual conflict, 0 errors (`reports/lane-b-merge.json`).

## Structural finding (affects every lane)

`git merge-file current base other` with `base = HEAD` treats only *uncommitted*
worktree changes as "ours". Any fork line that is **committed** (in HEAD) and
absent from the 1.1.1 snapshot is read as "upstream deleted it" and is
**silently dropped** whenever the file was clean in the worktree — for a clean
file the merge output is byte-identical to the snapshot. Only 1 of 61 files
produced a marker; the other 60 merged "cleanly", which for clean files means
"fork deltas dropped".

Verified experimentally and on `libs/image/kis_layer.cc` (merged == snapshot).
Every lane should audit its dropped-vs-HEAD hunks the same way this lane did,
not only the `<<<<<<<` markers. For each of the 61 files I diffed
`git show HEAD:<file>` vs the merged result and classified every dropped hunk
(upstream evolution vs fork feature). Almost everything was an upstream
evolution of the same feature (the trees are the same product); two real fork
features were missing upstream and were restored.

## Resolutions applied

1. **`libs/image/CMakeLists.txt` — 1 conflict marker.** Both sides extended the
   same `list(APPEND kritaimage_LIB_SRCS gpu/...)` line: fork's uncommitted
   `KisGpuPerspectiveWorker.cpp` vs upstream's `KisGpuColorTransform.cpp
   KisGpuColorReferenceTables.cpp`. Resolved as the union; all four sources
   listed.

2. **`libs/image/kis_paint_device.cc` — restored `burnKeyframe`.** The 29-line
   `burnKeyframe(int)`/`burnKeyframe()` block was the file's only fork-unique
   code dropped. The keep-list header `kis_paint_device.h` still declares the
   API (its only fork-superset content vs upstream — upstream removed the whole
   feature), so the .cc must provide it. Restored verbatim after
   `framesInterface()`; all internals it uses
   (`Private::colorSpace()`, `KisPaintDeviceFramesInterface::writeFrameToDevice`,
   `contentChannel->allKeyframeTimes/removeKeyframe/importFrame`) verified to
   still exist with matching signatures.

3. **`libs/image/kis_filter_strategy.cc` — restored Lanczos3 reduction.**
   `autoFilterStrategy()` reduction branch: fork `Lanczos3` vs upstream
   `Bicubic`. `git log -S` shows the fork line came from `a57a771f` "quality:
   sharper scale-down filter, vector import res, export defaults" — documented
   fork behaviour (`docs/agent/vector-quality-megaplan.md` V4). Upstream kept
   Bicubic; restored the fork's documented default.

## Dirty merges (uncommitted user work) — all survived verbatim

| File | Evidence |
| --- | --- |
| `libs/gpu/CMakeLists.txt` | merged-vs-snapshot diff is exactly the fork's `KisGpuPerspectivePass.cpp` + two `perspective_transform` shader entries; nothing else differs |
| `libs/image/CMakeLists.txt` | union resolution (above); upstream `KisClipStackLayer.cpp`, `KisDustDetector.cpp` entries also landed |
| `libs/image/kis_perspectivetransform_worker.cpp` | merged-vs-snapshot shows only fork additions: `gpu/KisGpuPerspectiveWorker.h` include, `buildRegionSpans()`, both `KisGpuPerspectiveWorker::run()` call sites, `<QPair>`/`<type_traits>` includes, `template<class` spacing. Fork's `KisGpuTileBackend::instance()->context()` + `KisGpuPerspectivePass::create/record` calls are API-compatible with merged headers |
| `libs/widgetutils/xmlgui/kcheckaccelerators.cpp` | the opt-in `AutoCheckAccelerators=false` change + Solstice comment preserved verbatim |

## Per-file audit (61 merge files)

"Absorbed" = every dropped HEAD hunk is an upstream evolution of the same code
(superset or refactor); fork features present in evolved form.

| File | Status |
| --- | --- |
| libs/brush/KisAbrParser.h | absorbed — doc comments evolved (`abr-import.md`) |
| libs/brush/KisAbrPresetConverter.h | absorbed — same |
| libs/brush/KisAbrStorage.cpp | absorbed — same |
| libs/brush/kis_abr_brush_collection.cpp | absorbed — same |
| libs/brush/kis_abr_brush_collection.h | absorbed — same |
| libs/brush/tests/TestAbrParser.cpp | absorbed — same |
| libs/flake/KoToolManager.cpp | absorbed — upstream tool-restore on unknown canvas supersedes fork's fallback |
| libs/global/KisSolsticeProfile.{cpp,h} | absorbed — builtInText EN/JA/ZH + error-helper refactor |
| libs/global/tests/KisSolsticeProfileTest.cpp | absorbed — cancel test uses localized builtInText |
| libs/gpu/CMakeLists.txt | union (dirty) — all upstream passes/shaders + fork perspective entries |
| libs/gpu/KisGpuContext.{cpp,h} | absorbed — `create(..., unverifiedDevice)` opt-in supersedes Blackwell warn |
| libs/gpu/KisGpuLayerCompositor.{cpp,h} | absorbed — shifted-layer `LayerParams` (bias/tableOffset/tableWidth, 48B) |
| libs/gpu/shaders/composite_layers.comp | absorbed — `compositeOver` moved to new `composite_over.glsl` include; shifted addressing |
| libs/image/CMakeLists.txt | conflict resolved (union) |
| libs/image/KisPuppetTransformWorker.{cpp,h} | absorbed — fork `touchesArtwork`/`m_hasSolid` superseded by upstream `SourceCoverage` (pixel-accurate); serialization format unchanged (10 fields), parsing hardened (finite/MaximumCells); `triangleSolid` kept for solver weights |
| libs/image/KisRedrawableLayerInterface.h | absorbed — brush-stroke-layer interface intact, doc-renamed |
| libs/image/commands/kis_node_property_list_command.cpp | absorbed — `withClippingExclusion` + `clippingUpdateRect` added, `tightUserVisibleBounds` kept |
| libs/image/gpu/KisGpuEngineSettings.cpp | absorbed — `Convert16` policy + `unverifiedDevice*` opt-in present |
| libs/image/gpu/KisGpuMergeBatch.{cpp,h} | absorbed — `flush(cpuReadsNext)`, `deferClear`, `replace` layers superset |
| libs/image/gpu/KisGpuProjectionCompositor.{cpp,h} | absorbed — shifted tile tables, `KisGpuShiftedCopyPass`, `copyShifted` |
| libs/image/gpu/KisGpuTileAccess.cpp | absorbed — `referencedTileData` for reads (CPU-writer race fix) |
| libs/image/gpu/KisGpuTileBackend.cpp | absorbed — `create(options, &error, &unverifiedDevice)` |
| libs/image/kis_async_merger.cpp | absorbed — `m_gpuBatch` evolved (mayCompositeOnGpu gating, deferClear) |
| libs/image/kis_filter_strategy.cc | **restored** Lanczos3 reduction |
| libs/image/kis_group_layer.cc | absorbed — group position via `m_d->offset`, projection stays 0,0 |
| libs/image/kis_image.cc | absorbed — compositions + `compositionsLock` mutex |
| libs/image/kis_layer.cc | absorbed — clip stack (`isClipped/clipStackLayer/clipRunBounds/gpuAppliesEffectMasks`) + `copyShifted`; fork GC-alpha sequence preserved in evolved form |
| libs/image/kis_layer_composition.{cpp,h} | absorbed — exportEnabled kept, uuid remapping added |
| libs/image/kis_layer_projection_plane.cpp | absorbed — same composite-op exclusion, hoisted var |
| libs/image/kis_layer_utils.cpp | absorbed — apply() calls present incl. clip-stack variants |
| libs/image/kis_paint_device.cc | **restored** `burnKeyframe` |
| libs/image/kis_paint_layer.cc | absorbed — GPU `copyShifted` path (phase 4.113) + CPU fallback |
| libs/image/kis_perspectivetransform_worker.cpp | dirty — fork GPU hooks verbatim (see above) |
| libs/image/kis_processing_applicator.{cpp,h} | absorbed — `NO_IMAGE_UPDATES`, `StrategyWithStatusPromise` intact |
| libs/image/kis_projection_leaf.cpp | absorbed — `skipSelectionMasksForward`, clip-stack member logic |
| libs/image/layerstyles/kis_layer_style_projection_plane.cpp | absorbed — `gpuCompositesStyles()` gate (phase 4.114) |
| libs/image/processing/kis_transform_processing_visitor.cpp | absorbed — doc rename only |
| libs/image/tests/KisGpuBrushTest.cpp | absorbed — "offset" now GPU-previews (phase 4.112) |
| libs/image/tests/KisGpuProjectionTest.cpp | absorbed — Convert16-era expectations (+1503 upstream) |
| libs/image/tests/KisPuppetTransformWorkerTest.cpp | absorbed — damaged-mesh/non-finite tests added |
| libs/image/tiles3/KisTiledExtentManager.cpp | absorbed — migration+extent lock split |
| libs/libkis/Selection.cpp | absorbed — `trimDeviceForClipboard`/`buildImageForClipboard` rewrite |
| libs/pigment/KoColorConversionCache.cpp | absorbed — `fastStorage` → `std::optional` + move ctor |
| libs/pigment/resources/KoColorSet.cpp | absorbed — hardened GPL parser (column bug fixed) |
| libs/pigment/tests/TestKoColorSet.cpp | absorbed — gimp_{v1,v2,fuzz}.gpl test data |
| libs/psd/psd_layer_section.cpp | absorbed — real `isClipped()` → Photoshop clipping |
| libs/resources/KisResourceCacheDb.cpp | absorbed — `pngWithoutTextChunks`/`removeTextFromPresetThumbnails` |
| libs/resources/KisResourceModel.cpp | absorbed — `rowForResourceId` hash replaces O(n) seek |
| libs/widgets/KisPaletteChooser.cpp | absorbed — export flow refactor |
| libs/widgets/KisPaletteModel.cpp | absorbed — `indexRowForInfo` refactor |
| libs/widgets/KoDockWidgetTitleBar.h | absorbed — solsticeLook + `updateSolsticeLook()` added |
| libs/widgetutils/KisSolsticeStyle.h | absorbed — style feature intact |
| libs/widgetutils/xmlgui/kcheckaccelerators.cpp | dirty — opt-in preserved (see above) |

## Keep files — untouched (verified)

`KisSynchronizedConnection.cpp`, `kis_paint_device.h`, `libkis/Node.{h,cpp}`,
`pigment/{CMakeLists.txt,KoCompositeOpRegistry.{h,cpp},compositeops/KoCompositeOps.h,tests/CMakeLists.txt}` — all carry only their pre-existing uncommitted user changes (+59 lines vs HEAD total). Spectral registration verified intact: `COMPOSITE_PIGMENT` in `KoCompositeOpRegistry.cpp:138`, `KoCompositeOpPigment` in `KoCompositeOps.h:30,381`, `KoSpectralMixing.cpp` + `TestKoSpectralMixing.cpp` in the pigment CMakeLists files. No BOTH-modified lane file references spectral symbols.

## Verification

- `python build-tools/rebase/rebase.py scan --lane b` → 0 marker files.
- `git diff --check` on all lane paths → clean (only expected LF→CRLF
  autocrlf warnings).
- `grep KisGpuPerspectivePass libs/gpu/CMakeLists.txt` → line 126 present.
- `grep COMPOSITE_PIGMENT|Pigment libs/image/kis_layer.cc libs/image/CMakeLists.txt` →
  none (correct: spectral lives in pigment keep files).
- `KisGpuPerspectiveWorker` API compatibility: uses
  `KisGpuTileBackend::instance()->context()` (present, `KisGpuTileBackend.h:62`),
  `KisGpuPerspectivePass::create/record` (fork-only pass), `KisGpuBuffer::create`,
  `context.sharedComputePipeline`, generated `PerspectiveTransformRgba{16f,32f}`
  symbols (emitted by the restored `kis_gpu_add_shader` entries). No central
  pass registry exists — each worker instantiates its pass — so "perspective
  pass in the pass list" reduces to the CMakeLists entries, which are present.

## Open questions / risks for the build gate

1. `burnKeyframe` has no callers in `libs/`/`plugins/` today; restored for
   keep-header consistency (dangling declarations otherwise). Build gate
   confirms it compiles.
2. `Lanczos3` restore: no upstream caller seems to depend on Bicubic-for-
   reduction; flagging in case a new upstream call site assumed it.
3. `libs/image/kis_layer.h` is not in lane B's manifest — another lane merged
   it (upstream clip-stack decls present). Its fork-side decls should get the
   same dropped-hunk audit by that lane.
4. Systemic: other lanes' clean merges silently dropped their fork-committed
   deltas too — the per-file HEAD-vs-merged audit done here should be repeated
   per lane before the build gate.
