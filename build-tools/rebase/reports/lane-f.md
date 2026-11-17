# Lane F merge report — plugins/python, krita/, packaging/, build-tools/, cmake/

Ran `python build-tools/rebase/rebase.py merge --lane f` (PYTHONUTF8=1):
9 files merged, 0 errors, 1 conflict-marker file (`krita/CMakeLists.txt`).
All conflicts resolved and every file audited for silent fork-line drops
(`git show HEAD:<file>` vs snapshot — every `<` line accounted for).

## Important tool caveat found during this lane

`git merge-file cur base oth` with `base = git show HEAD:<file>` treats the
fork's *committed* customizations as base content. Wherever upstream lacks a
fork-only line, the merge deletes it **silently, with no conflict marker**.
Four such drops were found and restored (below). Other lanes should audit
merged-vs-HEAD diffs for the same failure mode, not just scan for markers.

## Per-file status

| File | Result |
| --- | --- |
| `cmake/modules/SolsticeEmbedRcc.cmake` | Clean absorb of upstream's doc-comment update (`KConfig names and embedded defaults`). Merged == upstream. |
| `krita/CMakeLists.txt` (dirty) | 1 conflict resolved: kept worktree side `KRITA5_XMLGUI_VERSION 550` + hash `d9a36382…` — verified it is the MD5 of the kept worktree `krita5.xmlgui` (upstream's `546`/`7076128f` matches the upstream xmlgui we are NOT taking). Preserved dirty `${krita_windows_stub_ICONS_SRCS}` addition to `krita_windows_stub_com`. |
| `krita/krita.action` (dirty) | Upstream absorbed: 3 clip actions (`create_quick_clipping_group`, `toggle_layer_clipping`, `new_clipping_layer`), 7 animation-curve actions (`interpolation_*`, `tangents_*`, `zoom_to_fit_*`), `keyframe-add`/`keyframe-remove` icons. Fork kept: `rerender_brush_strokes`, `forget_brush_strokes` (dirty worktree additions, survived) and **`layer_to_vector` restored** — merge had silently dropped it (upstream has no trace-to-vector). |
| `krita/kritamenu.action` | Upstream absorbed: `fast_image_export` icon + `fast_image_export_run_all`/`_run_checked`. **`view_smooth_high_zoom` restored** — merge had silently dropped the fork's V1 smooth high-zoom action (wired in KisViewManager/kis_canvas_controller/krita5.xmlgui/e2e tests). |
| `krita/main.cc` | Merged == upstream (superset). Fork's JA/EN inline `solsticeUseJapanese()` prompt replaced by upstream `KisSolsticeProfile::builtInText(en, ja, zh)`; all fork strings preserved inside the calls, ZH added. `builtInText` already exists in `libs/global/KisSolsticeProfile.{h,cpp}`. |
| `packaging/windows/package-complete.py` (dirty) | Upstream absorbed: G'MIC runtime required-file validation + `share/gmic` merge, import reorder, `not X in` style, f-string. Fork kept: extended cache cleanup (`__pycache__`/`.pytest_cache`/`.mypy_cache`/`.ruff_cache`, dirty work survived), launcher/solstice.exe+solstice.com+solstice-launcher copies, QMLDIR args. **Restored the bundled-wheels block** (`pip install numpy==2.2.6 Pillow==11.3.0` cp313/win_amd64 into packaged site-packages) — silently dropped; required by vendored `ai_diffusion` at runtime. |
| `plugins/python/CMakeLists.txt` | **Restored the whole fork vendored block** — merge had dropped it entirely because upstream ships no vendored plugin dirs (verified: none of the 12 dirs exist in the snapshot). Merged = upstream's 13-entry `.action` install list + fork's 9 vendored installs + PyQt shim install. |
| `plugins/python/assignprofiledialog/assignprofiledialog.py` | Clean absorb: upstream fixed action-id typo `assing_profile_to_image` → `assign_profile_to_image`. Consistent with `assignprofiledialog.action` (already in worktree, uses correct id). No other references to the typo remain. |
| `plugins/python/channels2layers/channels2layers.py` | Clean absorb of upstream fixes: empty-bounds early return, `max(1, …)` preview dims, `setInheritAlpha` before mergeDown, group attached to document before building children. No fork deltas. |

## Keep files — untouched

`krita/data/shaders/highq_downscale.frag`, `krita/krita5.xmlgui`,
`krita/windows_stub_main.cpp` (per manifest; the xmlgui version/hash guard
in `krita/CMakeLists.txt` stays aligned with the kept worktree xmlgui).

## Fork installs preserved (`plugins/python/CMakeLists.txt`)

`timer_watch` (`*.png *.mp3 *.webm`), `shortcut_composer` (`*.action`),
`imagine_board` (`*.png *.webm *.webp *.kra *.action`),
`pigment_o` (`*.png *.zip *.webm *.jpeg *.eo *.action`),
`blender_layer` (`*.blend *.action`),
`bulinotes` (`*.svg *.png *.rcc *.qrc`),
`bulibrushswitch` (`*.svg *.png *.rcc *.qrc *.action`),
`ai_diffusion` (`*.svg *.json *.yml *.png *.ico *.css *.action *.js`),
`kritamcp`, plus the `PyQt5`/`PyQt` PyQt6-shim directory install.

## Upstream installs added

`.action` files (now installed via `install(FILES … actions)`):
`assignprofiledialog`, `channels2layers`, `colorspace`, `documenttools`,
`exportlayers`, `filtermanager`, `krita_script_starter`, `scripter`
(+ `mutator`, `plugin_importer`, `tenbrushes`, `tenscripts` already present;
`# highpass` commented as upstream). All 13 `.action` files verified present
in the worktree (lane-a copies the 9 new ones).

## Verification

- `rebase.py scan --lane f` → 0 marker files; manual `<<<<<<<`/`>>>>>>>`
  grep over all 9 files → clean.
- `git diff --check` on all 9 files → clean (stripped 4 trailing-whitespace
  spots that were verbatim upstream content: 1 in `kritamenu.action`,
  3 comment lines in `package-complete.py`).
- `python -m compileall` → OK on `package-complete.py`,
  `assignprofiledialog.py`, `channels2layers.py`.
- `xml.dom.minidom.parse` → OK on `krita.action`, `kritamenu.action`.
- No merged `.yml` files in this lane (yaml check n/a).

## Open questions / notes for other lanes

1. **`plugins/python/` shared dirs are fully synced already.** An
   EOL-normalized whole-tree compare shows *zero* real content diffs in the
   25 shared plugin dirs — the inventory's "real diffs" claim for
   scripter/plugin_importer/mutator etc. is stale (worktree already matches
   upstream modulo EOL). No lane needs to merge them; only the `.action`
   copies (lane-a) and the two `.py` merges (this lane) were real.
2. **`merge-file` silent-drop hazard** (above) — recommend every lane run
   `git show HEAD:<f> | diff - <merged>` and account for every fork-unique
   line, since conflicts only appear when *uncommitted* work collides.
3. `SolsticeEmbedRcc.cmake` now points at a `settings-location.md` section
   named "KConfig names and embedded defaults" — upstream renamed the doc
   section; confirm the fork's doc gets the same rename in whichever lane
   owns `docs/`, or the comment reference dangles (comment only, harmless).
4. `main.cc` takes upstream's `builtInText` helper — requires
   `libs/global/KisSolsticeProfile.{h,cpp}` upstream version, already present
   in the worktree (verified `builtInText` + test exist).
5. Upstream `package-complete.py` requires G'MIC payload files under
   `KRITA_INSTALL_DIR\share\gmic\` incl. `source/gmic-3.7.4.1.tar.gz` and
   `runtime.lock.json` — depends on the `build-tools/gmic/` runtime lane
   landing and the gmic install step actually producing `share/gmic`.
