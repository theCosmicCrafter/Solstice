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
`POST /` JSON `{action, params}` — 88 actions covering documents, layers,
painting, selections, filters, templates, export, UI controls, viewport/canvas
navigation, and the vendored ai_diffusion generation engine, plus:
- `trigger_action` (`params.name`): triggers any application action by
  id — the escape hatch for features with no dedicated command (e.g.
  `layer_to_vector`, `view_smooth_high_zoom`, workspace switches).
- `screenshot` (`params.filename`, optional `params.zoom`, optional
  `params.scope="canvas"`): grabs the main window — or only the canvas
  widget (`KisQPainterCanvas`/`KisOpenGLCanvas2`, located by
  `metaObject().className()`) — to PNG for display-level verification;
  canvas scope keeps window chrome out of pixel analysis.
- `show_docker` (`params.name`): show and raise a docker by objectName or
  windowTitle substring (e.g. `imageDiffusion`, `KisLayerBox`).
- `list_dockers`: queries all registered dockers with objectName, window
  title, and current visibility status.

`GET /health` is the only unauthenticated endpoint; `GET /info` requires
bearer authentication and advertises the full list of supported commands
(88 total) along with canvas and template directories and the `/mcp`
endpoint metadata.

### Unified `/mcp` endpoint (Phase 2, `unified-mcp-design.md`)

`POST /mcp` on the same listener is a JSON-RPC 2.0 (Streamable-HTTP
profile) front implemented in `plugins/python/kritamcp/mcp_rpc.py`:

- Methods: `initialize`, `ping`, `tools/list`, `tools/call`; protocol
  versions `2025-06-18`, `2025-03-26`, `2024-11-05` are negotiated.
  Notifications (no `id`) return 202; batches return 400; `GET /mcp`
  returns 405; a foreign `Origin` header returns 403 (DNS-rebinding
  guard); everything else inherits the bearer check.
- `tools/list` merges two families: `krita_<action>` for each of the 88
  envelope actions (single registry — `tools/list`, `/info`, and
  `list_actions` cannot drift) plus the native C++ TestControl tools,
  cached and invalidated when a proxy call fails.
- `tools/call krita_<action>` strips the prefix and dispatches through
  the same `CommandQueue` GUI-thread path as the envelope; the result
  dict is wrapped as `{content:[text], structuredContent}`; envelope
  `{"error":...}` maps to `isError: true`.
- `tools/call <native name>` proxies the raw JSON-RPC body to
  `127.0.0.1:$SOLSTICE_TEST_CONTROL_PORT/mcp` with `Authorization`
  rewritten to `$SOLSTICE_TEST_CONTROL_TOKEN` (300 s timeout for
  `wait_idle`/realtime strokes). Env unset or native side down → 503
  with a JSON-RPC error body.
- `SOLSTICE_TEST_CONTROL_TOKEN` (≥16 chars) is a second accepted bearer
  on :5678 so `dev-tools/test-control/run_scenarios.py` and MCP clients
  can drive the unified port without the generated token.

### AI generation actions (`ai_*`)

Parameterized Photoshop-parity generative editing through the vendored
`ai_diffusion` plugin — in-process, on the same managed ComfyUI session and
job queue the AI Diffusion docker uses. No second backend is started.

- `ai_status`: `{enabled, connected, state, server_url, checkpoints, gpu,
  style, jobs_queued, jobs_executing}` — call first; the other commands
  return a descriptive error when the plugin is disabled or ComfyUI is not
  connected.
- `ai_connect`: triggers `ai_diffusion`'s managed-server autostart and
  connection. Returns immediately (`{"status": "ok", "connected": bool, "started": bool}`);
  poll `ai_status` for state transitions (`stopped` → `starting`/`installing` →
  `connected`). In managed mode this launches ComfyUI; in external/cloud mode
  it initiates connection.
- `ai_styles`: style presets usable as `style` (`{name, filename,
  checkpoints, current}`).
- `ai_generate {mode, prompt, negative, style, strength, seed, count,
  queue, apply}`: enqueues a job on the active document. `mode` is
  `auto|fill|remove|expand|create`: fill/remove need a selection
  (`select_rect`/`select_all` first), expand needs a selection covering the
  canvas, `create` clears the selection for text-to-image, `auto` lets the
  model resolve by geometry. `apply` (default `true`) inserts the first
  result as a new layer when the job finishes — unless the user's
  finished-action already applies. Model props mutated for the call
  (strength, seed, style, batch, queue mode, inpaint mode) are restored
  afterwards; the prompt stays so the docker shows what the agent did.
  Jobs enqueue asynchronously: the response returns `jobs_from_index`;
  poll `ai_jobs` for the new entries.
- `ai_expand {left, top, right, bottom, ...ai_generate params}`: grows the
  canvas by the margins (content offset by left/top), selects all, and
  outpaints the new band — Photoshop Generative Expand in one call.
- `ai_jobs {limit, state}`: job history for the active document, newest
  first — `{index, id, kind, state, name, seed, results, used}`.
- `ai_apply {job_id|index, result}`: applies one result of a finished job
  as a new layer (defaults: latest finished job, result 0) — the
  variations-keeper step.
- `ai_discard {job_id, result}`: deletes one result image (default: result 0);
  a job whose last result is discarded is removed from the history — the
  delete-variation step.
- `ai_cancel {active, queued}`: cancels running and/or queued jobs.
- `ai_catalog`: lists the managed model catalog — architectures, and per
  category (required/checkpoints/upscale/optional) each resource's id,
  name, arch, files, `license`, `min_vram_gb`, `requirements`, and
  `installed` state. Works without a ComfyUI connection — lets agents
  pick VRAM-appropriate models before enabling anything.
- `ai_generate` extras: `similar_to` (job id/index — inherits prompt,
  negative, strength, style; fresh seed), `count`/`variations` for
  batches, and deterministic `[[a|b|c]]` specs that emit one job per
  option (capped at 8×16). `{a|b}` keeps native seeded wildcard
  semantics. Multi-variant enqueues force `QueueMode.back` after the
  first so nothing replaces queued siblings.
- `ai_workflows`: lists custom workflows (`.json` under the ai_diffusion
  `workflows/` folder) with declared params.
- `ai_workflow {workflow, params, apply}`: runs a custom workflow by id
  or name — style params take style names, image/mask params take layer
  names/ids. Poll `ai_jobs` for completion (jobs apply as `[Generated]`
  layers).
- `ai_upscale {model, factor, refine, color_correction, scale}`:
  document-wide upscale — `model:"SeedVR2"` for generative restoration
  (native SeedVR2 nodes) or an ESRGAN filename. NOTE: resizes the
  document at enqueue; cancelled/failed jobs roll the canvas back.
- `ai_remove_distraction {rect|point,radius,grow,prompt,...}`: builds a
  padded selection around a distraction and runs a `mode:"remove"` fill.
- `ai_live {active, strength, prompt, style, apply, layer_only}`: toggles
  the LiveWorkspace co-generation loop, sets its params, reports
  `{has_result, result_extent, doc_active, model_error}`, and `apply`
  pushes the stored preview as a dedicated layer. Live jobs never show
  in `ai_jobs` — poll `ai_live` for `has_result`.
- `ai_inbetween {first_layer, last_layer, frames, prompt, negative,
  seed, steps, first_frame, step, layer_name}`: Wan 2.2 FLF2V
  inbetweening — uploads the two keyframe layers to ComfyUI, queues the
  verified high/low-noise + lightx2v graph, returns a `prompt_id`.
- `ai_inbetween_status {prompt_id}`: polls the prompt, and on
  completion downloads decoded frames via `/view` and imports them as
  timeline keyframes on a dedicated animation layer (`importAnimation`).
  Job metadata lives in the module-global `_FLF_JOBS` — per-request
  state must not live on the Extension (duplicate .desktop
  registrations can create multiple extension objects serving the one
  bound server).
- `ai_interpolate {layer, multiplier, first_frame, layer_name}`: RIFE
  frame densification — reads an animated layer's keyframes via
  `pixelDataAtTime`, chains `FrameInterpolate` in ComfyUI, returns a
  `prompt_id`. Densification only: RIFE dissolves objects across large
  sparse displacement — use `ai_inbetween` (FLF2V) for sparse keys.
- `ai_interpolate_status {prompt_id}`: same shared poll+import path as
  `ai_inbetween_status` (imports onto `[Densified]` by default).
- `ai_anim {fps, frame, play_start, play_end, full_start, full_end}`:
  timeline get/set — always returns fps, current_time, playback_range,
  full_clip_range, animation_length, and `animated_layers` with
  keyframe times.
- `ai_lipsync {audio, mouth_group, start_frame, recognizer, dialog,
  name}`: runs bundled rhubarb (`D:\krita\tools\rhubarb\...\rhubarb.exe`,
  PATH fallback) → JSON mouth cues → exports mouth-shape paint layers
  (letters A-H/X, `mouth_*` names, "Mouths" group preferred) → writes
  the per-frame sequence → `importAnimation` onto `[Lipsync]`.
- `ai_region {list|layer,positive|link_layer,of_layer}`: list regions
  (linked layers, prompt, control count) or create one linked to a
  layer — `Region.link_target` semantics mean a layer inside a group
  links the group. `positive` sets the region prompt; `link_layer`
  adds a layer to the region found via `of_layer`.
- `ai_reference {layer, region, mode, strength, clear}`: bind a named
  image layer as a control-layer input (default `reference` mode) on
  the root region or the region linked to `region`. `ControlLayerList.
  add()` binds the ACTIVE layer — the action activates the target,
  adds, restores, then pins `layer_id` explicitly.
- `ai_propagate {control_layer, start, end, keyframes_only,
  reference_layer, prompt, negative, seed, steps, strength, width,
  height, length, first_frame, step, layer_name}`: VACE motion
  propagation — extracts control frames via `pixelDataAtTime` (dense
  by default, `keyframes_only` for sparse), optional `reference_layer`
  as appearance anchor, runs native `WanVaceToVideo` +
  `wan\wan2.1_vace_14B_fp8_e4m3fn` → `ai_propagate_status` imports the
  frames onto `[Propagated]`.
- `ai_animate {pose_layer, reference_layer, start, end,
  keyframes_only, prompt, negative, seed, steps, pose_strength,
  reference_strength, width, height, length, first_frame, step,
  layer_name}`: Wan 2.2 Animate performance transfer — pose video from
  an animated layer (`pixelDataAtTime`, dense) drives the
  `reference_layer` character via `WanAnimate2ToVideo` +
  `wan\wan2.2_animate_14B_bf16`; `ai_animate_status` imports frames
  onto `[Animated]`.
- `list_layers` reports `animated`/`keyframe_times` for animated nodes.
- `ai_h3 {prompt, negative, first_layer, width, height, frames, seed,
  steps, audio, audio_path, first_frame, layer_name}`: MiniMax H3
  joint video+audio generation (t2v or i2v when `first_layer` is
  given) → `ai_h3_status` imports frames onto `[H3]` and downloads the
  FLAC to `audio_path`. Requires the `qwen3vl_32b_minimax_h3_*` CLIP
  (the 8B variant is a 4096/5120 mat-mismatch), `minimax_h3_video_vae`
  + `minimax_h3_audio_vae_fp32`, an h3 UNET (`fasth3_omniref_8step_*`
  works at steps=8), frames on the 17k+5 grid.
- `ai_drag {layer, sx, sy, tx, ty, radius, width, height, seed, steps,
  guidance_scale, control_scale, layer_name}`: point-drag edit via
  FramePainter (Phase 13.3 shippable path). Renders a drag-track mask
  (source disc + tapered stroke to target) → SVD `svd_xt` checkpoint +
  `FramePainter` UNet/encoder under `models/FramePainter/` (junctioned
  into `ComfyUI/models/FramePainter`) → regenerated image lands on
  `[Dragged]`. NOTE: diffusion regeneration, not a pixel move — flat
  graphics come back reinterpreted. `clip-vision_vit-h.safetensors`
  (dash!) feeds `clip_vision`.
- `translate_layer {layer, dx, dy, wrap}`: deterministic pixel move —
  exact pixels, no generation (the primitive `ai_drag` is NOT).
  Undoable via `Node::setPixelDataUndoable` (`KisTransaction`), with a
  raw-write fallback on older builds (`undoable` flag in the response).
- `canvas_grab {path}`: Qt grab() of the real canvas widget — captures
  exactly what the user sees incl. zoom/filter state. Used it to prove
  the 200% canvas is bilinear-smooth while exports are pixel-exact.
- `ai_drag` now accepts `points:[{sx,sy,tx,ty,radius?}]` multi-drag
  (single-drag sx,sy,tx,ty form still works).
- `ai_pigment {top, bottom, opacity, layer_name}`: Kubelka-Munk
  pigment blend of two layers → `[Pigment]` layer. Backed by
  `spectral_mix.py`, a numpy port of spectral.js 3.0.0 (MIT) —
  luminance-weighted K/S mixing over a 38-band reflectance lift.
  Blue+yellow → olive green; identity and endpoints exact.

Bridge conventions that have bitten us: `ai_settings.server_url` may
lack a scheme (normalize to `http://` before urllib); never include an
`"error"` key with a null/None value in a response — the dispatcher
checks key presence and would report success as HTTP 500.

### Viewport and discovery actions

- `list_actions`: lists all 63 supported bridge actions in alphabetical order.
- `get_viewport`: queries current canvas viewport state: zoom level, zoom
  percentage, rotation angle, center coordinates `(x, y)`, mirror state,
  wraparound mode, and instant preview LOD mode.
- `set_zoom {percent | level | zoom}`: sets viewport zoom level. Accepts
  `percent` (e.g. 50, 100, 200), `level` (e.g. 1.0), or `zoom` (number or
  `"fit"`, `"100"`).
- `set_rotation {angle | rotation}`: sets viewport canvas rotation in degrees.
- `reset_view`: resets zoom to 100% and rotation to 0 degrees.
- `pan_canvas {x, y | dx, dy}`: pans canvas by setting preferred center
  `(x, y)` or moving by delta offset `(dx, dy)`.
- `set_canvas_mirror {mirror}`: sets horizontal canvas mirror view flip
  (boolean, default `true`).
- `set_canvas_lod {enable}`: enables or disables Instant Preview / Level of
  Detail mode (boolean, default `true`).

Typical generative-fill recipe:

    new_canvas → select_rect → ai_generate{mode:"fill", prompt:"..."}
    → poll ai_jobs until state "finished" → ai_apply{job_id, result:n}

Generative expand recipe: `ai_expand{right:512, prompt:"..."}` — the
command performs the resize+select+enqueue+apply sequence itself.

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

## External server configuration (`server.py`) — optional

Since Phase 2 the unified `POST /mcp` front on :5678 is the primary MCP
surface; the external `server.py` remains a supported client for
envelope-style workflows but is no longer required to talk MCP to the app.

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
