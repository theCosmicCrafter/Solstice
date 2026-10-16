# End-to-end testing (`tests/e2e/`)

Behavioral test suite that drives a **real `solstice.exe`** through the
vendored kritamcp bridge (`127.0.0.1:5678`). No krita stubs — every
assertion observes the running app: pixel readback, files on disk (magic
bytes, PNG dimensions), HTTP status codes, and process liveness.

## Files

| File | Role |
| --- | --- |
| `tests/e2e/solstice_bridge.py` | Minimal urllib client; token resolution mirrors `server.py` (`KRITAMCP_TOKEN` → `kritamcp.token` sidecar → kritarc `[kritamcp] token`; kritarc is parsed manually — KDE configs have keys before any `[section]`) |
| `tests/e2e/conftest.py` | `ManagedApp` launch/teardown fixture, `/health` wait loop, merged stdout+stderr capture to `tests/e2e/_last_run_stderr.log` (gitignored) |
| `tests/e2e/test_solstice_e2e.py` | 20 tests; see markers below |

## Running

```bat
pip install pytest pytest-timeout requests
set PYTEST_DISABLE_PLUGIN_AUTOLOAD=1  :: skips broken site plugins (logfire)
python -m pytest tests/e2e -v -p pytest_timeout --timeout 600
```

- **Launch mode (default)**: spawns `solstice.exe` itself, waits for the
  bridge, kills it on teardown. Refuses to start if something already
  serves `:5678`.
- **Attach mode**: `set SOLSTICE_E2E_ATTACH=1` — drives an
  already-running instance. Stderr capture and crash detection are
  unavailable (fixture skips `test_plugin_load_clean`).
- `SOLSTICE_EXE` overrides the binary; `SOLSTICE_SOAK_ITERS` (default 5)
  scales the soak loop; `-m "not soak"` skips the soak pass.

## Coverage map

- **Connectivity/auth**: `/health`, missing/wrong/correct bearer token,
  unknown action, `/info` surface; `Host` header enforcement tests are
  **PENDING** (Lane A is adding validation and tests).
- **Documents**: create → info → `.kra` save (PK zip check) → reopen
- **Pixels**: fill/stroke writes verified via `get_color_at` projection
  readback (inside vs. outside the shape)
- **Layers**: add/opacity/visibility/duplicate/remove, merge, flatten;
  undo/redo on node ops (pixel writes are not undoable in Krita)
- **Actions and vector conversion**: `trigger_action` action invocation and
  Trace to Vector Layer (`test_trigger_action_and_trace_to_vector`: raster
  alpha traced into a filled `KoPathShape` on a new `KisShapeLayer`).
- **Display and screenshots**: canvas-scoped screenshot capture with zoom
  levels (`screenshot` action, `zoom` and `scope="canvas"` parameters) and
  Smooth Rendering at High Zoom verification (`test_smooth_high_zoom_display`:
  pixel edge blending analysis at 400 % zoom).
- **Selections/filters**: rect/all/deselect, blur apply
- **Files & security**: PNG export (IHDR dims), `.kra` ZIP magic; basic path
  traversal rejected (`..` checked on the raw path — `abspath` resolves it away).
  Comprehensive traversal rejection across all file-writing actions
  (`export_document`, `export_layers`, `screenshot`, `save_as_template`) with a
  shared `_resolve_allowed_path()` resolver is **PENDING** (Lane A is adding
  them).
- **Stability**: `repeated_ops` rapid-fire loop, `soak` (create → paint →
  save → reopen ×N with process-liveness checks), stderr `Traceback` scan
- **Error paths**: bad params return clean errors; bridge stays responsive

## Timing notes

- Effects of `action.trigger()`-based commands (`undo`, `redo`) land
  asynchronously on Krita's stroke queue — poll, don't assert immediately.
- On first-ever launch the token doesn't exist until the server handles a
  request; the sidecar file makes it resolvable mid-session.

## Bugs this suite has caught (kritamcp hardening log)

1. **Startup deadlock** — `addView`/`createDocument` during window
   construction hung the command queue for 120 s/command. Fixed with a
   window-readiness gate (`windowCreated` + 15 s fallback).
2. **Crash: stack overflow** in `openDocument` on a path already open in a
   view. Bridge now returns the existing document's info.
3. **`get_color_at` 500** — `QByteArray[i]` yields `bytes` in PyQt6;
   needed `bytes()` coercion before `{:02x}` formatting.
4. **Path traversal bypass** — `..` check ran after `abspath()` resolved
   it away; a `..\..\evil.kra` actually wrote outside the sandbox.
5. **`flatten_image`/`set_resolution` API errors** — `flatten()` and
   `int` DPI per `libs/libkis/Document.h`.
6. **Undo flakiness** — `addView` didn't activate the view; `edit_undo`
   hit whatever view was already active. `showView` after `addView`.
7. **`save` flattened `.kra`** — `exportImage` flattens; `saveAs` is used
   for `.kra`/`.krz` so layers survive.
