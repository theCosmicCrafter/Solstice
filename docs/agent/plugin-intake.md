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
  PyQt6 directly, no shim needed
- Local patches:
  - `settings.py` — default `server_url` is `127.0.0.1:8189`
    (ComfyUI Desktop) instead of upstream `127.0.0.1:8188`
  - enhancements re-applied onto upstream: `Quantization`/`quantization`
    field kept on `CheckpointInfo`, parallel `discover_models`,
    extra model search-path patterns merged union-style, async file
    hashing in `files.py`, settings additions
- Default state: enabled
- Last re-check: 2026-10-14 — vendored; server connect/generation needs a
  real build + ComfyUI on :8189

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
- Default state: enabled
- Last re-check: 2026-10-14 — vendored; end-to-end MCP round-trip needs a
  real build + the external server running

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
| `QFontMetrics.width()`                                                                                       | Alias to `horizontalAdvance()`                                                                                                                                       |
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
- `QLayout.setMargin` -> `setContentsMargins` (NOT shimmed; patch vendor)
- `QMediaPlayer`/`QMediaContent` -> shim wrapper over `setSource`/`QAudioOutput`
- `QtXml`/`QDom*` -> requires Qt5Compat; otherwise patch to `QXmlStreamReader`
- Verify: signal names (`valueChanged(int)` overloads), `QProcess` start/finish
  signals, `QFileDialog` static signatures, clipboard/screen grabs
