# Lane A — copy lane report

Date: 2026-11-15 (local). Command:
`set PYTHONUTF8=1 && python build-tools/rebase/rebase.py copy --lane a`

Machine report: `build-tools/rebase/reports/lane-a-copy.json`.

## Counts

| Metric | Value |
| --- | --- |
| Manifest entries (`lane-a.json` `copy`) | 408 |
| Copied OK | 408 |
| Errors ("snapshot file missing" etc.) | 0 |
| New files on disk (did not exist before) | 285 |
| Tracked overwrites (upstream-superset absorbs) | 123 |
| `git status --porcelain` before | 171 |
| `git status --porcelain` after | 447 (+276) |
| `??` untracked entries after | 203 (includes collapsed new dirs) |

Lane A has 0 `merge` entries — pure copy lane.

## Subsystem spot-checks

Byte-identical verification (`snapshot bytes == worktree bytes`) on a 10-file
sample spanning new subsystems and support files: all OK.

| Subsystem | Expected | Found | Notes |
| --- | --- | --- | --- |
| `plugins/filters/solsticeeffects/` | 72 | 72/72 on disk, all in manifest | incl. `SolsticeEffectsPlugin.cpp`, 12 filters + helpers + tests |
| `plugins/extensions/testcontrol/` | ~31 | 31/31 (top-level + `tests/`) | `TestControlPlugin.cpp` byte-verified |
| `plugins/dockers/quickaccess/` new split | 41 in manifest | landed | `QuickAccessGestureDialog.cpp` byte-verified; dir now 63 entries total |
| `libs/ui/lazytools/` | 10 | 10 | FastExport/Dialog/Presets + RenameDialog/Presets `{h,cpp}` |
| `libs/ui/assetlibrary/` | 6 | 6 | Browser/Operations/Settings `{h,cpp}` |
| `libs/ui/brushstrokelayer/` | 4 | 4 | Monitor/Replay/Transform `.cpp` + `Private.h` |
| `libs/image/KisClipStackLayer.{h,cpp}` | 2 | 2 | byte-verified |
| `libs/image/KisDustDetector.{h,cpp}` | 2 | 2 | byte-verified |
| `libs/gpu/KisGpu*Pass.{h,cpp}` | 8 new | landed | ColorTransform/Coverage/ShiftedCopy/StrokeStyle sit beside fork's existing GridWarp/MaskingComposite/Perspective/SeparableConvolution/Transform passes — fork passes untouched |
| `libs/gpu/shaders/` | 5 new | landed | `apply_coverage.comp`, `color_transform.comp`, `composite_over.glsl`, `composite_stroke_style.comp`, `copy_shifted.comp` |
| `plugins/dockers/compositiondocker/CompositionDocumentState.{h,cpp}` | 2 | 2 | byte-verified |
| `build-tools/gmic/` | 4 | 4 | README, build-runtime.py, runtime-patches.json, runtime.lock.json (byte-verified; pins gmic 3.7.4.1) |
| `krita/pics` icons + qrcs | 8 | landed | 6 svgs + 2 qrc files |

## Removed-feature resurrection check

Searched the full manifest for `popup|oncanvas|on_canvas|palette` names: only
`libs/ui/KisPaletteEditor.cpp` (tracked overwrite — the color-swatch palette
editor, unrelated) and `plugins/dockers/quickaccess/QuickAccessDockPopups.cpp`
(documented quickaccess split file). Grepped the new subsystem dirs for
`PopupPalette|OnCanvasBrushEditor|on_canvas_brush` class references: **none**.
No Popup Palette / On-Canvas Brush Editor code was resurrected.

## `krita/main.cc` (upstream-superset check — NOT in lane A)

`krita/main.cc` is a **lane-f merge entry**, not a lane-a copy; lane A did not
touch it. Read-only findings on the current worktree state:

- Already ` M` (pre-existing dirt — a prior merge/partial lane-f run appears
  to have landed it; 0 conflict markers in the file).
- The fork's Krita-profile import prompt is present and uses
  `KisSolsticeProfile::builtInText(...)` with **three** strings
  (English/Japanese/Simplified Chinese) — i.e. the worktree already carries
  upstream's EN/JA/ZH superset, which covers the fork's JA-only prompt.
- `libs/global/KisSolsticeProfile.h` (also pre-modified, not lane-a) declares
  `builtInText(english, japanese, chinese)` — consistent.
- No edit made. Lane F should still run the manifest merge for `main.cc` as
  planned (merge-file will see the upstream side already applied and should
  produce no conflicts).

## `git diff --check`

- **Lane A's own 123 tracked overwrites: clean** (scoped run:
  `git diff --check -- <lane-a tracked paths>` → no output).
- Tree-wide `git diff --check` shows findings **only in files belonging to
  lanes C and F** (pre-existing worktree dirt, not introduced by this run):
  - Leftover conflict markers: `krita/CMakeLists.txt` (lines 121/127/133),
    `libs/ui/KisBrushStrokeLayer.cpp` (101/338/340/414/562/563),
    `libs/ui/canvas/kis_prescaled_projection.cpp` (107/112/114/124/129) —
    all are lane-c or lane-f merge targets; unresolved merge output already
    in the tree before lane A ran.
  - Trailing whitespace: `krita/kritamenu.action:174`,
    `libs/ui/KisViewManager.cpp:1398`, `libs/ui/KisViewManager.h:221`
    (lane-c/lane-f files).
- ~420 `LF will be replaced by CRLF` warnings are expected autocrlf noise:
  snapshot bytes were written verbatim as LF; git's clean filter normalizes.

## Anomalies / follow-up items

1. **Pre-existing conflict markers in lanes C/F files** (listed above) — for
   the merge lanes to resolve; not lane-A scope.
2. **`krita/main.cc` already carries upstream's superset** — flag for lane F:
   verify the prior merge is complete and intentional before re-running.
3. **New files reference both-modified plumbing** not yet landed: the copied
   subsystems include into headers/CMake registrations that live in lanes
   B–F (`libs/image/CMakeLists.txt`, `libs/ui/CMakeLists.txt`,
   `plugins/filters/CMakeLists.txt`, `plugins/extensions/CMakeLists.txt`,
   `default_tools.cc`, `tools.action`, etc.). The tree will not build until
   the merge lanes land — expected per the recommended merge order.
4. **`plugins/dockers/quickaccess/`**: 41 manifest files landed alongside the
   fork's monolith (`QuickAccessDock.cpp` et al. are lane-c/d merges). Audit
   for unported fork deltas remains per inventory §9 — deferred to those lanes.
5. No snapshot files were missing; nothing was improvised.

## Hard-rule compliance

No commits/stashes/resets/checkouts; no builds or tests run; no solstice.exe
launched; only manifest paths copied; snapshot bytes written verbatim.
