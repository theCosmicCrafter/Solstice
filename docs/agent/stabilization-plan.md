# Stabilization and release action plan — agent reference

Status: **merged** (`fdcdf212`..`df89b79d`, 2026-10-16); unified trial
build `38866815881` on `b6dff5c8` in flight — `pytest tests/e2e` on the
packaged artifact and the manual GUI pass remain.
Derived from a four-lane code audit of `plugin-integration-megaplan.md`
(phases 5–6) and `vector-quality-megaplan.md` (open items V2.3–V6).

Execution rule: all native (C++) fixes land together in **one** unified
GitHub Actions build (takes ~90 minutes), followed by `pytest tests/e2e` on
the packaged artifact, then the manual GUI verification pass. Python-only
fixes were verified in `_vendor_src/qt6venv` with 100% test pass.

## Corrections already applied to the docs (2026-10-15)

- Dates `2026-10-18` (future-dated) changed to `2026-10-15` in both
  megaplans, `feature-inventory.md` and `wiki/log.md`.
- `plugin-intake.md`: Blender Layer default state is **disabled**
  (matches its `.desktop` file, README and the megaplan).
- `plugin-integration-megaplan.md`: status line reconciled with the Phase 5
  header (phases 0–4 complete, 5 built / GUI pending, 6 pending).
- `e2e-testing.md`: 18 → 20 tests. `mcp-bridge.md`: 43 → 42 actions.

Still to correct (assigned in Lane F): tool counts in
`plugin-integration-megaplan.md:41` ("54 tools") and `plugin-intake.md:161`
("41-tool"); `GET /info` advertises 40 actions but the dispatcher has 42
(missing `screenshot`, `trigger_action`).

## P0 — defects in shipped or just-built features

### Lane A — Python plugins, shim and bridge (`plugins/python/` only)

| #   | Finding                                                                                                                                                                                                                                                                                               | Where                                                    | Fix                                                                                                                                                                                                                 |
| --- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| A1  | **Verified.** `QFontMetrics.width = QFontMetrics.horizontalAdvance` does not bind `self` under SIP; every `fontMetrics().width(...)` raises `TypeError`. Hits bulinotes/bulibrushswitch `wcodeeditor.py`, `wconsole.py`.                                                                              | `PyQt5/QtGui.py:33,35`                                   | Assign a wrapper function (`lambda self, *a, **k: self.horizontalAdvance(*a, **k)`) for `QFontMetrics` and `QFontMetricsF`.                                                                                         |
| A2  | **Verified.** `Qt.MidButton` does not exist in PyQt6; the `__members__` hoist claim in `plugin-intake.md` is wrong. Middle-click crashes blender_layer, bulinotes (post-it, image view, node editor), bulibrushswitch.                                                                                | `PyQt5/QtCore.py`                                        | Add `Qt.MidButton` and `Qt.MouseButton.MidButton` → `MiddleButton`. Correct the intake table.                                                                                                                       |
| A3  | **Verified.** `QSortFilterProxyModel.setFilterRegExp` missing; search/tag filtering in bulinotes and bulibrushswitch raises `AttributeError`.                                                                                                                                                         | `PyQt5/QtCore.py`                                        | Alias to `setFilterRegularExpression`; also check `filterRegExp()`, `QRegExp` arguments.                                                                                                                            |
| A4  | After A1–A3, grep every vendored plugin for the remaining checklist items (`setMargin`, `QtXml`, `QVideoWidget`, `QSound`, `QDesktopWidget`) and extend `_vendor_src/test_shim.py` with a regression case per fix.                                                                                    | `plugins/python/*`                                       | Re-run the shim harness.                                                                                                                                                                                            |
| A5  | **Verified.** Bridge binds `'localhost'`, not `127.0.0.1` (IPv6/hosts-file risk).                                                                                                                                                                                                                     | `kritamcp/__init__.py:263`                               | Bind `127.0.0.1`.                                                                                                                                                                                                   |
| A6  | Port 5678 collision raises an unhandled `OSError` in the server thread while the log says "started".                                                                                                                                                                                                  | `kritamcp/__init__.py:262-264`                           | `try/except OSError`, log clearly, expose in `/health`/status.                                                                                                                                                      |
| A7  | **Partly verified.** Only `open`/`save` check for `..`, and only by substring; no allowed roots. `export_document`, `export_layers`, `screenshot` (filename joined onto `CANVAS_OUTPUT_DIR`), `save_as_template` (`category`) are unchecked. Contradicts the megaplan's "export under allowed roots". | `kritamcp/__init__.py` ~939, 990, 1055, 1481, 1700, 1735 | One shared `_resolve_allowed_path()` (realpath + allowed-root list: canvas output, templates, active document folder, user-configured roots) used by every file-writing action; add E2E traversal tests per action. |
| A8  | No request body limit; no `Host` header check.                                                                                                                                                                                                                                                        | `kritamcp/__init__.py:231, 172`                          | 413 above ~50 MB; reject unexpected `Host`.                                                                                                                                                                         |
| A9  | **Verified.** `Settings.restore()` sets `ServerMode.managed`, contradicting "connect to existing ComfyUI".                                                                                                                                                                                            | `ai_diffusion/settings.py:548`                           | `ServerMode.external`; confirm the default at first run too.                                                                                                                                                        |

### Lane B — Display (`libs/ui/canvas`, `libs/ui/opengl`)

| #   | Finding                                                                                                                                                              | Where                                                                     | Fix                                                                                                                     |
| --- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------- |
| B1  | Toggling Smooth Rendering does not repaint the QPainter canvas until pan/zoom (cached `prescaledQImage` is not invalidated).                                         | `kis_prescaled_projection.cpp:127,155-160`, `kis_qpainter_canvas.cpp:268` | Invalidate and re-prescale in `updateSettings()`; repaint.                                                              |
| B2  | **Verified (code).** At ≥200 % the GL branch uses `GL_LINEAR` whenever `smoothHighZoom` (default ON), ignoring `NearestFilterMode`: pixel artists lose crisp pixels. | `KisOpenGLCanvasRenderer.cpp:972`                                         | Condition on `d->filterMode != NearestFilterMode`; same rule in the QPainter path; mention in `docs/canvas-display.md`. |
| B3  | On mipmapped LOD planes `GL_TEXTURE_MAG_FILTER` is never set and keeps a stale value.                                                                                | `KisOpenGLCanvasRenderer.cpp:969-970`                                     | Set `GL_LINEAR` explicitly.                                                                                             |
| B4  | Overview docker rescales with `FastTransformation`.                                                                                                                  | `overviewwidget.cc:177`                                                   | Low priority; use `SmoothTransformation`.                                                                               |

### Lane C — Smart Matting (`plugins/visionml`)

| #   | Finding                                                                                                                                                                                                                            | Where                                      | Fix                                                                                                                                   |
| --- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------- |
| C1  | **BLOCKER.** In _color-labeled layers_ mode the merge is queued asynchronously but `exactBounds()` is read at once on the GUI thread, so the region is empty and the tool silently does nothing. (Code reading matches the audit.) | `SegmentationToolHelper.cpp` ~237, 347-361 | Compute bounds from `image->bounds()` or crop inside the stroke command.                                                              |
| C2  | Lasso mask and source alpha are binarized: antialiased lasso edges and semi-transparent source pixels get full matte.                                                                                                              | `SegmentationToolHelper.cpp:291-294`       | Multiply `matte * mask * alpha / 255²` with rounding.                                                                                 |
| C3  | Wait cursor released when the stroke is _scheduled_, not finished; no progress, no cancel.                                                                                                                                         | `:224, 254-308`                            | Tie cursor/progress to stroke completion; add `KoUpdater`.                                                                            |
| C4  | Crop size uncapped (8K layers allocate hundreds of MB, single-threaded loops); crops under 64 px can fail in GGML.                                                                                                                 | `:238-241, 280-297`                        | Cap (about 3072 px) with downscale, enforce ≥ 64 × 64.                                                                                |
| C5  | Inference failure still commits an empty undo step.                                                                                                                                                                                | `:209, 260-308`                            | `applicator.cancel()` on failure.                                                                                                     |
| C6  | `tryDeselectCurrentSelection` receives image coordinates after padding.                                                                                                                                                            | `:245`                                     | Use view-space rect before padding, as `kis_tool_select_outline.cc:61-68`.                                                            |
| C7  | 8-bit non-sRGB layers skip colour conversion.                                                                                                                                                                                      | `VisionML.cpp:388-397`                     | Compare profile with sRGB before the raw fast path.                                                                                   |
| C8  | No tests exist for visionml.                                                                                                                                                                                                       | `plugins/visionml/`                        | Add the four tests listed in the audit (lasso alpha modulation, labeled-layers mode, colour-space conversion, missing-model failure). |

### Lane D — Trace to Vector (`libs/ui/kis_layer_manager.cc`)

| #   | Finding                                                            | Fix                                                                                                                                                                                                 |
| --- | ------------------------------------------------------------------ | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| D1  | `canvasBase()` dereferenced without a null check (lines ~832-854). | `KIS_SAFE_ASSERT_RECOVER_RETURN`, or take shape controller/resolution from the document and image.                                                                                                  |
| D2  | Fill rule not copied from the outline path.                        | `shape->setFillRule(outline.fillRule())`.                                                                                                                                                           |
| D3  | Alpha scan runs on the GUI thread (seconds on 8K × 8K).            | Move into a stroke job.                                                                                                                                                                             |
| D4  | Threshold fixed at 50 %; colour goes through 8-bit sRGB `QColor`.  | Document honestly now; setting later.                                                                                                                                                               |
| D5  | Zero unit tests (only the E2E).                                    | Add `KisLayerManagerTest::testTraceToVector` (points in pt = px·72/ppi, donut keeps its hole, empty layer is a no-op, single undo step) and `KisPrescaledProjectionTest::testSmoothHighZoomToggle`. |

## P1 — packaging and CI (Lane E)

- E1 `numpy` and `Pillow` are not in `windows-dependencies.lock.json`, but
  `ai_diffusion/image.py` imports them (lines ~552, 605, 671). **Resolved:**
  the bundled CPython 3.13 site-packages contain PyQt6 only, so
  `package-complete.py` now pip-installs `numpy==2.2.6` +
  `Pillow==11.3.0` cp313 win_amd64 wheels into the package
  (`b6dff5c8`).
- E2 `windeployqt` lacks `-multimedia` (`packaging/windows/package-complete.py`
  ~584): the shim exposes `QtMultimedia`. **Resolved:** `-multimedia`
  added to the windeployqt invocation (`b6dff5c8`).
- E3 CI never runs `pytest tests/e2e` (`BUILD_TESTING=OFF`). **Resolved:**
  `run_e2e` `workflow_dispatch` input added (`df89b79d`) — extracts the
  packaged ZIP and runs the suite in launch mode. Feasibility caveat
  stands (hosted runner has no GPU); first run will tell whether
  `solstice.exe` degrades far enough to answer the bridge.
- E4 E2E coverage gaps: ten bridge actions have no test (`clear`,
  `create_from_template`, `draw_shape`, `export_layers`, `get_canvas`,
  `get_layer_presets`, `list_templates`, `move_layer`, `save_as_template`,
  `set_layer_blend_mode`) plus `/info`. Add them together with the A7
  traversal tests.
- Verified clean: ZIP contents (plugins, shims, pigment_o archives, visionml
  DLLs and models, websockets), version `0.2.0-beta.4` consistency, all
  README/docs links.

## P2 — open vector-quality scope, revised

| Item                           | Revised status                                                                                                                                                                                                                                                                                                                                                            |
| ------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| V2.4 SVG/PDF export            | **Scope change.** `plugins/impex/{svg,pdf}` are import-only; there is no SVG/PDF export filter, and every export flattens vector layers. The megaplan's "export fidelity audit" is a non-item. Decision needed: build a `KisSVGExport` on `libs/flake/svg/SvgWriter` (new feature, medium effort) or drop the claim. Until decided, do not promise vector export in docs. |
| V4.4 supersampled export       | Concrete: checkbox in `libs/ui/imagesize/wdg_imagesize.ui`, two-step `scaleImage` (×2 then Lanczos3) at `KisImportExportManager.cpp:726`. Display-only caveat in docs.                                                                                                                                                                                                    |
| V3.1 lineart = stroke layer    | Concrete: `KisConfig` flag + `kis_layer_manager.cc:694/707`, `kis_node_manager.cpp:659`. After D (same file).                                                                                                                                                                                                                                                             |
| V5.2 Smart Matting default     | **Docs were wrong.** A clean install ships only `BiRefNet-lite-F16` (CMake fetches sam, lite, migan). Dynamic and HR-matting must be dropped in by the user. Decide: first-run optional download of Dynamic (plan already forbids bundling GB-scale weights) or correct the docs to say lite.                                                                             |
| V2.3 vector tool QA, V6 matrix | Unchanged; manual.                                                                                                                                                                                                                                                                                                                                                        |
| Rebecca's Krita Tools          | Never vendored and absent from the ledger. Recommend removing from Phase 2 as dropped.                                                                                                                                                                                                                                                                                    |

## Phase 6 — revised exit criteria

1. P0 lanes A–D merged; E1–E2 resolved.
2. One CI build from the merge head; `pytest tests/e2e` green with the new
   tests (trace and smooth-zoom must PASS, not skip).
3. Manual GUI matrix: toggle on QPainter and GL canvases, pixel-art
   `Nearest` mode, Smart Matting lasso in all three sampling modes, bulinotes
   and bulibrushswitch middle-click/search/code editor, Blender Layer off by
   default, AI Diffusion connect + one generation against ComfyUI on
   `:8189` (intake ledger: never verified on a real build).
4. Docs: `docs/smart-matting.md` (new), Smart Matting entry in
   `feature-inventory.md`, fix `vision-ml.md:82` (tool options do include the
   model selector), README note that the nine named extensions are in
   addition to Krita's 22 standard Python plugins, wiki log entry.
5. GitHub Release, then the website update per `AGENTS.md`.

## Delegation plan (lesser agents)

Lanes touch disjoint files, so they can run in parallel in separate
workspaces; the lead agent reviews each diff before merging.

| Lane | Files                                             | Verification                                                    |
| ---- | ------------------------------------------------- | --------------------------------------------------------------- |
| A    | `plugins/python/**`                               | `_vendor_src/test_shim.py`, new shim cases, E2E traversal tests |
| B    | `libs/ui/canvas/*`, `libs/ui/opengl/*`            | compile in CI; manual GUI                                       |
| C    | `plugins/visionml/**`                             | compile in CI; new tests                                        |
| D    | `libs/ui/kis_layer_manager.cc`, `libs/ui/tests/*` | compile in CI; unit + E2E                                       |
| E    | `.github/workflows`, `build-tools`, `packaging`   | dry-run workflow                                                |
| F    | `docs/**`, `README.md`                            | link check, `git diff --check`                                  |

Never revert unrelated working-tree changes; run clang-format on changed
lines of upstream files (see `coding-rules.md`).
