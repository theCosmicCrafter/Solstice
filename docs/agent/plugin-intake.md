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
- Pinned: TBD at vendor time (Phase 1 pilot)
- Vendored at: plugins/python/timer_watch/ (planned)
- License: GPL-3.0
- PyQt6 status: TBD — PyQt5 imports expected, candidate for shim
- Local patches: TBD
- Default state: enabled (planned)
- Last re-check: 2026-10-14 — initial evaluation

## PyQt5 -> PyQt6 delta checklist (for vendoring)

Apply at vendor time; prefer the shared shim over per-plugin rewrites:

- Module name: `PyQt5` -> `PyQt6` (or `from krita import` facades)
- `QAction`, `QActionGroup`, `QShortcut`: QtWidgets -> QtGui
- `QRegExp` -> `QRegularExpression`
- `exec_()` -> `exec()` (keep `exec_` alias if shim provides it)
- Enum scoping: `Qt.MidButton` -> `Qt.MiddleButton`, `Qt.Key_*` unchanged
- `QtGui.QRegExpValidator` / `QtGui.QIntValidator` moved to QtGui (unchanged) —
  verify usage sites
- `QDesktopWidget` removed -> `QGuiApplication.screenAt` / `primaryScreen`
- `sip` module: `sip.delete` still available via pyqt bindings — verify
- High-DPI: `AA_EnableHighDpiScaling` removed (default in Qt6)
- `QLayout.setMargin` -> `setContentsMargins`
- `QTime` -> `QElapsedTimer` for timing
