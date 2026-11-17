# End-to-end testing (`tests/e2e/`)

Behavioral test suite that drives a **real `solstice.exe`** through the
vendored kritamcp bridge (`127.0.0.1:5678`). No krita stubs — every
assertion observes the running app: pixel readback, files on disk (magic
bytes, PNG dimensions), HTTP status codes, and process liveness.

## Files

| File                             | Role                                                                                                                                                                                                              |
| -------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `tests/e2e/solstice_bridge.py`   | Minimal urllib client; token resolution mirrors `server.py` (`KRITAMCP_TOKEN` → `kritamcp.token` sidecar → kritarc `[kritamcp] token`; kritarc is parsed manually — KDE configs have keys before any `[section]`) |
| `tests/e2e/conftest.py`          | `ManagedApp` launch/teardown fixture, `/health` wait loop, merged stdout+stderr capture to `tests/e2e/_last_run_stderr.log` (gitignored)                                                                          |
| `tests/e2e/test_solstice_e2e.py` | 32 tests; see markers below                                                                                                                                                                                       |

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
  unknown action, `/info` surface; `Host` header enforcement (`_valid_host`)
  is **DONE** (implemented in kritamcp, tested in `tests/e2e/test_solstice_e2e.py`).
- **Documents**: create → info → `.kra` save (PK zip check) → reopen
- **Pixels**: fill/stroke writes verified via `get_color_at` projection
  readback (inside vs. outside the shape)
- **Layers**: add/opacity/visibility/duplicate/remove, merge, flatten;
  undo/redo on node ops (destructive pixel writes are undoable via
  `Node::setPixelDataUndoable` — `translate_layer` relies on it)
- **Actions and vector conversion**: `trigger_action` action invocation and
  Trace to Vector Layer (`test_trigger_action_and_trace_to_vector`: raster
  alpha traced into a filled `KoPathShape` on a new `KisShapeLayer`).
- **Display and screenshots**: canvas-scoped screenshot capture with zoom
  levels (`screenshot` action, `zoom` and `scope="canvas"` parameters) and
  Smooth Rendering at High Zoom verification (`test_smooth_high_zoom_display`:
  pixel edge blending analysis at 400 % zoom).
- **Selections/filters**: rect/all/deselect, blur apply
- **AI generation (`ai_*`)**: `ai_status` contract, `ai_generate` argument
  validation (bad mode, missing prompt, missing selection, unknown style),
  `ai_expand` canvas growth (asserted even without ComfyUI), and a slow
  `test_ai_generate_fill_then_apply` full loop — select → fill → poll
  `ai_jobs` → `ai_apply` — which **skips when no ComfyUI is connected**.
- **Live run 2026-10-27** (Solstice-0.5.0 build, external ComfyUI at
  `D:\krita` on `:8188`, RTX 5090): `ai_connect` → `connected:true`,
  200 checkpoints / 2420 loras indexed, GPU `vram_gb:32` reported;
  `ai_generate mode=create style="Qwen Image 2.1"` → job `finished`
  with 1 result in <10 s; `ai_apply` → `[Generated]` paint layer on
  canvas; `/loras` (LoRA Manager) HTTP 200. `server_state` reported
  `update_required` — version-file mismatch in external mode,
  cosmetic; managed autostart would need `ai_install`/`upgrade()`.
  `style` param matches style **name** (`"Qwen Image 2.1"`), not
  filename.
- **Live run 2026-10-26 cont.** (same build after ComfyUI core
  upgraded to `b9b290e` / 0.42.0-dev): `ai_generate
  style="MOSS Image"` → job `finished`, 3 results, layer applied —
  full MOSS arch path verified (checkpoint detect → `qwen_35_9b`
  encoder → `flux2` VAE → `moss_image` clip type). `ai_catalog`
  reports 22 arches (incl. Flux 3, MOSS Image). `ai_install{
  packages:[...]}` verified incl. Range-resume of a stalled
  `.part` (controlnet qrcode 1.98 GB → resumed, completed).
  MOSS edit mode remains gated: `TextEncodeMOSSImageEdit` node is
  absent from this ComfyUI core; the `lora-edit-moss` adapter is
  installed and waiting.
- **Files & security**: PNG export (IHDR dims), `.kra` ZIP magic; basic path
  traversal rejected (`..` checked on the raw path — `abspath` resolves it away).
  Comprehensive traversal rejection across all file-writing actions
  (`export_document`, `export_layers`, `screenshot`, `save_as_template`) with a
  shared `_resolve_allowed_path()` resolver is **DONE** (implemented in
  kritamcp, tested in `tests/e2e/test_solstice_e2e.py`).
- **Stability**: `repeated_ops` rapid-fire loop, `soak` (create → paint →
  save → reopen ×N with process-liveness checks), stderr `Traceback` scan
- **Error paths**: bad params return clean errors; bridge stays responsive

## Timing notes

- Effects of `action.trigger()`-based commands (`undo`, `redo`) land
  asynchronously on Krita's stroke queue — poll, don't assert immediately.
- `ai_generate`/`ai_expand` only *enqueue* — jobs get server ids inside an
  async coroutine on the GUI thread. Always poll `ai_jobs` for the new
  job's `id`/`state`; `ai_apply` needs `state == "finished"`.
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
8. **`ai_workflow` silent no-op** — `custom.generate()` schedules
   `_generate_custom` on the asyncio loop and returns immediately;
   restoring `model.workspace`/`workflow_id` in a synchronous `finally`
   reverted state before the coroutine read it → job never enqueued, no
   error. Custom-workflow calls must leave `Workspace.custom` set (the UI
   persists it anyway).
9. **IC-Light model format** — `iclight_sd15_fc.safetensors` is a full
   SD1.5 checkpoint and fails `UNETLoader` ("could not detect model
   type"); the UNet-loadable variant is `iclight_sd15_fc_unet_ldm`
   (huchenlei/IC-Light-ldm). Also: `-C -` resume prepended an
   "Entry not found" body to the header → "safetensors header too large"
   — always byte-sniff the first 8 bytes after a resumed download.
10. **Latent-size contract** — `ICLightAppply.c_concat` must equal the
    sampling latent dims; wire `GetImageSize(fg) → EmptyLatentImage`,
    never fixed 512×512 (a 1152×864 fg → 144×108 vs 64×64 mismatch).

### Custom-workflow bridge (`ai_workflows` / `ai_workflow`)

- `ai_workflows` lists local+remote workflows; `ai_workflow{workflow,
  params:{name:value}, apply}` runs one through the real
  `CustomWorkspace` path. Param mapping: `style` → style name,
  `image_layer`/`mask_layer` → layer name or id (resolved to QUuid),
  other params pass through raw.
- Verified e2e 2026-10-26: `harmonize-iclight` (IC-Light fc + RealVis
  NV + vae-ft-mse) → moonlit relit `[Generated]` layer applied.

### SeedVR2 upscale (`ai_upscale`) — 2026-10-26

- Verified e2e: 512² canvas → `ai_upscale{upscaler:"SeedVR2",
  factor:2.0, auto_apply}` → doc resizes to 1024² → `[Upscale]` +
  `[Generated]` layers applied with real upscaled pixels.
- Found+fixed: the upscale hook ended in `w.save_image` (plain SaveImage
  → files on disk, results never returned → job "cancelled" despite
  successful server execution). Must end in `w.send_image`
  (ETN_ReturnImage/ETN_SaveImageCache). Symptom: ComfyUI log shows a
  completed prompt, `ai_jobs` shows cancelled + 0 results.
- Found+fixed: `ComfyWorkflow.seedvr2_upscale` class method had the old
  signature + expected 2 outputs from `SeedVR2Preprocess` (native core
  emits 1) — the `hasattr` guard skipped the wrapper entirely, so the
  stale method ran. Fixed the class method itself.
- Found+fixed: `upscale_image()` resizes the document synchronously at
  enqueue; a failed/cancelled job left the canvas permanently resized.
  `JobParams.metadata["original_extent"]` is recorded at enqueue and
  `_finish_job` reverts on cancel/error when the doc still sits at the
  job's target extent (verified: enqueue→1024²→cancel→back to 512²).

### Phantom second bridge — root cause of ghost nondeterminism (2026-10-26)

- Symptom: deployed kritamcp changes not taking effect intermittently;
  debug prints never firing; `ai_status.build` marker absent.
- Cause: `share/krita/pykrita/ai_diffusion/ui/__init__.py` had been
  overwritten with a stale `kritamcp/__init__.py` (bad deploy). Every
  `ai_diffusion.ui` import executed a second bridge — two HTTP servers
  bound :5678 via Windows SO_REUSEADDR and requests raced between them.
- Detect: duplicated `HTTP server started`/`windowCreated` lines in
  stdout; `ai_status` missing a just-added field.
- Fix: restored the real `ui/__init__.py` (docstring-only). Also:
  pykrita registers THREE `.desktop` files for kritamcp
  (`kritamcp.desktop`, `kritapykrita_kritamcp.desktop` ×2) — watch for
  multi-instance symptoms after any deploy; add a `build` marker field
  to `ai_status` and verify it after restarts.

### Ideation surface (Phase 9.4) — 2026-10-26

- `ai_generate` gained `similar_to` (job id/index → inherits prompt,
  negative, strength, style; new seed) and deterministic `[[a|b|c]]`
  variant specs (one job per option, product-cap 8×16 jobs).
- `{a|b}` single-brace wildcards stay untouched for the plugin's native
  `eval_wildcards` (seeded per-seed picks) — verified `count:3` produced
  3 evaluated prompts.
- Found+fixed: `QueueMode.replace` inside a multi-generate loop cancels
  still-queued earlier variants → force `QueueMode.back` after the
  first enqueue.

### Distraction removal (`ai_remove_distraction`) — 2026-10-26

- `rect`/`point`+`radius`+`grow` → selection → `mode=remove` fill →
  applied `[Generated]` layer. Verified: ellipse removed, region padded
  and blended to background on a painted canvas.
- visionml `tool_select_segment_from_point/rect` are interactive canvas
  tools with no headless entry — SAM point-pick detection deferred.

### Live co-generation (`ai_live`) — 2026-10-26

- `ai_live` toggles `Workspace.live` + `LiveWorkspace.is_active`,
  sets `strength`/`prompt`/`style` and applies the stored preview via
  `apply` (forces `layer_only=True` → dedicated generated layer).
  Response diagnostics: `has_result`, `result_extent`, `model_error`,
  `doc_active`, `doc_name`.
- Verified: activate at strength 0.35 → paint stroke → `has_result`
  within ~5 s (live jobs regenerate on canvas changes through
  `LiveScheduler`); `apply` landed a named generated layer.
- Gotcha: `JobKind.live_preview` jobs are removed from the queue on
  finish (same as upscaling) so `ai_jobs` never shows them — poll
  `ai_live` for `has_result`. Results live in the docker preview
  until applied, never clobber real layers.
- Gotcha: sparse-sketch input at high strength mostly preserves the
  sketch — a params/quality limit, not a pipeline bug.

### FLF2V inbetweening smoke test (Phase 12.4) — 2026-10-26

- Direct ComfyUI `/prompt` chain verified: Wan 2.2 i2v high/low noise
  14B fp8 + i2v lightx2v 4-step loras + umt5_xxl_fp16 (type=wan) +
  wanVideo_clipVisionH + WanFirstLastFrameToVideo + 2×KSamplerAdvanced
  (split-step high→low, uni_pc/simple cfg 1.0) → VAEDecode →
  SaveImage PNG frames + CreateVideo→SaveVideo mp4.
- Result: 9-frame 480×264 inbetween of a moving ball — real motion
  interpolation (squash-and-stretch + trail), ~8 s warm / ~90 s cold
  (14B load). Test script: `~/CascadeProjects/flf2v_test.py`.
- Gotchas found: `format`+`codec` required on SaveVideo; height snaps
  to ÷8 silently (270→264); UNETLoader/LoraLoader names need
  subfolder prefixes (`wan\`, `Wan Video 2.2 I2V-A14B\`).

### FLF2V Krita integration (`ai_inbetween`) — 2026-10-27

- `ai_inbetween{first_layer,last_layer,frames,prompt,seed,steps,
  first_frame,step,layer_name}` → exports layers to PNG → /upload/image
  → queues verified Wan2.2 FLF2V graph → returns comfy prompt_id.
  `ai_inbetween_status{prompt_id}` → /history → /view frame download →
  `doc.importAnimation` → named `[Inbetween]` layer.
- Verified e2e: 9 frames → `animated=True`, keyframe_times [0..8] in
  ~10 s warm. `list_layers` now reports `animated`+`keyframe_times`.
- Found+fixed (3 real bugs):
  - `ai_settings.server_url` has no scheme → urllib "unknown url type";
    normalize to `http://`.
  - Response dicts must never contain `"error": <None>` — the POST
    dispatcher checks `if "error" in result` by key presence → success
    reported as HTTP 500 while the work actually completed.
  - `doc.importAnimation` ignores the active layer and creates its own
    paint layer → snapshot childNode uniqueIds and rename the new one;
    pre-creating a target layer just leaves a dead layer.
  - Job metadata must be module-global (`_FLF_JOBS`) — duplicate
    .desktop registrations mean a different Extension object may serve
    the next request; instance state is unreliable.

### RIFE frame interpolation (Phase 12.3) — 2026-10-27

- Native `FrameInterpolationModelLoader` + `FrameInterpolate` verified;
  `rife_v4.26.safetensors` (22.7 MB, Comfy-Org/Frame-Interpolation)
  installed to `D:\models\frame_interpolation`.
- Found+fixed: `krita-managed` extra_model_paths.yaml did not map
  `frame_interpolation` — added it to the live yaml AND to
  `server.py` (`_extra_model_paths_yaml` + `model_dirs`) so installs
  and upgrades keep it.
- Verified mechanically (0.4 s execution): 2 keyframes + multiplier 4
  → 5 output frames.
- LIMITATION found via e2e: RIFE dissolves objects on large
  displacement (300 px ball vanished in the mid frame). RIFE is a
  densification tool for existing dense sequences (small gaps, 1-2
  frames) — NOT a sparse-keyframe inbetweener. FLF2V (12.4) is the
  sparse-key path; document/label the distinction in the UI when 12.3
  ships.

### `ai_interpolate` bridge (Phase 12.3 shipped) — 2026-10-27

- `ai_interpolate`/`ai_interpolate_status` verified e2e on the real
  composition: FLF2V `[Inbetween]` (9 keys) → `ai_interpolate` →
  `[Densified]` layer with 17 keyframes. Same shared poll+import path
  as `ai_inbetween_status` (module-global `_FLF_JOBS`).

### `ai_anim` timeline bridge (Phase 12.1 partial) — 2026-10-27

- Verified live: setters (fps/frame/playback/full-clip range) apply and
  the response reports `animated_layers` with per-layer keyframe times.

### `ai_lipsync` Rhubarb (Phase 12.2 shipped) — 2026-10-27

- `rhubarb.exe -f json` (14.37 s clip → 72 cues) → per-frame PNG seq →
  `importAnimation` → `[Lipsync]` layer with 346 keyframes @24 fps.
- PIXEL-VERIFIED cue→shape mapping: painted mouth layers (A red, C
  green, X blue) land on the correct frames (f0=X, f24=C, f30=A).
- BUG FOUND+FIXED: removing a 346-keyframe animated layer then
  re-running a big `importAnimation` crashed solstice.exe (silent, no
  abort signature). `doc.waitForDone()` added after `node.remove()` in
  `remove_layer` and after `importAnimation` in `ai_lipsync`; verified
  stable across remove→reimport and double-run.
- Note: `fill`/`stroke`/`get_color_at` don't expose pixel colors for
  named colors — `set_color` first, `fill` uses the FG color.

### `ai_reference` per-region references (Phase 13.2 partial) — 2026-10-27

- Verified: assigns a named layer as `ControlMode.reference` control
  input (root region or a named region-linked layer), with strength.
- `ControlLayerList.add()` binds the ACTIVE layer — the action
  activates the target, adds, restores the previous active node, then
  pins `layer_id` explicitly.

### `ai_propagate` VACE (Phase 12.5 shipped) — 2026-10-27

- Model: `wan\wan2.1_vace_14B_fp8_e4m3fn.safetensors` (16.2 GiB) —
  plain `UNETLoader` load, native `WanVaceToVideo` 36-ch conditioning.
  `Wan2_1-VACE_module_14B_bf16` rejected correctly (patch, not UNET);
  `wan2_2_fun_vace` high-only unusable.
- Verified: animated `ctrl-anim` → 9 dense control frames +
  `painted-ref` reference → 13 output frames imported on `[Propagated]`
  (VAE temporal upsample pads latent length); frame content shows the
  reference appearance propagated through control structure.
- `ai_region`/`ai_reference` (Phase 13.2 bridge) verified: region
  created+prompted, layer bound as `ControlMode.reference` control
  scoped to that region.

### `ai_animate` Wan Animate (Phase 12.7 shipped) — 2026-10-27

- `WanAnimate2ToVideo` + `wan\wan2.2_animate_14B_bf16` (single model,
  no high/low split) + umt5 + wan VAE + clipVisionH.
- Verified e2e: animated `pose-src` → 9 dense pose frames →
  `pose_video`; `character` layer → reference_image + clip-vision
  encodes (ref on `clip_vision_output`, pose frame 0 on
  `clip_vision_output_pose`) → 13 frames on `[Animated]`, coherent
  generated content.

### MOSS region→layout prompts — 2026-10-27 (2 real bugs fixed)

- `format_moss_layout_prompt` was missing from workflow.py's
  `.comfy_workflow` import → NameError cancelled every MOSS+region job.
- `str(r.positive)` on a `TextPrompt` produced `<...object at 0x...>`
  reprs that MOSS faithfully rendered as image text — visible garbage.
  Fixed at 6 sites: use `positive.text` (or the raw string when it is
  already a str). Verified visually: lighthouse-left/ocean-right
  honored, clean output.
- The full loop `ai_region` (link+prompt) → `ai_generate` (MOSS Image
  style) → applied `[Generated]` layer is now verified e2e.

### GUI-thread stack overflow under command load — 2026-10-27 (fixed)

- Repro: ~25-35 `new_canvas` commands (or the full suite) killed
  `solstice.com` silently; no DrMingw record. lldb showed
  `STATUS_STACK_OVERFLOW` (0xc00000fd) on the GUI thread.
- Cycle: kritamcp's 50 ms `process_commands` poll and ai_diffusion's
  per-document `KritaDocument._poller` (20 ms) run Python slots that call
  `Document::activeNode()` → `KisSynchronizedConnectionBase::
  forceDeliverAllSynchronizedEvents()` → `processEvents()` → delivers the
  next pending timer → nested python slot → nested `activeNode()` →
  recursion depth ≈ active timer count → stack exhausted.
- Fixes: (1) `KisSynchronizedConnection.cpp` — `thread_local`
  re-entrancy guard in `forceDeliverAllSynchronizedEvents()`; the outer
  pump already delivers every pending event. (2) kritamcp
  `process_commands` — module-level `_command_processing` guard so a
  nested timer tick can never run a second command inside the current
  one (duplicate Extension instances share the guard).
- Also silenced a per-run storm of "Failed to create a precise timer"
  warnings by disabling the dev-tool `KisKCheckAccelerators` app-wide
  event filter (`kritarc` `[Development] AutoCheckAccelerators=false`);
  it restarted a 20 ms precise timer on every ChildAdded/Resize event.
- Verified: 60+ back-to-back `new_canvas` + `apply_filter
  background_removal` + `trigger_action layer_to_vector` survive; full
  suite: **30 passed, 2 skipped** (previously died mid-run ~cmd 180-196).
- Stale assertion fixed: `/info` advertises 88 commands, not 63
  (`test_info_advertises_all_actions`).

### Packaged-build verification + stderr traceback noise — 2026-10-27 (fixed)

- The self-contained package
  `C:\solstice-ci\packages\Solstice-0.2.0-beta.4-windows-x64-deb3ebd6a6f0`
  was launched by the fixture with `SOLSTICE_EXE` (no env crutches —
  plain `subprocess.Popen([exe])`) and ran the full suite: **34 passed,
  0 skipped** in ~2 min — every test runs, no soft-skips.
- `screenshot scope=canvas` wrong-view grab fixed the same day: with
  several open documents the handler's global `findChildren()` returned
  an arbitrary stale view's canvas, so the grab could miss the freshly
  painted content entirely ("canvas content not captured"). It now
  resolves the canvas inside `QMdiArea::activeSubWindow()` (the active
  view) with the global scan kept only as a fallback. Real bridge
  reliability defect — external agents hitting `new_canvas` →
  `screenshot` could capture the wrong document.
- Before that, the same run failed `test_plugin_load_clean` once:
  socketserver's default `handle_error` printed a full traceback when a
  client aborted mid-`send_error` (`WinError 10053`). Fixed in kritamcp
  with `QuietHTTPServer(HTTPServer).handle_error`: `ConnectionError` and
  `TimeoutError` now log a single `[KritaMCP] client ... went away`
  line; real handler bugs still print tracebacks.
- Confirmed present in the package: `translate_layer` reports
  `"undoable": true` (native `setPixelDataUndoable`), the 40-canvas
  churn regression test survives (commands ran past #206 where the old
  build died at ~#30), and no `Traceback`/`Failed to create a precise
  timer` warnings appear in stderr.
- Packaging note: `build-windows.ps1`'s `Tee-Object` pipeline stalled
  the packaging worker mid-copy; run `packaging/windows/package-complete.py`
  directly instead. The release zip can be patched in place with
  `7z u <zip> <root-relative-path>` for single-file fixes.
- Coverage gap E4 closed the same day: six new tests cover the ten
  previously untested bridge actions — `test_draw_shape`,
  `test_get_canvas_export`, `test_export_layers` (one PNG per visible
  layer, real dims asserted), `test_create_from_template` (preset +
  custom layers), `test_get_layer_presets`, `test_save_as_template`
  (writes .kra, cleans up after itself). Suite is now **40 tests**.
- 2026-10-27 follow-up: the Lane D trace-to-vector stroke refactor
  introduced a use-after-free the suite caught immediately — the
  `QSharedPointer<KoPathShape*>` holder's deleter freed the shape at
  function return while `KoShapeCreateCommand` was still queued in the
  applicator stroke (`KoShape::setParent` crash on the worker thread).
  Fixed by `*shapeHolder = nullptr` once the command takes ownership;
  the cancel path still frees late-arriving shapes. Result: **40
  passed, 0 skipped** on the self-contained package — including
  `test_trigger_action_and_trace_to_vector` and
  `test_matting_to_vector_pipeline` over the new stroke code.
