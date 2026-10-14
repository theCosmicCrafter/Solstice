# Custom feature inventory — agent reference

Where Solstice diverges from upstream Krita 6 (`krita/6.0`). Use it to find
every file a feature touches, to avoid breaking another feature's hook, and to
resolve upstream merge conflicts. Update this file whenever a custom touch
point is added, moved, or removed.

Regenerate the raw list with:

```bash
git diff --stat -w --ignore-cr-at-eol $(git merge-base krita-sol krita/6.0) krita-sol -- . ':!plugins/visionml/thirdparty'
```

## Documented features

| Feature | Own directory / files | Hooks in upstream files | Docs |
| --- | --- | --- | --- |
| GitHub Actions trial build | `.github/workflows/windows-build.yml`, `build-tools/github-actions/` | `packaging/windows/package-complete.py` (application QML import path for separate prefixes) | `docs/test-builds.md`, `docs/agent/github-actions.md` |
| Explorer thumbnails (Krita Shell Extension) | `build-tools/windows-shellex/` | `build-tools/github-actions/build-windows.ps1` (`--pre-zip-hook`, BUILD-INFO line); no upstream files | `docs/windows-shell-thumbnails.md`, `docs/agent/windows-shell-thumbnails.md` |
| Executable name (solstice.exe); package root launcher `Solstice.exe` (2026-10-13) | `krita/windows_root_launcher.cpp` | `krita/CMakeLists.txt` (stub `OUTPUT_NAME`, target `solstice_root_launcher`), `krita/versioninfo.rc.in`, `krita/main.cc` (application name), `packaging/windows/package-complete.py` (launcher copied to the root), `packaging/windows/package-complete-msvc.py` (same), `build-tools/github-actions/build-windows.ps1` (required launcher, version in the package name and BUILD-INFO), `run-krita.bat`, `build-tools/paint-trace/run.cmd` | `docs/test-builds.md`, `docs/agent/executable-name.md` |
| Quick Access Manager | `plugins/dockers/quickaccess/` (stroke previews through `KisBrushStrokePreviewCache` in `QuickAccessStrokePreviews`) | `plugins/dockers/CMakeLists.txt`, `libs/widgets/KisVisualColorSelector.*` (`setStretchLimit()` for HueSVC) | `docs/quick-access.md`, `docs/agent/quick-access.md` |
| Rest Note | `plugins/dockers/restnote/` | `plugins/dockers/CMakeLists.txt`; enable setting: `chkRestNoteEnabled` in `libs/ui/forms/wdggeneralsettings.ui` (Custom tab) and `GeneralTab` in `libs/ui/dialogs/kis_dlg_preferences.cc` | `docs/rest-note.md`, `docs/agent/rest-note.md` |
| Asset Library | `plugins/dockers/assetlibrary/`, `libs/ui/KisWelcomeAssetLibraryWidget.*` | `plugins/dockers/CMakeLists.txt`, `libs/ui/KisWelcomePageWidget.*`, `libs/ui/forms/KisWelcomePage.ui`, `libs/ui/CMakeLists.txt` | `docs/asset-library.md`, `docs/agent/asset-library.md` |
| Vision ML | `plugins/visionml/` (incl. vendored `thirdparty/`) | `plugins/CMakeLists.txt` | `docs/vision-ml.md`, `docs/agent/vision-ml.md` |
| Lazy Tools | `libs/ui/KisSolsticeLazyTools.*` | `libs/ui/KisViewManager.cpp`, `libs/ui/CMakeLists.txt`, `libs/ui/forms/wdggeneralsettings.ui`, `libs/ui/dialogs/kis_dlg_preferences.*`, `plugins/dockers/layerdocker/LayerBox.*`, `WdgLayerBox.ui`, `krita/kritamenu.action` | `docs/lazy-tools.md`, `docs/agent/lazy-tools.md` |
| Puppet Warp | Strategy/args/serialization inside `plugins/tools/tool_transform2/`, icons in `krita/pics/tool_transform/` | Transform Tool sources and tests, `krita/pics/tool_transform/tool-transform-icons.qrc` | `docs/puppet-warp.md`, `docs/agent/puppet-warp.md` |
| GPU Engine (branch `krita-sol-gpu`) | `libs/gpu/` (`kritagpu`, shaders, tests), `libs/image/gpu/`, `libs/image/tiles3/KisTileGpu*`, `libs/ui/opengl/KisGpuCanvasUploader.*`, `libs/ui/KisGpuEngineUi.*`, `Kis*Gpu*Test.cpp` | `libs/CMakeLists.txt`, tile engine (`libs/image/tiles3/`), `kis_async_merger.*`, `kis_updater_context.cpp`, OpenGL canvas (`libs/ui/opengl/`), `libs/ui/KisMainWindow.cpp`, `libs/ui/dialogs/kis_dlg_preferences.*`, `libs/ui/KisImportExportManager.cpp`, `plugins/impex/libkra/tests/` (`KisGpuSaveTest`), `plugins/paintops/defaultpaintops/brush/kis_brushop.{h,cpp}` (sequential GPU dab batch; CPU and GPU minimum update periods), `libs/ui/tool/strokes/freehand_stroke.cpp` (first update and after-batch re-check), `libs/ui/tool/strokes/kis_painter_based_stroke_strategy.cpp` (masked strokes on the CPU), `libs/image/kis_indirect_painting_support.*`, `libs/image/kis_paint_layer.cc` (CPU-painted temporary target), `plugins/paintops/defaultpaintops/brush/KisBrushOpResources.h`, `KisDabRenderingJob.cpp` (per-stroke GPU dab decision), `libs/image/gpu/KisGpuEngineSettings.*` (`Solstice/GpuBrush`, on by default), CMake files; full list in `docs/agent/gpu-engine.md` ("Status and locations") | `docs/gpu-engine.md`, `docs/agent/gpu-engine.md` |
| Brush option shared model (phase 1: Deform pilot; phase 2a: brush tip and masking brush values; phase 2b: Pixel Brush) | `libs/ui/KisPaintOpOptionsModel.*`, `plugins/paintops/libpaintop/KisPaintOpOptionStateUtils.h`, `plugins/paintops/libpaintop/KisBrushTipOptionData.*`, `plugins/paintops/libpaintop/KisBrushBasedOptionStates.*`, `plugins/paintops/deform/tests/`, `plugins/paintops/defaultpaintops/brush/tests/KisBrushTipOptionParityTest.cpp` and `data/brushtip/` | `libs/image/brushengine/kis_paintop_settings.*`, `KisPaintOpPresetUpdateProxy.*`, `kis_paintop_preset.cpp` (changed-key delivery); `libs/ui/kis_paintop_settings_widget.*`, `libs/ui/kis_paintop_box.cc`, `libs/ui/CMakeLists.txt`; `plugins/paintops/libpaintop/KisCurveOptionModel.*`, `KisStandardOptionData.*`; `plugins/paintops/deform/kis_deform_paintop_settings_widget.cpp`, `plugins/paintops/deform/CMakeLists.txt` (static library and tests); phase 2a: `plugins/paintops/libpaintop/KisAutoBrushModel.*`, `KisPredefinedBrushModel.*` (static `bakedOptionData()`, `effectiveResourceData()`), `kis_brush_option_widget.*` and `KisMaskingBrushOption.*` (one value, external cursor constructors), `plugins/paintops/libpaintop/CMakeLists.txt`, `plugins/paintops/defaultpaintops/brush/tests/CMakeLists.txt`; phase 2b: `libs/ui/KisPaintOpOptionsModel.*` (`addDependency()`, `writeOption()`), `plugins/paintops/libpaintop/KisStandardOptionData.*` (cursor variants), `KisTextureOptionModel.*` (static `bakedOptionData()`), `KisPaintOpOptionStateUtils.h` (`bakeLinkedCurveOption()`), `kis_brush_based_paintop_options_widget.*` (brush tip cursor constructor), `plugins/paintops/defaultpaintops/brush/kis_brushop_settings_widget.*` (model-based editor), `README.md` | `docs/brush-editor.md`, `docs/agent/brush-option-shared-model-plan.md`, `docs/agent/brush-option-shared-model-phase0.md` |
| UI modernization (phases 1-4) | `krita/data/themes/SolsticeDark.colors`, `libs/widgetutils/KisSolsticeStyle.*`, `libs/widgetutils/tests/KisSolsticeStyleTest.cpp` | `krita/data/themes/CMakeLists.txt` (install list); `libs/widgets/KoDockWidgetTitleBar.*` (`setSolsticeLookEnabled()`, `updateSolsticeLook()`, title band in `paintEvent()`); `libs/ui/KisMainWindow.cpp` (constructor sets the title bar flag, `customizeTabBar()` adds tab and toolbar style sheets and tab expansion); `libs/ui/dialogs/kis_dlg_preferences.*` (`m_chkSolsticeInterface`, `Solstice/ModernInterface`, compact Tree sidebar in `KisDlgPreferences`); `libs/ui/KisApplication.cpp` (startup creates `KisSolsticeStyle` for widgetStyle `Solstice`); `libs/ui/KisMainWindow.cpp` (Styles menu entry and `slotUpdateWidgetStyle()`); `libs/widgetutils/CMakeLists.txt`, `libs/widgetutils/tests/CMakeLists.txt`; interface scale: `krita/main.cc` (`QT_SCALE_FACTOR` from `SolsticeInterfaceScale` in `kritadisplayrc`), `libs/ui/dialogs/kis_dlg_preferences.*` (`m_spnInterfaceScale`); bundled UI font: `krita/data/fonts/` (Cantarell, `fonts.qrc`, OFL), `krita/CMakeLists.txt` (`krita_QRCS`), `krita/data/CMakeLists.txt`, `libs/ui/KisUiFont.{h,cpp}` (`registerBundledFonts()`, default family), `krita/main.cc`, `LICENSES/OFL-1.1.txt`; local proxy style bases through `KisSolsticeStyle::createStyle()` in `libs/widgetutils/KisMenuStyleDontCloseOnAlt.cpp`, `libs/widgetutils/config/krecentfilesaction.cpp`, `libs/ui/widgets/kis_color_filter_combo.cpp`, `plugins/dockers/layerdocker/LayerBox.cpp`, `plugins/dockers/storyboarddocker/StoryboardView.cpp` | `docs/ui-modernization.md`, `docs/agent/ui-modernization-plan.md` |
| Brush stroke previews in the toolbar popup and Brush Editor | none | `libs/ui/kis_paintop_box.cc` (`enableStrokePreviewSetting()` on the popup), `libs/ui/widgets/kis_paintop_presets_editor.cpp` (preview mode, hidden Display section, "Preview Size") | `docs/brush-stroke-preview.md`, `docs/agent/brush-stroke-preview.md` |
| Brush preset grouping | none (code in existing classes) | `libs/resourcewidgets/KisResourceItemListView.{h,cpp}` (`setGrouping()`, `doItemsLayout()`, `paintEvent()`); `libs/ui/widgets/KisPresetDockerFilters.{h,cpp}` (grouping dropdown, `groupOf()`); `libs/ui/widgets/kis_preset_chooser.cpp` (passes the view); `libs/ui/tests/KisBrushStrokePreviewTest.cpp` (`testDockerGrouping`); `kritarc` key `Solstice/BrushPresetGrouping` | `docs/brush-stroke-preview.md`, `docs/agent/brush-preset-grouping.md` |
| Brush option shared model, phase 4: Color Smudge | `plugins/paintops/colorsmudge/tests/KisColorSmudgeParityTest.cpp`, `tests/data/colorsmudge/` | `plugins/paintops/colorsmudge/kis_colorsmudgeop_settings_widget.{h,cpp}` (model-based editor, dependencies, Tool Options ids), `plugins/paintops/colorsmudge/CMakeLists.txt` (static library), `tests/CMakeLists.txt`; `plugins/paintops/defaultpaintops/brush/tests/KisBrushTestMain.h` (`SOLSTICE_BRUSH_TEST_MAIN_WITH_BUNDLES`) | `docs/brush-editor.md`, `docs/agent/brush-option-shared-model-plan.md` |
| Brush option shared model, phase 4: Sketch and Bristle | `plugins/paintops/sketch/tests/`, `plugins/paintops/hairy/tests/` (parity tests, bundled presets, references), `plugins/paintops/defaultpaintops/brush/tests/KisPaintOpParityTestUtils.h` | `plugins/paintops/sketch/kis_sketch_paintop_settings_widget.{h,cpp}`, `plugins/paintops/hairy/kis_hairy_paintop_settings_widget.{h,cpp}` (model-based editors, Tool Options ids), `plugins/paintops/sketch/KisSketchOpOptionWidget.cpp`, `plugins/paintops/hairy/KisHairyBristleOptionWidget.cpp` (page parameters), `plugins/paintops/{sketch,hairy}/CMakeLists.txt` (static libraries, tests), `plugins/paintops/libpaintop/kis_brush_option_widget.cpp` (`hideOptions()` drops hidden parameters), `libs/ui/kis_paintop_option.*` (`removeToolOptionsParameter()`) | `docs/brush-editor.md`, `docs/agent/brush-option-shared-model-plan.md`, `docs/agent/tool-options-brush.md` |
| Brush option shared model, phase 4: Tangent Normal, Hatching, Filter, Quick Brush | `plugins/paintops/{tangentnormal,hatching,filterop,roundmarker}/tests/` (parity tests, bundled presets, references) | the four settings widgets (model-based editors, Tool Options ids) and `CMakeLists.txt` files (static libraries, tests); page parameters in `KisTangentTiltOptionWidget.cpp`, `KisHatchingOptionsWidget.cpp`, `KisHatchingPreferencesWidget.cpp`, `KisRoundMarkerOpOptionWidget.cpp`, `plugins/paintops/libpaintop/KisFilterOptionWidget.cpp`; `KisFilterOptionModel.*` (static `bakeOptionData()`), `KisBrushBasedOptionStates.*` (`bakeFilterOption()`) | `docs/brush-editor.md`, `docs/agent/brush-option-shared-model-plan.md`, `docs/agent/tool-options-brush.md` |
| Brush option shared model, phase 4: Curve, Grid, Particle, Shape | `plugins/paintops/{curvebrush,gridbrush,particle,experiment}/tests/` (parity tests, bundled presets, references) | the four settings widgets (model-based editors, Tool Options ids) and `CMakeLists.txt` files (static libraries, tests); page parameters in `KisCurveOpOptionWidget.cpp`, `KisGridOpOptionWidget.cpp`, `KisParticleOpOptionWidget.cpp`, `KisExperimentOpOptionWidget.cpp`, `plugins/paintops/libpaintop/KisColorOptionWidget.cpp` | `docs/brush-editor.md`, `docs/agent/brush-option-shared-model-plan.md`, `docs/agent/tool-options-brush.md` |
| Brush option shared model, phase 4: Spray and MyPaint | `plugins/paintops/spray/tests/`, `plugins/paintops/mypaint/tests/KisMyPaintParityTest.cpp` and `tests/data/mypaint/` (bundled brushes, references) | `plugins/paintops/spray/kis_spray_paintop_settings_widget.cpp`, `plugins/paintops/mypaint/MyPaintPaintOpSettingsWidget.cpp` (model-based editors, Tool Options ids), `plugins/paintops/spray/CMakeLists.txt` (static library, tests), `plugins/paintops/mypaint/tests/CMakeLists.txt`; page parameters in `KisSprayOpOptionWidget.cpp`, `MyPaintBasicOptionWidget.cpp`; `libs/ui/KisPaintOpOptionsModel.*` (`addSharedKey()`) | `docs/brush-editor.md`, `docs/agent/brush-option-shared-model-plan.md`, `docs/agent/tool-options-brush.md` |
| Brush option shared model, phase 4: Clone | `plugins/paintops/defaultpaintops/brush/tests/KisCloneParityTest.cpp`, `data/clone/` | `plugins/paintops/defaultpaintops/duplicate/kis_duplicateop_settings_widget.{h,cpp}` (model-based editor, Tool Options ids), `KisDuplicateOptionWidget.cpp` (page parameters), `brush/tests/CMakeLists.txt` | `docs/brush-editor.md`, `docs/agent/brush-option-shared-model-plan.md`, `docs/agent/tool-options-brush.md` |
| Brush option shared model, phase 5: old path retired | `KisToolOptionsBrushTest::testLockWritesModelOption` | `libs/ui/kis_paintop_box.cc` (`slotGuiChangedCurrentPreset()`: no clear-and-rewrite for normal edits; warning safety net), `libs/ui/kis_paintop_settings_widget.cpp` (`lockProperties()` writes the model's option) | `docs/agent/brush-option-shared-model-plan.md` |
| Removal of the Popup Palette and the On-Canvas Brush Editor | `docs/agent/popup-palette-removal.md` | removed `plugins/dockers/brushhud/`, `libs/ui/kis_popup_palette.*`, `libs/ui/forms/WdgPopupPaletteSettings.ui`; `plugins/dockers/CMakeLists.txt`, `libs/ui/CMakeLists.txt`, `libs/ui/canvas/kis_canvas2.{h,cpp}`, `libs/ui/KisMainWindow.cpp`, `libs/ui/tool/kis_tool_paint.{h,cc}`, `libs/ui/dialogs/kis_dlg_preferences.{h,cc}`, `libs/ui/kis_config.{h,cc}`, `libs/ui/widgets/KisDockerHud.cpp` (defaults), includes in `input/kis_input_manager_p.cpp`, `input/KisPopupWidgetAction.cpp`, `kis_favorite_resource_manager.cpp` | `README.md`, `docs/quick-access.md`, `docs/brush-editor.md` |
| Brush stroke layer (stage 1 technical check, stage 2a recording, stage 2b redraw on scaling, stage 2c .kra saving, redraw with the Transform Tool, selection moves and the background record check, partial redraw) | `libs/ui/KisBrushStrokeLayer.{h,cpp}`, `libs/ui/KisBrushStrokeLayerIO.cpp`, `libs/image/KisRedrawableLayerInterface.{h,cpp}`, `plugins/paintops/defaultpaintops/brush/tests/KisBrushStrokeReplayTest.cpp`, `KisBrushStrokeLayerTest.cpp`, `KisBrushStrokeLayerTransformTest.cpp` | `libs/ui/tool/strokes/freehand_stroke.{h,cpp}` (recording), `kis_painter_based_stroke_strategy.{h,cpp}` (`setAdditionalUndoCommand()`, `setSequentialDabRendering()`), `libs/ui/tool/kis_tool.{h,cc}`, `kis_tool_freehand.h` (`supportsBrushStrokeLayer()`), `libs/ui/kis_layer_manager.{h,cc}`, `kis_node_manager.cpp`, `krita/krita.action`, `plugins/dockers/layerdocker/LayerBox.cpp` (creation), `libs/image/processing/kis_transform_processing_visitor.{h,cpp}` (redraw hook, pixel transform for the mismatching part), `plugins/impex/libkra/kis_kra_tags.h`, `kis_kra_savexml_visitor.cpp`, `kis_kra_save_visitor.{h,cpp}`, `kis_kra_loader.cpp`, `kis_kra_load_visitor.{h,cpp}` (.kra saving), `plugins/tools/tool_transform2/strokes/inplace_transform_stroke_strategy.{h,cpp}`, `transform_stroke_strategy.cpp` (Transform Tool redraw), `plugins/tools/basictools/strokes/move_selection_stroke_strategy.cpp` (selection moves carry the strokes), `libs/ui/CMakeLists.txt`, `libs/image/CMakeLists.txt`, brush tests `CMakeLists.txt` | `docs/brush-stroke-layer.md`, `docs/agent/brush-stroke-layer-plan.md` |
| Photoshop brush (ABR) import (phase 2 parser, phase 3 patterns, phase 4 presets, phase 5 folders as tags) | `libs/brush/KisAbrParser.{h,cpp}`, `libs/brush/KisAbrPresetConverter.{h,cpp}`, `libs/brush/tests/TestAbrParser.cpp`, `plugins/paintops/defaultpaintops/brush/tests/KisAbrPresetTest.cpp` | `libs/brush/kis_abr_brush_collection.{h,cpp}` (old parser replaced, patterns), `libs/brush/KisAbrStorage.cpp` (patterns, presets, folder tags), `plugins/paintops/libpaintop/kis_predefined_brush_chooser.cpp` (tip bundle filter), `libs/brush/CMakeLists.txt` (`kritapsdutils`), brush tests `CMakeLists.txt` | `docs/photoshop-brushes.md`, `docs/agent/abr-import-plan.md` |
| Brush options in Tool Options (phase 3a: eyes on option rows, Brush section; phase 3b: page parameters, shape tools) | `libs/ui/KisToolOptionsBrushItems.*`, `libs/ui/KisToolOptionsParameters.*`, `libs/ui/tool/KisToolOptionsBrushSection.*`, `plugins/paintops/defaultpaintops/brush/tests/KisToolOptionsBrushTest.cpp`, `KisBrushTestMain.h` | `libs/ui/kis_paintop_option.*` (Tool Options id, shown flag), `libs/ui/kis_categorized_list_model.h` (two roles), `libs/ui/kis_paintop_options_model.*`, `libs/ui/kis_categorized_item_delegate.*` (eye column), `libs/ui/kis_paintop_settings_widget.*` (`setPaintOpId()`, `toolOptionsOptions()`), `libs/ui/kis_paintop_box.*` (`sigCurrentSettingsWidgetChanged()`, `currentSettingsWidget()`), `libs/ui/tool/kis_tool_paint.*` (`showsBrushOptions()`), `libs/ui/tool/kis_tool_freehand.h`, `plugins/tools/basictools/kis_tool_line.h`, `libs/ui/CMakeLists.txt`, Pixel Brush and Deform settings widgets (`withToolOptionsId()`), brush tests `CMakeLists.txt`; phase 3b: `KisPaintOpOption::addToolOptionsParameter()`, `KisPaintOpSettingsWidget::toolOptionsParameterId()`, `plugins/paintops/libpaintop/kis_brush_option_widget.cpp`, `KisCompositeOpOptionWidget.cpp`, `KisPaintingModeOptionWidget.cpp`, `KisTextureOptionWidget.cpp`, `KisCurveOptionWidget.cpp` (registration), `libs/widgetutils/kis_slider_spin_box.*` (`exponentRatio()` getters), `libs/ui/tool/kis_tool_shape.*` (`createOptionWidgets()`, `moveBrushSectionLast()`), `kis_tool_rectangle_base.cpp`, `kis_tool_paint.*` (`createBrushOptionsSection()`), `plugins/tools/basictools/kis_tool_{rectangle,ellipse}.h`, `plugins/tools/tool_polygon/kis_tool_polygon.h`, `plugins/tools/tool_polyline/kis_tool_polyline.h`; kritarc `Solstice/ToolOptionsBrushItems/<paintop>`, `Solstice/ToolOptionsBrushCollapsed` | `docs/brush-editor.md`, `docs/agent/tool-options-brush.md` |
| Brush Editor name and thumbnail after Q (previous preset) and pen/eraser switches (Krita bug: only the settings followed) | none | `libs/ui/kis_paintop_box.cc` (`slotSwitchToPreviousPreset()`, `slotInputDeviceChanged()` call `m_presetsEditor->resourceSelected()`, as `resourceSelected()` and `restoreResource()` do) | `docs/brush-editor.md` |
| Option list scroll bars no longer white while the window has focus (Krita bug) | none | `libs/ui/widgets/kis_categorized_list_view.{h,cpp}` (`updateScrollBarPalettes()`, `changeEvent()`): the view replaces its active Window color with the text color for its checkboxes, and the scroll bars inherited it; they now keep the inactive Window color. Test: `KisToolOptionsBrushTest::testOptionListScrollBarColor` | `docs/agent/wiki/log.md` |
| Brush Presets: scroll to the selected preset (setting, off by default) | none (code in existing classes) | `libs/resourcewidgets/KisResourceItemListView.{h,cpp}` (`setFollowCurrentItem()`, `resizeEvent()`), `libs/resourcewidgets/KisResourceItemChooser.{h,cpp}` (`setViewCurrentIndex()`), `libs/ui/widgets/kis_paintop_presets_chooser_popup.{h,cpp}` (`enableScrollToSelectionSetting()`), `plugins/dockers/presetdocker/presetdocker_dock.cpp`, `libs/ui/tests/KisBrushStrokePreviewTest.cpp`; `kritarc` key `Solstice/BrushPresetScrollToSelection` | `docs/brush-stroke-preview.md`, `docs/agent/brush-preset-scroll.md` |
| Failing tests fixed (2026-10-08): per-test profile, PSD text strings, JXL layer names, Storyboard model signals | none | `libs/global/KisSolsticePaths.{h,cpp}` (`profileRoot()` in test mode), `libs/psdutils/cos/kis_cos_writer.cpp` (`writeString()`), `plugins/impex/jxl/JPEGXLImport.cpp`, `plugins/dockers/storyboarddocker/{StoryboardModel,CommentModel}.cpp`, tests `kis_transform_mask_test.cpp`, `TestKisSwatchGroup.cpp`, `TestFallBackColorTransformation.cpp`, `KisSolsticePathsTest.cpp` | `docs/agent/wiki/pitfalls/build-format-test.md` |
| Test Fontconfig configuration (2026-10-08) | none | `KoTestConfig.h.cmake` (`KRITA_FONTCONFIG_DIR_FOR_TESTS`), `sdk/tests/simpletest.h` (`kisTestSetupFontconfig()`), `sdk/tests/kistest.h` | `docs/agent/wiki/pitfalls/build-format-test.md` |
| Test MIME database; empty storage location | none | `sdk/tests/CMakeLists.txt` (`kritatestmimedatabase` OBJECT library added to `kritatestsdk`), `libs/resources/KisResourceStorage.cpp` (`autoDetectStorageType()`), `libs/resources/tests/TestResourceStorage.cpp` | `docs/agent/wiki/pitfalls/build-format-test.md` |
| Application name in interface strings; update checks off | none | about 88 files of `i18n`/`.ui`/`.action`/Python strings ("Krita" -> "Solstice" where it means the application), top-level `CMakeLists.txt` (`ENABLE_UPDATERS` OFF), `libs/ui/KisWelcomePageWidget.cpp` (news option text, Source Code link); pre-release pass 2026-10-13: `libs/global/kis_assert.cpp`, `libs/ui/animation/KisAnimationRender.cpp`, `libs/ui/animation/VideoImportDialog.ui`, `plugins/extensions/buginfo/DlgKritaLog.cpp`, `plugins/extensions/pykrita/plugin/krita/__init__.py`, `plugins/paintops/libpaintop/forms/wdgincremental.ui`, three Python plugin `Manual.html`, `libs/widgetutils/xmlgui/khelpmenu.cpp` (Report Bug opens the Solstice GitHub issues) | `docs/versioning.md`, `docs/agent/solstice-visual-branding-todo.md` |
| Welcome page News: Solstice releases below Krita news (2026-10-13) | `libs/ui/tests/data/rss_feeds/releases.atom` | `libs/ui/forms/KisNewsPage.ui`, `libs/ui/widgets/KisNewsWidget.{h,cpp}`, `libs/ui/KisRssReader.{h,cpp}` (Atom entries), `libs/ui/KisWelcomePageWidget.cpp` (menu text), `libs/ui/tests/KisMultiFeedRssModelTest.{h,cpp}` | `docs/versioning.md`, `docs/agent/welcome-news.md` |
| Settings location (`%APPDATA%\Solstice`, Krita profile import) | `libs/global/KisSolsticePaths.*`, `libs/global/KisSolsticeProfile.*`, their tests, `cmake/modules/SolsticeEmbedRcc*.cmake`, `krita/kritarc-defaults.qrc` | `krita/main.cc` (`prepareSolsticeProfile()`, `finishSolsticeImport()`, defaults mount), `krita/CMakeLists.txt`; callers in `docs/agent/settings-location.md` ("Phase 1 result") | `docs/settings-folder.md`, `docs/agent/settings-location.md` |
| Versioning (Solstice `0.1.0-alpha`, Krita compatibility version kept) | none | `CMakeLists.txt` (`SOLSTICE_VERSION_*`), `libs/version/kritaversion.h.cmake`, `libs/version/KritaVersionWrapper.*` (`solsticeVersionString()`), `krita/main.cc` (`KAboutData`), `krita/versioninfo.rc.in`, `krita/krita*.exe.manifest.in`, `plugins/extensions/pykrita/kritarunner/kritarunner*.exe.manifest.in`, `libs/global/KisUsageLogger.cpp`, `plugins/extensions/buginfo/dlg_buginfo.cpp`, `libs/metadata/kis_meta_data_filter_p.cc` (`CreatorTool`); `libs/ui/widgets/kis_preset_chooser.h` (`eventFilter` protected for sip) | `docs/versioning.md`, `docs/agent/versioning.md` |
| Overview live update | none | `plugins/dockers/overview/overviewwidget.{h,cc}` (live update members and slots), `plugins/dockers/overview/CMakeLists.txt` (Qt Concurrent), `libs/ui/dialogs/kis_dlg_preferences.*` (`m_chkOverviewLiveUpdate`, `Solstice/OverviewLiveUpdate`) | `docs/overview-live-update.md`, `docs/agent/overview-live-update.md` |
| Docker locks | none (code in `KisMainWindow`) | `libs/ui/KisMainWindow.{h,cpp}`: `event()` override, Dockers submenu actions, `updateSolsticeDockLocks()`, `slotSolsticeDockLocksToggled()`, `solsticeSeparatorLocked()`, `resetSolsticeSeparatorCursor()`, the destructor's docker signal disconnect; `kritarc` keys `Solstice/LockDockedDockerWidths`, `Solstice/LockDockedDockerHeights` (floating prevention removed 2026-10-07) | `docs/docker-locks.md`, `docs/agent/docker-locks.md` |
| Visual branding | `krita/pics/branding/`, splash data, `krita/pics/mimetypes/`, `krita/pics/svg/support-krita.svg`, MSIX and macOS package images (temporary gray placeholders), `pics/*_application-x-{krita,krz}.svg` and `pics/krita.png` (2026-10-13) | top-level `CMakeLists.txt` (Beta/Plus branding use Next), `krita/data/splash/splash.qrc`, `packaging/windows/package-complete*.py` (Krita shortcuts not copied), `packaging/windows/installer/` (`installer_solstice.nsi` replaces `installer_krita.nsi` and its translations; `MakeInstallerNsis.cmake.in`, `ConfigureInstallerNsis.cmake`), `packaging/windows/msix/` (manifest, `build_msix.py`, `ConfigureMsixFiles.cmake`, tile assets), `build-tools/ci-scripts/build-windows-package.py` (installer folder), `krita/main.cc` (short description), `libs/ui/KisWelcomePageWidget.cpp` (support and manual links hidden), `krita/CMakeLists.txt` (Windows ICO size limit), `libs/ui/kis_splash_screen.cpp`, `libs/ui/dialogs/kis_about_application.cpp`, `libs/widgetutils/config/kstandardaction_p.h` ("Configure/About Solstice"), `krita/kritamenu.action`, `krita/versioninfo.rc.in` | `docs/visual-branding.md`, `docs/agent/solstice-visual-branding-todo.md` |

GPU phase 4.12 also touches `KisDabRenderingQueue.{h,cpp}` and
`KisDabRenderingExecutor.{h,cpp}` under `plugins/paintops/defaultpaintops/brush/`
to bound GPU brush batches by completed source pixel bytes, with queue tests.

GPU phase 4.32 adds bulk-read hooks to
`libs/image/tiles3/kis_tiled_data_manager.cc` and deferred synchronization to
`kis_tile_data{,_interface}.h`. Both interleaved and planar CPU reads batch
stale GPU tiles while holding swap read locks; existing copy/stride behavior
and individual iterator synchronization remain intact.

GPU phase 4.34 adds a sequential GPU readback before the concurrent CPU
masking patches in `libs/ui/tool/strokes/kis_painter_based_stroke_strategy.cpp`.
The masking formulas and patch partitioning remain unchanged. Phase 4.35
changes the brush staging-context choice in `KisGpuBrushPainter.cpp` to reuse
existing large buffers without exceeding the existing memory cap.

GPU phase 4.101 (masking brush composite on the GPU) adds the new
`libs/gpu/KisGpuMaskingCompositePass.*`, `libs/gpu/shaders/masking_composite.comp`
and `libs/image/gpu/KisGpuMaskingWorker.*` (both `CMakeLists.txt` lists). Upstream
touch points: `dstDevice()`/`compositeOpId()` in
`libs/ui/tool/strokes/KisMaskingBrushRenderer.{h,cpp}`; `m_gpuMasking`, the
temporary-target flag and the GPU job in `doMaskingBrushUpdates()` in
`libs/ui/tool/strokes/kis_painter_based_stroke_strategy.{h,cpp}`;
`m_maskedStroke` and the masked GPU-brush decision and update period in
`plugins/paintops/defaultpaintops/brush/kis_brushop.{h,cpp}`.

GPU phase 4.102 (textured dabs on the GPU) changes upstream files:
`KisTextureOption::gpuDescription()`, `alphaCompositeOpId()`, `applyAlphaMask()` and the
`KisTextureCpuApplier` class in `plugins/paintops/libpaintop/kis_texture_option.{h,cpp}`
(`apply()` now calls `applyAlphaMask()`); `alphaBytes()` in
`plugins/paintops/libpaintop/KisTextureMaskInfo.{h,cpp}`; `gpuTexture()` and the textured
description branch in `plugins/paintops/defaultpaintops/brush/KisDabRenderingJob.{h,cpp}`;
the textured branch of `materialize()` in `libs/image/KisRenderedDab.h`.

GPU phase 4.103 (image tip dabs on the GPU) changes upstream files:
`KisQImagePyramid::createImage()` in `libs/brush/kis_qimage_pyramid.{h,cpp}` draws
described transforms with `KisImageTipSampler` instead of QPainter (the old body is
`createImageWithQPainter()`; new `describeImage()`, `renderDescribed()`,
`describedPixel()`, the premultiplied copy of each level); `KisBrush::proceduralImageDab()`
in `libs/brush/kis_brush.{h,cpp}`; `libs/brush/tests/CMakeLists.txt`
(`KisQImagePyramidDescribeTest`); `libs/image/CMakeLists.txt` (`KisImageTipSampler.cpp`).
GPU phase 4.104 extends `KisBrush::proceduralImageDab()` (color space and Lightness
strength arguments, the application tables) and changes the gradient branch of
`generateMaskAndApplyMaskOrCreateDab()` in `libs/brush/kis_brush.cpp` (transparent tip
pixels take their own gradient color); `plugins/paintops/defaultpaintops/brush/tests/CMakeLists.txt`
(`KisImageTipDescriptionTest`).
GPU phase 4.105 accepts RGBA F16 dabs in `KisBrush::proceduralImageDab()` (`libs/brush/kis_brush.cpp`)
and in `KisTextureOption::gpuDescription()` (`plugins/paintops/libpaintop/kis_texture_option.cpp`,
half constants); `libs/gpu/CMakeLists.txt` (two RGBA16F textured shader builds).
GPU phase 4.106 adds the RGBA16F masking composite build to `libs/gpu/CMakeLists.txt`
(`masking_composite_rgba16f`); no other upstream file changes.

`FreehandStrokeTest` (2026-10-12): `libs/ui/tests/freehand_stroke_test.{h,cpp}` (`initTestCase()`
loads the Krita 3 bundle, LOD strokes seeded), regenerated references in
`libs/ui/tests/data/freehand/` (auto textured 17, Mix dull) and new
`libs/ui/tests/data/freehand-lod/`.

GPU phases 4.36-4.37 forward per-channel layer flags in
`libs/image/gpu/KisGpuMergeBatch.*`, allow F16 channel masks in
`KisGpuProjectionCompositor.cpp`, and align the established F16 generic blend
arithmetic in `libs/gpu/shaders/composite_blend.glsl`. Projection tests cover
major modes, all channel masks, partial updates and transparent boundaries.

GPU phases 4.38-4.40 add per-submission upload arenas in `KisGpuTileAccess.*`
and `KisGpuProjectionCompositor.cpp`, bounded resource reclamation outside
the retirement lock in `KisGpuTileBackend.*`, and host-cached transfer-only
staging in `libs/gpu/KisGpuBuffer.*`. Paint-device tests cover suballocation,
GPU lifetime, failed submissions and concurrent retirement.

GPU phase 4.41 adds `tryEvictGpuTileDataBatch` to
`libs/image/tiles3/kis_tile_data_store.{h,cc}`, a batch hook in
`KisTileGpuState.h`/`KisTileGpuHooksStub.cpp`, and bounded F32/F16 eviction
readbacks in `KisGpuTileBackend.*`. No slot is released until CPU content is
valid; failed transfers preserve GPU copies. See the dedicated eviction tests.

GPU phases 4.43-4.45 add `duplicateCpuSnapshot` in `kis_tile_data_store.{h,cc}`
for GPU COW without redundant initialization, `KisTile::cloneShared` in
`kis_tile.{h,cc}` for whole-tile `bitBlt` sharing without CPU synchronization,
and batched partial-clear boundary reads in `kis_tiled_data_manager.cc`.
`KisGpuTileAccess.cpp` uses the snapshot copy under its existing residency or
swap protection. F32/F16 tests cover stale snapshots, current/old exact/rough
copies, partial copies, clear boundaries, submit failure and Undo/Redo.

GPU phase 4.46 adds RGBA16F Normal/Erase dab composition in
`libs/gpu/KisGpuDabCompositor.*`, `shaders/paint_dabs.comp` and shader CMake
entries. `KisGpuBrushPainter.cpp` passes the storage size and restricts F16
modes; `kis_brushop.{h,cpp}` admits both floating image depths while retaining
the owning-image scheduling gate. `KisGpuBrushTest` covers half arithmetic,
asynchronous lifetime and rollback; `KisGpuStrokeTest` covers actual F16
Buildup jobs. Phases 4.47-4.48 add F16 hard/creamy Alpha Darken and Normal/Erase
Wash preview/final merging. `shaders/composite_half_brush.glsl` shares scalar
half Normal/Erase arithmetic between dabs and `composite_layers.comp`.
The layer descriptors in `KisGpuLayerCompositor.*` and
`KisGpuProjectionCompositor.*` carry explicit half-brush/flag semantics so
ordinary layer projection retains its existing arithmetic. Tests cover both
Alpha Darken CPU variants, every Wash channel mask, failure/budget fallback
and actual Wash jobs with GPU counters and Undo/Redo. Phases 4.49-4.50 extend
F16 dabs and Wash to the basic generic modes through Pin Light. They reuse
`compositeGeneric` with half channel masks and per-dab storage rounding,
extend single-layer F16 coverage in `KisGpuProjectionCompositor`, and correct
Exclusion's half intermediate product in `composite_blend.glsl`. The separate
half-brush arithmetic flag remains specific to Normal/Erase. Phases 4.51-4.52
add a lazy extended F16 dab pipeline and enable Soft Light SVG, Color Dodge,
Color Burn and HSY Color/Hue/Saturation/Luminosity for Buildup/Wash. The shared
F16 brush support predicate in `KisGpuLayerCompositor.h` also gates Wash mask
coverage. Half Dodge/Burn round CPU inversion/clamping intermediates in
`composite_blend.glsl`; the other extended F16 brush modes retain CPU fallback.

GPU phase 4.53 updates `KisGpuProjectionCompositor.*` to track timeline values
per leased context, prefer completed/oldest contexts, and allow three pending
serial submissions. Resource creation and waits run outside the pool mutex;
failed waits leave command/table data untouched. `KisGpuBrushTest` adds gated
projection coverage for F32/F16, masks, Alpha Lock, source lifetime, bounded
reuse, failed-submit CPU replay and exact Undo/Redo.

GPU phases 4.54-4.55 add batched CPU readback in
`libs/image/filter/kis_filter.cc` before filter input conversion/processing
and selected or separate-destination copies. `kis_convolution_worker_fft.h`
also batches the FFT cache region and the row span locked by repeat-border
iterators. Both hooks are guarded by `HAVE_KRITA_GPU_ENGINE`.
`KisGpuPaintDeviceTest` covers actual Invert/Gaussian filters, F32/F16,
fractional selections, separate destinations, download failure recovery,
retained snapshots and exact Undo/Redo.

GPU phases 4.56-4.57 add readback hooks in
`libs/image/kis_transform_worker.cc`: full and partial affine transforms,
explicit-axis mirrors and centered mirror helpers prefetch existing device
tiles before CPU iteration. `KisGpuPaintDeviceTest` covers F32/F16 scaling,
shear, rotations, translation, both mirrors, failed downloads, snapshots and
exact Undo/Redo. Transform math and the final default-pixel purge are retained.

`KisGpuPaintDeviceTest::testTransformSequenceUndo` additionally covers
separate flip/rotation transactions and intermediate Undo/Redo states.
The Transform Tool Undo investigation is closed; temporary tool diagnostics
were removed without changing its established history behavior. See
`docs/agent/wiki/history/gpu-phases-4.17-4.57.md` ("Closed Transform Tool Undo investigation")
for the real-app findings.

GPU phase 4.58 adds the opt-in `libs/image/KisPaintTrace.*` CPU timeline and
`KisPaintTraceTest`, plus capture/summary helpers under `build-tools/paint-trace/`.
Phase 4.69 adds offline `timing.py` and regression coverage: joined-check inputs
are timed from a unique Qt input receipt to the last required upload's verified
swap acknowledgment, with per-stroke statistics and explicit exclusions. It adds
no native hooks and is not a physical input-to-pixel or effective GPU-path proof.
Phase 4.70 adds `paths.py` and trace markers at successful submissions in
`KisGpuBrushPainter.cpp` / `KisGpuProjectionCompositor.cpp`, CPU replay in
`KisGpuMergeBatch.cpp`, CPU apply in `kis_async_merger.cpp`, brush CPU branches
in `kis_brushop.cpp`, and completed upload-source inspection in
`kis_opengl_canvas2.cpp`. These provide per-input evidence, not pure-GPU classification.
Phase 4.71 adds reuse, skip and recalculation decisions in `kis_async_merger.cpp`
and `KisLayer::updateProjection` in `kis_layer.cc`. `paths.py` keeps them distinct
from CPU/GPU execution and preserves missing evidence for other walkers.
Phase 4.95 (Puppet Warp, user request) adds the mesh ARAP model:
- new `libs/image/KisPuppetTransformWorker.*` and its test
  `libs/image/tests/KisPuppetTransformWorkerTest.cpp`;
- `puppetMesh()/setPuppetMesh()/usesPuppetMesh()/createPuppetWorker()/puppetRotations()`
  plus copy, equality, transform and XML (`mesh`) in `tool_transform_args.*`;
- the mesh branches in `kis_transform_utils.cpp`;
- `ensurePuppetMesh()`, `cachedPuppetWorker()`, the image/thumbnail maps and the mesh overlay in
  `kis_warp_transform_strategy.cpp`;
- the serialization checks in `tests/test_animated_transform_parameters.cpp`.
Pin selection, click action and order (same phase, user request):
- `RUBBER_BAND` mode, multi-pin move and rotate, and `changePuppetOrder()` in
  `kis_warp_transform_strategy.*`;
- `PuppetClickAction`, `PuppetOrderChange`, `m_puppetOrders` and XML `orders` in `tool_transform_args.*`;
- the code-built "Click pin" combo and Order buttons in `kis_tool_transform_config_widget.*`;
- `slotPuppetOrderChange()` in `kis_tool_transform.*`;
- order levels (`orderAt()`, `orderLevels()`, `OrderFilterOp`) in `libs/image/KisPuppetTransformWorker.*`.
Puppet Warp mesh with the Accurate preview (2026-10-07, user request):
- `sigPreviewDeviceReady` and the preview job in
  `plugins/tools/tool_transform2/strokes/inplace_transform_stroke_strategy.{h,cpp}`;
- `createThumbnail()`, `slotInplacePreviewDeviceGenerated()`, `m_inplacePreviewDevice` and the
  mask source in `initGuiAfterTransformMode()` in `kis_tool_transform.{h,cc}`;
- `setPuppetMaskSource()` in `kis_warp_transform_strategy.{h,cpp}`;
- the copy for warps without control points in `kis_transform_utils.cpp`.
Phase 4.96 adds GPU rendering of Puppet Warp's mesh model:
- the GPU branches, `GroupRecordersOp` and `isGpuEnabled()` in `libs/image/KisPuppetTransformWorker.{h,cpp}`
  (reusing `KisGpuGridWarpWorker`); group layers take the destination's offset;
- tests: `KisGpuPaintDeviceTest::testGpuPuppetMatchesCpu` and the benchmark's mesh Puppet Warp rows.
Phase 4.98 adds the GPU Gaussian convolution (Gaussian Blur, Unsharp Mask, Gaussian High Pass):
- new `libs/gpu/KisGpuSeparableConvolutionPass.*` and `libs/gpu/shaders/separable_convolution.comp`
  (four shaders in `libs/gpu/CMakeLists.txt`);
- new `libs/image/gpu/KisGpuConvolutionWorker.*` (in `libs/image/CMakeLists.txt`);
- `runsOnGpu()` and the GPU branch of `applyGaussian()` in `libs/image/kis_gaussian_kernel.{h,cpp}`;
- the virtual `KisFilter::prefersSingleCall()` in `libs/image/filter/kis_filter.{h,cc}`, used in
  `libs/ui/tool/strokes/kis_filter_stroke_strategy.cpp` and overridden in
  `plugins/filters/blur/kis_gaussian_blur_filter.{h,cpp}`;
- tests: `KisGpuPaintDeviceTest::testGpuGaussianMatchesCpu`, `testGpuGaussianPatchesMatchCpu`,
  `testGpuGaussianFiltersMatchCpu`, the benchmark's GPU convolution rows, and the new
  `libs/ui/tests/KisGpuFilterStrokeTest.cpp` (in `libs/ui/tests/CMakeLists.txt`).
Phase 4.97 adds the GPU Liquify grid warp:
- new `libs/gpu/KisGpuGridWarpPass.*` and `libs/gpu/shaders/grid_warp.comp` (claim and resolve
  shaders in `libs/gpu/CMakeLists.txt`);
- new `libs/image/gpu/KisGpuGridWarpWorker.*` (in `libs/image/CMakeLists.txt`);
- the GPU branch in `KisLiquifyTransformWorker::run()` (`libs/image/kis_liquify_transform_worker.cpp`);
- `KisFourPointInterpolatorBackward::coefficients()` in `libs/image/kis_four_point_interpolator_backward.h`;
- tests: `KisGpuPaintDeviceTest::testGpuLiquifyMatchesCpu` and the benchmark's GPU Liquify row.
Phase 4.94 adds GPU affine transform passes:
- new `libs/gpu/KisGpuTransformPass.*` and `libs/gpu/shaders/transform_pass.comp`;
- new `libs/image/gpu/KisGpuTransformWorker.*`;
- `planGpuPass()/runGpuPasses()` and `m_wholeDevice` in `libs/image/kis_transform_worker.*`;
- `KisFilterWeightsApplicator::setupLine()` in `libs/image/kis_filter_weights_applicator.h`;
- `KisGpuDeviceInfo::supportsFloat64` and the optional `shaderFloat64` in `libs/gpu/KisGpuContext.*`;
- `KisPaintDevice::invalidateCachedBounds()` (`libs/image/kis_paint_device.*`), called by write
  accesses in `libs/image/gpu/KisGpuTileAccess.cpp`;
- tests: `KisGpuPaintDeviceTest::testGpuTransformMatchesCpu` and the benchmark's GPU affine row.
Phase 4.92 adds `KisCanvas2::slotCanvasCacheUpdated()` (`libs/ui/canvas/kis_canvas2.*`): immediate
`updateCanvasProjection()` on the shared GPU upload path instead of `frameRenderStartCompressor`
(`KRITA_GPU_CANVAS_IMMEDIATE_UPLOAD=0` restores it).
Phase 4.91 shares compute pipelines per context: `KisGpuContext::sharedComputePipeline()` and
`sharedComputePipelineCompileCount()` in `libs/gpu/KisGpuContext.*`, `shared_ptr` pipelines in
`KisGpuLayerCompositor`, `KisGpuDabCompositor` (both with `preparePipelines()`),
`KisGpuLayerStackCompositor`, `KisGpuTileFill` and `KisGpuCanvasPatchWriter`;
`KisGpuTileBackend::preparePipelines()` and the background start in `KisGpuEngineUi::install()`
(`libs/ui/KisGpuEngineUi.cpp`); tests `KisGpuEngineTest::testSharedComputePipelines` and
`KisGpuBrushTest::testPreparePipelines`.
Phase 4.90 adds replace layers for the Wash base copy: `KisGpuLayerCompositor::Layer::replace`
(`libs/gpu/KisGpuLayerCompositor.*` with the 32-byte `LayerParams` fill color, flag 8 and mask handling in
`libs/gpu/shaders/composite_layers.comp`), `KisGpuProjectionCompositor::Layer::replace`,
`KisGpuBrushPainter::paintWashPreview(..., base)/washBaseCopyCount()`, and the combined path
in `KisPaintLayer::copyOriginalToProjection()`; tests `KisGpuBrushTest::testWashPreviewBaseCopy`
and the base-copy assertion in `KisGpuStrokeTest`.
Phase 4.89 adds the chained GPU brush update job in `FreehandStrokeStrategy::tryDoUpdate()`
(`libs/ui/tool/strokes/freehand_stroke.cpp`) and paint-trace scopes in
`KisPaintLayer::copyOriginalToProjection()` (`libs/image/kis_paint_layer.cc`) and
`KisAsyncMerger::startMerge()` (`libs/image/kis_async_merger.cpp`).
Phase 4.88 shortens the GPU brush update period: `KisBrushOp::gpuMinimumUpdatePeriod()`
and `setGpuMinimumUpdatePeriodForTesting()` (`KRITA_GPU_BRUSH_MIN_UPDATE_MS`) applied in
`doAsynchronousUpdate()` of `plugins/paintops/defaultpaintops/brush/kis_brushop.*`, and the
initial period in `libs/ui/tool/strokes/freehand_stroke.cpp`; unmirrored blend-mode rows
in `KisGpuStrokeTest`.
Phases 4.86/4.87 add RGBA16F and Soft (curve) generated dabs:
`KisProceduralCircleDab::halfPixels/matchesPixel()/softFadeAt()`, Soft accessors in
`libs/image/kis_curve_circle_mask_generator.*`, the Soft table cache and F16 color in
`libs/brush/kis_auto_brush.*`, F16 acceptance in `KisDabRenderingJob.cpp`, the 112-byte
`KisGpuDabCompositor::Circle` with curve tables, `paint_dabs.comp::roundToHalf()/softFade()`;
tests in `KisGpuBrushTest` (F16 rows, Soft shapes) and `KisGpuStrokeTest::testSoftStroke`.
Phase 4.85 skips CPU generation of verified described dabs: `KisRenderedDab::pixelsPending`
and `materialize()` in `libs/image/KisRenderedDab.h`, `KisProceduralCircleDab::render()`;
`describeWithoutPixels()`, the per-kind verification gate and Postprocess handling in
`plugins/paintops/defaultpaintops/brush/KisDabRenderingJob.*`; propagation in
`KisDabRenderingQueue.cpp`; materialization before CPU use and `materializedDabCount()` in
`kis_brushop.*`; test hook `KisGpuBrushPainter::refusePendingBatchesForTesting()`; tests
`testPendingDabsFallBackToMaterializedPixels` and `KisGpuStrokeTest::testRefusedPendingBatches`.
Phase 4.84 adds `KisGaussCircleMaskGenerator::vectorCoefficients()` in
`libs/image/kis_gauss_circle_mask_generator.*`, read accessors in
`libs/image/kis_antialiasing_fade_maker.h`, Gaussian support in
`KisAutoBrush::proceduralCircleDab()` and the fused/exact math in
`KisProceduralCircleDab.h` and `paint_dabs.comp`; tests `testGeneratedCircleDabsExact`
and `KisGpuStrokeTest::testGaussStroke`.
Phase 4.83 (priority 3 step 1) adds `libs/image/KisProceduralCircleDab.h` and
`KisRenderedDab::procedural/proceduralFlips`; `KisCircleMaskGenerator::vectorCoefficients()`
in `libs/image/kis_circle_mask_generator.*`; `KisAutoBrush::proceduralCircleDab()` in
`libs/brush/kis_auto_brush.*`; description and pixel self-check in
`plugins/paintops/defaultpaintops/brush/KisDabRenderingJob.*`, propagation in
`KisDabRenderingQueue.*` and flip tracking in `kis_brushop.cpp` (CPU mirror jobs).
GPU side: `KisGpuDabCompositor::Circle`/`Dab::generated`, `paint_dabs.comp::generatedCircle()`,
`KisGpuBrushPainter::generatedDabCount()`; tests in `KisGpuBrushTest` and `KisGpuStrokeTest`.
Phase 4.82 adds an optional `stoppedByByteLimit` result to
`plugins/paintops/defaultpaintops/brush/KisDabRenderingQueue.*` /
`KisDabRenderingExecutor.*` and, in `kis_brushop.cpp`, a zero update period
after GPU batches cut by the source byte budget (`batch.byte_limited` trace link);
`tests/KisDabRenderingQueueTest.cpp` checks the flag.
Phase 4.81 batches concurrent canvas projection updates: new
`libs/ui/canvas/KisCanvasUpdateBatcher.*` (in `libs/ui/CMakeLists.txt`) and
`KisCanvasUpdateBatcherTest` (in `libs/ui/tests/CMakeLists.txt`); upstream touch
points are `kis_canvas2.cpp` (`startUpdateCanvasProjection()` batching path),
`kis_canvas_updates_compressor.*` (`putUpdateInfos()`),
`kis_abstract_canvas_widget.h` / `kis_canvas_widget_base.*`
(`sharesProjectionUploads()`, `startUpdateCanvasProjections()` defaults),
`kis_opengl_canvas2.*` (overrides, batch-wide GL hold), `KisOpenGLCanvasRenderer.*`,
`kis_opengl_image_textures.*` and `KisOpenGLUpdateInfoBuilder.*`
(`buildUpdateInfos()`; single-rect build delegates). `KisGpuCanvasUploader.*`
adds per-region source accesses in one submission and nested GL holds.
Phase 4.80 adds optional queue-lock/driver timestamps in `libs/gpu/KisGpuContext.*`
and `KisGpuCommandList.*`, deferred external-span recording in paint trace/tile
access, and four-thread submission/readback/timeline/failure regression coverage.
Phase 4.79 adds early main-buffer finalization in `libs/gpu/KisGpuCommandList.*`
and invokes it before residency locking in tile access; preamble and queue order
remain unchanged. Engine tests cover late preamble ordering and failed reuse.
Phase 4.78 adds residency-holder scopes in `gpu/KisGpuTileBackend.cpp` and
`gpu/KisGpuTileAccess.cpp`, a documented 10us hold threshold in `KisPaintTrace.cpp`,
and process/owner/thread overlap analysis in `build-tools/paint-trace/residency.py`.
Phase 4.77 adds opt-in tile preparation and submission substage scopes in
`gpu/KisGpuTileAccess.cpp`, preserving residency-lock lifetime and call order.
Phase 4.76 adds opt-in flow/job-linked CPU scopes in `KisGpuCanvasUploader.cpp`
and `gpu/KisGpuProjectionCompositor.cpp` for context reuse, preparation and
submission. Original waits, call order and failure handling remain unchanged.
Phase 4.75 adds explicit walker-to-ready joins and per-stroke CPU merge and
post-merge preparation intervals; ambiguous/missing records are excluded.
Phase 4.74 extends `overhead.py` with direct-parent update-ready to issue
intervals for deduplicated verified uploads, excluding ambiguous timestamps;
these mixed-condition diagnostics are not per-input latency partitions.
Phase 4.73 adds offline `overhead.py`: verified input intervals are partitioned
at their last required upload issue, with timestamp validation and regression
tests. It introduces no native hooks and is not a GPU execution-time measurement.
Hooks cover `kis_image.cc` (enqueue/end request), `kis_async_merger.cpp`,
`libs/ui/input/kis_input_manager.cpp`, `tool/kis_tool_freehand.cc`,
`tool/strokes/freehand_stroke.cpp`, `canvas/kis_canvas2.cpp`,
`opengl/kis_opengl_canvas2.cpp`, and brush `KisDabRenderingJob.cpp`/`kis_brushop.cpp`.

Phase 4.59 extends those hooks with scoped input identities, IDs in
`canvas/kis_update_info.*`, merge/replacement records in that implementation and
`canvas/kis_canvas_updates_compressor.cpp`, and upload issuance in
`opengl/kis_opengl_image_textures.cpp`. The tracing-only per-widget coverage
model is `canvas/KisCanvasPaintTrace.h`, tested by `KisCanvasPaintTraceTest`.
Its IDs and frame links do not yet establish input-to-pixel latency.

Phase 4.60 adds job creation/execution/destruction IDs in
`libs/image/kis_stroke_job.h` and thread-local job scopes in `KisPaintTrace.*`.
Nested CPU spans carry the executing job ID; summary tooling reports explicit
creation ancestry and scheduling intervals. This does not attribute every input
consumed by a dab batch or connect input pixels through projection yet.

Phase 4.61 adds request IDs to `KisDabRenderingJob.*`, records each consumed
request in `KisDabRenderingQueue.*`, and passes the diagnostic batch ID through
`KisDabRenderingExecutor.*`. `kis_brushop.cpp` records batch-ready, painting-job
and dirty-rectangle-recording boundaries. `KisDabRenderingQueueTest` covers
cache/postprocess membership across split batches. The trace `Scope` accepts
an optional record ID for matching generation spans to logical requests.

Phase 4.68 records collected/submitted dirty regions in `freehand_stroke.cpp`
and upload image bounds in `kis_opengl_canvas2.cpp`. Offline bridge checks link
dirty groups to projection requests and same-canvas compressed updates to uploads,
and become prerequisites in the input-level readiness report.

Phase 4.67 adds diagnostic input-to-stroke membership in `kis_tool_freehand.cc`
using a transient per-tool QObject property (no ABI/config change). The offline
summary joins per-input branch/geometry checks with explicit condition records
and reports exclusions; it does not publish latency.

Phase 4.66 extends `KisCanvasPaintTrace` with mapping checks and view-change
invalidation; `kis_opengl_canvas2.cpp` records per-upload image/widget patch
geometry. `geometry.py` compares those regions with render/blit tracking and
same-widget swap acknowledgments, retaining unsupported/offscreen cases.

Phase 4.65 adds rectangle/LOD events to `KisPaintTrace`, emitted by
`kis_simple_update_queue.cpp`, `kis_update_job_item.h` and `kis_canvas2.cpp`.
`build-tools/paint-trace/geometry.py` verifies LOD-0 projection request regions
and per-canvas notification regions; upload/presentation geometry is not yet
certified.

Phase 4.64 extends the offline summary with an all-recorded-branches audit,
terminal-stage diagnostics and same-canvas checks for update replacement edges.
It does not change native painting or certify geometric region coverage.

Phase 4.63 adds bounded stroke-start GUI condition snapshots in `KisPaintTrace`
and `kis_tool_freehand.cc`, plus per-input request/batch completeness auditing in
`build-tools/paint-trace/summarize.py`. It does not yet establish full-region
display coverage or an interactive latency baseline.

Phase 4.62 connects batch dirty regions to projection/canvas updates:
`kis_painter.{h,cc}`/`kis_painter_p.h` keep bounded diagnostic batch IDs until
the dirty drain; `freehand_stroke.cpp` passes explicit dirty context including
the deferred masked-brush callback. `kis_base_rects_walker.h`,
`kis_simple_update_queue.cpp` and `kis_update_job_item.h` preserve request and
walker identities across split/merge/recalculate/execute. `kis_canvas2.cpp`
records the direct projection-to-prepared-update link. Flow scope and queue
tests cover those boundaries; the summary still makes no pixel-latency claim.
It records raw spans/events, not validated per-input presentation latency;
see the measurement scope and remaining work in `docs/agent/wiki/history/gpu-phases-4.58-4.81.md`.

Brush Stroke Preview adds `KisBrushStrokePreviewRenderer.*` and
`KisBrushStrokePreviewCache.*` under `libs/ui/widgets/`, sharing the F5
stroke/background implementation with `kis_preset_live_preview_view.cpp`.
It touches `kis_preset_chooser.*`, `kis_paintop_presets_chooser_popup.*` and
`plugins/dockers/presetdocker/presetdocker_dock.cpp` for the docker-only layout;
`libs/resourcewidgets/KisResourceItemChooser.*` for the opt-in bottom controls
and restoration from an already active horizontal strip;
`KisPresetDockerFilters.*` for per-docker multi-select engine/bundle menus and
`KisTagFilterResourceProxyModel.*` for additional per-view metadata/storage facets;
`KisResourceLocator.*` for independent saved snapshots; `kis_image.{h,cc}`
for stroke-start notification; stroke random sources and `freehand_stroke.*`
for preview-only seeding; and `MyPaintPaintOp.cpp` for tool-independent preview
tracking. Build/test entries are in
`libs/ui/CMakeLists.txt` and `libs/ui/tests/`, including
`KisBrushStrokePreviewTest`. See `docs/brush-stroke-preview.md` and
`docs/agent/brush-stroke-preview.md` for lifecycle and compatibility rules.

GPU phase 4.11 changes `libs/image/tiles3/kis_tile_data_pooler.cc` to avoid
speculative CPU-clone downloads of GPU-only tiles after commits, and adds
GPU test friend access to the existing pooler suspend/resume helpers in
`kis_tile_data_store.h`.

GPU phase 4.8 also touches `libs/image/kis_paint_layer.cc` and
`libs/image/kis_indirect_painting_support.cpp` to batch readbacks before Wash
preview/final CPU compositing. Phase 4.9 also routes unrestricted Wash preview
through `KisGpuBrushPainter::paintWashPreview` from `kis_paint_layer.cc`.
Phase 4.15 overrides `writeMergeData` in `kis_paint_layer.{h,cc}` to GPU-merge
unrestricted Normal/Erase Wash rectangles inside the existing final-merge
transaction and barrier jobs; other indirect-painting subclasses are unchanged.
Full-stroke parity and timing coverage lives in
`plugins/paintops/defaultpaintops/brush/tests/KisGpuStrokeTest.cpp`.

Lazy Tools menu suppression also uses `libs/ui/utils/KisMenuMnemonicFilter.*`
and `libs/ui/tests/KisMenuMnemonicFilterTest.cpp`, registered in the UI and
test CMake files. The menu-bar-owned observer handles late XMLGUI menus/title
updates without intercepting Alt key events. See `docs/agent/lazy-tools.md`.

## Undocumented custom changes

These have no dedicated documents yet. When modifying one, create its user and
agent documents first (see "Adding a new feature" below).

| Change | Files |
| --- | --- |
| Export Region (`dninosores_export_region`, `Alt+Shift+E`): exports the selection, or the active layer bounds | `libs/ui/KisMainWindow.*` (`slotExportRegion()`), `krita/kritamenu.action`, `krita/krita5.xmlgui` |
| Desktop-only cleanup: Android sources, donation dialog, supporter bundles, logcat dumper removed | `libs/global/CMakeLists.txt`, `krita/data/CMakeLists.txt`, `libs/ui/dialogs/`, `libs/ui/animation/`, `plugins/extensions/buginfo/`, `plugins/extensions/resourcemanager/`, `krita/krita5.xmlgui`, `packaging/android/` |
| Android leftovers removed (2026-10-08): every `Q_OS_ANDROID` conditional resolved as undefined (71 C++ files, about 2,190 lines), `ANDROID` CMake branches, the S-Pen settings plugin, `StaticMessages.sh`, the Android Qt-patch flags, `KisDocument::autoSaveOnPause()`, the `krita.android` log category (`dbgAndroid`), `KisMediaEncoderFormat::Type::AndroidMediaEncoder`, the welcome page's Android supporter page and banner, the preferences' Android warning | many files; see the commit. Notable: `config-qt-patches-present.h.cmake`, `krita/data/kritarc` (`[SPenSettings]`), `libs/ui/forms/KisWelcomePage.ui` (+ generated `KisWelcomePage_ui.py`), `libs/ui/forms/wdggeneralsettings.ui` (+ `wdggeneralsettings_ui.py`), `build-tools/ci-scripts/`, `metainfo.yaml` |
| Small behavior fixes | `libs/ui/kis_clipboard.cc` + `libs/ui/tests/kis_clipboard_test.*` (empty clipboard handling), `libs/ui/KisImportExportManager.cpp` (dialog parent), `libs/ui/KisWidgetWithIdleTask.h`, `libs/widgets/KisVisualRectangleSelectorShape.*`, `plugins/dockers/layerdocker/NodeDelegate.cpp` (tooltip hit area), `libs/ui/KisLayerThumbnailCache.cpp` + `libs/ui/tests/KisLayerThumbnailCacheTest.cpp` (Layers docker thumbnails keyed by node address, see `docs/agent/wiki/pitfalls/weak-pointer-map-keys.md`) |
| Python binding return types match libkis (2026-10-14, Krita bug): `Node.save()`, `Filter.apply()` and `Scratchpad.setScale()` return `bool` to Python instead of `None` | `plugins/extensions/pykrita/sip/krita/Node.sip`, `Filter.sip`, `Scratchpad.sip`; see [wiki pitfall](wiki/pitfalls/sip-return-types.md) |
| Thread-safe projection updates filter stack (2026-10-14, Krita bug): `KisImage` guards `projectionUpdatesFilters` with a mutex and readers iterate a snapshot; fixes the crash in `enableDirtyRequests()` during repeated Python `Node.save()` | `libs/image/kis_image.cc` (`projectionUpdatesFiltersLock`, `projectionUpdatesFiltersSnapshot()`), `libs/image/tests/KisImageProjectionUpdatesFilterTest.cpp`; see [wiki pitfall](wiki/pitfalls/qt6-detach-on-iteration.md) |
| Development launcher | `run-krita.bat` |
| Generated `*_ui.py` inspection artifacts (not built; see `docs/agent/coding-rules.md`) | `libs/ui/forms/`, `libs/ui/animation/`, `plugins/impex/png/`, `plugins/dockers/storyboarddocker/`, `plugins/dockers/widegamutcolorselector/`, `plugins/tools/tool_transform2/` |

## Cross-feature hot spots

Several features share these upstream files. Keep each feature's edit small,
self-contained, and commented with the feature name so merges and reviews stay
tractable:

- `libs/ui/KisViewManager.cpp` — per-window feature construction/action setup.
- `libs/ui/KisMainWindow.*` — window-level actions; GPU engine load/save hooks.
- `libs/ui/forms/wdggeneralsettings.ui` and `libs/ui/dialogs/kis_dlg_preferences.cc`
  — General > Custom settings shared by several features.
  The GPU engine adds its group to the Performance tab from code.
- `krita/kritamenu.action`, `krita/krita5.xmlgui` — global actions and menus.
- `plugins/dockers/layerdocker/` — Layers docker extensions.
- `plugins/tools/tool_transform2/` — Puppet Warp and upstream transform modes.

## Adding a new feature

1. Choose the extension point (`docs/agent/codebase-map.md`,
   `docs/agent/extension-points.md`).
2. Create the implementation, preferring a new plugin directory.
3. Create `docs/<feature>.md` (user workflow, scope, limitations, screenshots
   in `images/`).
4. Create `docs/agent/<feature>.md` using the template below.
5. Add a README entry that contains only the feature name and a link to
   `docs/<feature>.md`.
6. Add the agent document to the index in `AGENTS.md` and a row to the
   inventory table above.

### Agent document template

```markdown
# <Feature> — agent development notes

## Status and locations
- Implementation: `plugins/...`
- Hooks in upstream files: ...
- Original reference (if ported): ...

## Build, test, and install
(targets, ctest names, cmake_install.cmake paths)

## Configuration and compatibility
(kritarc keys, data file paths, legacy formats, persisted ids)

## Architecture and lifecycle
(ownership, observers, event filters, threading, undo)

## Invariants
(behavior that must not change without user approval)

## Manual regression checklist
1. ...

## Known limitations / improvement plan
```
