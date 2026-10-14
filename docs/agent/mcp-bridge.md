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
`POST /` JSON `{action, params}` — 41 actions covering documents, layers,
painting, selections, filters, templates and export. `GET /health` and
`GET /info` are the only unauthenticated endpoints.

### Auth (Solstice hardening)

- On first request the bridge lazily generates a 48-hex token via
  `secrets.token_hex(24)` and stores it under `[kritamcp] token` in the
  Solstice kritarc (`%APPDATA%\Solstice\config\kritarc`).
- Every non-`/health` request must send `Authorization: Bearer <token>`;
  verification uses `hmac.compare_digest`. Failures get a JSON 401.
- The token is regenerated only if the setting is removed; no rotation
  mechanism exists.

## External server configuration (`server.py`)

- `KRITA_URL` env var overrides the endpoint (default
  `http://localhost:5678`).
- Token resolution order: `KRITAMCP_TOKEN` env var → Solstice kritarc
  `[kritamcp] token` → (legacy) `%APPDATA%\krita\kritarc`.
- Depends on `httpx` and `fastmcp`; run with the repo's `venv` or
  `pip install -r requirements.txt`. Default request timeout is 120 s —
  canvas export and save need it; do not lower it below ~30 s.
- MCP client registration: point the client's command at
  `python <path>\server.py` (or `fastmcp run`); see
  `krita-mcp/mcp_config.json` for the stock shape.

## Alternatives evaluated

| Option | Shape | Why not the primary |
| --- | --- | --- |
| krita-cli (edithatogo/krita-cli) | `uv tool install` CLI + same kritamcp bridge protocol; adds a `krita` command for scripting | Solstice keeps the vendored bridge as the stable contract; krita-cli remains a compatible client for shell use — it sends the same bearer token once configured |
| krita-codex-mcp (cyyprezz) | Windows-first MCP server + loopback bridge, ~13 tools, UUID layer addressing, bearer auth | Its auth pattern was adopted in the vendored bridge; the full plugin was not vendored because kritamcp's action surface is wider. Optionally installable later as a user plugin — no fork work required |

## Failure modes to keep in mind

- Port 5678 collision: a second Krita/Solstice instance silently fails to
  bind; the bridge logs to the plugin console only.
- Token is per-profile; a fresh Solstice profile means a new token — the
  server re-reads kritarc at each launch, so restarting the MCP server is
  enough.
- `POST` bodies are JSON only; a malformed body returns 400, unknown actions
  404, Krita-side exceptions 500 with the message in `error`.
