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

## PyQt5 -> PyQt6 compatibility shim

`plugins/python/PyQt5/` is a shared re-export layer installed to
`share/krita/pykrita/PyQt5` next to the bundled plugins. Vendored plugins keep
their `from PyQt5 import ...` lines unchanged. Current coverage:

| Qt5 surface                                                                                                  | Shim behaviour                                                                                                                                              |
| ------------------------------------------------------------------------------------------------------------ | ----------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `from PyQt5 import QtX` submodules                                                                           | Re-export the PyQt6 modules (`QtCore`, `QtGui`, `QtWidgets`, `QtMultimedia`, `QtNetwork`, `QtPrintSupport`, `QtSvg`, `QtSvgWidgets`, `QtXml`, `uic`, `sip`) |
| Unscoped enums (`Qt.AlignLeft`, `Qt.NoModifier`, `QEvent.MouseButtonPress`, `QMessageBox.Ok`, ...)           | Every enum member hoisted onto its class (`_expose_enum_members`); uses `__members__` so zero-valued flags (`NoModifier`) and aliases survive               |
| `exec_()`                                                                                                    | Alias to `exec()` on every PyQt6 class that has `exec`                                                                                                      |
| `QAction`/`QFileSystemModel`/`QShortcut`/`QUndo*`/`QStandardItem*` in QtWidgets                              | Re-exported from PyQt6.QtGui                                                                                                                                |
| `QRegExp`                                                                                                    | Subclass wrapper over `QRegularExpression` (`exactMatch`, `indexIn`, `cap`, case sensitivity)                                                               |
| `QRegExpValidator`                                                                                           | Alias to `QRegularExpressionValidator`                                                                                                                      |
| `QMediaPlayer.setMedia`/`setVolume`/`media`, `QMediaContent`                                                 | Wrapper classes; volume bridged through an owned `QAudioOutput`, `setMedia` maps to `setSource`                                                             |
| `QDesktopWidget` / `QApplication.desktop()`                                                                  | Minimal QWidget stand-in backed by `QGuiApplication` screens (`screenGeometry`, `availableGeometry`, `screenCount`)                                         |
| `QFontMetrics.width()`                                                                                       | Alias to `horizontalAdvance()`                                                                                                                              |
| `QMouseEvent`/`QWheelEvent` `pos()`, `globalPos()`, `x()`, `y()`, `localPos()`, `windowPos()`, `screenPos()` | QPoint/QPointF bridges over `position()`/`globalPosition()`/`scenePosition()`                                                                               |
| `PyQt5.uic`                                                                                                  | `PyQt6.uic` re-export (`loadUi`, `loadUiType`, `compileUi` verified)                                                                                        |
| `PyQt5.sip`, `import sip`                                                                                    | `PyQt6.sip` re-export                                                                                                                                       |
| `PyQt5.QtXml` (`QDom*`)                                                                                      | Re-export when Qt5Compat is present; raises a descriptive ImportError otherwise — patch the plugin at vendor time                                           |

Known limits (patch the plugin, don't extend the shim):

- `QMouseEvent.buttons()` during move events is unchanged, but synthesised
  events in plugins that relied on `Qt5` screen coordinates need review.
- Qt Multimedia widget classes (`QVideoWidget`, `QCamera*`) are not wrapped;
  Qt6's capture/session APIs differ structurally.
- `QSound`, `QSoundEffect` Qt5 constructors with different signatures are not
  normalised.
- `QtXml` without Qt5Compat is unavailable by design.

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
