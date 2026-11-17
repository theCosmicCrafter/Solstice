# Local asset roots (ai_diffusion) — agent development notes

Solstice feature, added 2026-12-02 (plugin-integration megaplan Phase 1).
Discovers optional machine-local asset folders for the vendored
`ai_diffusion` plugin. **Zero downloads** — files are only ever read from
local disk; a missing root is a graceful no-op (empty index).

## Status and locations

- Implementation: `plugins/python/ai_diffusion/local_assets.py` — a single
  `LocalAssets` QObject singleton (`local_assets`) holding three lazy,
  cached indexes plus the consumers wired so far.
- Settings keys (plugin `settings.json`, group-free flat keys like the rest):
  | Key | Default | Env override (wins over the setting) |
  | --- | ------- | ------------------------------------ |
  | `wildcards_folder` | `D:/wildcards` | `KRITA_AI_DIFFUSION_WILDCARDS_FOLDER` |
  | `poses_folder` | `D:/models/Poses` | `KRITA_AI_DIFFUSION_POSES_FOLDER` |
  | `luts_folder` | `D:/models/LUTs` | `KRITA_AI_DIFFUSION_LUTS_FOLDER` |

  Declared in `settings.py` as ordinary `Setting` entries after
  `_tag_files`; editable in **Settings > Interface** (three `TextSetting`
  rows under the tag-file list). Clearing a value or pointing it at a
  nonexistent folder disables that index.

## What is indexed (schema)

Index keys are always the file path **relative to the root, without
extension, forward slashes, lower case** (`_normalize_key`). Lookups go
through `_normalize_name` (same normalization applied to a user-typed
name, also strips surrounding `/\`).

| Kind | Files | Index | Notes |
| ---- | ----- | ----- | ----- |
| `wildcards` | `*.txt`, `*.yaml`, `*.yml` (recursive) | name → `list[str]` of candidate lines | Two-stage: a fast path index (name → `Path`, ~0.1 s for 2,325 files) plus **per-file memoized** line reads. `wildcard_names`/`wildcard_lines(name)` never bulk-read; the `wildcards` property materializes the whole map (first call ~10 s on the reference library) and is meant for bulk consumers only. `.txt`: non-empty, non-`#` lines. `.yaml`: `yaml.safe_load` flattened to leaf strings when PyYAML is present (it is a *server*-side dep — not guaranteed in Krita's Python); otherwise a lenient line fallback (`- item`, `key: value`). |
| `poses` | `*.png .jpg .jpeg .webp .bmp .tif .tiff` (recursive) | name → `Path` | ~16.9k image files on `D:\models\Poses` (~0.7 s). The folder also contains non-image companions (json/txt/zip), which are skipped by the extension filter. |
| `luts` | `*.cube .3dl .look` (recursive) | name → `Path` | ~10.4k files on `D:\models\LUTs` (~0.9 s). |

`reload(kind)` / `reload()` drops caches; changing any `*_folder` setting
key automatically reloads just that index (`_on_setting_changed` →
`_settings_keys` map). `counts()` reports index sizes for logging/UI.

## Seams

### Wired

- **`__name__` file wildcards in prompts** (Automatic1111 syntax).
  `text.eval_wildcards` calls `local_assets.expand_wildcards(text, rng)`
  inside its existing fixed-point loop — the *only* seam (2 lines + lazy
  import + comment, in `text.py`). Applies automatically everywhere
  `eval_wildcards` already runs: positive, negative and per-region prompts
  in `workflow.prepare_prompts`, including per-line batching and the
  seed-change re-evaluation in `model/model.py`. Unknown names stay
  literal; file content may nest `{a|b}` or further `__name__` (the loop
  iterates to fixpoint, same 10-pass cap). Expansion shares the seed's
  `random.Random`, so results are reproducible per seed.
- **Settings UI**: three `TextSetting` widgets in `InterfaceSettings`
  (`ui/settings.py`, import + 4 lines). `TextSetting` was chosen over
  `LineEditSetting`: the latter never sets `self._widget`, so
  `SettingsTab.write` skips it (`widget.enabled` is falsy).

### Documented only (no clean seam exists yet)

- **Pose library → ControlNet pose input.** `ControlLayer` binds only to
  Krita layers (`layer_id`); there is no file picker in
  `ui/control.py::ControlWidget`. Helper provided:
  `local_assets.create_pose_layer(document, name)` imports the indexed
  image as a `[Pose] <name>` paint layer centered on the canvas —
  the layer can then be selected in the control layer's combo. Intended
  seam: a searchable pose picker in `ControlWidget` shown when
  `mode is ControlMode.pose` (next to "Add Skeleton"), calling
  `create_pose_layer` then assigning `control.layer_id`. Not wired — a
  16k-entry picker needs a real search UI, which is beyond a seam.
- **LUT list → style/look presets.** No LUT/grading hook exists in the
  plugin (styles carry prompts/sampler/lora fields only). Intended seam:
  a `lut` field on `StyleSettings` (style.py) resolved through
  `local_assets.luts`, applied to result images before apply — or a
  post-generation filter. Not implemented; the index + `find("luts", name)`
  are ready.

## Manual verification

1. `python -m compileall plugins/python/ai_diffusion/local_assets.py`
   (done for all touched files).
2. In Solstice, Settings > Interface: confirm the three folder rows show
   the defaults; point Wildcard Folder at a small test folder.
3. Docker prompt: `a __<name>__ photo` with `<name>` = a `.txt` in that
   folder → generated prompt (job metadata `prompt_eval`) contains a
   random line; `__nonexistent__` stays literal. Same seed → same pick.
4. Rename/remove the folder → generation still works, tokens pass through.
5. Python-side check (Krita console): `from ai_diffusion.local_assets import
   local_assets; local_assets.counts()` → index sizes.

## Notes

- `pattern_file_wildcard = r"__(?!_)(.{1,200}?)__"`: non-greedy so names
  containing single `_` work (`__adjective_base__`); `____` stays literal.
- First `__token__` use per session pays ~0.1 s for the name scan; each
  referenced file is read once then memoized. `poses`/`luts` scans are
  name-only (no file I/O beyond stat).
- Env-var override is evaluated at lookup time (`root()`), so it beats the
  settings value without needing JSON edits — the setting stays visible in
  the UI either way.
- Threading: current call sites are main-thread only (prompt preparation
  inside `_generate`). Indexes are plain dicts; add locking before
  introducing a background-scan caller.
