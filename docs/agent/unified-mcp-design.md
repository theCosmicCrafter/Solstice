# Unified `/mcp` server — design (Megaplan Phase 2)

Design for merging the two MCP surfaces into one in-process endpoint inside
`solstice.exe`, retiring the external `krita-mcp/server.py` sidecar. Sources:
[plugin-integration-megaplan.md](plugin-integration-megaplan.md) §14.7,
[rendering-art-engine-megaplan.md](rendering-art-engine-megaplan.md) "Phase 2 —
Foundation rebase & unified `/mcp`", [mcp-bridge.md](mcp-bridge.md),
[e2e-testing.md](e2e-testing.md), and the upstream notes in
`C:\Users\richk\Downloads\Solstice-1.1.1\Solstice-1.1.1\docs\agent\test-control.md`.

## Surfaces today

### Fork side — `plugins/python/kritamcp/__init__.py`

- `QThread`-hosted `http.server.HTTPServer` bound to `127.0.0.1:5678`
  (`SERVER_PORT`, no env override). Single-threaded: requests are serialized.
  Enabled by default (`X-Solstice-EnabledByDefault=true` in
  `kritapykrita_kritamcp.desktop`).
- Envelope: `POST /` with JSON `{"action": <name>, "params": {...}}` → result
  JSON (HTTP 200; `{"error": ...}` → 500). `GET /health` unauthenticated,
  `GET /info` authed (returns `commands` list). Unknown path → 404, bad JSON →
  400, missing auth → 401, bad Host → 403, body > 50 MiB → 413.
- Commands are pushed onto `command_queue` and executed on the GUI thread by a
  50 ms `QTimer` (`process_commands` → `execute_command` → `cmd_<action>`),
  gated by `_window_ready` (requeues during window construction) and the
  `_command_processing` re-entrancy guard. Per-request wait timeout 120 s.
- Auth: bearer token generated lazily (`secrets.token_hex(24)`, 48 hex) into
  kritarc `[kritamcp] token` (`%APPDATA%\Solstice\config\kritarc`) and mirrored
  to sidecar `%LOCALAPPDATA%\krita\kritamcp.token`; `hmac.compare_digest`
  compare. Host header must be `127.0.0.1`/`localhost`/`[::1]` (any port).
- **88 actions** (the "63" in `mcp-bridge.md` is stale; `SUPPORTED_ACTIONS`
  currently lists 88):

```
new_canvas set_color set_brush stroke fill draw_shape get_canvas undo redo
trigger_action screenshot clear save get_color_at list_brushes open_file
add_layer remove_layer list_layers set_active_layer set_layer_opacity
set_layer_blend_mode set_layer_visibility move_layer duplicate_layer merge_down
create_from_template save_as_template list_templates get_layer_presets
get_document_info resize_canvas scale_image set_resolution flatten_image
select_all deselect select_rect apply_filter list_filters
ai_status ai_styles ai_generate ai_expand ai_jobs ai_apply ai_discard ai_cancel
ai_catalog ai_connect ai_install ai_workflows ai_workflow ai_upscale
ai_remove_distraction ai_live ai_inbetween ai_inbetween_status ai_interpolate
ai_interpolate_status ai_anim ai_lipsync ai_reference ai_region ai_propagate
ai_propagate_status ai_animate ai_animate_status ai_xsheet ai_h3 ai_h3_status
ai_pigment ai_drag ai_drag_status canvas_grab translate_layer
export_layers export_document show_docker list_dockers
list_actions get_viewport set_zoom set_rotation reset_view pan_canvas
set_canvas_mirror set_canvas_lod
```

### Upstream side — `plugins/extensions/testcontrol/` (Solstice-1.1.1)

- `Krita/ApplicationPlugin` (`TestControlPlugin`), loaded while the first main
  window is constructed; ctor only reads env and opens the socket. Inert unless
  `SOLSTICE_TEST_CONTROL_PORT` (1024-65535) and `SOLSTICE_TEST_CONTROL_TOKEN`
  (≥16 chars) are set; `SOLSTICE_TEST_CONTROL_ROOT` scopes file reads/writes.
- `TestControlHttpServer` (QTcpServer, `QHostAddress::LocalHost`): POST `/mcp`
  only; Host `127.0.0.1:<port>`/`localhost:<port>` + foreign-Origin rejection;
  constant-time bearer compare; one request/connection, 8 MiB body, 32 KiB
  head, 30 s idle timeout, `GET` → 405.
- `TestControlMcpServer`: MCP Streamable HTTP, JSON-RPC 2.0 —
  `initialize`/`ping`/`tools/list`/`tools/call`; notifications → 202, batches →
  400; protocol versions `2025-06-18`, `2025-03-26`, `2024-11-05`. `tools/call`
  runs one at a time in arrival order via `QTimer::singleShot(0)` queue; async
  replies supported. Results: `{content:[{type:"text",...}],
  structuredContent, isError}`.
- **26 tools**, registered by `registerTools()` in three tool classes:

| Tool | Side | Params (key) | Returns |
| --- | --- | --- | --- |
| `session_info` | C++ (canvas) | none | versions, platform, canvas widget, gpuEngine, process cpu |
| `document_new` | C++ (canvas) | width, height, colorDepth | document + canvas once view active |
| `document_open` | C++ (doc) | path (rel. test root) | document + canvas |
| `document_export_png` | C++ (doc) | path (rel., .png) | bytes written |
| `document_close` | C++ (doc) | all | docs closed/left |
| `tool_activate` | C++ (canvas) | id | active tool id, restarted |
| `input_stroke` | C++ (canvas) | points[x,y,pressure,tilt,t], device, coordinates, modifiers, realtime, releaseDelayMs | events sent, press/release accepted, timing |
| `input_click` | C++ (canvas) | x, y, device, button, doubleClick, modifiers | as input_stroke |
| `input_drag` | C++ (canvas) | from, to, steps, durationMs, modifiers | as input_stroke |
| `input_key` | C++ (canvas) | key (QKeySequence text), modifiers | key event delivery |
| `wait_idle` | C++ (canvas) | timeoutMs | idle confirmation, waited ms |
| `query_state` | C++ (canvas) | includeLayerHash | tool, doc, undo stack, selection hash, layer hash |
| `query_pixels` | C++ (canvas) | rect, source, includePng, reference, tolerances | SHA-256, stats, optional PNG, compare verdict |
| `query_layers` | C++ (doc) | none | layer tree (name/class/vis/opacity/blend/active/children) |
| `canvas_frames` | C++ (canvas) | action start/read/stop, sinceMs | frame timestamps on monotonic clock |
| `canvas_capture` | C++ (canvas) | maxSize | PNG as MCP image block |
| `action_trigger` | C++ (canvas) | name, async | action triggered (async for modal dialogs) |
| `view_reset` | C++ (doc) | width, height | window size, zoom 100%, rotation 0 |
| `brush_set` | C++ (doc) | preset, size, opacity, eraser, color, paintMode | full brush state; report-only w/o args |
| `layer_activate` | C++ (doc) | name, select | active + selected layer names |
| `ui_query` | C++ (ui) | nameFilter, within, window, includeUnnamed | visible widgets: objectName, class, value |
| `ui_click` | C++ (ui) | selectors (objectName/text/toolTip/className/standardButton), async | button state after click |
| `ui_set` | C++ (ui) | selectors, value | new widget state (combo/spin/check/lineedit) |
| `ui_close` | C++ (ui) | window | count of windows closed |
| `docker_show` | C++ (ui) | id, visible | docker shown/hidden; unknown id lists ids |
| `gpu_fault` | C++ (canvas) | reset, downloadFailures | gpuEngine state (dev-only fault injection) |

### Overlaps and collisions

**Exact name collisions: none.** All 26 upstream names differ from all 88
action names (`docker_show` vs `show_docker`, `action_trigger` vs
`trigger_action`, `document_new` vs `new_canvas`, ...). Semantic overlaps to
document (keep both — semantics genuinely differ):

| Upstream tool | Closest kritamcp action(s) | Difference |
| --- | --- | --- |
| `document_new` | `new_canvas` | C++: RGBA, 1 transparent layer, 300 ppi, no fill; action: bg-color fill + paint layer |
| `document_open` | `open_file` | C++: confined to test root, BatchMode; action: allowed-roots resolver, idempotent reopen |
| `document_export_png` | `save`, `export_document` | C++: PNG only, test root; action: any format, allowed roots |
| `query_layers` | `list_layers` | C++: docker-order tree w/ children; action: flat list + animated/keyframe_times |
| `layer_activate` | `set_active_layer` | C++: multi-node selection via KisNodeManager; action: single node by name |
| `brush_set` | `set_brush`, `set_color` | C++: report-only mode + eraser/paintMode; action: preset+size+opacity, separate color |
| `action_trigger` | `trigger_action` | same capability; C++ adds `async` for modal dialogs |
| `canvas_capture` | `screenshot`, `canvas_grab`, `get_canvas` | C++: MCP image block, widget grab; actions: PNG file paths, zoom/scope options |
| `docker_show` | `show_docker`, `list_dockers` | C++: by objectName id, hide too; action: id or title substring, raise |
| `query_pixels` | `get_color_at` | C++: region hash/stats/reference compare; action: single pixel hex |
| `query_state`/`session_info` | `get_document_info`, `get_viewport`, `ai_status` | C++: undo/selection/layer hashes + env; actions: doc metadata / viewport / ai_diffusion state |
| `input_stroke/drag/click` | `stroke`, `fill`, `draw_shape` | C++: synthetic events through `KisInputManager` (real tool path); actions: direct pixel writes |
| `view_reset` | `reset_view`, `set_zoom`, `set_rotation`, `set_canvas_mirror` | C++: window resize + reset bundle; actions: individual view controls |
| `wait_idle`, `ui_*`, `input_key`, `canvas_frames`, `tool_activate`, `gpu_fault` | — (none) | no kritamcp counterpart |
| — | `ai_*` family (34), `undo`, `redo`, `select_all`, `deselect`, `select_rect`, `apply_filter`, `list_filters`, `list_actions`, templates (4), `export_layers`, `resize_canvas`, `scale_image`, `set_resolution`, `flatten_image`, `move_layer`, `merge_down`, `pan_canvas`, `set_canvas_lod`, `list_brushes`, `clear`, `translate_layer`, `canvas_grab`, `add_layer`, `remove_layer`, `duplicate_layer`, `set_layer_*` (3), `draw_shape`, `fill`, `open_file`, `get_document_info` | no dedicated testcontrol tool (several reachable via `action_trigger`, e.g. `edit_undo`, `deselect`) |
| MCP `tools/list` | `list_actions` | native discovery vs action-name list |

## Recommended architecture — ONE listener, kritamcp fronts

**Keep kritamcp's Python `http.server` as the single public listener on
:5678; add `POST /mcp` handled by a Python JSON-RPC front that dispatches
action tools in-process and loopback-proxies the 26 native tools to the
upstream C++ listener.**

```
client ──► :5678 ──► kritamcp handler (Host + bearer, as today)
                       ├─ GET  /health, /info      → unchanged
                       ├─ POST /        {action,params} → execute_command (unchanged)
                       └─ POST /mcp     JSON-RPC front:
                          initialize/ping → answered locally
                          tools/list      → 88 krita_* action tools + C++ tools/list (cached)
                          tools/call krita_<action> → execute_command → MCP result wrap
                          tools/call <native>     → forward raw body to
                                                    127.0.0.1:$SOLSTICE_TEST_CONTROL_PORT/mcp
                                                    (Authorization rewritten to env token)
```

- **C++ side imports verbatim** in the Phase-2 merge (plugin, tools, injector,
  `dev-tools/test-control/` runner): it keeps binding
  `SOLSTICE_TEST_CONTROL_PORT` exactly as upstream. Python reads that env var
  to find its proxy target; when unset, `/mcp` still works — `tools/list` just
  omits the native family (preserves the upstream "inert unless env" rule).
- **Guard**: `SOLSTICE_TEST_CONTROL_PORT` must not equal 5678 (the front
  already serves `/mcp`; a second bind would recreate the phantom-second-
  bridge class of bug). C++ warns and refuses when it collides.
- **JSON-RPC front in Python** (~150 lines, new `mcp_rpc.py` module beside
  `kritamcp/__init__.py`): the same HTTP worker thread that today waits on
  `command_queue.get_result` waits identically for MCP calls — action tools go
  through the unchanged CommandQueue/QTimer path, so the `_window_ready` gate
  and `_command_processing` guard cover them for free.

### Why not C++ as the single listener

| | Python front (chosen) | C++ front |
| --- | --- | --- |
| e2e suite (40 `test_` fns in `tests/e2e/test_solstice_e2e.py`) | **Green by construction** — envelope, `/health`, `/info`, Host rules, status codes, `{"error"}` bodies untouched | Must re-implement Python's exact responses in C++ (today's C++ returns empty 401/403 bodies — every auth test breaks) |
| Action dispatch | Zero new plumbing — `execute_command` is already in-process | Needs a new C++→pykrita queue or a loopback hop anyway (C++ cannot call libkis Python directly); same complexity, moved |
| Upstream import | Verbatim — its HTTP/auth path is what its protocol tests exercise | C++ HTTP layer gets re-plumbed as a proxy it wasn't written for |
| Token ownership | Generated token + kritarc + sidecar logic already lives here | Token generation must move to C++ (KConfig) or chicken-and-egg with Python |
| Plugin lifecycle | kritamcp is enabled-by-default and already solves "commands before window ready" | ApplicationPlugin ctor is fine, but the action delegate depends on a Python plugin that may load later or be disabled |
| Cost | `/mcp` native tools need kritamcp enabled; one extra loopback hop; serial `http.server` | More new C++ code, higher regression risk on the proven auth surface |

The C++-front end-state (fully native listener, Python as delegate) stays a
possible later migration; it is not needed for Phase 2 and adds no client-
visible benefit over the proxy.

## Port, env and auth contract

- **Public endpoint**: `http://127.0.0.1:5678` (unchanged default). Only
  `SOLSTICE_TEST_CONTROL_PORT` may move the *native* listener — never the
  public front; `KRITA_URL` remains the client-side override (e2e, krita-cli).
- **Tokens accepted on :5678** (single bearer story):
  1. the generated kritamcp token (kritarc `[kritamcp] token` / sidecar /
     `KRITAMCP_TOKEN` on the client) — always;
  2. `SOLSTICE_TEST_CONTROL_TOKEN` when set — so the scenario runner and MCP
     clients configured for test control can also drive the unified port.
  The proxy forwards only after the outer check passes, rewriting
  `Authorization` to the env token so the inner hop stays constant-time-checked.
- **Headers**: Host rules unchanged (e2e-tested). New: foreign `Origin`
  rejected **on `/mcp` only** (matches the upstream invariant without
  tightening the envelope path existing clients depend on). `GET /mcp` → 405
  to mirror upstream semantics.
- **`/info`**: gains `{"mcp": {"endpoint": "/mcp", "native_tools": bool}}` —
  additive keys only; never remove `commands`.
- Native tools appear in `tools/list` only while the C++ listener is up —
  env-gated, exactly as upstream.

## Namespace plan

- `krita_<action>` — every kritamcp action exposed as an MCP tool under a
  `krita_` prefix (88 names today; future actions auto-join). Matches the
  external `server.py` naming (`krita_new_canvas`, `krita_stroke`, ...) so its
  41 tools repoint 1:1; `ai_*` actions become `krita_ai_generate`, etc.
- Bare snake names — the native MCP-first namespace: the 26 upstream tools
  verbatim (`document_*`, `input_*`, `query_*`, `ui_*`, `canvas_*`,
  `session_info`, `tool_activate`, `action_trigger`, `view_reset`,
  `brush_set`, `layer_activate`, `docker_show`, `wait_idle`, `gpu_fault`).
- Reserved for megaplan Phases 3/5 (native side, no `krita_` prefix):
  `showa_*`, `blockout_*`, `capture_at`. All are collision-free against both
  existing sets and the `krita_` prefix.
- Rule for new work: action-envelope additions land under `krita_`; native
  tools land bare and must not shadow a `krita_`-suffixed twin or an upstream
  name. Overlapping pairs (table above) stay separate — no aliasing layer.

## Migration and retirement

- **`krita` CLI / `krita-cli`**: speaks `{action,params}` to `KRITA_URL` —
  works unmodified; no migration.
- **external `krita-mcp/server.py`** (`C:\Users\richk\CascadeProjects\krita-mcp`):
  keep working indefinitely (the envelope it wraps is untouched); sunset path
  = repoint MCP clients at `http://127.0.0.1:5678/mcp` (Streamable HTTP +
  Bearer, same token sources). `krita_<name>` tool-name parity makes client
  configs drop-in. Codex-mcp stays an optional alternative, no fork work.
- **Scenario runner** (`dev-tools/test-control/run_scenarios.py`): unchanged —
  still drives `SOLSTICE_TEST_CONTROL_PORT`; additionally `:5678/mcp` accepts
  the env token as a second path to the same tools.
- **Invariants preserved**: `/health` open, everything else authed; Host
  enforcement on every request; token never logged; all file-writing actions
  keep `_resolve_allowed_path` (native `document_*` keeps its own test-root
  confinement — separate, stricter sandbox); serialized call order per side.

## Phased implementation (megaplan Phase 2)

1. **2a import**: `plugins/extensions/testcontrol/` verbatim from 1.1.1 +
   `add_subdirectory(testcontrol)` (Qt6 gate) + `dev-tools/test-control/` +
   its `tests/`. No behavior change; env-gated as upstream.
2. **2b front**: kritamcp `POST /mcp` JSON-RPC front (`initialize`, `ping`,
   `tools/list`, `tools/call`; notifications→202, batch→400, versions
   `2025-06-18`/`2025-03-26`/`2024-11-05`); Origin check; `GET /mcp`→405;
   accept env token; port-collision guard.
3. **2c action tools**: single registry generating `krita_<action>` tool
   descriptors (name/description/inputSchema) from `SUPPORTED_ACTIONS` — one
   source so `tools/list`, `/info` and `list_actions` cannot drift;
   `tools/call` → CommandQueue → MCP result envelope. Mine `server.py`'s 41
   signatures for initial schemas; generic `{"type":"object"}` acceptable for
   the rest in v1.
4. **2d proxy**: native-name calls forwarded raw to the env port (300 s
   timeout — `run_scenarios.py` uses 300; covers `wait_idle`/realtime
   strokes); cached `tools/list` merge with invalidation on proxy failure;
   `/mcp` → 503-with-JSON-error when native side is down.
5. **2e retire**: docs — repoint instructions, update `mcp-bridge.md`,
   `feature-inventory.md` touch points, wiki log; mark `server.py` optional.

## Test plan

- `tests/e2e` **unmodified and green** (both launch and attach modes) — the
  hard gate; the chosen architecture guarantees the envelope path is
  byte-identical.
- New additive e2e smoke tests (allowed: new tests, not edits): `/mcp`
  initialize, `tools/list` count (88 + env-dependent 26), `tools/call
  krita_new_canvas` round-trip, unknown tool → JSON-RPC error, bad token →
  401, foreign Origin → 403, `GET /mcp` → 405, `krita_ai_status` contract.
- `ctest -R TestControl` for the imported plugin (runs verbatim upstream).
- Runner pass: `run_scenarios.py --all --strict --launch` on the env port
  (unchanged) plus one scenario through `:5678/mcp` with the env token.
- `netstat` check: exactly one listener on :5678; no double-bind
  (phantom-bridge regression).
- Manual: MCP client (`claude mcp add --transport http`) against `:5678/mcp`.

## Open questions

1. Keep the serial `HTTPServer`, or `ThreadingHTTPServer` so `/health`
   answers during a 120 s command? (Today a hung command already stalls
   `/health`; defer unless it bites.)
2. Long-term: replace the loopback proxy with an in-process QObject bridge
   (`qApp->findChild` + `invokeMethod`, ticket-based like `CommandQueue`) —
   removes the second socket; unproven pykrita→C++-plugin plumbing, so not
   Phase 2.
3. Should `ai_*` also get bare aliases (`ai_generate` alongside
   `krita_ai_generate`)? `tools/list` noise vs ergonomics — decide at 2c.
4. Standalone fallback: if kritamcp is disabled, native `/mcp` is gone too —
   acceptable? (Fresh profiles enable kritamcp by default; runner uses fresh
   profiles.) Optional `SOLSTICE_TEST_CONTROL_STANDALONE` C++ listener is the
   escape hatch if a user case appears.
5. `capture_at`/`showa_*`/`blockout_*` homes: native C++ tools (bare names)
   vs Python actions (`krita_` prefix) — decide per feature; blockout's
   Vulkan staging work argues native.
