# LibreDiffusion intake — verified build/design doc

Status: intake survey complete (2026-11-15, against
`C:\Users\richk\Downloads\librediffusion-main.zip`, 8.3 MB, extracted to
`%TEMP%\librediff-intake`). Megaplan Phase 3 target: in-process
C++/CUDA/TensorRT real-time diffusion replacing the rejected FluxRT-TD
TouchDesigner bridge. Every megaplan claim below was checked against the zip
contents; corrections are flagged.

Upstream: `github.com/jcelerier/librediffusion` — a C++23/CUDA/TensorRT
reimplementation of StreamDiffusion, written for ossia score. The repo also
vendors the StreamDiffusion Python package under `src/streamdiffusion/` (used
only as the engine-build toolchain; Solstice never runs it at runtime).

## License — CORRECTION (was "MIT")

**`LICENSE` is GNU Affero GPL v3, not MIT.** No per-file headers override it;
`pyproject.toml`/`README.md` carry no license field. This collides with the
megaplan's boundary ("only `MIT OR Apache-2.0`/MIT code enters the tree") and
is the top decision item:

- Solstice is GPLv3. Linking/embedding AGPLv3 code makes the shipped
  combination AGPLv3 — including the §13 network-interaction source-offer
  clause, which a future MCP/`/mcp` HTTP surface could realistically trigger.
- Options: (a) treat librediffusion as an **external optional runtime
  dependency** — ship zero of its source/binaries, load `librediffusion.dll`
  at runtime when the user installs it (upstream already ships
  `src/librediffusion_loader.hpp`, a full `LoadLibrary`/`dlopen` symbol
  loader, for exactly this); (b) ask upstream for a MIT/Apache-2.0 or
  LGPL/static-link exception before vendoring; (c) vendor under AGPLv3 and
  accept the license upgrade for the whole binary (not recommended).
- Default proposal below is (a): **nothing of librediffusion enters the
  tree** — we keep a thin loader shim + engine-build scripts under our own
  license, and the DLL + engines are a user-installed optional component,
  same class as the ComfyUI install ai_diffusion drives.
- **DECISION 2026-11-15 (user):** option **(c)** — vendor/link in-process
  and accept AGPLv3 for the combined binary. User: "I'm perfectly fine
  sharing source code — this was going to be an open source project
  anyway." Consequences recorded in `integration-ledger.md`: the shipped
  combination is AGPLv3 (GPLv3 §13 explicitly permits the combination);
  §13's network-interaction source-offer applies to the whole app, so the
  `/mcp` surface should carry a source-availability notice; ship upstream's
  `LICENSE` alongside any vendored sources and mark modifications.

## Top-level layout (zip)

- `src/librediffusion*.cpp/hpp`, `librediffusion_c.{h,cpp}`,
  `librediffusion_c.{flux2,rife,img2img_turbo}.cpp` — the library.
- `src/librediffusion_loader.hpp` — runtime symbol loader (see above).
- `src/kernels*.cu`, `nchw.cu` — CUDA kernels (klein + RIFE included).
- `src/clip-tokenizer-c/` — Rust staticlib (CLIP + **Qwen** tokenizer C API,
  imported via Corrosion; `bpe_simple_vocab_16e6.txt.gz` vendored).
- `src/streamdiffusion/` — Python engine-build toolchain (`train-lora.py`
  drives it); `src/streamdiffusion/klein/` — FLUX.2-klein export/build/fix
  scripts + vendored assets (`rife_ifnet_fp16.onnx` 4.7 MB,
  `tokenizer.json` 11 MB, `bn_mean.bin`/`bn_std.bin` 512 B).
- `src/schedulers/*.hpp` — precomputed timestep tables (sd-turbo, sdxl-turbo,
  sdxs, LCM Dreamshaper, Segmind-VegaRT).
- `tools/` — `inspect_engine.py`, `generate_scheduler_tables.py`,
  `export_tokenizer.py`, `export_ipadapter_image_encoder.py`,
  `generate_prompt_embeddings.py`.
- `train-lora.py`, `pyproject.toml`, `uv.lock`, `CMakeLists.txt`,
  `.github/workflows/ci.yml`, `LICENSE` (AGPLv3), `README.md`.

## Build system (verified)

`CMakeLists.txt` (205 lines), `cmake_minimum_required(VERSION 3.18)`:

- C++23 / CUDA C++20; `CMAKE_POSITION_INDEPENDENT_CODE`, LTO/IPO on.
- `CMAKE_CUDA_ARCHITECTURES` defaults to `all` (CMake ≥3.23) or explicit
  `75-real;80-real;86-real;89-real;90-real;90a-real;100-real;103-real;110-real;120`
  — **`sm_120` support is real and in-tree** (comment: "120=50xx").
- MSVC branch sets `/Zc:preprocessor` "because CUDA 13's CCCL headers reject
  the traditional preprocessor" — CUDA 13 is the supported toolkit.
- Windows requires `-DTENSORRT_INCLUDE_DIR` + `-DTENSORRT_LIB_DIR`; links
  `find_library(... nvinfer nvinfer_11 nvinfer_10)`. `TENSORRT_RTX` option
  exists but the comment says it is **abandoned**.
- Deps: `CUDA::nppig`, `CUDA::curand`, cudart; **Boost ≥1.83 headers**
  (`boost::concurrent_flat_map` in `model_cache.hpp`); Rust via Corrosion
  v0.5 (FetchContent) for the tokenizer; Catch2 v2.13.10 (FetchContent) for
  tests; `3rdparty/cutlass` include dirs.
- Outputs: `librediffusion` SHARED (`LIBREDIFFUSION_BUILD_DLL`) + static
  `librediffusion_cuda`. Windows link adds `bcrypt ntdll userenv ws2_32
  advapi32` for the Rust staticlib.

**CI (`.github/workflows/ci.yml`) pins the certified combo:** CUDA **13.2**
(Jimver/cuda-toolkit `13.2.0`), **TensorRT Enterprise 11.0.0.114**
cuda-13.2 SDK zip (direct developer.nvidia.com download, no login),
Boost 1.86, Ninja; CPU-only compile gate. Windows job uses `msvc-dev-cmd`.

### Missing from the zip

- **`3rdparty/cutlass` is an empty submodule** (`.gitmodules` →
  `github.com/NVIDIA/cutlass`, no recorded pin). The zip cannot configure
  without it; pick and pin a CUTLASS release tag (4.x matches CUDA 13) in
  our build script and record it in `plugin-intake.md`.
- No prebuilt engines, no weights — expected; engines are built locally.

## Verified C API (`src/librediffusion_c.h`, 1388 lines)

ABI-stable `extern "C"` (`__cdecl` on Windows), opaque handles,
`librediffusion_half_t = uint16_t`, `librediffusion_stream_t = void*`.
`librediffusion_version()` returns `"1.0.0"`. Model enum:
`MODEL_SD_15 / MODEL_SD_TURBO / MODEL_SDXL_TURBO / MODEL_FLUX2_KLEIN_4B`
(SDXS/LCM run on the sd15 engine path — there is no dedicated enum).

Megaplan claims vs. reality — **all six verified verbatim**:

```c
/* header lines 1157-1161 */
LIBREDIFFUSION_API librediffusion_flux2_stream_handle LIBREDIFFUSION_CALL
librediffusion_flux2_stream_create(
    const char* transformer_engine, const char* qwen_engine,
    const char* vae_decoder_engine, const char* vae_encoder_engine,
    const char* tokenizer_json, int Th, int Tw,
    unsigned long long seed, int device);

/* 1202-1203 — re-tokenizes + re-runs Qwen encoder, caches embeds;
 * returns number of real text tokens or -1 */
LIBREDIFFUSION_API int LIBREDIFFUSION_CALL
librediffusion_flux2_stream_set_prompt(librediffusion_flux2_stream_handle s,
                                       const char* prompt);

/* 1189-1191 — "host RGBA [H,W,4]; WHITE = regenerate, BLACK = keep.
 * Area-averaged to the Th x Tw token grid and applied each denoise step
 * (latents = mask*generated + (1-mask)*noised_ref)" */
LIBREDIFFUSION_API void LIBREDIFFUSION_CALL
librediffusion_flux2_stream_set_mask(librediffusion_flux2_stream_handle s,
                                     const unsigned char* mask_rgba, int H, int W);

/* 1181-1183 — "Explicit FlowMatch sigma SCHEDULE ... HIGH->LOW (terminal 0
 * appended internally). {1.0, 0.64} = the 2-step default; {1.0} = 1 step.
 * SUBSUMES set_steps + set_strength" */
LIBREDIFFUSION_API void LIBREDIFFUSION_CALL
librediffusion_flux2_stream_set_schedule(librediffusion_flux2_stream_handle s,
                                         const float* sigmas, int n);

/* 1217-1222 — cached-reference fast path: VAE-encode once, then
 * denoise+decode only per frame */
LIBREDIFFUSION_API librediffusion_error_t LIBREDIFFUSION_CALL
librediffusion_flux2_stream_set_reference(
    librediffusion_flux2_stream_handle s, const unsigned char* input_rgba);
LIBREDIFFUSION_API librediffusion_error_t LIBREDIFFUSION_CALL
librediffusion_flux2_stream_frame_cached(
    librediffusion_flux2_stream_handle s, unsigned char* output_rgba);

/* 1290-1294 — RIFE, device-pointer variant (prev/cur/out on DEVICE) */
LIBREDIFFUSION_API librediffusion_error_t LIBREDIFFUSION_CALL
librediffusion_rife_interpolate_gpu_sized(
    librediffusion_rife_handle h, const unsigned char* prev_rgba_dev,
    const unsigned char* cur_rgba_dev, int H, int W,
    unsigned char* out_frames_dev, size_t out_capacity_bytes, int* out_count);
```

Also real: `librediffusion_flux2_stream_set_steps`,
`..._set_strength` (flow-match img2img `[0..1]`), `..._set_bn` (VAE
batch-norm, host fp32[128] — required before streaming),
`..._stream_frame` (host RGBA in/out), `..._stream_dims` (H=Th*16,
W=Tw*16), seed argument = fixed noise seed ("FluxRT uses 52; never
reseeded per frame" — confirms the seed-52 claim). Qwen3 hidden-state
cache confirmed: "the Qwen text encoder is CACHED: it runs only when the
prompt changes" (header 1138-1140); ehs is bf16 `[1,Lt,7680]` with `Lt`
fixed 512 (Qwen3-4B layers 9+18+27 concatenated: 3×2560=7680).

Beyond klein: full classic pipeline API (`config_*`, `pipeline_*`,
`img2img`/`txt2img` CPU and `_gpu_half`/`_gpu_float` device variants),
multi-ControlNet (`config_add_controlnet` + `set_controlnet_cond[_rgba]`),
IP-Adapter (baked into UNet engine; host-side CLIP image encode or
optional on-device encoder engines), runtime-LoRA scale slots
(`lora_scale[N]`, `PATH:runtime` export flag), StreamV2V temporal
coherence, engine LRU cache controls
(`librediffusion_engine_cache_clear/count/set_max_entries`, default 16),
CUDA graph opt-in (`config_set_cuda_graph`, ~+18% on 1-step SD-Turbo),
`img2img_turbo_*` handle API (GaParmar pix2pix-turbo skip-VAE), pinned/UA
memory helpers. `RIFE_MAX_EXP = 4` (up to 16× display multiplication);
deprecated unsized RIFE/`img2img_turbo_frame` forms — always use `_sized`.

**Nuances for the ledger:**

- `flux2_stream_frame`/`frame_cached`/`set_mask`/`set_reference` are
  **HOST** RGBA entry points. The internals already keep resident
  `in_rgba_dev`/`out_rgba_dev` device buffers and a dedicated RIFE stream
  (`librediffusion_c.flux2.cpp` ~235-243), so a `_frame_device` variant is
  a small upstream patch — zero-copy display is NOT blocked, it is just
  not yet exported on the stream API. The non-stream `flux2` C API and
  `img2img_gpu_half`/`rife_interpolate_gpu_sized` already take device
  pointers.
- `librediffusion_rgba_resize` (835) takes **device** pointers only;
  feeding it a host buffer is documented as a sticky, context-killing
  error (`LIBREDIFFUSION_ERROR_CUDA_CONTEXT_LOST`).
- SDXL Turbo 26 fps @1024², SD Turbo 96 fps @512², SDXS >600 fps @512² on
  RTX 5090 — README benchmarks, match the megaplan figures.

## Build requirements for this machine

Present (verified): CUDA **13.2, 13.3, 13.4** toolkits
(`CUDA_PATH=v13.3`), MSVC 14.44.35207 (VS2022 BuildTools — nvcc host
compiler OK; VS2019 14.29 too old for CUDA 13, ignore it), rustc 1.99 +
cargo, uv 0.11.19, Ninja, CMake ≥3.28.

Missing / mismatched:

- **TensorRT SDK: only TRT 10.16.1.11 found**
  (`C:\Users\richk\CascadeProjects\TensorRT-10.16.1.11`, `nvinfer_10.lib`).
  Upstream certifies **TRT 11**: CI uses the Enterprise 11.0.0.114
  cuda-13.2 SDK; `pyproject.toml` pins `tensorrt-cu13==11.2.1.2` and warns
  "do NOT let resolution slide to TRT 10 (different builder semantics)".
  Engines are TRT-major-locked (a TRT-11-built `.plan` will not
  deserialize on TRT 10). **Install the TRT 11.0.0.114 cuda-13.2 SDK zip
  and point `TENSORRT_INCLUDE_DIR`/`TENSORRT_LIB_DIR` at it.**
  `windows-dev.lock.json`/`devops-environments.md` currently say "TRT
  10.x min" — bump the pin to 11.x (doc + lock fix, follow-up).
- **CUTLASS** — fetch + pin (above).
- **Boost ≥1.83 headers** — not confirmed installed; CI downloads 1.86.
- **Dedicated Python 3.12 venv** (`requires-python = ">=3.12,<3.13"`; our
  repo tooling pins 3.13 — separate venv, do not reuse) populated by
  `uv sync` from `pyproject.toml`: torch/torchvision cu132 wheels,
  `tensorrt-cu13==11.2.1.2`, nvidia-modelopt ≥0.44, onnx/onnxruntime,
  transformers ≥5.10, **forked diffusers
  (`jcelerier/diffusers@kvo-cache-0.38` — "generic diffusers will NOT
  work")** + `livepeer/Diffusers_IPAdapter@405f87da`. Expect ~6-8 GB of
  wheels. `klein_env.sh` is bash/`LD_LIBRARY_PATH`-oriented; on Windows
  set `PATH` to the venv's `tensorrt_libs`/`nvidia` DLL dirs instead
  (translate, don't source).
- MSVC shell env for the C++/CUDA build — nvcc on Windows requires cl.exe;
  Solstice's llvm-mingw cannot compile the `.cu` files. Build
  `librediffusion.dll` under the VS2022 BuildTools prompt, consume the
  DLL from llvm-mingw Solstice through the C API (exactly what the ABI
  shim is for: "supports both MSVC cl.exe and MinGW").

### Local toolchain state (2026-11-16)

- Source: `C:\Users\richk\Downloads\librediffusion-main.zip` → extracted
  to `F:\krita-work\librediffusion\librediffusion-main`.
- **CUTLASS pinned: `v4.2.1`** (release tag, cloned into
  `3rdparty/cutlass`; headers-only consumer per CMakeLists:143,187).
- **TensorRT Enterprise 11.0.0.114 cuda-13.2** — pinned URL from upstream
  `ci.yml` (`TRT_WINDOWS_URL`), extracted to
  `F:\krita-work\librediffusion\deps\TensorRT-11.0.0.114`
  (SDK zip ~1.94 GB; **runtime DLLs live in `bin/`**, link libs in
  `lib/` — `TENSORRT_LIB_DIR` points at `lib`, runtime PATH at `bin`).
- **Boost headers 1.86.0** — `F:\krita-work\librediffusion\deps\boost_1_86_0`
  (`boost/unordered/concurrent_flat_map.hpp`).
- Host env: `vcvars64.bat` from VS2022 BuildTools
  (`C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools`),
  `CUDAToolkit_ROOT=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.2`,
  `sm_120` arch for the RTX 5090 (`CMAKE_CUDA_ARCHITECTURES=120-real`
  keeps the build to one SM instead of `all`).
- **BUILD DONE (2026-11-16)**: `F:\krita-work\librediffusion\build\`
  produces `librediffusion.dll` (5.9 MB) + `librediffusion.lib` +
  cargo `clip_tokenizer_c.dll`. 4 CUDA TUs → static
  `librediffusion_cuda`; 22 C++ TUs. Smoke: DLL loads, TRT/NPP resolve,
  `librediffusion_version()` → `1.0.0`; 141 `librediffusion_*` exports.
- Build scripts: `F:\krita-work\librediffusion\run-configure.bat` /
  `run-build.bat`. **Gotchas learned:** (1) set `TMP`/`TEMP` to a real
  Windows dir — Devin shells export `TMP=/tmp`, nvcc exits 2 silently;
  (2) force `-DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl
  -DCMAKE_CUDA_HOST_COMPILER=cl` or llvm-mingw clang wins PATH order and
  nvcc cannot host it; (3) `--fresh` clears the half-detected cache.
- Runtime DLL search dirs: `TensorRT-11.0.0.114\bin`, CUDA
  `v13.2\bin\x64` (CUDA 13 moved NPP/cudart DLLs under `bin\x64\`),
  dir containing `librediffusion.dll`. Imports: `nvinfer_11.dll`,
  `nppig64_13.dll`, MSVC CRT only — `clip_tokenizer`/`onig` are
  statically linked in.

### Engine build recipes (from README + klein scripts)

`uv run python train-lora.py --type {sd15|sdxl|img2img-turbo|klein}` —
produces `<out>/unet.engine`, `vae_encoder.engine`, `vae_decoder.engine`,
`clip.engine` (+`clip2.engine` SDXL, +ControlNet/IP-Adapter variants;
`--type klein` instead dispatches to `src/streamdiffusion/klein/`:
`export_klein.py` ONNX → `fix_qwen_complex.py` +
`fix_klein_dynamic_seq.py` rewrites → `build_klein_engines.py` producing
`transformer_bf16.plan`, `qwen3_encoder_bf16.plan`,
`vae_{encoder,decoder}_bf16.plan`; optional `export_klein_fp8_calib.py` +
`build_one_fp8.py` FP8 path via modelopt). `build_rife.py` builds
`rife_ifnet_fp16.plan` from the vendored ONNX. LoRA fusion at export
(`-l REPO|file`, repeatable; `PATH:runtime` keeps a live `lora_scale`
slot; runtime LoRA incompatible with FP8).

- Build cost (estimate, sm_120): klein 4-engine set ≈ 30-90 min first run
  (ONNX parse of ~8 GB transformer + tactic selection; workspace 8-14 GB
  per engine); SDXL set ≈ 10-30 min; SD15/turbo/img2img/RIFE minutes.
  Engines persist in `<out>/` — rebuilt per model/precision/geometry, not
  per run; `KLEIN_HW_COMPAT={none|ampere_plus|same_cc}` controls portable
  vs build-GPU-only engines (default `none` = RTX 5090 only — correct for
  this box; `ampere_plus` if engines ever ship to other GPUs, ~5-15%
  slower).
- Weights: HF `black-forest-labs/FLUX.2-klein-4B` (diffusers layout —
  transformer ~7.8 GB bf16 + Qwen3-4B text_encoder ~8 GB + 32-ch VAE;
  Apache-2.0 per BFL's klein release — re-verify the repo license at
  download; gated repos need an HF token). Local disk already has
  single-file `Flux.2 Klein 4B` UNet (7.75 GB), `flux2-vae.safetensors`,
  and `mistral_3_small_flux2`/`Qwen3` encoder files under `D:\models` —
  usable only after repacking to the diffusers repo layout; the clean
  path is the HF download (~16-20 GB). SD/SDXL turbo weights from
  `stabilityai/*` — Stability Community License, NOT redistribution-safe:
  **never ship `.engine`/weights; ship the build scripts and let users
  point at local or HF weights** (same policy as `D:\models`).
- VRAM budget (estimate): klein stream resident ≈ 16-18 GB bf16
  (transformer ~8-9 + Qwen3 ~8 + VAEs ~0.6 + buffers); FP8 transformer →
  ~12-13 GB. SDXL-Turbo set ≈ 6-7 GB; SD-Turbo ≈ 2-3 GB; RIFE < 0.1 GB.
  RTX 5090 32 GB fits klein + live canvas + ComfyUI co-residency, tight
  with a large ComfyUI model loaded — the `gpu-queue` VRAM guard and
  `engine_cache_set_max_entries(0)` are the levers. Open question: the
  Qwen engine is held by the stream handle even when the prompt is
  static — consider unload-on-idle or FP8/INT8 Qwen export later.

## Integration seam proposal

Placement: **new `plugins/livediffusion/` Krita extension plugin**
(not `libs/` — this is an optional runtime feature, not core image
functionality; `libs/gpu` is the Vulkan canvas engine and must not gain a
CUDA dep). Mirror `plugins/visionml` conventions: `src/` plugin class +
tools, `thirdparty/` for any vendored shim, `kis_add_library(... MODULE)`
+ `generate_export_header`, actions via `.action` files, models/engines
under `share/krita/livediffusion/` or auto-discovered `D:\models`
roots.

Link strategy (superseded — decision is option (c), in-process link; the
loader notes below remain useful if the strategy is ever revisited):

1. **Runtime-loaded DLL (recommended).** Ship a thin `KisLiveDiffusion`
  Qt plugin that `QLibrary`/loader-shims `librediffusion.dll` from an
  install dir (`share/krita/livediffusion/bin` or a configured path,
  plus `nvinfer_11.dll`, `cudart`, `nppig`, `curand` DLLs alongside —
  standard TRT SDK runtime packaging). Feature absent → option greyed
  out; zero hard dep in solstice.exe. Upstream
  `librediffusion_loader.hpp` is AGPLv3, so re-implement the ~10 needed
  `GetProcAddress` bindings in our own code (trivial) or ask upstream to
  split it.
2. Static/in-tree C++ link — only if relicensed (option b/c); keep the
   C API boundary regardless (llvm-mingw ↔ MSVC).

Display path — two tiers:

- **Tier A (phase-in, ship first):** host RGBA. `flux2_stream_frame`
  returns `[H*W*4]` bytes; at the stream's native ~320×576-576×320
  geometry that is ~0.7 MB/frame — a `KisPaintDevice` write or direct
  texture upload is sub-millisecond. Zero-copy is *not* required at
  these resolutions; measure before building Tier B.
- **Tier B (optional):** CUDA→Vulkan/D3D11 zero-copy via
  `cudaExternalMemory` + NT shared handles (the Dawn `LatticeSharedImage`
  pattern), needed only if a ≥1024² SDXL real-time tier lands. Requires
  the upstream `_frame_device` patch noted above — keep it a small
  additive patch, not a fork.

`ai_diffusion` LiveWorkspace selection: `model/model.py::LiveWorkspace`
routes through `DocumentModel._generate_live` → ComfyUI `client.enqueue`.
Add an engine selector in settings (`"Live Engine"`:
`Native TRT (librediffusion)` vs `ComfyUI`) that swaps the job producer
behind the same `result_available`/`apply_result` surface:

- ComfyUI path stays untouched (fallback for arbitrary LoRA stacks,
  ControlNet graphs, non-sm_120 machines).
- Native path needs a Python→C bridge into the Python plugin: prefer
  `ctypes` directly on `librediffusion.dll` (ABI-stable `__cdecl` — no
  pybind build) for prompt/mask/schedule/frame calls, or a
  `plugins/livediffusion` C++ bridge exposed through kross/kritamcp-style
  actions if we want the engine lifecycle in C++. Recommend ctypes for
  the first slice, C++ only when Tier B zero-copy lands.
- Canvas→reference handoff: `KisPaintDevice` → RGBA bytes →
  `set_reference`; result → `frame_cached` → paint device → existing
  `set_result`. Mask from Krita Selection alpha → `set_mask` RGBA
  (white=regenerate). Fixed seed 52 + cached reference for frame
  coherence, `{1.0,0.64}` 2-step default.

## Risks

| Risk | Mitigation |
| --- | --- |
| **AGPLv3 license** (was assumed MIT) | **Resolved 2026-11-15** — option (c): in-process link, combined binary is AGPLv3 (user decision, open source accepted). §13 source-offer notice needed on the `/mcp` surface. |
| **TRT major drift** — machine has TRT 10.16, upstream certifies 11.0.0.114 / `tensorrt-cu13==11.2.1.2` | Install TRT 11 SDK; bump lock pin to 11.x; engines are non-portable across TRT majors — rebuild engines on any TRT bump. |
| **CUTLASS submodule missing from zip** | Pin a release tag in our build script + `plugin-intake.md`. |
| **Engine build cost** (~30-90 min/model, ~8-14 GB builder workspace, HF downloads ~16-20 GB) | One-time local builds; script it (`tools/` recipe doc); `KLEIN_HW_COMPAT=none` on this box. |
| **Weights licensing** — Stability Community License (SD/SDXL turbo), FLUX.2-klein Apache-2.0 (verify; HF gating) | Ship scripts only, never engines/weights; auto-discover `D:\models`. |
| **VRAM budget** — klein bf16 stream ~16-18 GB co-resident with painting + ComfyUI | FP8 transformer path exists; `engine_cache_set_max_entries(0)`; `gpu-queue` guard; defer Qwen idle-unload. |
| **MSVC↔llvm-mingw** — Solstice toolchain cannot run nvcc | Build DLL under VS2022 BuildTools; consume via C API (designed for it). |
| **Host-RGBA stream API** — no device-side stream output exported | Small upstream `_frame_device` patch when Tier B needed; Tier A is already fast enough at stream resolutions. |
| **klein Windows build path** — `klein_env.sh` is bash/Linux-oriented | Translate env (PATH not LD_LIBRARY_PATH); first build on this box validates it. |
| Single-file klein weights ≠ diffusers layout | Use HF repo for export; local safetensors need repack (open question). |

## Phased implementation steps

1. **License decision** (user): external-dependency model vs upstream
   exception request. Update `integration-ledger.md` + this doc.
2. **Toolchain**: install TRT 11.0.0.114 cuda-13.2 SDK; fetch+pin CUTLASS;
   install Boost 1.86 headers; create Python 3.12 uv venv; bump
   `windows-dev.lock.json` TRT pin to 11.x (+`check-local-toolchains.py`
   probes: `TENSORRT_DIR`, cutlass dir, boost). Fix
   `devops-environments.md` "TRT 10.x" → 11.x.
3. **Build `librediffusion.dll`** under VS2022 BuildTools +
   `-DCMAKE_CUDA_ARCHITECTURES=120` (single-arch dev) → `all` for release.
4. **Engine bake day**: RIFE (minutes), SD-Turbo/SDXL-Turbo sets, then
   klein bf16 (and FP8 attempt); record engine sizes/timings in
   `wiki/benchmarks/`.
5. **ctypes smoke path** (no Krita UI): script driving
   `flux2_stream_create/set_prompt/frame` on a test image → verify
   output coherence + latency numbers.
6. **`plugins/livediffusion`** skeleton + `ai_diffusion` "Live Engine:
   Native TRT" selector; `set_reference`/`frame_cached`/`set_mask` live
   loop on the active document; ComfyUI remains the default engine.
7. Optional Tier B zero-copy (CUDA↔Vulkan NT handles) after Tier A
   latency is measured; optional Qwen idle-unload / FP8 encoder.

## Open questions

- Does BFL's `FLUX.2-klein-4B` HF repo license confirm Apache-2.0, and is
  it gated behind HF ToS acceptance (token needed for CI/offline)?
- Can local `Flux.2 Klein 4B`/`flux2-vae`/Qwen3 safetensors be repacked
  into the diffusers layout to skip the ~20 GB HF download?
- Qwen3-4B engine: bf16 (~8 GB) resident vs FP8/INT8 export — is FP8
  calibration applied to `qwen3_encoder` too, or transformer-only
  (`build_one_fp8.py` scope)?
- klein engine build on Windows — any hardcoded Linux paths in
  `train-lora.py --type klein` beyond `klein_env.sh` (verify at step 4).
- Engine/scheduler table coverage: `schedulers/*.hpp` covers sd-turbo,
  sdxl-turbo, sdxs, LCM, Segmind-VegaRT — confirm the klein stream path
  needs no table (it doesn't: explicit `set_schedule` sigmas).
- Should `librediffusion.dll` ship inside the Solstice installer as an
  optional component (user-consented download of the upstream release
  zip — CI builds `librediffusion-windows-x86_64` artifacts), or
  build-from-source only?
- `MODEL_SD_15` enum covers SD1.5/SDXS/LCM/Hyper-SD — naming only; any
  scheduler-table differences handled by the exported bundle.

## Phase 3 build-out — verified results (2026-11-16)

- `librediffusion.dll` (5.9 MB): built with MSVC 14.44 + CUDA 13.2 + TRT SDK
  11.0.0.114 + Boost 1.86 headers + CUTLASS 4.2.1; 141 `librediffusion_*`
  exports; imports `nvinfer_11.dll`, `nppig64_13.dll` + VC CRT.
- klein engine bundle (`<local>\livediffusion\engines\klein\`, ~14.2 GB):
  `transformer_bf16.plan` 7.8 GB, `qwen3_encoder_bf16.plan` 6.3 GB,
  `vae_decoder_bf16.plan` 108 MB, `vae_encoder_bf16.plan` 78 MB,
  `rife_ifnet_fp16.plan` 37 MB, `tokenizer.json` 11 MB, `bn_mean/std.bin`
  512 B each. Built `ampere_plus` (SM 8.0+ portable). Model:
  `black-forest-labs/FLUX.2-klein-4B` — **not gated, Apache-2.0**.
- Native live engine (`ai_diffusion/native_engine.py` ctypes bridge +
  `live_engine` selector + `native_engine_dir`): verified in-app — native
  frames at ~0.17 s + capture/scaling overhead; ComfyUI fallback intact.
- Caveats discovered: TRT plans are exact-version-locked (rebuilds need the
  SDK wheel + cwd=bin); the pipeline is static 320x576 (frames are scaled
  in/out); fp8 build needs calibration goldens (`KLEIN_FP8_GOLD`).
  See `native-live-engine.md` Verification/Known-constraints.
- Remaining: `bundle.json` manifest write, dynamic-geometry engines, fp8,
  installer bundling decision, `/mcp` source-availability notice (AGPL §13).
