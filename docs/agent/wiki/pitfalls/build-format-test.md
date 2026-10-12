---
type: pitfall
updated: 2026-10-12
sources:
  - docs/agent/coding-rules.md ("Formatting and diffs")
  - docs/agent/development-workflow.md (build, test, install)
  - docs/agent/gpu-engine.md ("Build, test, and install")
  - experience from the 2026-10 sessions (Quick Access, docker locks, GPU Liquify)
related:
  - ../concepts/cpu-gpu-bit-parity.md
---

# Build, format and test pitfalls

The commands themselves are in `development-workflow.md` and in
`gpu-engine.md` "Build, test, and install". This page collects what went
wrong anyway.

## Formatting

- **`clang-format --lines` still re-sorts an include block** when one of
  its lines changed. Formatting only the changed lines of an upstream file
  can reorder that file's includes. Compare with `git diff` after
  formatting. Restore the original order and keep the new include at the
  end of the block, matching the file's style.
- **Shells drop empty arguments.** A helper called as
  `script "" "a.cpp,b.cpp"` from PowerShell lost the empty first argument,
  and whole upstream files were formatted. Call such helpers from bash, or
  avoid positional empty arguments.
- **Check list separators.** A helper expecting comma-separated files
  received space-separated ones and reported "formatted 0 ranges".
- **Mixed line endings.** Many files are CRLF in the working tree while
  edits arrive as LF. Edit helpers must match either. Run `git diff --check`
  before committing.

## Shaders and GPU structs

- **GLSL reserved words.** A function named `sample` failed to compile
  ("unexpected SAMPLE"). Other reserved names include `input`, `output`,
  `filter`, `common`, `partition` and `active`.
- **std430 layout.** `static_assert` the C++ mirror of every buffer
  struct, and compute the size by hand:
  - `ivec4` + 2 x (`ivec2` + 2 `int`) + 4 `int` + `vec4` = 80 bytes, not 64;
  - a struct containing `dvec4` has 32-byte alignment, so its array stride
    is a multiple of 32.
- **Push constants** carry buffer device addresses only. Every buffer is
  passed by address (`GL_EXT_buffer_reference`), in the same order as the
  C++ struct.

## KConfig paths

- `KConfig::setMainConfigName()` must not be absolute: KConfig 6.7 appends
  the main config name to `GenericConfigLocation`, and looks up defaults at
  `":/kconfig/" + name`. Relative names with `..` work, and the defaults path
  is cleaned by Qt (`docs/agent/settings-location.md`).
- The main config is first opened inside the temporary `QCoreApplication` in
  `krita/main.cc`, by a static startup function (`KisAnimAutoKey`). Anything
  that changes it must run before that point.

## C++ access

- `KisPaintDevice::fastBitBltPossible()` is protected. Compare `x()`, `y()`
  and color spaces instead.

## Tests

- Run GPU tests through the validation wrapper (`KRITA_GPU_VALIDATION=1`,
  `VK_LAYER_PATH` pointing at the Vulkan SDK `Bin`).
- Opt-in benchmarks: `KRITA_GPU_BENCHMARK_FILTERS=1` for
  `benchmarkFiltersAndTransforms`. `KRITA_GPU_TRANSFORM_DEBUG=1` prints
  stage times of the GPU transform paths.
- Offscreen UI tests need `QT_QPA_PLATFORM=offscreen` **and**
  `QT_QPA_FONTDIR=C:\Windows\Fonts`. Without the fonts, layout tests fail;
  this is environmental, not a regression.
- Do not run the `libs-ui-*` suite through ctest or `kis_kra_saver_test`
  on the desktop session: they open blocking dialogs.
- **Test programs embed the MIME database** (since 2026-10-07). Qt 6 in
  this build has no built-in MIME database; the application embeds
  `krita/data/mime-database/freedesktop.org.xml` at
  `:/qt-project.org/qmime/packages`.
  - Before, tests got empty MIME types, so resource loaders were not found
    ("Could not create loader for symbols test.svg ''") and resource tests
    failed.
  - `sdk/tests/CMakeLists.txt` now compiles the resource once into the
    OBJECT library `kritatestmimedatabase`, and adds its object to
    `kritatestsdk` (an INTERFACE library), so every test registers it at
    startup.
  - An empty storage location is no longer detected as a folder: Qt 6 treats
    `QFileInfo("")` as the current directory (`autoDetectStorageType()` in
    `KisResourceStorage.cpp`). `TestResourceStorage` now expects the
    designed result, an invalid memory storage.
- **Known failing tests (2026-10-08)**, from `ctest -j 12 -E
  "libs-ui|kis_kra_saver_test|KisGpuSaveTest"` (291 tests): 5 failed, down
  from 13 on 2026-10-07. Remaining, not investigated further:
  - `TestSvgParser`, `TestSvgParserCloned`, `TestSvgParserRoundTrip`: 5 mesh
    gradient renders each, 7-17 levels off the reference images (the
    rasterization approximates; the references were made elsewhere);
  - `kis_cage_transform_worker_test`: the two unity cages produce one more
    opaque pixel at the right edge (x = 299) than the reference;
  - `kis_tiff_test` (`testFiles`): `quad-strip-jpeg.tif` and
    `quad-tile-jpeg.tif` differ in one pixel each by a few levels (the JPEG
    decoder build).

  Fixed on 2026-10-08 (cause, fix):
  - `KisBrushModelTest` (initTestCase): all tests shared one test profile,
    `qttest\Solstice`; now one per test program
    (`KisSolsticePaths::profileRoot()`, `docs/agent/settings-location.md`).
  - `psd_cos_parser_test` (string round trips): Qt 6's UTF-16BE codec writes
    no byte order mark, which PSD text engine strings need and the parser
    requires; `KisCosWriter` (`writeString()`) now writes FE FF and the
    UTF-16BE bytes itself. This also affected PSD export of text.
  - `kis_jpegxl_test` (CMYK with layers, multipage): JXL frame names kept
    their NUL terminator, because Qt 6's `QString(QByteArray)` does not stop
    at NUL; `JPEGXLImport.cpp` decodes the name with its length.
  - `StoryboardModelTest`: `QAbstractItemModelTester` reported
    `layoutChanged()` without `layoutAboutToBeChanged()`
    (`StoryboardModel::slotCommentDataChanged()`), and comment insertion and
    removal that returned between `begin...Rows()` and `end...Rows()` for an
    invalid position (`CommentModel.cpp`).
  - `kis_transform_mask_test`: Qt 6's `QRectF::toRect()` rounds origin and
    size, Qt 5 rounded the corners; the test compares with Qt 5's rounding.
  - `TestKisSwatchGroup`, `TestFallBackColorTransformation`: 134 s and 108 s
    alone, time-outs in parallel. Their `QTEST_GUILESS_MAIN` did not set
    `KRITA_PLUGIN_PATH`; `KoJsonTrader` then found no plugin folder and
    `QDirIterator` scanned the working directory recursively, loading every
    `krita*` file in `_build/bin` as a plugin. With `SIMPLE_TEST_MAIN`: 1 s
    and 0 s. Any test main must set `KRITA_PLUGIN_PATH`.
  - `kis_linked_pattern_manager_test` failed once in the parallel run (no
    output) and passed alone; it shared the test profile too.
  - `TestCompositeOpInversion` (598 cases) takes about 200 s alone and 270-290
    s in the parallel run, close to the 300 s limit.

  With the MIME database, `TestSvgParserRoundTrip` went from 8 failures to 5
  and `kis_jpegxl_test` from 3 to 2. Fontconfig warned "Cannot load default
  config file" in tests that load fonts. Resolved (2026-10-08): tests run
  from `_build/bin`, where `KoFontRegistry` looks for
  `<application root>/etc/fonts` (`_build/etc/fonts`, absent). The test mains
  (`sdk/tests/simpletest.h`, `kistest.h`) now set `FONTCONFIG_PATH` to the
  installed `etc/fonts` (`KRITA_FONTCONFIG_DIR_FOR_TESTS` in
  `KoTestConfig.h.cmake`) when it is unset and `fonts.conf` exists. With the
  configuration, `TestSvgText` fails 41 cases instead of 46 (all
  pre-existing; it is not in the ctest list above).
- **A new virtual function in a plugin base class breaks the installed
  plugins** (2026-10-07). Tests load filter plugins from the install prefix.
  After `KisFilter::prefersSingleCall()` was added, `kis_filter_test`,
  `kis_filter_mask_test` and `KisGpuPaintDeviceTest` crashed with
  0xc0000005 in old plugins until `cmake --install` of the whole build.
- **Removing a platform's conditionals: resolve them, do not hand-edit.**
  The Android cleanup (2026-10-08) resolved 267 `Q_OS_ANDROID` lines in 71
  files with a small unifdef-like script that treats the macro as undefined
  and simplifies mixed expressions (`defined(Q_OS_LINUX) &&
  !defined(Q_OS_ANDROID)` -> `defined(Q_OS_LINUX)`). Check afterwards for
  blank lines left at the end of files (`git diff --check`), and that no new
  unused-variable warnings appear.
- **`.ui` edits regenerate tracked `*_ui.py` files.** The user's editor runs
  pyuic6 when a `.ui` file changes, including the tracked
  `libs/ui/forms/*_ui.py`. A broken intermediate `.ui` produced a broken
  `.py`; validate `.ui` XML (`xml.dom.minidom.parse`) before saving, and
  commit the regenerated files with their `.ui`.
- **Git Bash `sed -i` rewrites CRLF files with LF.** Running `sed -i` on a
  CRLF source file converted the whole file, so `git diff` showed every
  line. Edit CRLF files with the edit helper (or Python in binary mode) and
  check `file <path>` afterwards.
- **`Select-Object -First N` stops the producer.** Piping
  `cmake --build` into `... | Select-Object -First 30` in PowerShell ended
  the build early once 30 lines matched. Redirect the build to a log file and
  filter the file instead.
- **The test resource database has no brush tips or patterns.** `kistest.h`
  (`registerResources()`) deletes the test profile and starts the resource
  locator on `<application root>/share/krita`, which does not exist for
  `_build/bin`. A brush editor widget then hits the safe asserts for the
  fallback brush (`KisPredefinedBrushModel.cpp`) and pattern
  (`KisTextureOptionModel.cpp`). A safe assert opens a modal dialog, so the
  test hangs until its time-out unless `KRITA_NO_ASSERT_MSG=1` is set in the
  environment (`qputenv` inside the test was not enough). Found with
  `lldb -p <pid>` on the hung test. `KisBrushTipOptionParityTest` has its own
  `main()` that adds `krita/data/bundles/Krita_4_Default_Resources.bundle`
  with `KisResourceLocator::addStorage()` after `registerResources()`.
- **Create the resource models before `addStorage()`.** Adding a bundle
  loads its presets, which can create a `KisAllResourcesModel` between the
  locator's `beginExternalResourceImport` and `endExternalResourceImport`.
  That model gets the end without the begin and crashes in
  `endInsertRows()`. Call `KisResourceModelProvider::resourceModel()` for
  every type of `KisResourceLoaderRegistry` first.
- **A parity reference must be checked against the old code too.** For
  phase 2a of the brush option shared model, the references were written by
  the old code (with the change set aside as a patch) and compared with the
  old code again: the first serialization split multi-line values into
  lines and did not reproduce. Escape line breaks in values.
- **A brush tip option created alone leaks its configuration page.**
  `KisBrushOptionWidget` does not own its page (the editor's page stack
  does). In a test the page outlives the option, and its preview timer
  crashes in the next `QTest::qWait()`. Delete the page with the option.
- **Restoring a file from a backup copy can skip the rebuild.**
  PowerShell `Copy-Item` keeps the copy's modification time, older than
  the object file, so ninja did not recompile the restored source and the
  tests ran the previous build. Touch the file after restoring it.
- Test executables are named after their source files
  (`kis_liquify_transform_worker_test`, `KisGpuPaintDeviceTest`). Check
  `_build/bin` when a CMake target name is not found.
- **`FreehandStrokeTest` passes since 2026-10-12** (upstream registers it
  with `krita_add_broken_unit_test`). Its references were stale: Mix dull
  was made by code that still honoured the indirect-painting flag (the
  incremental and indirect references differ, the current results do not),
  textured 17 differed by 4 in the alpha of 3 pixels, and the
  `freehand-lod` references never existed. The current deterministic
  results are the references now; the LOD tests seed the stroke
  (`setPreviewRandomSeed(1)`, the `.gih` tip picks randomly) and load the
  Krita 3 bundle for `3_eroded.gih` (`initTestCase()`). A failing stroke
  test writes `<name>.png` and `<name>_expected.png` to `_build/bin`.
- **Open:** before those changes the LOD auto-brush test lost a tile-sized
  part of the stroke in the layer itself 3 times in 92 runs (cut at x or y
  = 256); 95 later runs did not reproduce it, with or without
  `KRITA_GPU_PROJECTION`. Investigate with many repeated runs if it comes
  back.

## Installation

- A running Krita locks plugin and library DLLs. Check that no `krita`
  process runs before installing. Never stop it without the user's approval.
- After installing, compare file hashes of the build and install copies.
- Changes under `libs/*` need that library's `cmake_install.cmake`, not only
  the plugin's.

## Measuring

- To find where time goes, add temporary `QElapsedTimer` prints. Back up the
  file, or remove the lines by marker afterwards. Then rebuild, and confirm
  `git diff` shows no leftovers before formatting and committing.
