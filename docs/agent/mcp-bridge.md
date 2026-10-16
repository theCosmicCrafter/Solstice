# MCP agent bridge — agent development notes

## Status and locations

- In-app bridge (vendored, enabled by default):
  `plugins/python/kritamcp/__init__.py`,
  desktop file `kritapykrita_kritamcp.desktop`
  (`X-Solstice-EnabledByDefault=true`).
- External MCP server (not vendored, lives outside the repo):
  `C:\Users\richk\CascadeProjects\krita-mcp\server.py`
  (nanayax3/krita-mcp, MIT, local clone at `83fec0e`).
- Upstream: https://github.com/nanayax3/krita-mcp

## Architecture

The bridge is a `QThread`-hosted `http.server.HTTPServer` on `127.0.0.1:5678`.
All Krita API calls are marshalled onto the GUI thread; requests are
`POST /` JSON `{action, params}` — 42 actions (including the two below) covering documents, layers,
painting, selections, filters, templates and export, plus
`trigger_action` (`params.name`) which triggers any application action by
id — the escape hatch for features with no dedicated command (e.g.
`layer_to_vector`) — and `screenshot` (`params.filename`, optional
`params.zoom`, optional `params.scope="canvas"`) which grabs the main
window — or only the canvas widget (`KisQPainterCanvas`/
`KisOpenGLCanvas2`, located by `metaObject().className()`) — to PNG for
display-level verification; canvas scope keeps window chrome out of
pixel analysis. `GET /health` and `GET /info` are the only
unauthenticated endpoints.

### Auth (Solstice hardening)

- On first request the bridge lazily generates a 48-hex token via
  `secrets.token_hex(24)` and stores it under `[kritamcp] token` in the
  Solstice kritarc (`%APPDATA%\Solstice\config\kritarc`).
- kritarc only syncs when settings are saved (typically app exit), so a
  freshly generated token is also mirrored to a sidecar file immediately:
  `%LOCALAPPDATA%\krita\kritamcp.token` (QStandardPaths.AppConfigLocation —
  the Qt app name stays `krita`). Clients should check the sidecar before
  kritarc so they can authenticate in the same session the token was created.
- Every non-`/health` request must send `Authorization: Bearer <token>`;
  verification uses `hmac.compare_digest`. Failures get a JSON 401.
- The token is regenerated only if the setting is removed; no rotation
  mechanism exists.

## External server configuration (`server.py`)

- `KRITA_URL` env var overrides the endpoint (default
  `http://localhost:5678`).
- Token resolution order: `KRITAMCP_TOKEN` env var → sidecar file
  (`%LOCALAPPDATA%\krita\kritamcp.token`, written at generation time) →
  Solstice kritarc `[kritamcp] token` → (legacy) `%APPDATA%\krita\kritarc`.
- Depends on `httpx` and `fastmcp`; run with the repo's `venv` or
  `pip install -r requirements.txt`. Default request timeout is 120 s —
  canvas export and save need it; do not lower it below ~30 s.
- MCP client registration: point the client's command at
  `python <path>\server.py` (or `fastmcp run`); see
  `krita-mcp/mcp_config.json` for the stock shape.

## Alternatives evaluated

| Option                           | Shape                                                                                       | Why not the primary                                                                                                                                                                                     |
| -------------------------------- | ------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| krita-cli (edithatogo/krita-cli) | `uv tool install` CLI + same kritamcp bridge protocol; adds a `krita` command for scripting | Solstice keeps the vendored bridge as the stable contract; krita-cli remains a compatible client for shell use — it sends the same bearer token once configured                                         |
| krita-codex-mcp (cyyprezz)       | Windows-first MCP server + loopback bridge, ~13 tools, UUID layer addressing, bearer auth   | Its auth pattern was adopted in the vendored bridge; the full plugin was not vendored because kritamcp's action surface is wider. Optionally installable later as a user plugin — no fork work required |

## Additional Solstice hardening (found by the E2E suite)

- **Startup gate**: commands are requeued until the main window is fully
  created. `window.addView()`/`createDocument()` deadlock when invoked
  during window construction (the QTimer can fire inside a nested
  `processEvents` loop). Gate: `notifier.windowCreated` plus a 15 s
  fallback (`windowIsCreated` exists in the API but is never emitted).
- **`showView` after `addView`**: `new_canvas`/`open_file` now activate
  the new view explicitly — view-scoped commands (`edit_undo` etc.) would
  otherwise silently target whichever view was already active.
- **`open_file` is idempotent**: re-opening a path that already has an
  open view crashes Krita with a stack overflow in the import path; the
  bridge returns the existing document's info instead.
- **`save` uses `saveAs` for `.kra`/`.krz`** (keeps layers); `exportImage`
  for everything else. Path-traversal validation now checks the raw path —
  `abspath()` resolves `..` before the old check ran.
- PyQt6 fixes: `QByteArray`→`bytes()` coercion in `get_color_at`,
  `flatten()` (not `flattenImage`), `setResolution(int)`.
- Stderr instrumentation: `[KritaMCP] exec/done #id action` lines make a
  hung command attributable in `_last_run_stderr.log` captures.

## Failure modes to keep in mind

- Port 5678 collision: a second Krita/Solstice instance silently fails to
  bind; the bridge logs to the plugin console only.
- Token is per-profile; a fresh Solstice profile means a new token — the
  server re-reads kritarc at each launch, so restarting the MCP server is
  enough.
- `POST` bodies are JSON only; a malformed body returns 400, unknown actions
  404, Krita-side exceptions 500 with the message in `error`.
- Re-opening a file that is already open in a view **crashes Krita**
  (stack overflow in the import path) — an upstream libkis issue; the
  bridge guards it, direct Scripter `openDocument` calls do not.

## Pipeline recipes

Compound workflows built from existing actions — these are the
documented chains higher-level features build on:

- **Generate/matte → vector**: `apply_filter` with
  `{"name": "background_removal"}` runs the Vision ML BiRefNet matte on
  the active layer headlessly (default config), then
  `trigger_action` with `{"name": "layer_to_vector"}` traces the
  resulting alpha into a new vector layer. Covered by
  `test_matting_to_vector_pipeline` in `tests/e2e`.
- **Any menu action**: `trigger_action` resolves by action id —
  `view_smooth_high_zoom`, tool ids like `SmartMattingTool`, filters
  like `krita_filter_background_removal` (the last opens its dialog;
  for headless matting use `apply_filter` instead).
