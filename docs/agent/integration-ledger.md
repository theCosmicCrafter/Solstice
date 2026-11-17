# Integration ledger — agent reference

Rolling tally of every external capability, codebase, and asset evaluated
for integration into Solstice under the rendering & art-engine megaplan
(`rendering-art-engine-megaplan.md`). Seeded 2026-11-15 from the source
production plan (`solstice_rendering_and_art_engine_production_plan.md`,
external planning export dated 2026-11-15).

Mirrors the `upstream-intake.md` / `plugin-intake.md` convention: one row
per item with a status, provenance (source + license), target location and
notes. When a status changes, edit this ledger **and** append to
`wiki/log.md`; when a phase completes, update the megaplan's status header.

## Status values

| Status | Meaning |
| --- | --- |
| Integrated | Shipped in the Solstice repo / baseline |
| Ready | Approved; needs only a mechanical step (file copy, config flip) |
| Planned | Approved for a megaplan phase (P1/P2/P3 = priority band) |
| Reference | Read-only idea/pattern mine; no code imported |
| Rejected | Decided against; reason recorded |
| Skip | Out of scope |

Priority bands map to the megaplan roadmap: **P1** -> Phase 2,
**P2** -> Phases 3-4, **P3** -> Phase 5 (see megaplan "Phased roadmap").

## Tally

| Status | Item | Category | Target / location | Notes |
| --- | --- | --- | --- | --- |
| Integrated | Backend/attention quick-win setup (ComfyUI-Easy-Install, `--use-sage-attention --highvram`, `nvvfx`, ComfyUI-TwinFlow) | AI quick wins | External runtime / config | Removed from backlog per user confirmation (source plan). |
| Integrated | `TAEF1`/`TAEF2` + WebSocket JPEG live preview (`is_live=True`) | AI quick wins | Live preview path | Removed from backlog per user confirmation. |
| Integrated | `FluxKVCache` on `flux2_4b`/`flux2_9b` | AI quick wins | `plugins/python/ai_diffusion/backend/workflow.py`, `comfy_workflow.py` | Verified wired in repo (source plan). |
| Integrated | Live resolution cap + optional `RTXVideoSuperResolution` post-upscale | AI quick wins | Live post-process | Removed from backlog per user confirmation. |
| Integrated | `NitroFusion2`/`TwinFlow` live style preset | AI quick wins | Live styles | Removed from backlog per user confirmation. |
| Integrated | Fork baseline `deb3ebd6`: Vulkan GPU engine 4.x (`KisGpu*`), `ai_diffusion` (21 arches incl. `flux3`, `qwen2`, `qwen_l`), `kritamcp`, SmartMattingTool, DepthMapFilter, ESRGANFilter, Trace to Vector Layer, `kis_svg_export`, `KisViewResVectorCompositor` (D7), AI Generative Fill, `KoSpectralMixing` (CPU slice), FramePainter (`ai_drag`), Qwen Edit, IC-Light (`harmonize-iclight`), SeedVR2 upscale, LiveWorkspace backend | Baseline | Core repo | Already shipped on `main`. |
| Rejected | External TouchDesigner `FluxRT-TD` bridge | Real-time AI | — | Replaced by direct `librediffusion` C++ port + Solstice-native live patterns (source-plan decision #2). |
| Integrated | VisionML base GGUFs: `BiRefNet-lite-F16.gguf` (84.54 MiB), `MIGAN-512-places2-F16.gguf` (14.07 MiB), `MobileSAM-F16.gguf` (25.65 MiB) | Segmentation / VisionML | Copied 2026-11-15 from `Solstice-1.1.1-windows-x64` -> `%APPDATA%\Solstice`, `%APPDATA%\krita`, `F:\krita-work\krita\_install`, `D:\krita\share\krita`, `F:\solstice-test\app-49697aad\...` (each `visionml\models\{sam,migan,birefnet}\`) | Byte sizes verified against source package. |
| Planned (P1) | Selective `Solstice-1.1.1` foundation rebase: GPU Engine `1.0.0` passes, Blackwell auto-start, Clipping Masks, Through-Layer Eraser, Remove Dust, `solsticeeffects` + G'MIC, Layer Comps + Fast Export, QuickAccess gestures | Foundation / core engine | `libs/`, `plugins/`, `krita/` | Preserve 19 `libs/` + 15 `plugins/` fork-only files + `plugins/python/`; see megaplan Phase 2. |
| Planned (P1) | Unified built-in `/mcp` server: merge upstream C++ `TestControlMcpServer` (26 tools) + fork `kritamcp` (63 `ai_*`/viewport tools) + `showa_*`/`blockout_*`/`capture_at` | Built-in MCP / automation | `plugins/extensions/testcontrol` + `plugins/python/kritamcp` | Eliminates external `krita-mcp/server.py` sidecar; see `mcp-bridge.md`. |
| Integrated | `ai_diffusion` segmentation/layering gates: `matte_output`/`vectorize_output` outside `generate` — **done 2026-11-15** (all kinds except `control_image`); `supports_alpha` expansion — **done** (`is_flux2` + qwen2/qwen_l); `qwen_l` `generate()` latent layers — **done** (`setup_latent_layers`/`pack_latent_layers` parity + `[Layered]` group wrap in `apply_generated_result`); Lightning 4/8-step presets — **done** (LoRA filenames verified on `D:\models`); ComfyUI 4-ch `sd.py` Flux VAE — **applied 2026-11-15** to `D:\krita\ComfyUI\comfy\sd.py` (backup `sd.py.bak-rgba-vae`, compiles); patch copy kept at `docs/agent/comfyui-rgba-vae.patch` | Segmentation & layering | `plugins/python/ai_diffusion/` (`workflow.py`, `model.py`, `resources.py`); `D:\krita\ComfyUI\comfy\sd.py` | Unblocks the existing `D:\models` stack; GUI/e2e verification pending. Open: true I2L decomposition needs `reference_latent`/`text_encode_qwen_image_edit` for qwen_l (upstream ships the same gap). |
| In progress (P1) | `librediffusion` real-time backend: `FLUX.2-klein-4B` streaming, token-grid inpaint mask, `SDXL`/`SD Turbo`, `RIFE` | Real-time canvas | **AGPLv3 — accepted (user decision 2026-11-15):** in-process link, combined binary is AGPLv3; source `C:\Users\richk\Downloads\librediffusion-main.zip` | **Build done 2026-11-16:** `librediffusion.dll` 5.9 MB + `clip_tokenizer_c.dll` under VS2022 + CUDA 13.2 + TRT 11.0.0.114 + Boost 1.86 + CUTLASS 4.2.1, sm_120; ctypes smoke passes (`version()` = 1.0.0, 141 exports). `ai_diffusion` seam landed: `native_engine.py` + `live_engine`/`native_engine_dir` settings + `LiveWorkspace` branch (`docs/agent/native-live-engine.md`). RIFE plan built (37 MB); klein weights downloading. Obligations: ship upstream `LICENSE` with vendored code, mark modifications, §13 source-availability notice on the `/mcp` surface. |
| Planned (P2) | `SAM 3.1 Multiplex` -> Krita Region auto-decomposition + `BiRefNet_toonout`/`BEN2` refinement + `craft-ai-integrations` (`ai-engine-rten` CPU ONNX SAM2.1/BEN2/ViTMatte + `ai-engine-candle` SAM3) | Segmentation & regions | `plugins/visionml`, `plugins/python/ai_diffusion`; source `D:\krita\ComfyUI\comfy_extras\nodes_sam3.py`, `storytold-artcraft/craft-ai-integrations` | Static C-FFI CPU fallback (zero PyTorch) + `D:\models` GPU stack. |
| Planned (P2) | `Qwen-Image-Layered` Layer Studio + NightShift Prompt/Skill/Blockout pack (44 skills + 137 vault skills, node-DAG `@image` composer, `blockout.py` pinhole 3D staging, `gpu-queue` VRAM guard, starter + local `D:\wildcards`/`Poses` discovery) + `Stimma` local voice-to-prompt & PNG EXIF provenance | AI studio / MCP / staging | `plugins/python/ai_diffusion/ui/generation.py`, new docker(s) | 100% built-in. **Local asset discovery landed early 2026-11-15** (`local_assets.py`, `__wildcard__` expansion in `text.py`, `docs/agent/local-assets.md`; live scan: 2,325 wildcards / 16,871 poses / 10,386 LUTs). Remainder P2. |
| Planned (P2) | `ComfyUI-MPEG-Diffusion` 2K dual-res saliency & phase-aligned seam merger for Live->2K idle refinement | Upscale / local detail | Live pipeline; source `C:\Users\richk\Downloads\ComfyUI_MPEG_Diffusion_Suite` | Raised-cosine Hann blend + transition variance lock. |
| Planned (P2) | `VectorCraft` static C-FFI crates: `vectorcraft-trace` (multi-color/centerline/gradient), `vectorcraft-pathops` (cubic-Bezier booleans + `live_paint`/`shape_builder`), `craft-affinity` `.af` reader; CLIP STUDIO-style line-width pressure, perspective rulers, comic panels/balloons | Vector / tracing / formats | New native module(s); source `storytold-artcraft/vectorcraft` (`MIT OR Apache-2.0`) | Upgrades Trace to Vector Layer, comic flatting, `.af` import natively — no `vectorcraft-cli` sidecar. |
| Planned (P2) | Built-in `PureRef` pinboard docker + floating click-through ghost tracing overlay (`WS_EX_TRANSPARENT`, `Alt+G` value check, `Shift+H`/`V` flip, crop, `S` color pick, native SQLite `.pur` importer) + Eagle-style hex/tag Asset Vault docker | Reference / asset vault | New dockers (clean-room C++/Qt6) | `.pur` compat scans `E:\AI_Art\PureRefBoards\`; replaces external `PureRef.exe` workflow. |
| Planned (P2-P3) | `LookFX` + `Flarecore` 100% native Vulkan: 17 halftone screens, 21 paper substrates, 37 print presets, 41 flare presets/182 elements, capillary bleed, CMYK 4-plate separation; `SHOWA` 7-stage analog/CRT (9 presets, deterministic `L=height/480`) | Tactile print / analog FX | `KisFilter`/`KisGeneratorLayer` + bundled KTX2 assets; sources `F:\lookfx`, `F:\AIPrograms\_Dev_Projects\flarecore` (`Apache-2.0`), `showa/engine.py` | Supersedes `phase11-implementation-plan.md` §11.5 external worker. |
| Planned (P3) | Art-engine viewport, input & GPU compute: `CAS`/`NIS`, `SMAA`, `photocraft-tablet` `WM_POINTER*`/predictive spline, `EffectCraft` `inpaint.wgsl` + `fx_lut.wgsl`, `lightcraft` `bm3d.wgsl`/`nn_conv`, `KoSpectralMixing` GPU compute, `Rebelle`-style wet/impasto, `Sentinel` `datamosh_pro`/`dither_mosh`/`blend2d`/`nvngx_vsr` + in-app shader watchdog, `moshdither-studio` dithering/pixel-sort | Art engine / rendering | `libs/gpu`, viewport, filters; sources `storytold-artcraft/photocraft|effectcraft|lightcraft`, `moshdither-studio` (`MIT`) | 100% native GLSL/Vulkan + C++; `Optics`-style halation/deband, `Luminar`-style depth portrait relight. |
| Planned (P3) | Solstice Look Engine (`filmcraft-scopes` RGB Parade/Vectorscope/Waveform/Histogram + 2D LUT atlas strip + live split wipe + `D:\models\LUTs` 10,383 `.cube` auto-scan); `Lattice` (`panel_set` HUD, multi-aspect batch export, `LatticeSharedImage` NT interop, WGSL stdlib `solver`/`rig`); `anicraft`/`filmcraft`/`Lottie`/`GainMap`/`JXL`/`PSD`, `materialcraft`/`texturecraft` UV surface painting, `spark` GLSL | Grading / HUD / anim / 3D | New dockers/viewport passes; sources `storytold-artcraft/*` (`MIT OR Apache-2.0`, `spark` `MIT`) | Clean-room Shader-Art-Studio & `.milk` preset concepts. |
| Reference | `FigCraft` (`boolean.rs` cache, `backdrop.rs`, `.fig`), `DesignCraft` (CMYK spot-ink PDF, Knuth-Plass text), `soundcraft` (DSP/LUFS), `cadcraft`, `font-preview-service`, `artcraft.exe` pre-gen validation UX, `LUTs-Manager-Testing` UX | Patterns only | — | `LUTs-Manager-Testing` is xFxDesigns proprietary — zero code reuse. |
| Skip | `Wicked Engine` (embed), `PixelAlchemist`/`glitchVids`/`S4W-Scanlines` (code), `Tiny-Consequences`, `Night Shift Setup` installer (absorbed natively), `Paradoxal Creamation 5.1.7z`, `PdfCraft` (cos/forms/OCR + no CLI), `deckcraft`, `artcraft`/`artcraftx` wrappers, `craft-anything`, external `TouchDesigner`/`SynthEyes`/`Resolve`/`FreeCAD`/`UE`/`Unity`/`Topaz`/`Rebelle` binaries | Out of scope | — | Keep Solstice lean and 100% self-contained (source plan §4/§7B/§7C/§7E). `glitchVids` remains optional CC BY 4.0 test media only. |

## Provenance entries

Detail records for items whose code or assets will be vendored/built in.
Format follows `plugin-intake.md`: source, pin, license, integration path,
notes.

### librediffusion

- Source: `C:\Users\richk\Downloads\librediffusion-main.zip` (upstream
  `LibreDiffusion` project)
- Pinned: zip snapshot `main`, acquired 2026-11 (record exact sha when
  vendored into the repo)
- License: **AGPLv3 — correction 2026-11-15** (source plan claimed MIT;
  verified by intake audit, `docs/agent/librediffusion-intake.md`).
  Linking it in-tree makes the combined Solstice binary AGPLv3 — **user
  accepted this 2026-11-15** ("perfectly fine sharing source code; open
  source anyway"). GPLv3 §13 explicitly permits the combination; AGPL
  terms govern the combined work. Obligations: ship upstream `LICENSE`
  with vendored code, mark modifications, add a source-availability
  notice covering the `/mcp` HTTP surface (§13 network clause).
- Integration: C API `librediffusion_c.h` (1388 lines, all claims
  verified: `flux2_stream_set_prompt` Qwen3 cache, `set_reference`/
  `frame_cached` VAE-encode split, `set_mask` RGBA->token-grid delta,
  `set_schedule` FlowMatch sigmas, `rife_interpolate_gpu_sized`,
  seed-52 fixed noise, `sm_120` in `CMAKE_CUDA_ARCHITECTURES`).
- Build blockers found by intake: requires **TensorRT 11** (machine has
  10.16.1.11 — engines are TRT-major-locked), `3rdparty/cutlass`
  submodule ships empty in the zip, FLUX.2-klein weights need HF
  repo-layout repack. llvm-mingw can't host nvcc — build the DLL under
  MSVC 14.44 (present) and consume via the C API.
- Toolchain gate: `build-tools/toolchains/windows-dev.lock.json` (Phase 3)

### Solstice-1.1.1 upstream snapshot

- Source: `C:\Users\richk\Downloads\Solstice-1.1.1` (source tree `ac468d9`)
  + `Solstice-1.1.1-windows-x64-ac468d986539` (built package)
- Pinned: `ac468d9`
- License: GPL-3.0 (Krita-derived, same as Solstice)
- **Corrected understanding (inventory audit 2026-11-15,
  `foundation-rebase-inventory.md`):** this is a *newer release of this
  same project*, not upstream Krita — the merge is divergence-repair, not
  an upstream sync. Verified counts: ~13,700 differing files but only
  **426 real content diffs** (~97% CRLF-vs-LF noise); direction split 123
  upstream-superset (safe absorb) / 30 fork-superset (keep) / 273
  both-modified. **No merge base exists** (`ac468d9` and claimed
  fork-point `e88c2289` absent from the object store) — ~426 manual hunk
  merges, no true 3-way possible. Corrected claims: 203 upstream-only
  plugins (not 194); `testcontrol` is **27** tools (not 26); `visionml`
  exists upstream (12 files fork-only, all 2,579 `thirdparty/vision.cpp`
  diffs EOL-only); `plugins/python` is shared, not fork-only. "Blackwell
  auto-start" is actually upstream's `Convert16` RGBA16F policy +
  `unverifiedDevice*` opt-in. 46 uncommitted-dirty files collide directly
  with upstream diffs — those are manual merges against in-flight work.
- Integration: selective merge per the inventory's verified merge order —
  EOL normalization first, then zero-conflict absorbs, libs/image+gpu,
  libs/ui hubs, plugins in dependency order, plugins/python, packaging,
  `/mcp` reconcile. Hard gates: `KisDustDetector` before
  solsticeeffects/remove-dust; `KisClipStackLayer` before clip actions;
  `KisThroughLayerEraserTargets` before the tool; GPU passes before GPU
  color filters; testcontrol is Qt6-gated.
- Assets: 3 VisionML GGUFs copy from the built package (Integrated row
  above).

### ComfyUI-MPEG-Diffusion

- Source: `C:\Users\richk\Downloads\ComfyUI_MPEG_Diffusion_Suite` (paper
  `2611.12069v1.pdf` + 4 nodes: `MPEGSaliencyProbe`,
  `MPEGDualResAttentionPatcher`, `MPEGTokenUpscaler`, `MPEGSeamMerger`)
- License: record at vendor time (suite ships no LICENSE — verify before
  any code port; paper is arXiv)
- Integration: idle 2K dual-res refinement of live canvas/base latent;
  Hann blend + transition-variance phase lock.

### ArtCraft craft crates (`storytold/*`)

- Source: `C:\Users\richk\CascadeProjects\storytold-artcraft\` (25 cloned
  repos, `--depth 1`, upstream `https://github.com/storytold/*`)
- License: `MIT OR Apache-2.0` (GPLv3-compatible; `spark` MIT).
  **Brand assets under `docs/brand/` must NOT be reused.**
- Integration: static C-FFI (`cbindgen`) or clean-room WGSL->GLSL/Vulkan
  ports only — no `egui` UIs, no `<name>-cli.exe` sidecars.
- Adopted crates: `vectorcraft` (`trace`, `pathops`/`linesweeper`,
  `affinity`), `effectcraft` (`gpu/shaders` 32 `.wgsl`, `sam2`, `segment`,
  `lottie`, `psd`, `track`), `photocraft` (`photocraft-tablet`,
  `camera_raw.wgsl`), `lightcraft` (`bm3d_*.wgsl`, `nn_*.wgsl`), `anicraft`
  (`anicraft-gpu` binning, `craft-xfl`), `materialcraft` + `texturecraft`
  (DAG materials, UV G-buffer paint, `bake.wgsl`, `trace.wgsl`),
  `filmcraft` (`filmcraft-scopes`, `lut.wgsl`, edit algebra),
  `craft-ai-integrations` (`ai-engine-rten`, `ai-engine-candle`),
  `craft-libs` (`craft-gainmap`, `craft-jxl`, `craft-psd`/`kra`/`ora`,
  `craft-lottie`, `craft-materialx`, `craft-cms`), `soundcraft`, `spark`,
  `cadcraft`, `font-preview-service`.
- Reference-only: `figcraft` (`boolean.rs`, `backdrop.rs`), `designcraft`
  (`gradient_inks.rs`, Knuth-Plass), `pdfcraft` (skip core & CLI).
- Skip: `deckcraft`, `artcraft`/`artcraftx`, `craft-anything`,
  `craft-extensions`, `craft-skills`, `FineTrainers-Conditioning`,
  `indd-utils` (keep only `artcraft.exe` pre-gen validation UX idea).

### NightShift desktop pack

- Source: `C:\Users\richk\AppData\Local\Programs\night-shift-desktop\`
  (Night Shift Desktop v2.7.11 resources)
- Components: `showa-studio/showa/engine.py` (7-stage analog pipeline, 9
  presets, deterministic per `(seed, frame_index, stage)`),
  `vault-seed/.claude/skills/camera-blockout/scripts/blockout.py`
  (pinhole 3D staging), 44 `vault-seed/.claude/skills/` + 137 vault skill
  dirs, `app/server/NODES.md` (node-DAG schema), `gpu-queue` VRAM guard.
- License: local install only — clean-room port of behavior; do not copy
  branding.
- Integration: native `KisFilter`/Vulkan shader + staging docker +
  `/mcp` actions; external `Night Shift Setup 2.5.0.exe` installer is
  superseded (never required).

### LookFX + Flarecore

- Source: `F:\lookfx` (`src/cmykmagic`, ~2.4k LOC, 144 files: 17 halftone
  screens, 21 paper substrates, 37 print presets, Ben-Day screening,
  capillary bleed, CMYK 4-plate separation);
  `F:\AIPrograms\_Dev_Projects\flarecore` v0.1.2b1 (41 flare presets,
  182 element textures, depth occlusion, linear-space render)
- License: `Apache-2.0` (both)
- Integration: Vulkan compute `KisFilter`/`KisGeneratorLayer` + bundled
  KTX2 assets. Supersedes `phase11-implementation-plan.md` §11.5's
  external-worker tier.

### Sentinel patterns

- Source: `C:\Program Files\OODLabs\Sentinel\` v0.6.15 (`AGENTS.md`,
  31 shader files)
- License: pattern mine only — port `datamosh_pro`, `dither_mosh`,
  `blend2d` fallback, `nvngx_vsr`, `LaserTraceGpu` profiling, 240 Hz
  decoupled parameter clock, LFO/WASAPI envelopes, `capture_at`
  (`combo_mode: "cartesian"`) parameter sweep, `shader_worker` TDR
  watchdog sandbox. Pair dithering with `moshdither-studio` (MIT).
- Integration: clean-room GLSL/Vulkan; no Sentinel binaries.

### Proprietary zero-copy list

Read-only UX/concept mines — **no code, assets, or brand may be copied**:
`PureRef` (.pur format is reverse-engineered for import compat only),
`Rebelle 8` (wet/impasto physics concepts), `CLIP STUDIO` (rulers,
line-width, panels/balloons concepts), `BorisFX Optics 2025` (halation/
mist/deband concepts), `Luminar Neo`/`Colourlab Ai Pro`/`Topaz`
(depth relight, hero-shot match, diagnostics concepts), `Stimma`
(speech-to-prompt + EXIF provenance UX), `Eagle` (hex/tag vault UX),
`artcraft.exe` (pre-gen validation gate UX), `LUTs-Manager-Testing`
(xFxDesigns), `Shader-Art-Studio`/`projectMilkdrop .milk` (preset
concepts only).

## Build milestone — 2026-11-15

Solstice-1.1.1 selective merge **compiles end-to-end** (`F:\krita-work\solstice\_build`, Ninja/LLVM-MinGW, Qt5): 408 absorbed + 273 merged files, ~30 fork-superset keeps. Fixes applied during triage: Qt6→Qt5 API adaptations (QHash iterators, QMouseEvent::position, QComboBox/QCheckBox signal overloads, QKeyCombination guard), restored fork exports/includes (`KRITAUI_EXPORT KisRemoteFileFetcher`, `KoPathShape` fwd-decl, kis_layer_manager includes), `HAVE_KRITA_GPU_ENGINE` guards (Vulkan SDK absent → kritagpu not built this pass), `cmake/modules/Qt5ResourceCompat.cmake` (Qt6 resource-form shim). `cmake --install` complete → `F:\krita-work\solstice\_install`; `solstice.com --help` exits 0. Triage detail: `F:\krita-work\solstice	riage-log.md`.

## Phase 3 — librediffusion native engine (2026-11-16) — FUNCTIONAL

- Vendored AGPLv3 librediffusion → `librediffusion.dll` (TRT 11.0.0.114,
  CUDA 13.2, MSVC 14.44, Boost 1.86, CUTLASS 4.2.1).
- FLUX.2-klein-4B (Apache-2.0, ungated) → ONNX → TRT plans (bf16, ampere_plus
  portable) + RIFE plan; staged as the node-ready bundle at
  `%LOCALAPPDATA%\Solstice\livediffusion\engines\klein\`.
- `ai_diffusion/native_engine.py`: ctypes seam into LiveWorkspace — engine
  selector (`live_engine: native|comfyui`), bundle discovery, RGBA
  capture/mask/reference, `JobKind.live_preview` jobs through the normal
  JobQueue → `set_result`/`apply_result` unchanged.
- Verified: stream smoke (0.17 s/frame), in-app native live result + apply,
  bundle-missing → ComfyUI fallback with surfaced notice.
- Done since: dynamic-geometry profiles rebuilt (Th 18-48, Tw 16-48,
  `Limg=2*Th*Tw <= 4608` → `Th*Tw <= 2304`; verified 320x576/512x512/
  768x384 raw + 768x768/640x480 in-app); RIFE plan rebuilt under matched
  TRT + wired (`native_rife_exp` 0-3 → Off/2x/4x/8x, intermediates via
  `live.set_result`); `bundle.json` manifest written with the
  `dynamic_geometry` block; DevSec review (security-auditor + gitleaks +
  semgrep) → all findings fixed (`frame_cached` `.raw` temp-buffer bug,
  stream-handle destroy race via `threading.Lock`, `export_layers` prefix
  traversal, `for_write` existence check, `D:`-path fingerprints → env
  vars/empty defaults, `set_prompt` mandatory-before-frame).
- Deferred: fp8 (needs `KLEIN_FP8_GOLD` goldens), zero-copy display.
