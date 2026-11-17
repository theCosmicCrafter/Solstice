# Plugin intake ledger — agent reference

Tracks vendored third-party Python plugins: source, pinned revision, local
patches and update cadence. Mirrors the `upstream-intake.md` approach — every
vendored plugin diverges from its upstream, so record what makes replaying an
update possible.

## Ledger format

```
### <plugin name>
- Source: <repo URL> (+ mirror, if any)
- Pinned: <release tag or commit sha> (<date>)
- Vendored at: plugins/python/<dir>/
- License: <spdx>
- PyQt6 status: native | shim-only | patched (files touched)
- Local patches: <list of files and why>
- Default state: enabled | disabled
- Last re-check: <date> — <result: up to date | update available>
```

## Entries

### timer_watch

- Source: https://github.com/EyeOdin/Timer_Watch
- Pinned: master `d9f5dd6afba5e80cbfdb3026076c93da5db87c89` (2026-01-06),
  no tagged release
- Vendored at: plugins/python/timer_watch/
- License: GPL-3.0-or-later (per plugin header; repo declares GPLv3+ LGPLv3+)
- PyQt6 status: shim-only — no source edits. Covered by `plugins/python/PyQt5`.
  Verified 2026-10-14 in a standalone PyQt6-6.11 venv (offscreen platform):
  module import, `uic.loadUi` of both .ui files, `QMediaPlayer`/`QMediaContent`
  alarm path, unscoped enums (`Qt.NoModifier`, `Qt.AA_*`), `exec_()`, and
  `Krita.instance()`/`notifier`/`icon`/`readSetting` call surface all pass
  under a stub `krita` module.
- Local patches: none
- Default state: enabled (`X-Solstice-EnabledByDefault=true` in
  `kritapykrita_timer_watch.desktop`)
- Last re-check: 2026-10-14 — vendored, shim-verified; GUI click-through
  pending a real Solstice build

### shortcut_composer

- Source: https://github.com/wojtryb/Shortcut-Composer
- Pinned: `e3fe151c01387c2d14d1d9ebfae0b85c36645e49` (master, 2026-10-14)
- Vendored at: plugins/python/shortcut_composer/
- License: GPL-3.0
- PyQt6 status: upstream already dual-Qt — it ships its own `PyQt`
  unversioned package that imports PyQt6 when present. No shim needed.
- Local patches: `__init__.py` — the Krita-6 OpenGL startup warning is
  removed entirely. Solstice renders via its Vulkan engine and exposes no
  OpenGL toggle, so the warning could never be satisfied and blocked every
  first paint with a modal.
- Default state: enabled
- Last re-check: 2026-10-14 — vendored; pie menus/trackers need GUI
  verification on a real build

### imagine_board

- Source: https://github.com/EyeOdin/imagine_board
- Pinned: `fe56088a65d7c5217ccedd92222852311fd33141` (master, 2026-10-14)
- Vendored at: plugins/python/imagine_board/
- License: GPL-3.0
- PyQt6 status: shim-only — `from PyQt5 import Qt, ...` imports resolve via
  the shared shim; import-level verified 2026-10-14
- Local patches: none
- Default state: enabled
- Last re-check: 2026-10-14 — vendored; overlaps native Asset Library —
  evaluate deduplication in a later phase

### pigment_o

- Source: https://github.com/EyeOdin/Pigment.O
- Pinned: `046a13854be082cc115413b7341fd9a6050e33d8` (master, 2026-10-14)
- Vendored at: plugins/python/pigment_o/
- License: GPL-3.0
- PyQt6 status: shim-only; import-level verified 2026-10-14
- Local patches: none
- Default state: enabled
- Last re-check: 2026-10-14 — vendored; bundles 33 .zip resource archives
  (gamut/mask presets) installed via `*.zip` pattern

### blender_layer

- Source: https://github.com/Yuntokon/BlenderLayer
- Pinned: `6df69d2278e51800916ad1b8841aada7f9e3c566` (master, 2026-10-14)
- Vendored at: plugins/python/blender_layer/
- License: GPL-3.0
- PyQt6 status: shim-only; import-level verified 2026-10-14. Depends on
  pykrita's builtins injection (`builtins.Krita` — confirmed present in
  `plugins/extensions/pykrita/plugin/krita/__init__.py`), so the bare
  `Krita.instance()` in `navigateWidget.py` needs no patch.
- Local patches: none
- Default state: enabled
- Last re-check: 2026-10-14 — vendored; function requires the Blender-side
  companion add-on (bundled `*.blend` assets ship with the plugin)

### bulinotes

- Source: https://codeberg.org/Grum999/BuliNotes (GitHub mirror archived)
- Pinned: `4a3655eaec339cadd4250656f2a283b886604a09` (main, 2026-10-14)
- Vendored at: plugins/python/bulinotes/
- License: GPL-3.0
- PyQt6 status: shim-only (`from PyQt5.Qt import *` covered by the `Qt`
  catch-all shim module); import-level verified 2026-10-14. Ships
  Qt5-compiled `.rcc` icon resources loaded through pktk's UITheme —
  Qt6 `QResource` reads them; verify icon rendering on a real build.
- Local patches: `pktk/widgets/wseparator.py` — `VLine|Sunken` split into
  `setFrameShape` + `setFrameShadow` (PyQt6 rejects the int produced by
  OR-ing two enums)
- Default state: enabled
- Last re-check: 2026-10-15 — vendored; upstream is archived, treat as
  effectively self-maintained

### bulibrushswitch

- Source: https://codeberg.org/Grum999/BuliBrushSwitch (GitHub mirror archived)
- Pinned: `8ca1523f41b169d5ba59bb6a9e7699b5658ff7ed` (main, 2026-10-14)
- Vendored at: plugins/python/bulibrushswitch/
- License: GPL-3.0
- PyQt6 status: shim-only; import-level verified 2026-10-14. Same pktk
  `.rcc` caveat as bulinotes. `QSignalMapper.mapped[str]` bridged in the
  shim (`ekrita_tools` tool-change tracking).
- Local patches: `pktk/widgets/wseparator.py` — `VLine|Sunken` split into
  `setFrameShape` + `setFrameShadow` (PyQt6 rejects the int produced by
  OR-ing two enums)
- Default state: enabled
- Last re-check: 2026-10-15 — loads clean on a real build (windowCreated
  path exercised, no tracebacks)

### ai_diffusion

- Source: https://github.com/Acly/krita-ai-diffusion +
  local enhanced clone `C:\Users\richk\CascadeProjects\krita-ai-diffusion`
- Pinned: upstream `origin/main` (post-1.53, ~commit after tag v1.53.0)
  merged with the local enhancement set originally based on `ef1b04c`
  (2026-03-26). Merge tree kept in `_vendor_src/ai_diffusion_upstream`
  (branch `enhancements`, merge `f628900`).
- Vendored at: plugins/python/ai_diffusion/ (minus `debugpy` dev debugger
  and the `.git` internals of the `websockets` submodule; vendored
  websockets @ `624a36cc` ships as source)
- License: GPL-3.0
- PyQt6 status: native — upstream migrated to PyQt6; vendored tree imports
  PyQt6 directly. On a Qt5 Solstice build (`HAVE_PYQT6` false) the
  reverse shim `plugins/python/PyQt6/` (PyQt6 → PyQt5, only `QAction`
  needs relocating) is installed so the plugin loads; on Qt6 builds it
  is not installed.
- Local patches:
  - `__init__.py` — the Krita 6.x version gate also accepts the Qt5
    Krita 5.3.x host when the reverse PyQt6 shim is present
    (2026-11-16; v1.51 was the last release that targeted Krita 5)
  - `util.py` `exclusive_poll_tick`, used by `document.py`
    `KritaDocument._poll_tick` and `layer.py`
    `LayerManager._update_tick` (2026-11-16) — every open document
    owns a 20 ms poll timer and a 500 ms layer timer, and both call
    `Document.activeNode()`, which pumps the event loop
    (`forceDeliverAllSynchronizedEvents`). Each tick then fired the
    other documents' timers nested inside itself, so nesting grew with
    the number of open documents and about 23 documents overflowed the
    GUI stack (0xC00000FD). The shared guard skips a nested tick.
    Direct `update()` calls are not guarded. Keep both patches on
    re-vendor.
  - `document.py` `PoseLayers._timer` also wrapped in
    `exclusive_poll_tick` (2026-11-16) — a module-level singleton with an
    unconditional 500 ms timer calling `KritaDocument.active_instance()`;
    same re-entrancy class, found by the resource-management audit.
  - `style.py` `Styles.find_style_files` skips `.json` files under
    hidden folders (2026-11-16). Before this, an editor's
    `.vscode/settings.json` in the user styles folder loaded as a style
    with no checkpoints, named "Default Style".
  - `settings.py` — default `server_url` is `127.0.0.1:8189`
    (ComfyUI Desktop) instead of upstream `127.0.0.1:8188`; new
    `civitai_lookup` (opt-in) and `show_legacy_styles` (default off)
    settings
  - enhancements re-applied onto upstream: `Quantization`/`quantization`
    field kept on `CheckpointInfo`, parallel `discover_models`,
    extra model search-path patterns merged union-style, async file
    hashing in `files.py`, settings additions
  - Phase 7 deepening (2026-10, `plugin-integration-megaplan.md` Phase
    7): `backend/modelspec.py` safetensors header parsing +
    `ModelMetadataCache` metadata-driven `deduce_from_filename` wiring
    (`backend/comfy_client.py` filename→path resolver); `Arch.generic`
    fallback architecture; `backend/civitai.py` hash-lookup client +
    `ui/model_browser.py` card browser (opt-in); `backend/sweep.py`
    `{{axis:…}}` parameter sweeps; `text.py` `@snippet` expansion;
    `model.py` per-line batching, layered-result semantic names +
    `ai_diffusion_params` runtime provenance, `matte_output`/
    `vectorize_output` post-processing (Smart Matting / Trace to
    Vector); `ui/generation.py` Image Compare + Image Grid;
    `presets/models.json` SOTA-first curation (`legacy_checkpoints`
    split, ancillaries demoted to `optional`)
  - Bug fix: `Arch.sd35` referenced by `from_checkpoint_name` did not
    exist — model folders named `sd3.5/` crashed discovery
  - Phase 7.10 model metadata (2026-10-26, `openfill-intake.md`):
    `ModelResource.license` / `min_vram_gb` fields in
    `backend/resources.py` (parsed + round-tripped via `as_dict`);
    `presets/models.json` annotated — non-commercial flags on FLUX
    dev/Kontext checkpoints, the Flux VAE and MAT inpaint,
    `apache-2.0`/`mit`/`openrail` labels elsewhere; `ui/server.py`
    renders `name · license` on package rows and disables
    over-VRAM entries with a "Needs N GB VRAM" status fed by the
    connected client's `device_info` (`PackageGroupWidget.device_vram`)
  - Lora-Manager intake (2026-10-26): `optional_custom_nodes` gained
    `ComfyUI-Lora-Manager` (willmiao, MIT, pinned commit
    `1fb1d5bb` = v1.2.6) — its manager UI serves at `<server>/loras`
    on the managed ComfyUI; `_install_custom_node` now runs
    `pip install -r requirements.txt` for nodes that ship one (a
    general installer improvement — previously only ComfyUI's own
    requirements were installed); `backend/modelspec.py`
    `extract_sidecar_metadata` reads LM's `<name>.metadata.json`
    sidecars into `File.metadata` (`modelspec.title`/`base_model`,
    `trigger_words`/`lora_triggers`, `civitai` block) so LM-managed
    models show name/arch/triggers in the model browser with no
    network lookup. Runtime verified 2026-10-27: node installed at
    `D:\krita\ComfyUI\custom_nodes\ComfyUI-Lora-Manager` (pinned
    1fb1d5bb), 16 deps in managed venv, import clean, `/loras`
    serves HTTP 200 on :8188.
  - Phase 8.3 installer hardening (2026-10-27): `_extract_archive`
    zip-slip validation; `_verify_downloaded` inline sha256 check
    after every model download + re-download (removes corrupt files
    fast); `_write_metadata_sidecar` emits LM-compatible sidecars for
    managed downloads (never overwrites existing). Verified upstream
    already ships: Range-resume `.part` downloads, 3x retry, stall
    timeout, `try_install` rollback, ComfyUI-GGUF node, GGUF catalog
    entries.
  - Phase 8.1 model wave (2026-10-27): cherry-picked upstream PR
    #2641 (Flux.3 Klein: `Arch.flux3`, qwen3vl_4b clip wiring,
    sampler + style, VAE mapping); 3 hunks hand-merged
    (resources/workflow/samplers). PR's `tests/*` files absent
    locally — not ported. Added catalog entries with HF-API-verified
    sha256: Qwen 2.1 int8 + Turbo int8 checkpoints, `qwen3vl_8b` /
    `qwen3vl_4b` encoders, `vae-default-qwen2`, `vae-default-flux3`;
    styles `qwen-image-2[-turbo].json`. Flux3 checkpoint download
    stays manual — BFL repo license-gated, no anonymous hash.
    URL liveness sweep: all 77 existing file URLs return 200.
  - MOSS Image (2026-10-26): cherry-picked upstream PR #2640 —
    `Arch.moss`, `moss_image` clip type in workflow, MOSS samplers,
    `styles/moss-image.json`, theme icons, `lora-edit-moss` catalog
    entry, edit gating via `TextEncodeMOSSImageEdit` node +
    edit-LoRA availability. Merge fixes applied locally:
    `Arch.moss` added to `Arch.list()` (without it per-arch scans
    silently skip VAE/encoder resolution), `ui/upscale.py` bad
    `..backend.image` import corrected to `..image`, `qwen35formoss`
    added to `qwen_35_9b` encoder patterns (matches the user's
    original `qwen35ForMossImage_v10` file). Weights are the user's
    own: `moss_image_sft_int8_convrot` + `qwen3.5_9b_w4a8_gptq`
    copied F:→`D:\models`; ComfyUI core upgraded to commit
    `b9b290e` (0.42.0-dev) which carries native `moss_image`/`flux3`
    support (custom_nodes, user, input, output,
    extra_model_paths.yaml preserved). `TextEncodeMOSSImageEdit` is
    NOT in that core — edit mode stays gated off until a later
    ComfyUI or a custom node provides it; t2i is unaffected.
    kritamcp `ai_status`/`ai_*` now surface the real plugin import
    error when `ai_diffusion` is absent from sys.modules instead of
    a blanket "not enabled".
  - Runtime verified (2026-10-27, external ComfyUI at D:\krita on
    :8188, RTX 5090 32 GB): `ai_connect` → connected; 200 checkpoints
    + 2420 loras indexed from D:\models junction; `/loras` HTTP 200
    (LM node imported clean); `ai_generate` style "Qwen Image 2.1" →
    job finished, `ai_apply` → generated layer on canvas. Same
    session also verified: style "MOSS Image" → job finished,
    3 results, generated layer applied; `ai_catalog` lists 22 arches
    incl. Flux 3 + MOSS Image; `ai_install{packages}` targeted
    downloads with `.part` Range-resume verified on a stalled
    1.98 GB controlnet (resumed at byte offset). Note:
    `server_state` stayed `update_required` (version-file mismatch)
    in external mode — cosmetic; heavyweight `upgrade()` is the fix
    if it ever blocks managed autostart.
  - Native live engine (2026-11-16, megaplan Phase 3 — see
    `docs/agent/native-live-engine.md`): NEW FILE `native_engine.py`
    (ctypes bridge to `librediffusion.dll`, engine discovery, klein
    stream wrapper, per-document `NativeLiveEngine`); `settings.py`
    `live_engine` + `native_engine_dir` settings; `ui/settings.py`
    combo + text row; `model/model.py` — `LiveWorkspace` owns the
    engine, `_continue_generating` branches on `live_engine` with
    one-shot error report + ComfyUI fallback, `toggle(False)` releases
    the stream. Keep all four on re-vendor.
- Default state: enabled
- Last re-check: 2026-10-27 — live-verified on Solstice-0.5.0 build;
  external ComfyUI path exercised end-to-end (connect → generate →
  apply). Managed-mode autostart still gated by `update_required`
  until `upgrade()` runs.
- Re-vendor trigger: upstream `v1.54.0` final (due ~end of Oct 2026;
  `v1.54.0-pre` shipped 2026-10-03 with the upstream Krita 6/Qt6
  migration and Qwen Image 2.1 support). Procedure: fast-forward
  `_vendor_src/ai_diffusion_upstream` `origin/main`, re-apply the
  `enhancements` branch (merge `f628900` is the template), keep the
  `:8189` server_url patch, re-run plugin load + ComfyUI connect check.
- Runtime verification owed: connect to ComfyUI Desktop on `:8189`,
  run one generation, confirm result lands as a layer. Manual GUI
  matrix item.

### kritamcp

- Source: https://github.com/nanayax3/krita-mcp, local clone at
  `C:\Users\richk\CascadeProjects\krita-mcp` @ `83fec0e`
- Pinned: local clone `83fec0e` (41-tool HTTP bridge)
- Vendored at: plugins/python/kritamcp/
- License: MIT
- PyQt6 status: shim-only (`PyQt5` imports covered); import-level
  verified 2026-10-14
- Local patches:
  - bearer-token auth added to the loopback bridge — generated once via
    `secrets.token_hex(24)`, stored in kritarc group `kritamcp`,
    constant-time compared; `/health` stays open, `/info` + all POSTs
    require `Authorization: Bearer`
  - companion change outside this repo: `krita-mcp/server.py` reads the
    token from `KRITAMCP_TOKEN` or kritarc and sends the header
  - Phase 7.10 AI-generation actions (2026-10-26, `openfill-intake.md`):
    `ai_status`, `ai_styles`, `ai_generate` (modes auto/fill/remove/
    expand/create through `root.model_for_active_document()` →
    `DocumentModel.generate()` in-process on the shared managed
    ComfyUI), `ai_expand` (canvas grow + select-all + outpaint in one
    call — Photoshop Generative Expand), `ai_jobs`, `ai_apply`
    (apply any result as a layer), `ai_discard` (delete a variation),
    `ai_cancel`. Async enqueue: `ai_generate` returns
    `jobs_from_index`; callers poll `ai_jobs`. `apply:true` (default)
    auto-applies result 0 on `job_finished` unless the user's
    finished-action already does. Mutated model props are restored
    after enqueue; the prompt stays for docker transparency.
    Details: `docs/agent/mcp-bridge.md` §AI generation
- Default state: enabled
- Last re-check: 2026-10-14 — vendored; end-to-end MCP round-trip needs a
  real build + the external server running

### lookfx — local source asset (planned feature port, Phase 11.5)

- Source: `F:\lookfx` (user's own project; also `F:\lookfx-dist`,
  `F:\lookfx-wt` worktree). Upstream license: see `F:\lookfx\LICENSE`
- Not yet vendored. Target: halftone screens / paper substrates /
  capillary bleed / plate misregistration / C-M-Y-K separations as a
  Solstice generator layer + filter (Phase 11.5, megaplan).
- Relevant code: `src/lookfx_core/` (effect.py, colorspace.py,
  tensors.py, chain.py — torch GPU pipeline, chunk-invariant rendering,
  in-memory texture cache patterns) and `src/cmykmagic/` (print presets,
  paper substrates, ink params).
- Port decision open: preferred native filter port; fallback embedded
  python GPU worker.

### flarecore — local source asset (planned feature port, Phase 11.5)

- Source: `F:\AIPrograms\_Dev_Projects\flarecore`
- Upstream: optical engine authored by cyco.creates — **Apache-2.0**
- Not yet vendored. Target: lens-flare generator/filter (41 presets, 182
  element textures under `src/flarecore/elements/`, depth-map occlusion,
  auto light-source detection, multi-frame tracking, linear-space torch
  render). Also the code reference for Harmonize (9.2) light inference.

### cosmic_nodes_v2 — local source asset (reference/mining)

- Source: `F:\cosmic_nodes_v2` (user's own 174-node ComfyUI pack;
  worktrees `cosmic_nodes_v2-baseline`, `cosmic_nodes_v2-wip`)
- Not vendored into Solstice. Relevant for phases 9-12 mining:
  `nodes/hdr/` (HDR pipeline), `nodes/video/` (video gen helpers),
  `nodes/lora/`, `nodes/prompt/` + `cosmic_prompt_server.py` (LLM prompt
  building — useful for ideation-surface prompt mutation later),
  `nodes/image/`, `nodes/save/` (ProRes/FFV1 writers).
- License: see `F:\cosmic_nodes_v2\LICENSE`; `agpl_licensed/` subtree is
  AGPL — keep anything ported OUT of that subtree.

## PyQt5 -> PyQt6 compatibility shim

`plugins/python/PyQt5/` is a shared re-export layer installed to
`share/krita/pykrita/PyQt5` next to the bundled plugins. Vendored plugins keep
their `from PyQt5 import ...` lines unchanged. A second package,
`plugins/python/PyQt/`, serves plugins that use the unversioned `PyQt`
import name.

Both packages additionally expose:

- `Qt.py` — the `PyQt5.Qt` catch-all module (`from PyQt5.Qt import *`)
  combining QtCore/QtGui/QtWidgets/QtSvg/QtPrintSupport
- `QtSql.py` — re-export of `PyQt6.QtSql`

The upstream import guard in
`plugins/extensions/pykrita/plugin/krita/__init__.py`
(`importAvoidWrongPyQtHack`) normally blocks `PyQt5` entirely on a Qt6 build.
It was extended to allow a `PyQt5` top-level package only when its spec
resolves inside a `pykrita` directory — i.e. our pure-Python shim — while
still rejecting the real PyQt5 bindings (which would load a second Qt and
crash). `PyQt` (unversioned) is not affected by the guard.

Current coverage:

| Qt5 surface                                                                                                  | Shim behaviour                                                                                                                                                       |
| ------------------------------------------------------------------------------------------------------------ | -------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------- | ------------------------------------------------------------------------------------------------------------------------------------------------ |
| `from PyQt5 import QtX` submodules                                                                           | Re-export the PyQt6 modules (`QtCore`, `QtGui`, `QtWidgets`, `QtMultimedia`, `QtNetwork`, `QtPrintSupport`, `QtSql`, `QtSvg`, `QtSvgWidgets`, `QtXml`, `uic`, `sip`) |
| `from PyQt import QtX`, `from PyQt5.Qt import *`                                                             | `PyQt` unversioned alias package + `Qt` catch-all module combining QtCore/QtGui/QtWidgets/QtSvg/QtPrintSupport                                                       |
| Unscoped enums (`Qt.AlignLeft`, `Qt.NoModifier`, `QEvent.MouseButtonPress`, `QMessageBox.Ok`, ...)           | Every enum member hoisted onto its class (`_expose_enum_members`); uses `__members__` so zero-valued flags (`NoModifier`) and aliases survive                        |
| `exec_()`                                                                                                    | Alias to `exec()` on every PyQt6 class that has `exec`                                                                                                               |
| `QAction`/`QFileSystemModel`/`QShortcut`/`QUndo*`/`QStandardItem*` in QtWidgets                              | Re-exported from PyQt6.QtGui                                                                                                                                         |
| `QRegExp`                                                                                                    | Subclass wrapper over `QRegularExpression` (`exactMatch`, `indexIn`, `cap`, case sensitivity)                                                                        |
| `QSignalMapper.mapped[str                                                                                    | int                                                                                                                                                                  | QObject]` | Property returning a subscriptable proxy over Qt6's `mappedString`/`mappedInt`/`mappedObject` (the Qt5 `QWidget` overload has no Qt6 equivalent) |
| `QRegExpValidator`                                                                                           | Alias to `QRegularExpressionValidator`                                                                                                                               |
| `QMediaPlayer.setMedia`/`setVolume`/`media`, `QMediaContent`                                                 | Wrapper classes; volume bridged through an owned `QAudioOutput`, `setMedia` maps to `setSource`                                                                      |
| `QDesktopWidget` / `QApplication.desktop()`                                                                  | Minimal QWidget stand-in backed by `QGuiApplication` screens (`screenGeometry`, `availableGeometry`, `screenCount`)                                                  |
| `QFontMetrics.width()`                                                                                       | Wrapped callable forwarding to `horizontalAdvance()` (avoids SIP unbound descriptor TypeError)                                                                       |
| `Qt.MidButton` / `Qt.MouseButton.MidButton`                                                                  | Aliased to `Qt.MouseButton.MiddleButton` in `QtCore.py`                                                                                                              |
| `QSortFilterProxyModel.setFilterRegExp()`                                                                    | Subclass method mapping string/QRegExp/QRegularExpression to `setFilterRegularExpression`                                                                            |
| `QLayout.setMargin(m)`                                                                                       | Wrapper calling `setContentsMargins(m, m, m, m)` in `QtWidgets.py`                                                                                                   |
| `QMouseEvent`/`QWheelEvent` `pos()`, `globalPos()`, `x()`, `y()`, `localPos()`, `windowPos()`, `screenPos()` | QPoint/QPointF bridges over `position()`/`globalPosition()`/`scenePosition()`                                                                                        |
| `PyQt5.uic`                                                                                                  | `PyQt6.uic` re-export (`loadUi`, `loadUiType`, `compileUi` verified)                                                                                                 |
| `PyQt5.sip`, `import sip`                                                                                    | `PyQt6.sip` re-export                                                                                                                                                |
| `PyQt5.QtXml` (`QDom*`)                                                                                      | Re-export when Qt5Compat is present; raises a descriptive ImportError otherwise — patch the plugin at vendor time                                                    |

Known limits (patch the plugin, don't extend the shim):

- `QMouseEvent.buttons()` during move events is unchanged, but synthesised
  events in plugins that relied on `Qt5` screen coordinates need review.
- Qt Multimedia widget classes (`QVideoWidget`, `QCamera*`) are not wrapped;
  Qt6's capture/session APIs differ structurally.
- `QSound`, `QSoundEffect` Qt5 constructors with different signatures are not
  normalised.
- `QtXml` without Qt5Compat is unavailable by design.

### krita-smart-select (native port — not a pykrita vendor)

- Source: `C:\Users\richk\cascade-projects\krita-smart-select-windows`
  (user's Windows port, commit `493cb9b`; upstream BMFreed/krita-smart-select)
- Disposition: ported natively into `plugins/visionml` as the Smart Matting
  Selection tool (`src/smartselect/SmartMattingTool.cpp`,
  `SegmentationToolHelper::applyMattingMask`). The external Python worker,
  `QProcess` launcher, and `.npy` mask exchange were dropped — inference runs
  in-process through `VisionModels::removeBackground` (BiRefNet via
  vision.cpp, CPU or Vulkan).
- Behaviour preserved: freehand lasso region, padded crop, soft-alpha matte
  clipped to lasso ∩ layer alpha, grow/feather/anti-alias and all five
  selection actions, sampled-layers modes (current / all / color-labeled).
- Model: reuses the `background_removal` task slot; BiRefNet-HR-matting GGUFs
  dropped into `visionml/models` raise matte quality.
- License: GPL-3.0-or-later (upstream), port is a rewrite on the visionml
  architecture.
- Last re-check: 2026-03 — ported; pending real-build GUI verification.
- Note: the user's legacy pykrita `smart_select` install under
  `%APPDATA%\Solstice\resources\pykrita` was set aside
  (`smart_select.disabled`) — its ctypes DLL loader is superseded by the
  native tool.

## Shim test harness

`_vendor_src/` (workspace, not committed): `qt6venv` holds PyQt6 6.11,
`krita_stub.py` fakes the `krita` module (`DockWidget`, `DockWidgetFactory`,
`Krita.instance()` with `notifier`/`icon`/`readSetting`/`writeSetting`,
plus star-exported Qt names like the real module), `test_shim.py` runs the
import + docker-instantiation pass. Re-run per vendored plugin:

```
QT_QPA_PLATFORM=offscreen PYTHONUTF8=1 _vendor_src/qt6venv/Scripts/python.exe -B _vendor_src/test_shim.py
```

## Plugin default-enable key

`kritapykrita_<name>.desktop` may carry `X-Solstice-EnabledByDefault=true`.
`PythonPluginManager::scanPlugins` uses it as the fallback default for
`enable_<module>`; a stored user setting still wins, so disabling a vendored
plugin persists.

## PyQt5 -> PyQt6 delta checklist (for vendoring)

Apply at vendor time; prefer the shared shim over per-plugin rewrites:

- Module name: `PyQt5` -> shim (`plugins/python/PyQt5`) — leave imports as-is
- `QAction`, `QActionGroup`, `QShortcut`: QtWidgets -> QtGui (shim re-exports)
- `QRegExp` -> `QRegularExpression` (shim wraps common surface)
- `exec_()` -> `exec()` (shim aliases)
- Enum scoping: unscoped `Qt.*`/`QEvent.*`/`QMessageBox.*` names (shim hoists);
  `Qt.MidButton` -> `Qt.MiddleButton` is a rename the hoist also covers via
  `__members__` aliases
- `QDesktopWidget` removed -> shim stand-in over `QGuiApplication` screens
- `sip` module: `PyQt6.sip` re-exported
- High-DPI: `AA_EnableHighDpiScaling` removed (default in Qt6) — flag is a
  no-op if referenced; prefer deleting the call
- `QLayout.setMargin` -> `setContentsMargins` (shimmed in QtWidgets.py)
- `QMediaPlayer`/`QMediaContent` -> shim wrapper over `setSource`/`QAudioOutput`
- `QtXml`/`QDom*` -> requires Qt5Compat; otherwise patch to `QXmlStreamReader`
- Verify: signal names (`valueChanged(int)` overloads), `QProcess` start/finish
  signals, `QFileDialog` static signatures, clipboard/screen grabs
