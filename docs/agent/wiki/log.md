---
type: log
---

# Wiki log

Append-only. One entry per ingest, lint pass or restructuring, newest last.
Format: `## YYYY-MM-DD <kind>: <subject>`, then what changed and the sources.

## 2026-10-07 setup: wiki created

- Created the index with conventions and workflows; `AGENTS.md` points to it.
- Source: the user's request after comparing Karpathy's LLM wiki pattern
  (gist 442a6bf555914893e9891c11519de94f) and nashsu/llm_wiki.

## 2026-10-07 restructure: GPU engine phase records

- Moved `docs/agent/gpu-engine.md` "Review status" and every phase record
  (phases 4.1-4.97, about 4,750 non-empty lines) verbatim into six
  `history/` pages. Headings were promoted one level, and relative links were
  adjusted. A line-by-line comparison with the previous version matched.
- `gpu-engine.md` keeps status, decisions, architecture, plan, build/test,
  source maps, configuration, invariants, checklist and risks. Its new
  "Phase history" section lists every moved section.
- Rewrote the anchor links in `README.md`, `docs/gpu-engine.md` and
  `paint-trace-baseline-runs.md`, and the section references in
  `feature-inventory.md`, `gpu-engine-handoff.md` and
  `gpu-work-priorities.md`.

## 2026-10-07 ingest: GPU affine and Liquify work, session pitfalls

- New pages:
  - `concepts/cpu-gpu-bit-parity.md`;
  - `concepts/krita-copy-semantics.md`;
  - `concepts/qt-numeric-and-geometry.md`;
  - `decisions/gpu-phase-numbering.md`;
  - `decisions/xmlgui-unchanged.md`;
  - `pitfalls/build-format-test.md`;
  - `benchmarks/transform-and-filter-costs.md`.
- Sources:
  - commits `29912c4daf` (GPU Liquify, phase 4.97) and `3828911e98` (GPU affine,
    phase 4.94);
  - `history/gpu-phases-4.93-.md`;
  - qtbase v6.8.0 `src/gui/painting/qpolygon.cpp`;
  - the installed Qt 6.8 `qnumeric.h`;
  - this session's build and test experience.

## 2026-10-07 ingest: settings location phase 1

- `pitfalls/build-format-test.md`: KConfig main config names and the early
  first open. Source: KConfig 6.7.0 `kconfig.cpp`, `KisSolsticePathsTest`.

## 2026-10-07 ingest: Solstice versioning

- New page `decisions/versioning.md`. Source: `docs/agent/versioning.md` and
  the version uses found in `kra_converter.cpp`, `KisResourceLocator.cpp`,
  `KisResourceCacheDb.cpp` and `libkis/Krita.cpp`.

## 2026-10-07 ingest: README benchmark refresh

- `benchmarks/transform-and-filter-costs.md`, section "README refresh
  (2026-10-07)": all README rows remeasured (three fresh processes each),
  with the affine and Liquify GPU rows added to the README.

## 2026-10-07 ingest: test MIME database and known failing tests

- `pitfalls/build-format-test.md`:
  - the MIME database embedded in tests, and the empty storage location
    fix;
  - the list of known failing tests, with a baseline comparison (each of
    them also fails without the change).

## 2026-10-07 ingest: settings location complete

- No new wiki page: the decisions and findings are in `docs/agent/settings-location.md`.
  These include the relative KConfig names, the mounted kritarc defaults, the import rules,
  the Solstice defaults and the verification.
- Pitfall recorded in `docs/agent/settings-location.md` (phase 5): starting `krita.exe`
  directly does not work in the development environment; use `run-krita.bat`.

## 2026-10-07 ingest: GPU Gaussian blur family (phase 4.98)

- `history/gpu-phases-4.93-.md`: the phase 4.98 record.
- `concepts/cpu-gpu-bit-parity.md`: the tolerance rule for the FFT-based
  Gaussian convolution.
- `concepts/krita-copy-semantics.md`: the Copy op's batch-wide HDR clamping
  with a selection.
- `pitfalls/build-format-test.md`: installing plugins after a vtable change;
  PowerShell `Select-Object -First` ending a build.
- `benchmarks/transform-and-filter-costs.md`: GPU Gaussian blur rows.
- `decisions/gpu-phase-numbering.md`: 4.98 done, next 4.99.

## 2026-10-07 ingest: GPU Puppet Warp mesh rendering (phase 4.96)

- `history/gpu-phases-4.93-.md`: the phase 4.96 record.
- `concepts/krita-copy-semantics.md`: RGBA32F composites depend on the tile
  address (malloc alignment, SIMD/scalar split).
- `concepts/cpu-gpu-bit-parity.md`: which Puppet Warp results are compared
  bit for bit.
- `pitfalls/build-format-test.md`: Git Bash `sed -i` and CRLF files.
- `benchmarks/transform-and-filter-costs.md`: the Puppet Warp row.
- `decisions/gpu-phase-numbering.md`: 4.96 implemented.

## 2026-10-07 ingest: Puppet Warp mesh with the Accurate preview

- `history/gpu-phases-4.93-.md`: phase 4.96 reverted and reapplied; the
  Accurate preview never built the mesh (fixed, `docs/agent/puppet-warp.md`).

## 2026-10-08 ingest: Android leftovers removed

- `pitfalls/build-format-test.md`: resolving preprocessor conditionals with a
  script, and editing `.ui` files that the user's tool regenerates.

## 2026-10-08 lint: first pass

- Every page is listed in the index. Frontmatter: all pages have `type`,
  `updated`/`status` and `sources`; history pages have no `related`, which
  the index now states as the rule for archives.
- References (scripted, `git ls-files` and `git grep -w`): 93 paths and
  relative links, and the class/function names in the non-history pages, all
  exist. Two non-symbols were flagged and are fine (`Q_OS_ANDROID` named as
  removed, the `VK_LAYER_PATH` environment variable).
- Contradictions fixed:
  - `docs/agent/gpu-engine.md` said `Solstice/GpuEngine` defaults to false;
    the embedded default kritarc sets true since 2026-10-07 (the code
    fallback stays false).
  - `decisions/xmlgui-unchanged.md`: the user's local `krita5.xmlgui` now
    lives in the Solstice profile (`%APPDATA%\Solstice`).
  - `pitfalls/build-format-test.md`: `updated` date.
- Benchmarks: all dated 2026-10-07 (current builds); nothing stale.

## 2026-10-08 ingest: test Fontconfig configuration

- `pitfalls/build-format-test.md`: why tests warned "Cannot load default
  config file" and how the test mains now find the installed `fonts.conf`.

## 2026-10-08 ingest: known failing tests 13 -> 5

- `pitfalls/build-format-test.md`: the remaining failures, and the causes
  and fixes of eight others (shared test profile, Qt 6 UTF-16 byte order
  mark and NUL handling, model signal pairing, `QRectF::toRect()`, test mains
  without `KRITA_PLUGIN_PATH`).

## 2026-10-08 ingest: brush option shared model phase 2a

- `pitfalls/build-format-test.md`: the test resource database has no brush
  tips or patterns (a bundle is added in the test `main()`), resource models
  must exist before `addStorage()`, parity references are checked against
  the old code, a brush tip option created alone leaks its page, and
  `Copy-Item` keeps the old modification time.
- Lesson recorded in `brush-option-shared-model-plan.md` (phase 2a): an
  option must notify only changes of what it writes. The masking brush
  notified the end of its preserve mode while the editor was reading the
  brush tip, which started a full rewrite inside the read (safe asserts on
  Shift + drag resize).

## 2026-10-08 ingest: brush option shared model phase 2b

- Lesson recorded in `brush-option-shared-model-plan.md` (phase 2b): with
  per-option writes, an option whose written data is baked from another
  option's state (painting mode, Lightness Strength, the masking size
  coefficient) must be written again when that option changes;
  `KisPaintOpOptionsModel::addDependency()` declares it. Each dependency is
  covered by a test that fails without it.
- `KisLockedPropertiesProxy::setProperty()` writes nothing to settings
  without an update listener, so a test that needs a full write of a model
  writes the options directly instead of `writeAll()`.

## 2026-10-08 ingest: Brush Presets scroll to the selected preset

- Feature document `brush-preset-scroll.md`. Lesson: `QAbstractItemView`
  scrolls to a new current item only with `autoScroll` on, also later when a
  hidden view is shown, so turning it off around a programmatic
  `setCurrentIndex()` keeps the position without affecting drag auto scroll.
  Manually checked 2026-10-08.

## 2026-10-08 ingest: brush options in Tool Options, phase 3a

- Feature document `tool-options-brush.md`. Lessons: a delegate that draws
  an extra column must shift the rectangle it passes to
  `QStyledItemDelegate::editorEvent()` too, or the checkbox hit area stays
  under the new column; a stable, untranslated id per option (the
  options-model id) is what kritarc stores, because labels repeat ("Size"
  in General and Masked Brush) and are translated.

## 2026-10-08 ingest: brush options in Tool Options, phase 3b

- Feature document `tool-options-brush.md` (page parameters, mirrors,
  preset preview, shape tools). Lessons: a copy of a lager-bound control
  must write through the control the way its connection listens
  (`setSpacing()` and `setCompositeOp()` do not emit, so the copy emits the
  signal) and read the control after the option's change signal, queued,
  because the model updates some controls with blocked signals; a page's
  own tabs must not hide a parameter, so only an explicit mode widget (the
  Auto or Predefined tip page) does; a script that assigns ids from
  variable names in a line can pick the wrong one (Painting Mode got the
  masked brush's id), so tests check that ids are unique.

## 2026-10-08 ingest: Opacity and Flow strength in Tool Options

- `tool-options-brush.md`: curve options without a row checkbox (Opacity,
  Flow) register their strength bar and Enable Pen Settings as page
  parameters in `KisCurveOptionWidget`; checkable curve options keep the
  row checkbox. README highlight with a screenshot
  (`docs/images/tool-options-brush.png`, shown at the Brush Presets image's
  width of 480).

## 2026-10-08 ingest: Fade as one Tool Options parameter

- `tool-options-brush.md`: a group box whose grid holds mirrorable controls
  is mirrored cell by cell; the auto tip's Fade (two values and the link
  button) is one parameter, hidden for the Soft mask type through its mode
  widget `PageFade`. The README screenshot was replaced.

## 2026-10-08 ingest: Brush Editor fixes (previous preset, scroll bar color)

- Two Krita bugs, recorded in `feature-inventory.md`:
  `KisPaintopBox::slotSwitchToPreviousPreset()` (Q) and
  `slotInputDeviceChanged()` (pen/eraser) changed the settings but not the
  Brush Editor's name and thumbnail, because only `resourceSelected()` and
  `restoreResource()` called `m_presetsEditor->resourceSelected()`;
  `KisCategorizedListView` replaces its active Window color with the text
  color, which its scroll bars inherited (white while focused). Lesson: a
  palette role changed on a scroll area reaches its scroll bars; set the
  role back on the bars, and again on each PaletteChange.

## 2026-10-08 ingest: Color Smudge on the shared model (phase 4)

- `brush-option-shared-model-plan.md`: Color Smudge's 22 options live in
  `KisPaintOpOptionsModel` with four dependencies (Smudge Length, Paint
  Thickness and Overlay Mode on the brush tip; Smudge Radius on Smudge
  Length). Lesson: the bundled presets use neither the new smudge engine nor
  a lightness tip, so parity variants must force the dependent bakes
  (`SmudgeRadiusVersion=2` with an out-of-range radius, a color tip from
  `RGBA_brushes.bundle`); otherwise the references prove nothing.
  `SOLSTICE_BRUSH_TEST_MAIN_WITH_BUNDLES` loads extra bundles in tests.
- `tool-options-brush.md`: Opacity and Flow's Enable Pen Settings rows carry
  only the option's name, keeping the label column narrow. README images
  moved to a new Screenshots section; the Tool Options image was replaced.

## 2026-10-08 ingest: Sketch and Bristle on the shared model (phase 4)

- `brush-option-shared-model-plan.md`: Sketch and Bristle (Hairy) options
  live in `KisPaintOpOptionsModel`, without dependencies. Their engine pages
  register their main sliders as Tool Options parameters. Parity helpers
  moved to `KisPaintOpParityTestUtils.h` for the next engines.
- `tool-options-brush.md`: `KisBrushOptionWidget::hideOptions()` drops the
  parameters of hidden tip controls. Lesson: Bristle's hide list names
  `KisBrushChooser/Spacing`, which no longer exists, so the predefined tip's
  spacing stays visible; check what a hide list really hides before
  asserting on it.
- `todo.md` (new): the user's task backlog; first entry, ABR import
  improvements with storytold/photocraft as a reference.

## 2026-10-08 query: missing .kra thumbnails

- New .kra files saved by Solstice showed no Explorer thumbnail. The files
  were fine (valid `preview.png` and `mergedimage.png`); no `.kra`
  thumbnail handler was registered, because Krita's shell extension comes
  with Krita's installer and Krita was no longer installed. Older files
  showed thumbnails from the Windows thumbnail cache only. Bundling the
  extension is recorded in `todo.md`.

## 2026-10-08 ingest: Tangent Normal, Hatching, Filter, Quick Brush (phase 4)

- `brush-option-shared-model-plan.md`: four more engines on the shared
  model, without dependencies. Filter's bake (fallback filter while none is
  set) is `KisFilterOptionModel::bakeOptionData()`, reached through the
  exported `KisBrushBasedOptionStates::bakeFilterOption()`. Lesson: libpaintop
  model classes are not exported; a plugin can only call libpaintop code
  marked `PAINTOP_EXPORT`, so bakes go through `KisBrushBasedOptionStates`.
  Bundled presets never lack a filter, so the fallback has its own test.
- Pitfall: clang-format re-sorts include blocks and rewraps the public
  constructor of upstream files; `restore_upstream`-style fixes put the HEAD
  include order back and append new includes.

## 2026-10-08 ingest: Quick Adjust controls as in Tool Options

- `quick-access.md`: the HueSVC popup and Quick Brush Adjustments use
  `KisDoubleSliderSpinBox`, `KisSliderSpinBox` and `KisAngleSelector`
  instead of `QSlider` rows and a custom dial; rotation still drives the
  `brushRotation` resource. README main image replaced.
- Pitfall: a child widget left out of a layout keeps the default geometry
  (0, 0, 100 × 30) and stays shown with its parent, above earlier siblings.
  The popup's unused docker columns covered the Size bar's left part and
  took its clicks; hide such widgets. An offscreen probe test (widget
  geometry, simulated clicks, `grab()`) separated this from the slider's own
  behavior before the fix.
- A long prefix in a value bar widens its layout column; give such bars a
  horizontal `QSizePolicy::Ignored` when columns must stay equal.

## 2026-10-09 ingest: Curve, Grid, Particle, Shape (phase 4); live Tool Options preview

- `brush-option-shared-model-plan.md`: four more engines without a brush tip
  on the shared model, no dependencies. Only Spray and MyPaint remain.
  Parity tests come from a scaffold script (static library, test target,
  bundled presets, references written by the old code); a template
  placeholder that silently fails to match leaves a test uncompilable, so
  check the generated file.
- `tool-options-brush.md`: the Tool Options preview renders a modified
  preset itself (debounced clone render with its own
  `KisBrushStrokePreviewRenderer`); the cache keeps rendering saved presets
  only. Lesson: a renderer with a running stroke must outlive its owner
  until `finished`, so detach it and delete it then.

## 2026-10-09 ingest: Spray and MyPaint on the shared model (phase 4)

- `brush-option-shared-model-plan.md`: Spray's shape page reads the spray
  area's diameter and scale through cursors on the SprayOp state but writes
  only its own data, so no dependency. MyPaint's options all patch
  `MyPaint/json`; `KisPaintOpOptionsModel::addSharedKey()` seeds a
  per-option write with the preset's current document. Lesson: the model's
  per-option write starts from an empty scratch configuration, so an option
  that read-modify-writes a shared key silently drops everything else in
  it; the full rewrite hid this because `resetSettings()` keeps the key.
  A parity test against the full rewrite alone does not catch it; test a
  single-option edit against the preset's document.
- Only Clone (`duplicate`) is left before phase 5.

## 2026-10-09 ingest: Clone on the shared model; phase 4 complete

- `brush-option-shared-model-plan.md`: Clone (`duplicate`), missing from the
  plan's engine list, now uses the model; every brush engine does, so
  phase 5 (retiring the clear-and-rewrite path) can start. Lesson: check
  the engine list against the settings widgets that lack
  `setOptionsModel()` instead of trusting a plan's list.

## 2026-10-09 ingest: shared model phase 5

- `brush-option-shared-model-plan.md`: the clear-and-rewrite in
  `slotGuiChangedCurrentPreset()` is only a warned safety net; locks write
  the model's option; Uniform Properties stay as they are (decision and
  reasons in the plan).
- Pitfall: Windows PowerShell 5.1 drops an empty-string argument to a
  native program, so `script.py "" "a,b"` arrives as `script.py "a,b"`.
  The formatting helper then formatted two upstream files whole; they were
  restored from HEAD and the edits reapplied. The helper now takes `-` for
  an empty list; never pass `""` positionally from PowerShell.

## 2026-10-09 ingest: Popup Palette and On-Canvas Brush Editor removed

- `popup-palette-removal.md` (new): what was removed and what was kept.
  Lesson: Krita's right-click "Show Popup Widget" input action is shared
  with the tools' context menus (`KoToolBase::popupActionsMenu()`); remove
  the popup widget, never the action or its profile entries. A removed
  plugin's DLL stays in the test installation until deleted by hand.

## 2026-10-09 ingest: Tool Options brush section without headings

- `tool-options-brush.md`: Enable Pen Settings sits beside its strength bar
  when both are shown; the "Brush" heading and the category headings are
  gone (they took too much room), replaced by thin lines; nothing folds.
  Lesson: a setting a test changes (kritarc) persists between test runs, so
  a failed run leaks state into the next; reset such settings in `init()`.

## 2026-10-09 ingest: brush stroke layer plan, stage 1

- `brush-stroke-layer-plan.md` (new): a layer that records brush strokes
  and redraws them at a new resolution. Stage 1 (`KisBrushStrokeReplayTest`)
  shows a recorded stroke replays pixel-identically with a fixed seed and
  redraws sharply at 4x. Lessons: the dab rendering queue needs an update
  job (`KisAsynchronousStrokeUpdateHelper::UpdateData`) before the stroke
  ends, or nothing is drawn; freehand smoothing sends curve jobs, so record
  the jobs, not only points.

## 2026-10-09 ingest: brush stroke layer, stage 2a

- `brush-stroke-layer-plan.md`: `KisBrushStrokeLayer` derives from
  `KisPaintLayer` (not `KisExternalLayer`), so Wash/indirect painting, the GPU
  wash preview and `.kra` saving as a paint layer come for free. Strokes are
  recorded in `FreehandStrokeStrategy` and join the stroke's undo command
  through `KisPainterBasedStrokeStrategy::setAdditionalUndoCommand()`.
  Lesson: a stroke's undo is one parent command built in
  `finishStrokeCallback()`; extra state that must undo with the pixels goes
  in as a child of it, not as a separate undo step.

## 2026-10-09 ingest: brush stroke layer, stage 2b

- `brush-stroke-layer-plan.md`: scaling redraws the recorded strokes through
  `KisRedrawableLayerInterface`, asked by `KisTransformProcessingVisitor`
  before it resamples a paint layer. The layer first redraws its record at
  the current size and compares it with its pixels; any other edit falls back
  to resampling. Lessons: an interface used with `dynamic_cast` across
  libraries needs an out-of-line destructor in the defining library; running
  a private image's stroke and waiting for it inside the main image's
  processing job did not deadlock.

## 2026-10-09 ingest: brush stroke layer, stage 2c

- `brush-stroke-layer-plan.md`: the strokes are saved in a `.kra` as a
  `layers/<layer>.brushstrokes` file (QDataStream) beside the pixels, and
  the layer element stays a `paintlayer` with an extra attribute, so
  original Krita opens it as a paint layer. Lessons: `KisPaintOpPreset::toXML()`
  looks up linked resources, which asserts off the GUI thread, and `.kra`
  saving runs on another thread; a preset loaded with the global resources
  must get a resources snapshot (`cloneWithResourcesSnapshot()`) on the GUI
  thread before it is used in image processing.
- Follow-up (same day): a layer with Color Smudge blur strokes fell back to
  resampling in an RGBA float document. `KisUsageLogger` diagnostics showed
  only fully transparent pixels differed in color (black vs white). Lesson:
  compare redraws by premultiplied color; the color of a transparent pixel is
  not content. Log fallback reasons to `krita.log` so they can be read after
  a manual repro.

## 2026-10-10 ingest: brush stroke layer with the Transform Tool

- `brush-stroke-layer-plan.md`: a Free Transform that only scales and moves
  redraws the strokes once, when the transform is applied
  (`InplaceTransformStrokeStrategy::redrawRecordedStrokes()`; the overlay
  preview strategy redraws instead of its final pixel transform). The redraw
  interface now takes the content before the transform, because the
  in-place stroke keeps it in its device cache while the device shows the
  preview. Lessons: in `finishAction()` the last update may still be
  pending (updates are throttled by a timer), so read `currentTransformArgs`
  inside the job; in tests, force the update with
  `KisAsynchronousStrokeUpdateHelper::UpdateData(true)`, and never call
  `image->waitForDone()` while a stroke is open (it waits forever).
- Tests comparing redraws of smudge strokes must compare premultiplied
  images; the color under alpha 0 varies with the random seed.

## 2026-10-10 ingest: brush stroke layer, exact redraws and brush tips in .kra

- `brush-stroke-layer-plan.md`: the strokes' brush tips and textures are
  saved with the strokes (the recorded preset's resources snapshot, written
  with `saveToDevice()`, loaded through `KisResourceLoaderRegistry`; ABR tips
  cannot be saved). Two causes made live strokes differ from their redraw:
  Pixel Brush dabs rendered on several threads draw random numbers (image
  pipes with `sel0:random`) in thread order, and the brush tool starts the
  dab spacing from the last hover position. Recorded strokes and redraws now
  render dabs sequentially (`setSequentialDabRendering()`), and the start of
  each copy's `KisDistanceInformation` is recorded (format version 2).
- Lessons: a redraw that differs only at the stroke's outline points to dab
  placement (spacing start), not color; to tell a live-only difference from
  a replay one, feed the recorded jobs through a live recording stroke in a
  test and compare both ways; a random-choice pipe brush makes the default
  multithreaded dab rendering nondeterministic, so exact replay needs
  sequential dabs. `FreehandStrokeTest` has four failures that predate this
  work (textured 17, Mix dull, two LOD strokes).

## 2026-10-10 ingest: ABR import plan, phase 1

- `abr-import-plan.md` (new, from the todo backlog): photocraft (MIT or
  Apache-2.0, Rust) reads ABR tips, preset descriptors, patterns and folders;
  Solstice reads only the tips (versions 1, 2, 6), misreads 16-bit tips and
  drops presets and patterns. Phase 1 saves ABR tips with brush stroke layers
  as PNG (`KisAbrBrush` has no file of its own). Lesson: preset brush tips are
  found by md5 and file name (`KisPredefinedBrushFactory`), so any resource
  class with the same signature can stand in for the original. ABR files are
  already storages (`AdobeBrushLibrary`); the Brush Presets bundle filter
  lists only storages with presets, so ABR files appear there once ABR
  presets exist (phase 4).

## 2026-10-10 ingest: ABR import, phase 2

- `abr-import-plan.md`: `KisAbrParser` replaces the old ABR parser (versions
  6-10, 16-bit tips, bounds and size limits, fuzzed in tests) and tips are
  named after the presets that use them, read with `KisAslReader::readFillLayer()`
  (a version-16 descriptor to XML). Lessons: keep a resource's file name
  when renaming what users see, because presets and the resource database
  identify tips by file name and md5; the resource database does not refresh
  the names of unchanged resources of an existing storage, so renamed tips
  show only after the library is imported again.

## 2026-10-10 ingest: ABR import, phase 3

- `abr-import-plan.md`: ABR `patt` patterns become pattern resources of the
  ABR storage (`<identifier>.pat`, as PSD and ASL patterns), read with
  `KisAslReader::readPsdSectionPattern()` one block at a time. Lesson: that
  reader stops at the first pattern it cannot read (CMYK, Lab), so a section
  of length-prefixed blocks is best split by the caller; a storage plugin can
  serve several resource types if its iterator, `resourceItem()` and
  `resource()` agree on the type of each URL.

## 2026-10-10 ingest: brush tip bundle filter

- `brush-preset-grouping.md`: `KisPresetDockerFilters` takes a resource type;
  the Brush Editor's predefined tips get the bundle facet and grouping, and
  ABR libraries count as bundles. The tip list now shares the editor's extra
  width (stretch 3:2). Lesson: a Qt box layout without stretch factors gives
  extra width to the widgets with expanding policies, so a list next to
  expanding spin boxes never grows.

## 2026-10-10 ingest: ABR import, phase 4

- `abr-import-plan.md`: ABR brush presets become Pixel Brush presets of the
  ABR storage. Option keys are written as `.kpp` stores them, because the
  option data classes live in `kritalibpaintop`, which `kritalibbrush` cannot
  link; tests read them back with those classes. Lessons: every curve option
  needs at least one sensor (a constant one is pressure with a flat curve);
  descriptor integers come out unsigned; Krita's texture brightness is
  subtracted and its height modes subtract the mask, so Photoshop's depth
  modes need an inverted pattern; a storage must hand out copies and support
  `loadVersionedResource()` for presets to be reloadable; and it must not set
  an md5 of its own on presets, because the stroke preview only renders when
  a reloaded preset's md5 equals the database's.

## 2026-10-10 ingest: grouped Brush Presets scroll position

- `brush-preset-grouping.md`: selecting a preset relayouts the list, and
  Qt's plain `QListView::doItemsLayout()` sets a scroll range without the
  group headers, clipping the position. The grouped layout saves the scroll
  value first and restores it after its own geometry update. Lesson: a view
  that adds rows to Qt's layout must restore anything Qt clamps in between.

## 2026-10-10 ingest: ABR import, phase 5

- `abr-import-plan.md`: the `phry` section's `hierarchy` token list
  (`Grup` with `Nm  `, `preset`, `groupEnd`) becomes preset tags named
  `<file> / <folder> / ...`; preset tokens map by order to the `desc`
  presets. The format has no public specification and none of the local
  files has folders, so the reader is lenient (class, single child key or
  value names the token; nested lists close themselves; stray ends ignored;
  depth capped at 32). Storage tags are re-added by `synchronizeDb()` on
  every start, so already registered libraries gain new tags without a
  re-import; `KisAbrStorage::tags()` must load the file first.

## 2026-10-10 ingest: ABR import, finishing work

- `abr-import-plan.md`: color dynamics map to the Pixel Brush's Mix and
  HSV options, and the scatter count divides the spacing. Pitfall found on
  the way: a curve option written with `<id>UseSameCurve` true takes the
  `commonCurve` for every sensor (`KisCurveOption::generateSensors()`), so
  per-sensor curves were silently replaced by the identity and "constant"
  options followed pressure; generated options must write it false. Also
  learnt from real brushes: in Krita's and Photoshop's subtract and height
  texture modes the pattern's lightness is taken from the paint, so the
  pattern must not be inverted, and Photoshop inverts the pattern before
  its brightness while Krita inverts after. Verify such mappings against
  real presets (authors' settings reveal the intended direction), not just
  against the documentation of another mode.

## 2026-10-10 ingest: input-to-display re-baseline, GPU brush by default

- `gpu-work-priorities.md`, `paint-trace-baseline-runs.md`: after phases
  4.81-4.92 the GPU brush reaches input to display in about 5 ms against
  18-22 ms on the CPU (one process per mode). The CPU brush's minimum update
  period (10 ms) and the 40 ms first-update wait were the CPU path's main
  delay and now apply only as adaptive periods; the GPU brush is on by
  default (`Solstice/GpuBrush`). Lessons: an opt-in feature's gains never
  reach the user until it is the default, so check what the user's own
  launcher enables before reporting a speedup; strokes whose later stages run
  on the CPU (the masking brush composite, its Wash preview and merge) must
  stay on the CPU brush, or tiles ping-pong between GPU and CPU on every
  update; and never read the environment (`qgetenv` takes a global lock) per
  dab on worker threads: it slowed every path, CPU included, by more than
  half for dense brushes. Decide once per stroke.

## 2026-10-10 ingest: Solstice application icon and splash

- `solstice-visual-branding-todo.md`: the user's icon and splash artwork
  replaced the gray placeholders. Masters live in
  `krita/pics/branding/source/`; generated sizes go to `Next/` (the
  development build's variant) and `default/`, the splash to
  `krita/data/splash/` as an 8-bit copy. Lesson: artwork exported from an
  RGBA float document embeds a linear profile (`sRGB-elle-V2-g10.icc`);
  convert from the embedded profile to sRGB before scaling or compositing,
  or the generated assets come out dark and lose subtle tints. Also: the
  splash text was already top-right (`Qt::AlignRight`); read the layout code
  before describing it.

## 2026-10-10 ingest: document icons and README header

- `solstice-visual-branding-todo.md`: `.kra`/`.krz` icons are drawn from the
  application icon (a page with a folded corner; `.krz` labeled "KRZ").
  README now opens with the splash (480 px wide, centered), a separator and
  a one-line description before the main window screenshot; the `# Solstice`
  heading was dropped because the splash carries the name.
- README introduction rewritten (same day, user request): three columns
  (GPU acceleration, workflows from Clip Studio Paint, Krita refined) in an
  HTML table; links inside the table are HTML anchors because Markdown is not
  rendered in table cells.

## 2026-10-10 ingest: GPU masking brush composite (phase 4.101)

- Masked strokes on RGBA32F use the GPU brush; the masking composite runs on
  the GPU, bit-identical to the CPU in the ten modes without strength
  ([gpu-engine.md](../gpu-engine.md), "Masking brush";
  [history](history/gpu-phases-4.93-.md#gpu-masking-brush-composite-phase-4101)).
- Lesson: for float parity, match the CPU's `compositetype` (float, not
  double) with `precise float`; only divisions go through double.
- Lesson: per-batch fixed costs (a masking composite plus a projection merge)
  make the GPU minimum update period counterproductive when dabs arrive
  slowly from CPU generation; masked strokes keep the adaptive period.
- User manual check OK on 2026-10-10.

## 2026-10-10 ingest: new splash artwork and README screenshot

- The user replaced the splash master (`krita/pics/branding/source/
  solstice-splash.png`, a linear-profile export of a float document). The
  app splash `krita/data/splash/solstice-splash.png` was regenerated the same
  way as before: converted to sRGB, composited over white, 8-bit RGB PNG at
  full size (3146x1920); it is compiled into `krita.dll` through `splash.qrc`.
- The README main image `docs/images/solstice-main-window.webp` is the user's
  new screenshot, used as supplied.

## 2026-10-10 ingest: Explorer thumbnails (Krita Shell Extension)

- Windows packages ship the Krita Shell Extension 1.2.4d (MIT) in `shellex/`
  with per-user registration scripts
  ([windows-shell-thumbnails.md](../windows-shell-thumbnails.md)); the todo
  item moved there.
- Decision: register in HKCU only (no administrator rights, Krita's HKLM
  registration stays intact and wins again after unregistering); thumbnails
  only, no file association or property handler.
- Lesson: `package-complete.py --pre-zip-hook` adds files to the ZIP without
  changing the upstream packager.
- Lesson: registry scripts get a `-TestRoot` so the real write/delete code
  can be tested under a throwaway HKCU key; only `Software\Classes` is
  redirected for the 32-bit view.
- User manual check OK on 2026-10-10.

## 2026-10-10 ingest: version 0.2.0-alpha

- Solstice version raised to `0.2.0-alpha` (MINOR, a milestone) at the
  user's request before a trial build; the Krita compatibility version is
  unchanged ([versioning.md](../versioning.md), "History").

## 2026-10-10 ingest: resized application icon

- The user resized the icon master (`krita/pics/branding/source/
  solstice-icon-1024.png`); the application icons (Next and default, ICO,
  scalable resource) and the `.kra`/`.krz` document icons were regenerated
  the same way as before.
- Pitfall: the document icon tool draws the "KRZ" label with system fonts;
  run it without `QT_QPA_PLATFORM=offscreen`, which has no fonts and draws
  empty boxes.

## 2026-10-10 ingest: Rest Note enable setting, Custom tab color buttons

- Rest Note is off by default (`Solstice/RestNoteEnabled`, Custom tab); the
  plugin registers the docker only when it is on, and a loaded docker stops
  at once when it is turned off ([rest-note.md](../rest-note.md)).
- Lesson: every registered docker is created with each main window, hidden
  or not, so a timer docker keeps running while hidden; gating the factory
  registration is the reliable "off".
- The Custom tab's foreground color buttons use a fixed size policy like the
  Cursor tab's outline color button.
- Pitfall: `wdggeneralsettings.ui` has mixed line endings; match edits with
  the section's own endings.
- User manual check OK on 2026-10-10.

## 2026-10-11 ingest: HueSVC shape button and swatches

- HueSVC chooses its own shape (hue bar + square by default, or square in a
  hue ring) from a header button and no longer follows the Advanced Color
  Selector's `colorSelectorConfiguration`; its swatches match the Wide Gamut
  Color Selector's 24 px toggle ([quick-access.md](../quick-access.md)).
- Lesson: a fresh profile showed a triangle because HueSVC inherited the
  Advanced Color Selector's default; shared configuration keys make one
  docker's defaults leak into another.
- Pitfall: an off-screen `KisVisualColorSelector` used for previews needs a
  color space and a color (`slotSetColorSpace()`, `slotSetColor()`), or it
  renders nothing.
- User manual check OK on 2026-10-11.

## 2026-10-11 ingest: default workspace and first layout

- The user's "Solstice" workspace became `krita/data/workspaces/Default.kws`
  and the embedded `[MainWindow] State`, so a new profile's first layout and
  the Default workspace match ([settings-location.md](../settings-location.md)).
- Lesson: the first-start layout comes from the embedded kritarc, not from
  `Default.kws`; changing only the workspace file leaves new profiles as
  they were.
- User manual check OK on 2026-10-11.

## 2026-10-11 ingest: solstice.exe

- The Windows launchers are `solstice.exe` and `solstice.com`; `krita.dll`
  and the application name `krita` stay ([executable-name.md](../executable-name.md)).
- Lesson: before `KAboutData`, Qt takes the application name from the
  executable, so renaming the launcher needs an explicit
  `setApplicationName("krita")`; the profile paths were already explicit.
- Pitfall: CMake installs do not remove the old `krita.exe`/`krita.com`.
- User manual check OK on 2026-10-11.

## 2026-10-11 ingest: software Vulkan devices refused

- GPU device selection refuses CPU-type devices (software Vulkan, common
  in virtual machines), which would be slower than the CPU paths
  ([gpu-engine.md](../gpu-engine.md), "Configuration and compatibility").
- Decision: integrated and virtual GPUs stay accepted when they meet the
  feature requirements; `KRITA_GPU_ALLOW_SOFTWARE_DEVICE=1` re-enables
  software devices for diagnostics.
- User manual check OK on 2026-10-11.

## 2026-10-11 ingest: GPU textured dabs (phase 4.102)

- RGBA32F circle dabs with a pattern texture in the 14 alpha-only modes are
  generated and textured on the GPU ([gpu-engine.md](../gpu-engine.md),
  "Textured dabs"; [history](history/gpu-phases-4.93-.md#gpu-textured-dabs-phase-4102)).
  300px textured strokes: CPU 54 ms, GPU brush 57 -> 15 ms.
- Lesson: keep the CPU fallback on the real CPU code (an applier object)
  instead of a second C++ copy of the math; test the shader copy per mode.
- Pitfall: adding unused code to an existing exact shader variant changed
  its results (RGBA16F by one half ULP, the Soft circle no longer
  bit-exact); new features get their own shader variants.
- User manual check OK on 2026-10-11.

## 2026-10-11 ingest: README benchmark refresh, masked update period

- README benchmarks measured again with the 2026-10-07 method plus two
  textured stroke rows ([benchmarks](benchmarks/transform-and-filter-costs.md#readme-refresh-2026-10-11));
  the stale "no GPU advantage at 64px" sentence and the "dabs, textures and
  masking on the CPU" status were corrected.
- Decision: masked strokes keep the adaptive update period; after phase
  4.102 the GPU minimum period made no measurable difference
  ([gpu-engine.md](../gpu-engine.md), "Masking brush").
- User check OK on 2026-10-11.

## 2026-10-11 ingest: GPU image tip dabs (phase 4.103)

- RGBA32F dabs of predefined image tips (GBR, PNG, SVG, ABR) in alpha-mask
  mode are generated on the GPU, also textured
  ([gpu-engine.md](../gpu-engine.md), "Image tip dabs";
  [history](history/gpu-phases-4.93-.md#gpu-image-tip-dabs-phase-4103)).
- Decision (user, "A2"): `KisQImagePyramid::createImage()` no longer uses
  QPainter, whose output depends on the CPU; the shared integer code is at
  most one 8-bit step from upstream and about twice as slow on the CPU
  ([parity](concepts/cpu-gpu-bit-parity.md), technique 8).
- 160px rotated Wash with mirrors: CPU 26 ms, GPU canvas only 32, GPU
  brush 22; at 48px no gain.
- User manual check OK on 2026-10-11.

## 2026-10-11 ingest: faster CPU image tip transform

- `KisImageTipSampler::render()` gained AVX2 kernels and a gray-level path
  (same bytes as the SSE2 and three-channel paths, tested on every case);
  the CPU image tip transform went from about 2x to about 1.3x QPainter's
  time ([gpu-engine.md](../gpu-engine.md), "Image tip dabs", CPU speed).
- Pitfall: an interpolated opaque tip has alpha 0xfffe, so the unpremultiply
  runs for nearly every pixel; measure by disabling pieces before
  optimizing (the gathers and the factor table were not the cost).
- User manual check OK on 2026-10-11.

## 2026-10-11 ingest: masking coefficient after a canvas resize

- Bug fix (user report): after a canvas resize, the first Brush Editor edit
  rewrote `MaskingBrush/MasterSizeCoeff` for the old size, so masked strokes
  became thin and stayed thin
  ([brush-option-shared-model-plan.md](../brush-option-shared-model-plan.md),
  phase 5 fix).
- Lesson: an option whose value derives from another (`addDependency()`)
  must be re-read when the other changes outside the editor, not only
  rewritten when it changes inside it.
- User manual check OK on 2026-10-11.

## 2026-10-11 ingest: GPU colour, Lightness and Gradient image tips (phase 4.104)

- Image stamp, Lightness map and Gradient map tips are generated on the GPU
  from 256-entry tables made with the CPU code
  ([gpu-engine.md](../gpu-engine.md), "Image tip dabs", Applications;
  [history](history/gpu-phases-4.93-.md#gpu-colour-lightness-and-gradient-image-tips-phase-4104)).
  120px textured colour stamp: CPU 26 ms, GPU brush 10 ms.
- Lesson: when an output depends on a few 8-bit inputs, a table built by
  calling the CPU code is exact by construction and avoids copying color
  math (HSL, LCMS) into a shader; check, do not assume, that the inputs
  are independent (the stamp's per-channel conversion).
- Decision: the gradient branch's transparent pixels take their own color
  (invisible upstream change, needed for a per-pixel description).
- User manual check OK on 2026-10-11.

## 2026-10-11 query: masking brush mask on the GPU (deferred)

- Measured before building: skipping the CPU mask composite entirely would
  take a 300px masked stroke on the GPU brush from 32 to 24 ms and change
  nothing at 150px; GPU mask dabs would save at most about 5 ms more. The
  work needs a GrayA8 GPU tile format and an integer Alpha Darken (exact is
  possible: the U8 op is integer arithmetic). Deferred by the user
  ([gpu-work-priorities.md](../gpu-work-priorities.md)).
- Lesson: measure an upper bound (temporarily skip the work) before
  building; summed CPU time of parallel jobs overstated the wall-clock gain.

## 2026-10-11 ingest: RGBA16F image tips and textures (phase 4.105)

- RGBA16F image tips and textured dabs are generated on the GPU
  ([gpu-engine.md](../gpu-engine.md), "RGBA16F image tips and textures";
  [history](history/gpu-phases-4.93-.md#rgba16f-image-tips-and-textures-phase-4105)).
- Pitfall: for `half`, read the C++ types of every expression: Imath's half
  has no arithmetic operators (float), KoColorSpaceMaths uses double, and
  each half result rounds double -> float -> half.
- Lesson: when the stroke tests have a tolerance (RGBA16F 0.002), add an
  exhaustive exact test of the math.
- User manual check OK on 2026-10-11.

## 2026-10-12 ingest: RGBA16F masking brush composite (phase 4.106)

- Masked RGBA16F strokes use the GPU brush; the masking composite repeats
  `KisMaskingBrushCompositeOp<half>` bit for bit
  ([gpu-engine.md](../gpu-engine.md), "Masking brush";
  [history](history/gpu-phases-4.93-.md#rgba16f-masking-brush-composite-phase-4106)).
- RGBA16F masked 300px textured stroke: CPU brush (the only path before)
  129.5 ms, GPU brush 36.4 ms.
- Lesson: the half arithmetic rules ([gpu-engine.md](../gpu-engine.md),
  "RGBA16F image tips and textures") carry over to any `half` composite.
- User manual check OK on 2026-10-12.

## 2026-10-12 ingest: missing Layers docker thumbnails

- `KisLayerThumbnailCache` keyed its records by `KisNodeWSP`; a destroyed
  node's key compared as `nullptr` and hid other layers' thumbnails (1x1
  placeholder dots). Keyed by address now, with the weak pointer in the
  record; test `KisLayerThumbnailCacheTest`.
- Lesson: [weak pointers as ordered map keys](pitfalls/weak-pointer-map-keys.md).
- Not related to the GPU engine: GPU strokes update thumbnails correctly.
  Thumbnails of GPU-composited groups take longer (52 ms in a probe) and
  switch to the coarse bounds mode, as upstream does for slow layers.

## 2026-10-12 ingest: FreehandStrokeTest repaired

- The four long-standing `FreehandStrokeTest` failures were stale or missing
  references and a missing resource, not regressions
  ([build-format-test.md](pitfalls/build-format-test.md), Tests): textured 17
  and Mix dull references regenerated from the deterministic current
  results, `freehand-lod` references added, the LOD tests seeded and given
  the Krita 3 bundle. 15 full runs pass.
- Open: an intermittent tile-sized loss in the LOD auto-brush stroke (3 of
  92 runs earlier, 0 of 95 later); cause unknown.
- Lesson: before blessing references, check determinism (identical output
  over runs) and look at the image; a 0x0 reference means a missing file.

## 2026-10-12 query: intermittent loss of part of a Wash stroke

- Reproduced in one process (about 1 in 2,500 strokes under load), not
  LOD-specific; localized to the final merge or the following projection
  update; the Solstice scheduling, GPU merge and tile clone changes ruled
  out ([build-format-test.md](pitfalls/build-format-test.md), Tests, Open).
  The root cause is not found; the investigation stopped here.
- Lesson: summed or per-process rates mislead; an in-process repetition
  loop with background CPU load turned a 1-in-30 process failure into a
  measurable rate, and toggling suspects by environment variables in one
  build compared them quickly.

## 2026-10-12 ingest: remaining blend modes (phase 4.107)

- RGBA16F brushes use the GPU in every GPU blend mode; the HSI/HSL/HSV
  families and HSY Tint and increase/decrease modes run on the GPU for
  layers and brushes ([gpu-engine.md](../gpu-engine.md), "Remaining blend
  modes (phase 4.107)";
  [history](history/gpu-phases-4.93-.md#remaining-blend-modes-phase-4107)).
- Lessons ([cpu-gpu-bit-parity.md](concepts/cpu-gpu-bit-parity.md),
  techniques 9-11): the composite type of float is float; the NVIDIA driver
  fuses multiply-adds despite `precise`; per-dab debugging with ULP-scaled
  GPU intermediates finds a 1-ULP drift quickly. Modes already enabled
  without tests had drifted; add a test row whenever a mode is enabled.

## 2026-10-13 ingest: all other blend modes (phase 4.108)

- Nearly every blend mode runs on the GPU
  ([gpu-engine.md](../gpu-engine.md), "All other blend modes (phase 4.108)";
  [history](history/gpu-phases-4.93-.md#all-other-blend-modes-phase-4108)).
- Lessons ([cpu-gpu-bit-parity.md](concepts/cpu-gpu-bit-parity.md),
  techniques 12-14): check `KoColorSpaceMaths` specializations before
  reproducing a helper (`clamp<float>` and `clamp<half>` do nothing);
  GLSL transcendental functions need accurate replacements; compare NaN as
  equal and print inputs and outputs of the worst pixel in failure messages.

## 2026-10-13 ingest: precise contraction audit (phase 4.109)

- The driver keeps `precise` in small shaders but fused it in the real blend
  code ([gpu-engine.md](../gpu-engine.md), "Precise contraction audit
  (phase 4.109)"; [history](history/gpu-phases-4.93-.md#precise-contraction-audit-phase-4109));
  `testBlendProbe` is the reproduction, `roundedProduct()` the remedy, now
  also in the F16 lerps, F16 Alpha Darken and Liquify.
- Lesson ([cpu-gpu-bit-parity.md](concepts/cpu-gpu-bit-parity.md),
  technique 10): a minimal reproduction can hide a context-dependent driver
  behavior; keep the reproduction that calls the real code. Updated the
  phase numbering page ([gpu-phase-numbering.md](decisions/gpu-phase-numbering.md)).

## 2026-10-13 ingest: pre-release review

- Three parallel read-only audits (public docs, interface text, visual
  assets) found stale GPU statements, development-diary text and internal
  links in the user guides, leftover "Krita" text, Krita's bug tracker in
  Help > Report Bug, Krita shortcuts in the ZIP, a gray-icon trap for stable
  builds and old Krita document icons inside the application; fixed and
  recorded in [the branding checklist](../solstice-visual-branding-todo.md),
  "Pre-release review (2026-10-13)", with the open decisions.
- Lesson: user guides written phase by phase accumulate history; rewrite
  them for the reader before a release and archive the history
  ([archive](history/gpu-user-guide-archive.md)). Lesson: a bash heredoc
  with Python string literals turns a backslash followed by r in Windows paths into a carriage
  return; write such scripts to a file.

## 2026-10-13 ingest: pre-release decisions

- The user decided the open review items: the installer and MSIX are
  rebranded (a new Solstice NSIS script that never touches Krita), the
  welcome page loses the upstream support and manual links, saved files keep
  naming Krita, the About data gets a description and one copyright text,
  names stay, and the Vision ML guide is expanded
  ([branding checklist](../solstice-visual-branding-todo.md), "Pre-release
  review (2026-10-13)").
- Lesson: an inherited installer is not just strings; Krita's also removed
  Krita installations and took machine-wide integration, so a rebrand meant
  a smaller script of Solstice's own.

## 2026-10-13 ingest: brush stroke layer selection moves and record check

- A user's layer enlarged blurry: Move Tool selection moves had cut and
  pasted pixels without moving the record, so the redraw check failed and
  the layer was scaled as pixels, noticed only after scaling. Selection
  moves now move the strokes wholly inside the selection (last in the
  order, as the pasted pixels lie on top), and a background check after
  non-brush edits warns with a canvas message and a layer icon badge
  ([plan](../brush-stroke-layer-plan.md), "選択範囲の移動と記録の照合").
- Diagnosis method: render each recorded stroke alone and measure the share
  of its own opaque pixels that differ; whole strokes at 100% point to an
  edit that moved pixels, not to brush nondeterminism.
- Pitfalls: a queued Qt event that owns a KisNodeSP deadlocks at exit (the
  event is destroyed under Qt's posted-event lock, and the node's QObject
  destructor takes it again); and changing a kritaui class layout requires
  reinstalling the plugins, because tests load `_install` plugins built
  against the old size (heap corruption in an unrelated place).
- Follow-up the same day: a layer still enlarged blurry after the warning
  and an undo. Its brush had a texture, and the layer had been moved; the
  texture is anchored to the canvas (`X - OffsetX`), so drawing a moved
  stroke again picked another part of the pattern (small alpha
  differences on every line). Redraws now move the texture offset with
  the stroke. Lesson: a redraw must reproduce every canvas-anchored input
  of a dab (texture, and anything else keyed to image coordinates), not
  only the dab positions.

## 2026-10-13 ingest: brush stroke layer partial redraw

- Scaling a brush stroke layer now draws the strokes again where the layer
  still matches its record and transforms only the mismatching part as
  pixels (grown 2 px, feathered, blended with `COMPOSITE_COPY` through the
  mask); most of the layer mismatching still falls back to pixels. The
  callers pass their own pixel transform so the fallback part looks as it
  did ([plan](../brush-stroke-layer-plan.md), "一致しない部分だけ画素で拡大する").
- Lesson: when a fallback replaces a whole result, ask whether the check
  that triggers it can say where it failed; the mismatch mask was already
  computed and only needed to be kept.

## 2026-10-13 ingest: 0.2.0-beta, main branch, package root launcher

- The version label is `beta` (0.2.0-beta, [versioning](../versioning.md)).
  The GitHub Actions package name and BUILD-INFO carry the version.
- `main` is the primary and default branch, created from `krita-sol-gpu`,
  which is retained and no longer updated (AGENTS.md branch policy).
- The ZIP root has `Solstice.exe`, a small launcher for `bin\solstice.exe`
  ([executable name](../executable-name.md)). Lesson: a helper executable
  placed outside `bin` must not import the toolchain's runtime DLLs
  (llvm-mingw's `libc++.dll`); link it statically and check its imports with
  `llvm-objdump -p`. A stand-in target that records its command line tests a
  launcher without starting the application.

## 2026-10-13 ingest: Windows CI dependency lock refresh

- Every locked KDE registry package had been removed (HTTP 404), and only
  the branch-scoped cache on `krita-sol-gpu` kept builds working. The lock
  is regenerated by `build-tools/github-actions/refresh-dependency-lock.py`
  ([GitHub Actions](../github-actions.md), "Lock refresh"). It is a
  dependency upgrade (Boost 1.90, Eigen 5, exiv2 0.28, FFmpeg 8, Imath
  split out), not just a rebuild.
- Lesson: the KDE registry keeps only recent dependency builds, and Actions
  caches are per branch, so a new default branch, or a cache unused for a
  week, exposes a stale lock at once. Refresh the lock before a release,
  and read the dependency recipe diff between the old and new source
  revisions to know what is really changing.

## 2026-10-13 ingest: welcome page Solstice releases

- The welcome page News is split: Krita news above, the Solstice
  repository's GitHub releases (Atom feed) below, both behind the existing
  opt-in news switch ([welcome news](../welcome-news.md)). KisRssReader
  reads Atom entries now.

## 2026-10-13 ingest: Windows dependency mirror and weekly check

- Permanent fix for the dependency registry removals: a public mirror
  repository, `udtren/solstice-deps`, holds each locked set as release
  assets; the bootstrap prefers it, and a weekly workflow checks the mirror
  and the registry ([GitHub Actions](../github-actions.md), "Dependency
  mirror"). Lesson: a mirror repository's own workflow can upload to its
  own releases with the built-in token, so no cross-repository secret is
  needed.

## 2026-10-14 ingest: 0.2.0-beta release, launch, Python plugins, beta.2

- 0.2.0-beta was published as a GitHub prerelease and the website went live
  at solstice.tidalboxstudio.com (Cloudflare Workers, `develop` merged into
  `main`). The user then found no Python Plugin Manager: the CI build had
  always configured without the Python plugins
  ([GitHub Actions](../github-actions.md), "Python plugins"). 0.2.0-beta.2
  is the corrected build.
- Lesson: an optional feature that configure drops silently must be checked
  in CI like the GPU engine is, or a release can ship without it unnoticed;
  check what the published package contains against a local build, not only
  that the build succeeds.

## 2026-10-14 docs: original plugin repositories

- The user guides of the features ported from Python plugins (Quick Access,
  Lazy Tools, Rest Note, Asset Library, Vision ML) now point to the original
  plugin repositories for details (user request); the guides cover the
  native versions and where they differ.

## 2026-10-14 docs: Krita User Manual and the website

- README, the guide index and the website point to the Krita User Manual
  (`https://docs.krita.org/en/user_manual.html`) for the basics of using
  the application (user request); Solstice's guides cover what it adds.
- README's Project page links the live website instead of calling it
  planned; AGENTS.md's README rule names the website.

## 2026-10-14 release: 0.2.0-beta.2

- GitHub Release `v0.2.0-beta.2` (prerelease, commit `9224a5d118`) was
  published; the user confirmed the Python Plugin Manager in its package
  ([GitHub Actions](../github-actions.md), "Python plugins"). The
  `v0.2.0-beta` notes now say it is superseded, and the merged
  `ci/python-plugins` branch was deleted.

## 2026-10-14 ingest: upstream selective intake

- New decision page [upstream-selective-intake](decisions/upstream-selective-intake.md):
  weekly upstream review by Hermes Agent, user decision, cherry-picks by
  Claude; `AGENTS.md` Branch policy and index, new
  [upstream-intake.md](../upstream-intake.md) (procedure and ledger),
  `development-workflow.md` upstream section marked historical.
- Validation run 2026-W42 on the DGX share: 70 commits, 32 reviewed in 16
  items; series dry runs and an isolated object directory were needed.

## 2026-10-14 intake: upstream 2026-W42

- Adopted items U-2026W42-01 to 06 (nine upstream commits: thumbnail cache
  assert, animation curves scrolling, AssistantsToolOptions.ui class name,
  perspective transform conditions, second-wheel horizontal scrolling, PSD
  group collapse and layer locks); every other W42 candidate rejected by
  the user. Ledger in [upstream-intake.md](../upstream-intake.md).
- Applying from the shallow share mirror: `git fetch` would make this
  repository shallow, so commits are applied with the share's
  `scripts/intake.sh` (`git am -3 --keep-non-patch`; plain `git am` drops
  `[psd]`); recorded in
  [upstream-selective-intake](decisions/upstream-selective-intake.md).
- `kis_psd_test` loads the PSD import/export plugins from the install tree:
  the new round-trip tests failed until `libs/psd` and `plugins/impex/psd`
  were installed.

## 2026-10-14 release: 0.2.0-beta.3

- GitHub Release `v0.2.0-beta.3` (prerelease, commit `c89bc02475`, Actions
  run 38503291774): the 2026-W42 upstream intake. Package checked: checksums,
  5,300 ZIP entries intact, Python plugins and PSD plugins present.
- Website `solstice/src/site.ts` updated and `develop` merged into `main`;
  the live Download page showed `0.2.0-beta.3` a few minutes later. New rule
  (user decision): every release is followed by the website update
  ([versioning](../versioning.md), "Releasing").
- `SHA256SUMS.txt` has CRLF line endings: check it with
  `tr -d '\r' < SHA256SUMS.txt | sha256sum -c -`.
