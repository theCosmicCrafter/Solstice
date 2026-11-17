# OpenFill intake — survey and integration plan

Status: items 1–3 shipped (2026-10-26); items 4–8 remain a menu. Survey of
[beckettharriman/openfill](https://github.com/beckettharriman/openfill)
(Apache-2.0) following the Phase 7.9 "borrowed UX" precedent: evaluate what
Solstice can take, what it already has, and what to skip. Nothing is vendored —
OpenFill's engine duplicates the vendored `ai_diffusion` backend, and its
editor adapters target GIMP 3 and PhotoCraft, not Krita.

OpenFill is a generative fill / object removal / canvas expand / text-to-image
front-end that runs fully local through ComfyUI. Two editor front-ends share
one engine (`gimp-openfill/openfill/`): a GIMP 3 plug-in and a PhotoCraft
companion process driving that editor over a JSON-lines control channel. Its
design doc credits `Acly/krita-ai-diffusion` as the reference for the graph
shapes and server management — the same plugin Solstice vendors.

## Verified against the vendored tree — already have

Checked in `plugins/python/ai_diffusion/` (post-1.53 + enhancements, merge
`f628900`):

- **Per-mode / per-arch prefill.** `backend/workflow.py::detect_inpaint`
  already picks fill modes the way OpenFill's per-tier table does, with more
  modes: fill → `FillMode.blur`, expand → `FillMode.border`
  (navier-stokes + blur — OpenFill's outpaint prefill), add_object →
  `FillMode.neutral` (OpenFill's "grey hole"), **remove_object →
  `FillMode.inpaint` (dedicated LaMa/MAT inpaint model — OpenFill's removal
  path)**, replace_background → `FillMode.replace`, edit-reference on
  flux2_4b/qwen2 → `FillMode.green`. `use_inpaint_model` is strength/arch
  aware.
- **Multi-pass inpaint.** `ScaledExtent` initial-context generation → crop to
  masked region → upscale → refine → `color_match` → composite
  (`workflow.py:1011-1155`). This supersedes OpenFill's single-context-window
  approach; their "expand strip-context" fix exists only because they send
  the whole canvas in one pass.
- **Managed ComfyUI.** Upstream first-run flow installs ComfyUI
  (7.6, verify pending); Solstice default points at the user's ComfyUI
  Desktop on `127.0.0.1:8189`.
- **Model metadata + browser.** Phase 7.1 `modelspec.py` safetensors
  parsing + `ModelMetadataCache`; 7.3 CivitAI hash lookup + card browser.
- **Agent bridge.** kritamcp 44 actions incl. `trigger_action`,
  `apply_filter`, selections, layers; e2e suite `tests/e2e` (green).
- **Upload path.** Images reach ComfyUI via `PUT api/etn/image/<id>` —
  Acly's external-tooling endpoint, not `/upload/image`. The PyAV
  format-probe bug OpenFill works around with a 2048-byte `tEXt` pad does
  not apply to this path.

## Worth taking (ranked)

### 1. Parameterized generation over the bridge — the real gap — SHIPPED

Implemented as the `ai_*` action family in `kritamcp/__init__.py`:
`ai_status`, `ai_styles`, `ai_generate` (modes auto/fill/remove/expand/
create on `root.model_for_active_document()` → `DocumentModel.generate()`),
`ai_expand` (canvas grow + select-all + outpaint in one call), `ai_jobs`,
`ai_apply` (apply any result as a layer — the variations-keeper step),
`ai_cancel`. Async enqueue: `ai_generate` returns `jobs_from_index` and the
caller polls `ai_jobs`; `apply:true` (default) auto-applies result 0 on
`job_finished` unless the user's finished-action already does. Mutated
model props are restored after enqueue; the prompt stays for transparency.
Docs: `docs/agent/mcp-bridge.md` §AI generation. E2E: `test_ai_status`,
`test_ai_generate_validation`, `test_ai_expand_grows_canvas`,
`test_ai_generate_fill_then_apply` (slow, skips when ComfyUI is
disconnected) in `tests/e2e/test_solstice_e2e.py`.

OpenFill's PhotoCraft companion proves an external driver can do
"select → prompt → masked layer" end to end. Solstice's equivalent channel
(kritamcp) can `trigger_action` on `ai_diffusion_generate` but cannot pass
prompt/parameters — an agent can only fire whatever the docker holds.

Work item: new kritamcp command (e.g. `ai_generate`) that calls into the
in-process `ai_diffusion` plugin — `prompt`, `mode` (fill | remove |
expand | txt2img), `style`, `strength` — and enqueues a job on the same
ComfyUI session the docker uses. Honors the Phase 3 rule (jobs queue on
the shared ComfyUI only — never a second inference path, never a second
server). Implementation: thin adapter in `plugins/python/kritamcp/` that
imports the sibling plugin's job-creation entry point (the same call the
Generate action reaches) — keep it behind a lazy import + a clear error
when `ai_diffusion` is disabled. E2E: new `tests/e2e` case — select rect →
`ai_generate` fill → assert a new layer lands, on a real build with
ComfyUI on `:8189`.

### 2. License metadata on model/workload entries — SHIPPED

`ModelResource` gained optional `license` / `min_vram_gb` fields
(`backend/resources.py`, parsed + serialized in `from_dict`/`as_dict`).
`presets/models.json` annotated for the legally significant set:
non-commercial flags on FLUX dev/Kontext checkpoints, the Flux VAE and
MAT inpaint; `apache-2.0` on Flux 2 klein, Z-Image, T5-XXL, Qwen 3,
HAT upscalers, IP-Adapters; `mit` on Clip-L; `openrail-m`/`openrail++`
on the SD1.5/SDXL bases. License renders as `name · license` in the
Workloads/package lists (`ui/server.py::_package_name`). Tests:
`tests/test_model_metadata.py`.

OpenFill's `models/catalog.json` carries per-model license labels and
flags non-commercial weights (FLUX.1 dev, MAT). `presets/models.json` has
no license field. Add `license:` (SPDX-ish string) per workload/package,
surface a badge/footnote in `ui/model_browser.py` and the Workloads tab.
Natural extension of the Phase 7 metadata direction.

### 3. VRAM-fit filtering in the Workloads tab — SHIPPED

`min_vram_gb` on checkpoint entries uses the same floors the plugin's
workload cards already recommend (SD1.5 4, SDXL/Illustrious 6, FLUX 10,
Flux 2 klein 8, Z-Image 12). `PackageGroupWidget` gained a `device_vram`
property fed from the connected client's `device_info.vram`
(`CustomPackageTab._device_vram_gb`); items over budget are disabled
with a "Needs N GB VRAM" status — never hidden, like the workload gate.
When no client is connected no filtering happens (nothing to compare).

OpenFill gates its catalog on detected `min_vram_gb`. Solstice's workload
checkboxes are arch-grouped but not hardware-gated. Add `min_vram_gb` to
the package metadata, detect adapter VRAM at settings time, grey out
entries that don't fit with a "needs ≥ N GB VRAM" tooltip — still
selectable, never hidden. Files: `presets/models.json` schema,
`backend/resources.py`/`backend/server.py` package model, `ui/server.py`
workloads list.

### 4. catalog.json as data input (not a loader)

Mine OpenFill's `models/catalog.json` for entries missing from
`presets/models.json` — DreamShaper 8 Inpainting (2 GB SD1.5 fill for
low-VRAM users), RORem (SDXL eraser), FLUX.1 Fill removal LoRA — plus its
`min_vram_gb`/license values feeding items 2–3. Copy values, not the
loader; record the data lift in `plugin-intake.md` and add the
attribution line to `LICENSES/` / `plugins/python/README-vendored.md`.

### 5. Idle-timeout for the managed server (optional)

OpenFill stops ComfyUI after N idle minutes. The vendored managed server
lives as long as the app; Solstice's default is an external `:8189`
server anyway. Small optional setting if managed mode gets promoted.

### 6. Variations picker (optional, low)

OpenFill's batch → thumbnails → Keep / Keep all / Discard modal.
`CompareDialog` + history (7.9) cover most of it; could be a
pick-N-of-batch flow on batch completion.

### 7. Fill-quality evidence matrix (optional, process)

OpenFill's `docs/evidence/` ablation reports (model × scenario matrix
with recorded outputs) are a good QA pattern. A `tests/e2e` fill matrix
(mode × arch on fixtures, assert layer stats) would give regression
coverage for the inpaint graph the way `tests/e2e` covers the bridge.

### 8. AMD awareness in the managed installer (scope-dependent)

OpenFill detects AMD GPUs, downloads ComfyUI's AMD build, and marks GGUF
models reported to give noise on Radeon. The managed-install path is
verified on NVIDIA Blackwell only (7.7b). If AMD users are in scope: GPU
vendor detect in the install flow + ROCm-vs-CPU guidance + GGUF warning
flags on affected packages. Otherwise document "managed install is
NVIDIA-first; AMD users use external server" in `docs/`.

## Skip — recorded for the next survey

- **OpenFill's engine** (`app.py`, `workflow.py`, `geometry.py`,
  `backends/`, `server.py`, `supervisor.py`) — duplicates
  `ai_diffusion/backend`; its supervisor solves a GIMP short-lived-plugin
  problem Krita plugins don't have (long-lived, single-instance app →
  watcher-pids and start locks are moot).
- **GIMP adapter and PhotoCraft companion** — wrong editors; kritamcp is
  the Solstice-side channel.
- **A1111 backend** — ComfyUI-only is the settled decision (Phase 3 +
  megaplan "backend swap off ComfyUI" deferred).
- **`task_lama` in visionml** — vision.cpp has no LaMa arch
  (`src/visp/arch/`: birefnet, depth-anything, dino, esrgan, migan,
  mobile-sam, swin); porting FFC blocks is a large lift and the ComfyUI
  path already provides `big-lama.pt`. MI-GAN covers the native
  smart-fill slot.
- **PNG `tEXt` padding** — etn upload path unaffected (see above).
- **Second ComfyUI manager** — violates the shared-GPU-queue rule; the
  external `:8189` server is the default anyway.

## Conventions followed

- Ideas land inside the vendored `ai_diffusion` and `kritamcp` — no new
  plugin, no vendored code, matching the 7.9 Pixaroma/Lora-Manager
  outcome. Record `ai_diffusion` and `kritamcp` patch lines in
  `plugin-intake.md`; keep them on the `enhancements` branch so the 7.8
  re-vendor merge survives.
- User-visible changes get `docs/` + `docs/agent/` updates in the same
  change; new e2e coverage goes in `tests/e2e/` per `e2e-testing.md`.
- OpenFill is Apache-2.0 — compatible with GPL-3.0 Solstice; if any code
  or the catalog data is copied, keep the attribution.

## Suggested order

1. kritamcp `ai_generate` (item 1) — the only capability neither
   `ai_diffusion` nor `visionml` can do today.
2. License metadata + VRAM filter (items 2–4) — one pass over
   `presets/models.json` schema covers all three.
3. Optional items (5–8) as scope allows.

DoD per item: feature visible in a real build, e2e or unit test where it
fits the existing suites, intake ledger updated, megaplan phase marked
implemented.
