# Foundation rebase inventory — verified merge map

Selective absorb of `Solstice-1.1.1` (`C:\Users\richk\Downloads\Solstice-1.1.1\Solstice-1.1.1`,
commit `ac468d9` per megaplan) into fork `main` (`deb3ebd6`). Read-only audit
performed 2026-W47; no files changed in either tree except this document.

## Executive summary

- **The trees are the same product, not fork-vs-Krita.** `Solstice-1.1.1` ships
  the same Solstice feature set (GPU engine, quickaccess, restnote, assetlibrary,
  lazytools, brush stroke layer, visionml, solstice packaging) evolved further.
  Its `AGENTS.md` is a later revision of this repo's own policy file.
- **~97% of the "modified" file set is line-ending noise.** The fork checkout is
  CRLF (`core.autocrlf=true`); the 1.1.1 snapshot is LF — identical to the fork's
  git blobs. Of ~13,700 differing files only **426** differ in real content.
- **No merge base exists.** Neither `ac468d9` nor the claimed fork point
  `e88c2289` is present in the fork's object store, and the 1.1.1 snapshot has
  no `.git`. A real 3-way merge is impossible; every both-modified file needs a
  manual hunk merge (or a re-derived base).
- **Megaplan counts verified with corrections:** `libs/` 19 fork-only / 72
  upstream-only exact; `krita/` 6 icons exact; `plugins/` claims need fixes
  (see confirm/deny table — actual upstream-only is 203, `plugins/python/` is
  NOT entirely fork-only, and several "upstream-only" features already exist
  in the fork in earlier form).
- **Biggest collision:** features both lines evolved — quickaccess (fork
  2,183-line `QuickAccessDock.cpp` vs upstream's 29-file split, ~8.9k lines),
  restnote, lazytools, assetlibrary, brush stroke layer (upstream split it into
  `libs/ui/brushstrokelayer/` while the fork's monolith grew to 924 lines).
- **46 in-flight files collide.** Files dirty in the fork working tree
  (uncommitted spectral-mixing, view-res vector, SmartMatting, stroke-layer
  work) are also changed upstream — merge before or commit first.

## Method

Content-verified per file: size → MD5 → CRLF/LF-normalized text compare across
`libs/`, `plugins/`, `krita/`, `3rdparty*`, `build-tools`, `packaging`, `cmake`
(18,379 files fork side; `.git`, `__pycache__` excluded). Fork side = working
tree on disk (includes 56 untracked + ~105 modified tracked files).

## Verified counts

| Tree | only-in-fork | only-in-1.1.1 | identical | EOL-only | real diff |
| --- | --- | --- | --- | --- | --- |
| `libs/` | 19 | 72 | 1,026 | 4,116 | **214** |
| `plugins/` | 3,199 | 203 | 1,254 | 6,007 | **199** |
| `krita/` | 0 | 6 | 1,340 | 482 | **9** |
| `3rdparty{,_plugins,_vendor}` | 0 | 0 | 0 | 18 | **0** |
| `build-tools/` | 4 | 4 | 4 | 35 | **1** |
| `packaging/` | 0 | 0 | 57 | 62 | **2** |
| `cmake/` | 0 | 0 | 0 | 49 | **1** |

Real-diff direction (from `diff` of each pair): **123** files where upstream is
a pure superset (safe absorb), **30** where the fork is a pure superset (keep
fork side), **273** where both sides changed content (manual merge).

## Only-in-fork — claim verification

| Megaplan claim | Verdict | Verified detail |
| --- | --- | --- |
| 19 `libs/` files (perspective pass+shader+worker; spectral trio; view-res vector pair) | **Count correct, enumeration incomplete** | The 19 are exactly: `libs/gpu/KisGpuPerspectivePass.{h,cpp}`, `libs/gpu/shaders/perspective_transform.comp`, `libs/image/gpu/KisGpuPerspectiveWorker.{h,cpp}` (5); `libs/pigment/KoSpectralMixing.{h,cpp}`, `compositeops/KoCompositeOpPigment.h`, `tests/TestKoSpectralMixing.{h,cpp}` (5); `libs/ui/canvas/kis_view_res_vector_compositor.{h,cpp}` + `_decoration.{h,cpp}` (4); **plus** `libs/pigment/tests/data/gimp.gpl`, `libs/ui/tests/KisLayerManagerTest.{cpp,h}`, `libs/ui/tests/kis_view_res_vector_compositor_test.{cpp,h}` (5). All exist on disk (all untracked or new); none upstream. |
| 15 `plugins/` files (`kis_svg_export`, visionml contents) | **Confirmed exactly** | `plugins/impex/svg/kis_svg_export.{h,cc}` + `krita_svg_export.json` (3); visionml: `src/filters/{DepthMapFilter,ESRGANFilter}.{h,cpp}`, `src/segmentation/SegmentationMatteUtils.h`, `src/smartselect/SmartMattingTool.{h,cpp}`, `icons/{dark,light}_tool_smart_matting.svg`, `tests/{CMakeLists.txt,VisionMLMatteTest.{h,cpp}}` (12). |
| `plugins/python/` entirety fork-only | **Denied** | Upstream has `plugins/python/` too: 9 upstream-only `.action` files (`assignprofiledialog`, `channels2layers`, `colorspace`, `documenttools`, `exportlayers`, `filtermanager`, `highpass`, `krita_script_starter`, `scripter`) and 25 shared dirs with real diffs (`scripter`, `plugin_importer`, `mutator`, ...). Fork-only = 12 dirs + 3 stray files: `ai_diffusion` (650 files), `bulibrushswitch` (1,140), `bulinotes` (921), `pigment_o` (177), `shortcut_composer` (160), `imagine_board` (70), `timer_watch` (20), `blender_layer` (10), `PyQt`/`PyQt5` shims (14+14), `kritamcp` (3), `.appdata` (2), `moss.rej`/`moss_plugin.diff`/`moss_pr.diff` (3 — stray patch artifacts). |
| `plugins/visionml` fork-only | **Denied as a whole** | visionml exists upstream with its own evolution: `src/VisionML.cpp` (127−/7+), `VisionMLPlugin.cpp`, `SegmentationToolHelper.*` (316−/12+), `InpaintTool.*`, `SelectSegmentFrom*Tool.*`, actions, icons all both-modified. Only the 12 files above are fork-only. `thirdparty/vision.cpp` is identical modulo EOL in all 2,579 files — no vendor bump needed. |

## Only-in-1.1.1 — verified file sets

| Subsystem | Count | File set |
| --- | --- | --- |
| GPU 1.0.0 passes | 16 | `libs/gpu/KisGpu{ColorTransform,Coverage,ShiftedCopy,StrokeStyle}Pass.{h,cpp}` (8); `libs/gpu/shaders/{apply_coverage,color_transform}.comp`, `composite_over.glsl`, `composite_stroke_style.comp`, `copy_shifted.comp` (5); `libs/image/gpu/KisGpuColorTransform.{cpp,h}`, `KisGpuColorReferenceTables.cpp` (3) |
| Clipping masks | 6 | `libs/image/KisClipStackLayer.{h,cpp}`, `libs/image/tests/KisClippingTest.cpp`, `plugins/impex/libkra/tests/KisClippingKraTest.cpp`, `krita/pics/svg/{dark,light}_clipping-enabled.svg` |
| Remove dust | 7 | `libs/image/KisDustDetector.{h,cpp}`, `libs/image/tests/KisDustDetectorTest.cpp`, `plugins/tools/tool_enclose_and_fill/KisToolRemoveDust.{h,cpp}` + `KisToolRemoveDust.action` + `KisToolRemoveDustFactory.h` |
| Through-layer eraser | 8 | `libs/ui/tool/KisThroughLayerEraserTargets.{h,cpp}` (2), `plugins/tools/basictools/KisToolThroughLayerEraser.{h,cpp}` + scenario JSON (3), `plugins/paintops/defaultpaintops/brush/tests/KisThroughLayerEraserTest.cpp` (1), tool icons in `krita/` (2) |
| solsticeeffects | 72 | `plugins/filters/solsticeeffects/` — 12 filters (`Kis{ChromaticAberration,Color,ColorReference,Crystal,DropShadow,Film,FilmGrain,Glow,LineArt,Pencil,RemoveDust,Sharpness}Filter` + config widgets), helpers (`KisCpuEffectKernels.h`, `KisCpuPlaneBlur`, `KisCrystalLattice`, `KisEffectColorSampler`), `SolsticeEffectsPlugin.cpp`, 12 tests |
| G'MIC-Qt | 6 | `plugins/extensions/qmic/KisQmicRgbConverter.h`, `tests/KisQmicIntegrationTest.cpp`; `build-tools/gmic/{README.md,build-runtime.py,runtime-patches.json,runtime.lock.json}` (lock pins gmic 3.7.4.1) |
| Layer Comps | 4 | `plugins/dockers/compositiondocker/CompositionDocumentState.{h,cpp}` + `tests/{CMakeLists.txt,CompositionWorkflowTest.cpp}` |
| quickaccess evolution | 38 | 29 sources (`QuickAccess{Gesture{Dialog,Presentation,Preview},Dock{Popups,Widgets},Grid{Canvas,EditMetrics,EditOperations,ItemButton},Item{DragController,Presentation,Properties},ProfileLoader,Settings{,Store},Adjust{DockHistory,ToolOptionsPad}}`) + 9 tests/scenarios |
| restnote evolution | 11 | `RestNote{Config,Notifications,SettingsDialog,Timer}.{h,cpp}` + tests |
| lazytools evolution | 17 | `libs/ui/lazytools/KisLazyTools{FastExport,FastExportDialog,FastExportPresets,RenameDialog,RenamePresets}.{h,cpp}` + `libs/ui/tests/KisLazyToolsTest.cpp` + 6 scenario JSONs |
| assetlibrary evolution | 7 | `libs/ui/assetlibrary/KisAssetLibrary{Browser,Operations,Settings}.{h,cpp}` + `libs/ui/tests/KisAssetLibraryTest.cpp` |
| brushstrokelayer split | 4 | `libs/ui/brushstrokelayer/KisBrushStrokeLayer{Monitor,Replay,Transform}.cpp` + `KisBrushStrokeLayerPrivate.h` |
| Reference image filters | 5 | `libs/ui/KisReference{FilterTemplates,ImageFilters}.{h,cpp}` + `KisReferenceImageFiltersTest.cpp` |
| Min dab size | 2 | `libs/image/brushengine/KisMinimumDabSize.h`, `plugins/paintops/defaultpaintops/brush/tests/KisBrushMinimumOnePixelTest.cpp` |
| Lens blur highlights | 4 | `plugins/filters/blur/KisLensBlurHighlights.{h,cpp}` + `tests/{CMakeLists.txt,KisLensBlurFilterTest.cpp}` |
| Misc | 37 | `libs/ui/utils/KisShowInFileExplorer.{h,cpp}` (2); `libs/ui/tests/{KisGpuEngineUi,KisPartCloseSession}Test.cpp` (2); `libs/flake/tests/TestKoToolManager.cpp`; `libs/image/tests/KisLayerCompositionTest.cpp` + 5 benchmarks (`Kis{Clone,Filter,Masked,Move}LayerBenchmark`, `KisLayerStyleBenchmark`); `libs/pigment/tests/data/gimp_{fuzz,v1,v2}.gpl` (3); `plugins/paintops/defaultpaintops/brush/tests/KisGpuProjectionStressTest.cpp` + 5 scenario files; `plugins/impex/libkra/tests/KisMoveDocumentBenchmark.cpp`; `plugins/dockers/layerdocker/tests/scenarios/color-label-controls.json`; `plugins/tools/selectiontools/tests/scenarios/*.json` (5); `plugins/tools/tool_transform2/tests/scenarios/*.json` (9); `plugins/tools/basictools/tests/scenarios/*.json` (1) |
| `krita/` icons | 6 | `pics/svg/{dark,light}_clipping-enabled.svg`, `pics/tools/SVG/16/{dark,light}_krita_tool_{remove_dust,through_layer_eraser}.svg` |
| build-tools | 4 | `build-tools/gmic/*` (above) — fork-only side: `github-actions/check-docs.py`, `release/package-local.ps1`, `toolchains/{check-local-toolchains.py,windows-dev.lock.json}` |

## Both-modified hot zone (top conflicts)

Fork−/upstream+ line deltas on the 273 both-modified files; biggest first:

| File | Fork− | Up+ | Nature |
| --- | --- | --- | --- |
| `plugins/dockers/quickaccess/QuickAccessDock.cpp` | 1,424 | 55 | Upstream split the monolith; fork copy may hold unported deltas — audit needed |
| `libs/ui/KisBrushStrokeLayer.cpp` | 804 | 55 | Upstream refactored to `brushstrokelayer/`; fork monolith has re-render/forget + Move-Tool carry |
| `plugins/dockers/quickaccess/QuickAccessGridEditDialog.cpp` | 609 | 55 | Same split collision |
| `libs/image/gpu/KisGpuProjectionCompositor.cpp` | 10 | 594 | Upstream far ahead (GPU 1.0.0); port fork's 10 lines |
| `plugins/dockers/restnote/RestNoteDock.cpp` | 520 | 40 | Upstream split; fork monolith |
| `plugins/extensions/qmic/kis_qmic_simple_convertor.cpp` | 495 | 24 | Bridge rewrite upstream vs fork's larger copy — verify fork deltas |
| `libs/ui/kis_clipboard.cc` | 361 | 97 | Fork clipboard work vs upstream changes |
| `libs/ui/KisSolsticeLazyTools.cpp` | 341 | 71 | Fork feature body vs upstream additions |
| `plugins/visionml/src/segmentation/SegmentationToolHelper.cpp` | 316 | 12 | Fork SmartMatting helpers vs upstream version |
| `libs/ui/kis_layer_manager.cc` | 285 | 34 | Fork stroke-layer/lazy-tools actions vs upstream clip/comps wiring |
| `plugins/dockers/assetlibrary/AssetLibraryDock.cpp` | 256 | 41 | Divergent |
| `libs/ui/KisWelcomeAssetLibraryWidget.cpp` | 250 | 42 | Divergent |
| `libs/ui/kis_mimedata.cpp` | 59 | 224 | Upstream far ahead |
| `plugins/dockers/compositiondocker/compositionmodel.cpp` | 67 | 214 | Upstream Layer Comps model rewrite |
| `libs/ui/forms/wdggeneralsettings.ui` | 64 | 206 | Both add settings widgets |
| `krita/main.cc` | 48 | 44 | Fork JA-only profile-import prompt vs upstream EN/JA/ZH `builtInText()` — upstream superset |
| `libs/ui/canvas/kis_prescaled_projection.cpp` | 105 | 29 | Fork V1/D7 rendering vs upstream |
| `plugins/visionml/src/VisionML.cpp` | 127 | 7 | Fork SmartMatting/ESRGAN/DepthMap registration vs upstream |
| `libs/image/kis_perspectivetransform_worker.cpp` | 89 | 6 | Fork GPU-perspective hooks (dirty in tree) vs upstream |
| `plugins/paintops/libpaintop/*` (13 files) | ~60 | ~120 | Fork shared-model vs upstream MinDabSize/model edits |
| `plugins/tools/tool_transform2/*` (15 files) | ~70 | ~150 | Fork Puppet Warp vs upstream transform fixes + scenarios |
| `plugins/impex/libkra/*` (8 files) | ~40 | ~150 | Fork stroke-layer `.kra` saving vs upstream clip round-trip |

Fork-superset files (keep fork version; 30): `krita/data/shaders/highq_downscale.frag` (V1 smooth rendering, 83 lines upstream lacks), `krita/windows_stub_main.cpp`, `krita/krita5.xmlgui`, `libs/libkis/Node.{h,cpp}` (undoable pixel writes), `libs/global/KisSynchronizedConnection.cpp`, `libs/image/kis_paint_device.h`, `libs/pigment/{CMakeLists,KoCompositeOpRegistry.{h,cpp},compositeops/KoCompositeOps.h,tests/CMakeLists}` (spectral), `libs/ui/imagesize/wdg_imagesize.{cc,h,ui}` (supersampled export), `libs/ui/kis_action{,_manager}.{h,cpp}`, `libs/ui/canvas/{KisCanvasState.h,kis_canvas_controller.h}`, `libs/ui/dialogs/kis_dlg_preferences.h`, `libs/ui/tests/kis_prescaled_projection_test.h`, `plugins/dockers/animation/KisAnimCurvesDocker.cpp`, `plugins/extensions/pykrita/sip/krita/Node.sip`, `plugins/impex/svg/CMakeLists.txt`, `plugins/visionml/{CMakeLists.txt,icons/kritavisionml.qrc,src/VisionML.h,src/VisionMLPlugin.cpp,src/vision_filters.action,src/vision_tools.action}`.

## Subsystem merge blocks

Dependencies = upstream-internal code each subsystem needs; "touches" = fork files it collides with.

1. **libs/image core (clip stack + dust detector + min dab)** — gates #4 (libkra/PSD round-trip), #6 (solsticeeffects), #7 (eraser), #8 (remove-dust tool).
   Files: new `KisClipStackLayer`, `KisDustDetector`, `KisMinimumDabSize.h`;
   touches `kis_layer.{cc,h}` (upstream adds `isClipped/clipStackLayer/clipRunBounds/gpuAppliesEffectMasks`), `kis_projection_leaf.*`, `kis_layer_utils.cpp`, `kis_layer_properties_icons.*`, `kis_group_layer.*`, `kis_mask.cc`, `kis_image.*`, `kis_node_property_list_command.cpp`, `kis_adjustment_layer.*`, `kis_paint_device.*`, `kis_filter_mask.*` (upstream adds GPU filter-mask hooks), `kis_async_merger.cpp`, `libs/image/CMakeLists.txt`. Conflicts: fork's GPU/stroke-layer hooks in the same files; `kis_paint_device.cc` 31−/354+.
2. **GPU 1.0.0 + device gating** — passes + shaders listed above; `KisGpuEngineSettings` gains `Convert16` (RGBA16F target = the "Blackwell auto-start" mechanism) + `unverifiedDevice*` opt-in; `KisGpuTileBackend`, `KisGpuMergeBatch`, `KisGpuLayerCompositor`, `KisGpuContext`, `libs/gpu/CMakeLists.txt`, `composite_layers.comp` all both-modified. Fork's `KisGpuPerspectivePass` must be added back into upstream's `libs/gpu/CMakeLists.txt` + `KisGpuLayerCompositor` pass list. Consumers: `plugins/filters/{posterize,colorsfilters}` GPU path, `solsticeeffects` ColorReference, `KisGpuProjectionTest` (+1,503 upstream).
3. **libs/ui hubs** — `KisViewManager`/`KisMainWindow`/`KisDocument`/`KisPart`/`KisView`/`KisApplication`, `kis_node_manager` (fork re-render/forget actions vs upstream `create_quick_clipping_group`/`toggle_layer_clipping`/`new_clipping_layer`), `kis_layer_manager`, `kis_clipboard`, `kis_mimedata`, `kis_config`, `kis_action_manager`, dialogs/forms, canvas stack (`kis_qpainter_canvas` fork V1/D7 + `KisOpenGLCanvasRenderer` + shader loader). Upstream adds `lazytools/`, `assetlibrary/`, `brushstrokelayer/` subdirs + `KisReferenceImageFilters` + `KisThroughLayerEraserTargets` + `KisShowInFileExplorer` — all in the both-modified `libs/ui/CMakeLists.txt`.
4. **libkra + PSD clip round-trip** — `kis_kra_tags.h` adds `CLIPPED` tag (fork file has stroke-layer tags), `kis_kra_{loader,saver,savexml_visitor}`, `kra_converter`, `libs/psd/psd_layer_section.cpp` (`isClipped()` → Photoshop clipping), `impex/psd` loader/tests. Gate: needs #1 landed.
5. **compositiondocker (Layer Comps + export)** — new `CompositionDocumentState` + tests; rewrites `compositionmodel`/`compositiondocker_dock`/`wdgcompositiondocker.ui`; `exportCompositions` button + `setExporting()` refcount. Uses `kis_layer_composition` (both-modified).
6. **solsticeeffects** — self-contained module, `target_link_libraries(solsticeeffects PUBLIC kritaui)`; **hard dep on `KisDustDetector`** (`KisRemoveDustFilter.h` includes it) — land after #1. Registration: `plugins/filters/CMakeLists.txt` (+1 upstream line).
7. **through_layer_eraser** — tool in basictools (`default_tools.cc` registration +1, CMakeLists +1, `tools.action` +10, icons); needs `libs/ui/tool/KisThroughLayerEraserTargets` + `kis_resources_snapshot` (+52 upstream); test `KisThroughLayerEraserTest` in brush tests CMakeLists.
8. **remove_dust tool** — `KisToolRemoveDust*` in tool_enclose_and_fill (`KisToolEncloseAndFillPlugin.cpp` +2, CMakeLists), needs `KisDustDetector`; overlaps solsticeeffects `KisRemoveDustFilter` (same detector).
9. **quickaccess** — upstream refactor+growth (new config static lib `kritaquickaccessconfig`, KF::I18n/ConfigCore deps, gestures, grid editing, quick-adjust pads, profiles, 8 tests). Fork's `QuickAccessDock.cpp` is bigger than upstream's — audit for unported fork deltas before taking upstream wholesale. `KisBrushStrokePreviewCache` dep is satisfied upstream.
10. **restnote** — upstream split (Config/Notifications/SettingsDialog/Timer + tests); fork monolith keeps `RestNoteConfig` struct + `RestNoteEnabled` legacy key — verify upstream preserves the legacy key, else migrate.
11. **testcontrol** — self-contained `plugins/extensions/testcontrol` (31 files), links `kritaui` + `Qt::Network`, gated `if(BUILD_WITH_QT6)` in `plugins/extensions/CMakeLists.txt` (+4 upstream, clean add). 27 registered MCP tools (claim said 26), env `SOLSTICE_TEST_CONTROL_PORT/TOKEN/ROOT`. Reconcile with `plugins/python/kritamcp` (63 actions on :5678) per megaplan Phase 2.
12. **qmic / gmic** — runtime lock `build-tools/gmic/` (adopt) + `3rdparty_plugins/ext_gmic` (EOL-only diffs, no action) + `extensions/qmic` bridge rewritten upstream (9 real-diff files; fork's `kis_qmic_simple_convertor.cpp` is 495 lines bigger — audit).
13. **plugins/python + pykrita** — `plugins/python/CMakeLists.txt` both-modified: keep fork's `install_pykrita_plugin(...)` vendored block + PyQt shim installs, add upstream's 9 `.action` installs + files; `pykrita/plugin/krita/__init__.py` both rewrote the PyQt5 guard differently — merge semantics; `Node.sip` keep fork (undoable write binding).
14. **krita/ app shell** — icons+qrcs are upstream-adds (safe); `krita.action` adds 3 clip actions (merge with fork's solstice actions); `kritamenu.action` both-modified; `main.cc` upstream superset (adds Simplified Chinese); `windows_stub_main.cpp` keep fork; `krita/CMakeLists.txt` both-modified.
15. **packaging/build-tools** — `package-complete.py` both-modified (35−/28+: fork launcher/QML-prefix vs upstream changes); `build-windows.ps1` upstream +7 (safe absorb); `linux/appimage/.../libstdc++.so.6` binary differs — pick upstream.
16. **Scenario test infra** — ~30 `tests/scenarios/*.json` + test registrations in both-modified `*/tests/CMakeLists.txt` files; consumed by upstream test suites (e.g. `KisLazyToolsTest`, `CompositionWorkflowTest`); land with their subsystems.

## Recommended merge order

Top line: **normalize EOL → leaf additions → image core → GPU → ui hubs → plugins → python/packaging.**

1. **Step 0 (required):** decide EOL strategy — diffing CRLF working tree vs LF
   snapshot is unusable otherwise. Compare against git blobs (LF) or normalize
   the snapshot; do NOT mass-convert the fork tree.
2. **Step 1 — zero-conflict absorbs:** `krita/` 6 icons + qrc entries;
   `build-tools/gmic/`; test-data (`gimp_*.gpl`, scenario JSONs); 123
   upstream-superset files after spot-checking none resurrect removed features
   (checked: no Popup Palette / On-Canvas Brush Editor code in the set).
3. **Step 2 — `libs/image` + `libs/gpu` + `libs/pigment`:** clip stack, dust
   detector, min dab size, GPU passes, `KisGpuEngineSettings` gating; re-add
   fork's `KisGpuPerspectivePass`/spectral/view-res registration into upstream
   CMakeLists; merge `KoCompositeOpRegistry`/`KoCompositeOps.h` (fork spectral +
   upstream edits). Run `libs/image`+`libs/gpu`+`libs/pigment` ctest.
4. **Step 3 — `libs/ui` hubs:** node/layer managers (interleave clip actions with
   fork's re-render/forget actions), clipboard/mimedata, canvas stack, dialogs;
   land `lazytools/`, `assetlibrary/`, `brushstrokelayer/` subdirs and reconcile
   the monoliths (audit fork deltas vs upstream splits — see open questions).
5. **Step 4 — plugins in dependency order:** `impex/libkra`+`impex/psd` (clip
   round-trip) → `dockers` (composition, assetlibrary, quickaccess, restnote,
   layerdocker, textproperties) → `filters` (`solsticeeffects` after dust
   detector; blur highlights; colorsfilters GPU) → `tools` (eraser, dust,
   transform2) → `paintops` (libpaintop merge incl. MinDabSize) → `extensions`
   (testcontrol, qmic, pykrita) → `impex/svg` (keep fork export).
6. **Step 5 — `plugins/python`:** merge CMakeLists both ways; copy 9 upstream
   `.action` files; keep all 12 fork-only dirs; delete/ignore `moss*.diff/rej`.
7. **Step 6 — `krita/` + packaging + build-tools:** action/xmlgui merges,
   `main.cc` take upstream, `windows_stub_main.cpp` keep fork,
   `package-complete.py` manual merge, `build-windows.ps1` absorb.
8. **Step 7 — reconcile `/mcp`:** kritamcp + testcontrol port/tool split per
   megaplan Phase 2.

## Open questions for the user

1. **No merge base:** `ac468d9` and `e88c2289` are absent from the repo's object
   store; the 1.1.1 drop has no git metadata. Can the upstream repository (or a
   bundle) be obtained so a real `git merge`/3-way tool can run? Otherwise this
   is a ~426-file manual hunk merge.
2. **Working-tree state:** 46 files are both uncommitted-dirty and
   upstream-changed (spectral, view-res vector, SmartMatting, stroke layer,
   launcher, Node.sip). Commit/stash in-flight work before merging, or merge
   against the dirty tree?
3. **EOL:** adopt a policy (`.gitattributes` normalization, or per-file LF) —
   the snapshot is LF and matches fork blobs; simplest is "upstream file wins
   on EOL" but that rewrites the working tree.
4. **Shared-feature direction:** quickaccess/restnote/lazytools/assetlibrary/
   brushstrokelayer — upstream versions are larger and restructured. Take
   upstream wholesale and port fork deltas (recommended), or keep fork files
   and port upstream features? Fork `QuickAccessDock.cpp`/`RestNoteDock.cpp`/
   `KisSolsticeLazyTools.cpp`/`KisBrushStrokeLayer.cpp` deltas need a delta
   audit either way.
5. **`plugins/python` scope:** megaplan treats it as fork-only; upstream ships
   extra `.action` files and modified shared dirs — confirm adopting them
   (recommended: yes; they register upstream plugin actions).
6. **qmic bridge:** fork's `kis_qmic_simple_convertor.cpp` is ~500 lines larger
   than upstream's rewrite — is the fork's version carrying needed work, or is
   it the older copy to discard?
7. **kritamcp vs testcontrol:** keep both (env-gated) or fold kritamcp into the
   C++ server? testcontrol is Qt6-only; the fork's Python path also runs Qt5.
8. **`libstdc++.so.6` appimage blob** differs — take upstream's?
9. **`packaging/` strategy:** 62 EOL-only + `package-complete{,-msvc}.py` —
   confirm packaging stays fork-side except gmic/testcontrol additions.
10. **VisionML `.qrc`/README/README docs:** upstream added visionml icons to
    `kritavisionml.qrc` and rewrote `README.md` — fork versions have SmartMatting
    entries; merge both.

_Generated from verified tree diff; regenerate the raw lists with the same
size+hash+EOL-normalized compare before each merge pass._
