# Native live engine (librediffusion / TensorRT)

Phase-3 megaplan item. An optional, lazily-activated alternative to the
ComfyUI server for **Live mode** generation, driving in-process TensorRT
engines through librediffusion's C API from Python (`ctypes` — no pybind,
no compile step; the C ABI is `__cdecl`, stable across MSVC↔MinGW).

Upstream details, license decision (AGPLv3 — option (c)), engine recipes
and toolchain state: `librediffusion-intake.md`. This file documents the
Solstice-side seam only.

## Architecture

```
LiveWorkspace._continue_generating          (model/model.py)
  ├─ settings.live_engine == "comfyui"  → _prepare_live_workflow → ComfyUI (unchanged)
  └─ settings.live_engine == "native"
        ├─ _native.availability() != None → report once → ComfyUI path (graceful fallback)
        └─ _native.generate_frame()       → NativeLiveEngine (native_engine.py)
              _capture()  — GUI thread: canvas RGBA (get_image), selection mask,
                            region prompt, live.strength
              _run_frame()— worker thread (asyncio.to_thread): ctypes C calls
                            set_prompt (cached) / set_schedule / set_mask /
                            set_reference / frame_cached
              job = JobQueue.add(Job("native-N", live_preview, params))
              jobs.set_results → jobs.notify_finished
                  → existing handle_job_finished → set_result → preview overlay
                  → apply_result / seed rotation / UI all reuse ComfyUI path
```

The stream (`KleinStream`) is created lazily on first frame and kept for
the document; destroyed on `LiveWorkspace.toggle(False)` or GC
(`KleinStream.__del__` → `flux2_stream_destroy`).

## Semantics (from `librediffusion_c.h`, verified)

- `flux2_stream_create(transformer, qwen, vae_dec, vae_enc, tokenizer.json,
  Th, Tw, seed, device)` — token grid: **H = Th*16, W = Tw*16**. Canvas
  extent maps to `Th=H/16, Tw=W/16`, clamped to [1..64] (1024px ceiling).
- `set_prompt` re-runs the Qwen encoder only when the text changes (C-side
  cache; Python still calls it every frame — cheap no-op when unchanged).
- `set_reference` VAE-encodes the host RGBA frame once; `frame_cached`
  then denoise+decodes only. Reference is re-sent every frame (canvas
  mutates per stroke).
- `set_mask(rgba, H, W)`: **white luminance = regenerate, black = keep**;
  area-averaged to the token grid each denoise step. Fed from Krita
  Selection via `Mask.to_image` → RGBA8888.
- `set_schedule(sigmas)`: FlowMatch sigmas high→low, subsumes steps +
  strength. Live strength s<1 → `[s, 0.64*s]`; s=1 → `[1.0, 0.64]`
  (upstream 2-step default). Fixed seed 52 (upstream FluxRT convention;
  never reseeded per frame — frame coherence).
- `set_bn(mean[128], std[128])` — required once before frames; read from
  bundle's `bn_mean.bin`/`bn_std.bin` (128 f32 each).
- Result is stream-dim RGBA8888 host bytes → `QImage.Format_RGBA8888` →
  `Image` → scaled to canvas bounds if dims differ.

## Frame skipping

Each frame's fingerprint `(prompt, strength, has_mask, qimage.cacheKey())`
is compared to the last; unchanged canvases/prompts skip the ~40-90ms
GPU frame entirely — Live mode idles at the scheduler poll rate like
ComfyUI's `should_generate` gate.

## Discovery

- `librediffusion.dll`: `SOLSTICE_LIBREDIFFUSION_DIR` env →
  `<install>/share/krita/livediffusion/bin` →
  `%LOCALAPPDATA%/Solstice/livediffusion/bin`. Runtime deps resolved via
  `os.add_dll_directory`: TRT `bin/` (`TENSORRT_DIR`), CUDA `bin/x64`,
  the DLL's own dir.
- Engine bundle (klein): `settings.native_engine_dir` →
  `%LOCALAPPDATA%/Solstice/livediffusion/engines/klein` →
  `<install>/share/krita/livediffusion/engines/klein` →
  `D:/models/livediffusion/klein`. Required files: `transformer_bf16.plan`,
  `qwen3_encoder_bf16.plan`, `vae_{encoder,decoder}_bf16.plan`,
  `tokenizer.json`, `bn_mean.bin`, `bn_std.bin`; optional
  `rife_ifnet_fp16.plan`.

## Settings (new, kritarc-backed)

- `live_engine` — `"comfyui"` (default) | `"native"`. Combo in
  Settings → Interface, next to the other live apply settings.
- `native_engine_dir` — explicit bundle dir override (empty = discovery).
- `native_precision` — `"bf16"` (default, full quality) | `"fp8"` (fast
  preview). When `fp8` and `transformer_fp8.plan` exists in the resolved
  bundle, `find_engine_bundle` selects it; otherwise silently stays bf16.

## Failure / degradation model

- Missing DLL/engines → `availability()` string → one-time
  `report_error`, ComfyUI path continues uninterrupted.
- Runtime `NativeEngineError` (OOM, TRT init fail) → stream destroyed,
  reason latched in `_unavailable_reason`, report once, ComfyUI takes
  over. `release()` (live toggle off) clears the latch so a later
  toggle-on re-probes.
- `_busy` guard prevents overlapping frames; capture errors (no visible
  layers) deduped by message.

## Limitations / follow-ups

- Only the **klein stream path** is wired; SD/SDXL-Turbo classic
  pipelines (`config_*`/`img2img`), ControlNet, IP-Adapter, RIFE display
  multiplication and img2img-turbo are exported by the DLL but not yet
  consumed.
- Prompt uses `active_regions.active_or_root.positive` only — region
  weights/nesting and style prompt templates are not merged (ComfyUI
  does that in `workflow.prepare_prompts`; port if needed).
- No VRAM precheck — klein bf16 stream ≈ 16-18 GB resident; TRT failure
  surfaces as an engine error. `engine_cache_set_max_entries` and idle
  Qwen unload are future levers.
- Host-RGBA only (Tier A). Zero-copy needs the upstream
  `*_frame_device` export + CUDA↔Vulkan NT-handle sharing.
- `plugins/livediffusion` C++ plugin (intake doc) deferred — ctypes
  covers Tier A; revisit only for Tier B.

## Manual regression checks

- Live mode on ComfyUI still works with `live_engine=comfyui` (default).
- Select native with no DLL/engines → single error banner, ComfyUI
  frames continue.
- With engines present: live frames produce `[Generated]`-style results,
  apply via existing apply_result, selection masks regenerate only the
  selected region, canvas idle costs no GPU frames.

## Verification (2026-11-16)

End-to-end verified on the RTX 5090 dev box:

- `stream_smoke.py` (raw ctypes, no Krita): stream create ~9-11s (14.2 GB plans),
  23 prompt tokens via Qwen, `frame_cached` 0.16-0.31 s/frame at the
  `{1.0, 0.64}` 2-step schedule — non-degenerate watercolor output. Verified
  at 320x576 (Th,Tw=36,20), 512x512 (32,32), 768x384 (48,24).
- In-app via `kritamcp` `ai_live`: `live_engine: native` loads from the real
  settings store (`%APPDATA%\krita\ai_diffusion\settings.json`), bundle resolves,
  `ai_live {active: true}` produces real results with `model_error: None`,
  and `ai_live {apply: true}` lands the frame as a `[Live]` layer. Verified at
  768x768 and 640x480 canvas extents — streams are created at native canvas
  geometry (`creating klein stream 640x480`), no rescaling squash. Applied
  output pixel-verified (non-zero, diffusion-refined reference at 0.3 strength).
- RIFE enabled in-app (`native_rife_exp: 1`): stream attaches
  `rife_ifnet_fp16.plan`, intermediates emit through `live.set_result`.
  Raw `rife_smoke.py`: exp=2 produces 4 frames in ~20 ms with monotonic
  interpolation; `required_out_bytes` matches the spec.
- Fallback verified: with no bundle, `ai_live` falls back to ComfyUI and reports
  `Native engine unavailable, using ComfyUI: <reason>`; any runtime failure in
  the native path (engine error, OSError, conversion failure) takes the same
  degrade path instead of stalling the live loop.

## Known constraints

- **Dynamic geometry via ranged TRT profiles** (built 2026-11-16): token grid
  Th in [18,48], Tw in [16,48], VAE latent max (1,32,96,96), transformer
  `Lp <= 4608` where the reference path concatenates `2*Th*Tw` tokens — so the
  bridge clamps `Th*Tw <= 2304` (~768x768 max). Larger canvases are
  grid-clamped + scaled, not failed.
- **`set_prompt` is mandatory** before `frame_cached` — the C API returns
  `INVALID_ARGUMENT` until a prompt has been encoded; the bridge always sets
  one (`""` encodes fine and marks `have_prompt`).
- **ctypes output buffers must be passed by object, not `.raw`.**
  `create_string_buffer().raw` is a fresh immutable copy — passing it hands the
  C function a temporary that is freed on return (silent all-zeros output, and
  a UAF if the DLL retains the pointer). This bug shipped and was caught in
  review; `frame_cached` now passes the buffer object.
- **Stream handle lifetime is thread-serialized.** `KleinStream` carries a
  `threading.Lock`: `_run_frame` holds it for the whole C-call sequence and
  `close()` holds it during destroy, so toggling Live off mid-frame cannot
  UAF the native handle.
- **TRT plan version lock.** `.plan` files must be built with the *exact* TRT
  the DLL links (11.0.0.114). The unified venv pins PyPI `tensorrt` 11.2.1.2 —
  rebuilds require the SDK's own `tensorrt-11.0.0.114-cp312` wheel (thin
  binding, needs a `__init__.py` shim + `sitecustomize.py` doing
  `os.add_dll_directory(<TRT>/bin)`), and `build_klein_engines.py` must run
  with cwd = TRT `bin` (its libLoader resolves `nvinfer_builder_resource_*`
  cwd-relative). The 11.2 `tensorrt_cu13*` dists must be uninstalled first —
  their `tensorrt_libs/nvinfer_11.dll` shadows the SDK runtime.
- **fp8 transformer** (`transformer_fp8.plan`, opt-in via
  `native_precision=fp8`): built 2026-11-17 from real `KLEIN_FP8_GOLD`
  activations (captured via `capture_klein_goldens.py` hooking
  `Flux2KleinPipeline`), `KLEIN_FP8_LEVEL=all` — 100/108 linears
  quantized, embedders/modulation/norm_out/proj_out kept bf16. Measured
  vs bf16 on identical seed/schedule across 3 sizes x 3 prompts:
  ~24.8 dB mean PSNR / 0.91 SSIM (same composition, fine-detail drift),
  ~35% faster, 4.08 GB vs 7.79 GB. The `ff` level (attention bf16) was
  tested and rejected — same quality cost, ~5% speed. bf16 remains the
  shipped default; fp8 is a preview/draft tier, not a replacement.
  Rebuild: goldens in `goldens-klein/`, ONNX intermediates in
  `onnx-fp8/transformer_fp8_calib[_dyn]`, plan via `build_fp8_plans.py`
  (deployed dynamic profile, hw_compat none — `ampere_plus` starves
  fp8 tactics of smem).
- Engines built with `--hw-compat ampere_plus` (portable SM 8.0+).
