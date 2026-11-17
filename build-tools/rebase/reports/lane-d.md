# Lane D merge report — plugins/dockers + plugins/extensions (41 files)

Status: **complete**. `rebase.py scan --lane d` → 0 files with conflict markers.
`git diff --check` → clean (only autocrlf LF→CRLF notices). `__init__.py` →
`python -m compileall` OK.

## Merge mechanics note (important for reviewers)

`rebase.py merge` runs `git merge-file <worktree> <HEAD> <snapshot>`. For every
lane-D file except `LayerBox.cpp` the worktree equalled HEAD (fork work is
*committed*), so the 3-way merge degenerated to "take upstream" — the merged
files came out byte-identical to the 1.1.1 snapshot with **no conflict markers
and no trace of the fork's deltas**. That is the intended shape for this
rebase (upstream becomes the new base), but it means every file needed a
fork-delta audit to prove nothing was silently dropped.

Method: pre-merge backups of all 44 lane files → normalized diffs vs snapshot
→ per-file "orphan" analysis (fork lines present nowhere in upstream
`plugins/`, `libs/`, `krita/` trees) → method-name-level check for wholesale
removals → semantic read of each unmatched block against the upstream file
that absorbed it.

Result: **upstream is a superset almost everywhere.** Only two files needed
manual fork-delta ports, plus two small fork-restoration edits.

## Restructure cases — audit conclusions

### quickaccess monolith → upstream 29-file split: upstream superset, no port
Fork `QuickAccessDock.cpp` (2,184 lines) and `QuickAccessGridEditDialog.cpp`
(963 lines) dissolved into upstream's split. Every fork-unique feature was
found intact upstream, usually improved:

| Fork feature (monolith) | Upstream home | Verdict |
| --- | --- | --- |
| `attachItemDrag`/`dragTarget`/`cancelItemDrag` + `eventFilter` (Ctrl+drag item move) | `QuickAccessItemDragController.cpp` | Equivalent (same GridCellSpacing=2, rubber-band highlight, deferred commit via `moveItem`) |
| `upgradeLegacySettings` (legacy settings JSON → KConfig groups) | `QuickAccessSettingsStore::importLegacy` | **Upstream superset** — also migrates `blender_mode_list`→BlendModes, gesture `Enabled`/`ShowPreview`, `huesvc_enabled`→HueSVCEnabled |
| Migration flow (`MigrationVersion` < 2, `importDirectory`, `repairBrushSpans`) | `QuickAccessProfileLoader::load` | **Upstream superset** — adds `normalizeIconItemSpans` on top of `repairBrushSpans` |
| `aliasFor`/`compatibleString` (custom_name/customName compat) | `QuickAccessItemPresentation.cpp`, `QuickAccessGridItemButton.cpp`, `QuickAccessGesturePresentation.cpp` | Same helpers, same semantics |
| Grid-edit widgets (`isStrokePreview`, `strokePreviewSize`, `updateStrokePreview`, `itemId`, `updateSelectionStyle`, `configureAppearance` incl. `#3a263f`/`#263a2f` colors, `isResizable`/`resizeVertically`/`onResizeHandle`, `iconPath`, `paintEvent`) | `QuickAccessGridItemButton.cpp` / `QuickAccessGridCanvas.cpp` | Functionally identical bodies verified |
| `showDropHighlight`/`clearDropHighlight`/`updateSelectionStyles` | `QuickAccessGridCanvas.cpp` | Present |
| Tool-options pad (`ensureToolOptionsPad`, `setToolOptionsPadVisible`, `resizeToolOptionsPad`, `positionToolOptionsPad`, `returnToolOptionsDocker`) | `QuickAdjustToolOptionsPad.cpp` | **Upstream superset** — adds `ToolOptionsPosition` config, screen-bounds clamping, docker-visibility restore, app-state show/hide |
| `showPreview` (gesture preview QFrame) | `QuickAccessGesturePreview.cpp` | Same stylesheet/cells/deferred-move; config keys `Enabled`/`ShowPreview`/`MinimumPixelsToMove` identical |
| `QuickAdjustKeyController` window lookups | same file | **Upstream superset** — tracks `m_activeWindow`/`m_activeViewManager`, deactivates on app/window deactivate (multi-window fix) |
| Model error strings (6 cases) | `QuickAccessModel.cpp` | Same validations, now `i18n()`-wrapped |
| `QuickAccessSettingsDialog.h` (52−) | upstream header | 0 orphan lines — fully absorbed |

### restnote monolith → split: upstream superset, no port
- `RestNoteConfig` struct: **byte-identical** fields, same JSON schema, same
  file path (`rest_note/config/main.json`).
- **Legacy key preserved**: `Solstice/RestNoteEnabled` read at
  `RestNoteDock.cpp:53` — no migration shim needed (resolves the workflow's
  open item).
- Micro breaks + idle detection: `microEnabled`/`microIntervalMinutes`/
  `microSkipThresholdSeconds`/`idleEnabled`/`idleThresholdSeconds` all present
  in `RestNoteConfig.h`, `RestNoteTimer.cpp` (same Running/Idle/Paused/Break/
  **MicroBreak** machine, same `std::min` clamp on config change),
  `RestNoteSettingsDialog.cpp`, `RestNoteNotifications.cpp` (`BreakOverlay`/
  `MicroToast` moved here, `createEyeBreak`/`cancelEyeBreak` helpers).
- `maybeStartMicroBreak` → folded into `RestNoteTimer::tick` (same
  `m_remaining <= microSkipThresholdSeconds` skip check).

### assetlibrary: upstream superset, no port
Fork's `AssetLibrarySettings`/`AssetPathEntry`/`Section`/`collectSections`/
thumbnail cache/insert operations → `libs/ui/assetlibrary/`:
- `KisAssetLibrarySettings`: same JSON file, same keys incl. `settings_json`
  legacy KConfig fallback (line 54), `auto_columns`, `thumbnail_size`,
  per-path `extensions`, font sizes with `font_size` legacy fallback +
  `object.remove("font_size")` on save, splitter/expanded/collapsed sizes.
  Adds `welcome` defaults + notifier.
- `KisAssetLibrary::collectSections` / `thumbnailColumns` /
  `KisAssetLibraryThumbnailCache` (encapsulates fork's `m_thumbnailCache`
  pixelRatio cache keys).
- `insertAsLayer`/`insertAsFileLayer`/`requestAssetPath` in
  `KisAssetLibraryOperations` — **byte-for-byte semantics verified**
  (KisImportCatcher svg→KisShapeLayer, KisFileLayer parent pick,
  `slotNonUiActivatedNode`, filename validation).
- `AssetLibraryDock.h` keeps `AssetLibrarySettings`/`AssetPathEntry` as
  aliases to the new classes — fork API shape preserved.

### compositiondocker: upstream rewrite wins (as designed)
- `compositionmodel.cpp` → upstream Layer-Comps model (`moveRows` +
  `CompositionDocumentState`); `.ui` QListView→QTreeView consistent.
- **Behavior delta (upstream-intended):** up/down buttons now
  `applyAdjacent(±1)` ("Apply Previous/Next Layer Comp") instead of
  reordering. Reorder capability is preserved via `InternalMove` drag-and-drop
  + `CompositionModel::moveRows` — no fork feature lost, one UX change.
- Fork's post-delete `siblingAtRow` fixups superseded by upstream's
  `selectionModel()->setCurrentIndex` management; `doubleClicked`, Up/Down
  key handling present upstream.

## Standard merges — upstream wins (fork deltas confirmed covered or none)
- `KisAnimTimelineFramesModel.cpp`: upstream fix `!copyFrames && !cloneFrames`.
- `NodeDelegate.cpp`: upstream clipping-icon/bar + Alt+click toggle (Solstice-marked).
- `palettedocker_dock.cpp`: upstream lock-aware undo/redo lambdas,
  QSignalBlocker palette-switch fix, null guards. Fork's enable-block
  superseded by `slotLockPalette(!colorSet || colorSet->isLocked())`.
- `TextPropertiesDock.cpp`: upstream lazy QML load on first Show + perf logger.
- `QuickColorSelectorDock.cpp`: upstream RGB-fallback `selectorColorSpace()`
  (non-RGB nodes no longer collapse the selector) + colorspace change signals.
- `QuickAdjustKeyController.cpp`, `QuickAccess*.{h,cpp}` listed above.
- `qmic` bridge: see audit below.
- CMakeLists (quickaccess, restnote, compositiondocker): upstream adds the
  new sources/static libs/tests subdirs — all referenced files exist in tree
  (lane-A copies verified on disk).
- `wdgcompositiondocker.ui`, QML files, `kis_qmic_interface.h`,
  `kis_qmic_synchronize_layers_command.h`, `QuickAccessLegacyImporter.cpp`,
  `QuickAccessProfileRepository.cpp` (i18n): upstream.

## qmic audit — open question #6 RESOLVED
Fork's `kis_qmic_simple_convertor.cpp` was ~500 lines **larger** because it
carried the *old* fast-path implementation (`KisColorFromFloat`,
`KisColorFromGrayScale{,Alpha}Float` classes + `createTransformationFromGmic`
+ hand-rolled tile loops in `convertFromGmicFast`/`convertToGmicImageFast`) —
stock-Krita-era machinery, **not fork work** (no fork commits touched the
logic; the file sat unchanged at its imported state).

Upstream 1.1.1 rewrote that same functionality into the new
`KisQmicRgbConverter.h` (copied by lane A): `dispatch()` covers the same
u8/u16/f16/f32 RGBA spaces; `toPlanar`/`fromPlanar` handle the same spectrum
cases (gray→green=blue=red, spectrum 2 gray+alpha, 3 rgb+opaque alpha,
4 rgba) with the same `scaleToA` arithmetic and `crop(0,0,w,h)` finish.
`convert*Fast` are now thin dispatch-with-fallback wrappers.

**Decision: take upstream.** The fork's extra lines are superseded older code,
not unported work.

Related qmic files — all upstream superset:
- `kis_qmic_interface.{cpp,h}`: `checkedElementCount` overflow guard +
  `length_error`, `m_image`/`m_activeNode` captured at call time (safer than
  fork's repeated viewManager dereference), `outputApplied`/`errorOccurred`
  signals, locked-layer rejection, `InPlace` unsupported-output error.
- `kis_qmic_synchronize_layers_command.*`: caller passes
  `selectedNodes`/`activeNode` (replaces fork's fragile mainWin→nodeManager
  lookup); `transaction.endAndTake()` composite-child undo replaces the
  `KisSurrogateUndoAdapter` — equivalent semantics; `m_imageCommands` was a
  dead member (only ever cleared, never filled).
- `QMic.cpp`: `m_running`/`m_hasAppliedFilter` + `runtimeActive` guards,
  `KisSolsticePaths` GMIC_PATH/`SOLSTICE_GMIC_SETTINGS` env setup,
  `blockUntilOperationsFinished` kept, action-enable rework (fork's blanket
  `setEnabled(true)` had a Reapply bug), `pluginBase` delete fix.
- `kis_qmic_import_tools.cpp`: upstream fix — `AllVisible` visibility flag
  (fork passed `AllInvisible` for both branches = pre-existing fork bug,
  fixed upstream).

## Manual edits applied (4)
1. `plugins/extensions/pykrita/plugin/krita/__init__.py` — **both guards'
   semantics merged**: upstream's unconditional wrong-PyQt block kept for real
   bindings + fork's pykrita-shim exemption restored (spec origin must live
   inside a `pykrita` dir; Solstice's pure-Python PyQt5 compat package under
   `<data>/pykrita` is whitelisted). Compiles clean.
2. `plugins/extensions/pykrita/plugin/PythonPluginManager.cpp` — ported
   `X-Solstice-EnabledByDefault` fallback for `enable_<module>` (9 vendored
   plugins in `plugins/python/` declare it; user config still wins).
3. `plugins/dockers/layerdocker/LayerBox.cpp` — restored doc reference to
   `brush-stroke-layer-plan.md` (the doc that exists in this tree; upstream's
   `brush-stroke-layer.md` isn't shipped by any lane). Real 3-way merge kept
   both upstream clip actions (`new_clipping_layer`, `toggle_layer_clipping`)
   and the fork's `rerender_brush_strokes`/`forget_brush_strokes` block.
4. `plugins/dockers/overview/overviewwidget.cc` — restored
   `Qt::SmoothTransformation` for the overview preview. Fork commit
   `60bdd402` ("Lane B: display fixes") deliberately upgraded Fast→Smooth;
   upstream never had it. Consistent with the file's own `pixelArt ? Fast :
   Smooth` path and Solstice's smooth-display direction.

## Keep files (untouched, verified)
- `plugins/dockers/animation/KisAnimCurvesDocker.cpp` — unmodified.
- `plugins/extensions/pykrita/sip/krita/Node.sip` — unmodified; retains the
  fork's uncommitted `setPixelDataUndoable` binding.

## Per-file status
| File | Result |
| --- | --- |
| animation/KisAnimTimelineFramesModel.cpp | upstream (bugfix) |
| animation/KisAnimCurvesDocker.cpp | keep (untouched) |
| assetlibrary/AssetLibraryDock.{cpp,h} | upstream; dock delegates to `libs/ui/assetlibrary` (superset verified) |
| compositiondocker/* (5 merge files) | upstream rewrite wins; reorder via InternalMove DnD |
| layerdocker/LayerBox.cpp | real 3-way merge, both sides retained + 1 doc-ref fix |
| layerdocker/NodeDelegate.cpp | upstream (clipping indicators) |
| overview/overviewwidget.cc | upstream + fork Smooth restore |
| palettedocker/palettedocker_dock.cpp | upstream (lock-aware rewrite) |
| quickaccess/* (14 merge files) | upstream split = superset; all fork deltas located in new files |
| restnote/* (3 merge files) | upstream split = superset; legacy key + schema identical |
| textproperties/* (3 files) | upstream (lazy QML + padding fixes) |
| pykrita PythonPluginManager.cpp | upstream + `X-Solstice-EnabledByDefault` port |
| pykrita krita/__init__.py | merged dual guard |
| pykrita sip Node.sip | keep (untouched) |
| qmic/* (7 files) | upstream; fast path superseded by `KisQmicRgbConverter.h` |

## Open questions for follow-up
1. **`bnRedo` suspect (pre-existing, both trees):** `palettedocker_dock.cpp`
   has `m_ui->bnRedo->setEnabled(colorSet->undoStack()->canRedo() &&
   colorSet->isLocked())` — `isLocked()` looks inverted (redo only while
   locked?). Present in BOTH fork and upstream (context line); not a merge
   artifact. Worth a follow-up fix upstream.
2. **compositiondocker UX delta:** buttons apply-adjacent vs fork's reorder —
   intended upstream Layer-Comps design, but flag in release notes for users
   of the old panel.
3. **`KisImportCatcher`/`KisShowInFileExplorer` deps:** merged AssetLibraryDock
   uses `KisShowInFileExplorer` (`libs/ui/utils/`, lane-A copy) — present.
4. **Docs:** upstream ships `brush-stroke-layer.md` (split doc) + new
   `quickaccess`/`restnote` agent docs; no lane carries `docs/` — reconcile
   when docs are merged.
