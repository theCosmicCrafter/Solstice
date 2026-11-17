# Plugin integration megaplan — agent reference

Status: approved (2026-10-14), **phases 0–4 complete; phase 5 built, GUI
check pending; phase 6 pending** (audit 2026-10-15).
Roadmap for shipping a custom Solstice build with selected third-party
Krita plugins integrated — vendored Python plugins, a bundled MCP agent
bridge, and one native port (Smart Select) into `plugins/visionml`.

Decisions (user, 2026-10-14): mixed integration depth (per-plugin choice),
agent/MCP bridge is core scope, end state is a buildable fork, Smart Select
gets a full native port, builds run via GitHub Actions on this repository,
this document is the canonical plan.

**Where this plan stands:** every plugin is vendored and loading clean on
a real build (2026-10-15 verification — zero startup tracebacks, incl.
the two PyQt6 fixes recorded in `plugin-intake.md`); the kritamcp bridge
is hardened with 44 actions and covered by `tests/e2e/` (27 tests, all green);
Smart Matting ships native in `plugins/visionml`. Remaining: Phase 6
packaging/release and the GUI verification pass. Follow-on work moved to
`vector-quality-megaplan.md`.

See also: `stabilization-plan.md` (2026-10-15 audit - defect list and revised open items; wins on conflicts until merged back).

## Anchor: what Solstice already provides

- `plugins/visionml` — native vision.cpp runtime (CPU or Vulkan) with
  MobileSAM segment-selection tools, MI-GAN Smart Fill and BiRefNet
  background removal. Extra GGUF models drop into
  `resources/visionml/models/{sam,migan,birefnet}` under the resource folder.
- `plugins/python/` — bundled pykrita plugins registered through
  `install_pykrita_plugin(<name>)` in `plugins/python/CMakeLists.txt`;
  installed to `share/krita/pykrita` with `kritapykrita_<name>.desktop`.
- `plugins/extensions/pykrita` — intact Python plugin framework, PyQt6.
- `.github/workflows/windows-build.yml` — self-contained `workflow_dispatch`
  build; produces unsigned x64 ZIP artifacts (7-day retention).
- Precedent for Python-to-native migration: Quick Access, Rest Note,
  Asset Library, Vision ML.

## Evaluation of the candidate plugins

| Plugin                              | Verdict                                                         | Notes                                                                                                                                                                                                                                                      |
| ----------------------------------- | --------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Krita AI Diffusion (Acly)           | Integrate — vendor latest upstream, re-apply local enhancements | Flagship. Upstream is already PyQt6. Local enhanced copy is PyQt5-era; extract still-missing enhancements (subfolder model detection, quantization display, metadata cache) onto upstream >= 1.52. Connect to existing ComfyUI at 127.0.0.1:8189. GPL-3.0. |
| MCP bridge                          | Integrate — nanayax3/krita-mcp base + bearer auth               | nanayax3/krita-mcp based in-app HTTP bridge (44 POST actions + GET /health, /info, bearer token, MIT). krita-cli remains a compatible external client; codex-mcp stays a documented minimal alternative.                                                   |
| Krita Smart Select (Windows port)   | Integrate — native port into `plugins/visionml`                 | Own project; lasso -> BiRefNet soft-alpha matting is the one capability native visionml lacks. Native tool + GGUF inference removes the PyTorch worker.                                                                                                    |
| Shortcut Composer                   | Integrate — vendor + PyQt6 patch                                | Own pie-menu/cursor-tracker widgets; unaffected by removed popup palette. GPL-3.0.                                                                                                                                                                         |
| Pigment.O                           | Integrate — vendor + PyQt6 patch                                | Largest PyQt surface; stress test for the compat layer. `mask_set` kritarc key already handled by the settings-import rewrite list. GPL-3.0.                                                                                                               |
| Imagine Board                       | Integrate — vendor + PyQt6 patch                                | PureRef-style board + folder browser + LUT analyzer. GPL-3.0.                                                                                                                                                                                              |
| BuliNotes + BuliBrushSwitch         | Integrate — vendor + PyQt6 patch                                | Active on Codeberg (Grum999). GPL-3.0.                                                                                                                                                                                                                     |
| Timer Watch                         | Integrate — vendor + PyQt6 patch (pilot)                        | Correct attribution: `EyeOdin/Timer_Watch`. Smallest plugin; Phase 1 pilot. GPL-3.0.                                                                                                                                                                       |
| Blender Layer                       | Integrate — vendor, disabled by default                         | Streams Blender viewport incl. depth/normal passes; companion Blender addon ships inside the plugin dir. GPL-3.0.                                                                                                                                          |
| Rebecca's Krita Tools               | Optional — cherry-pick scripts                                  | Crop-to-Margin and recorder-sequence reorder only; MIT.                                                                                                                                                                                                    |
| Krita Vision Tools                  | Skip                                                            | Already natively ported (`plugins/visionml`).                                                                                                                                                                                                              |
| Smart Segments                      | Defer — fold SAM2 into visionml                                 | Linux-first, 5-6 GB PyTorch venv. Prefer a SAM2 GGUF encoder in `visionml/models/sam/` if vision.cpp supports it.                                                                                                                                          |
| Krita-AIColorization                | Skip                                                            | Stale, heavy torch deps, Drive-hosted checkpoints; superseded by ComfyUI workflows.                                                                                                                                                                        |
| Theme Creator / Subwindow Organizer | Skip                                                            | Solstice ships its own theme system; organizer targets legacy MDI workflows.                                                                                                                                                                               |
| QuickExport (tom-jk)                | Skip                                                            | Krita 5.3-only; Krita 6 incompatible. Optional alternative: Grum999/JPEGExport (Codeberg).                                                                                                                                                                 |

## Architecture decisions

1. Two lanes. Lane A (vendored Python): `plugins/python/<name>/` +
   `kritapykrita_<name>.desktop` + `install_pykrita_plugin` line. Lane B
   (native C++): new module under `plugins/` following `visionml`/`restnote`
   precedent. Smart Select takes Lane B — its canvas tool is native code and
   the prebuilt DLL targets Krita 5.3/Qt5 regardless.
2. PyQt6 compatibility. Try a `PyQt5` -> `PyQt6` alias shim in
   `plugins/python/libkritapykrita/` first (module aliasing plus the known
   deltas: `QAction` QtWidgets->QtGui, `exec_`->`exec`, enum renames). If a
   plugin resists (Pigment.O is the stress case), patch its imports at vendor
   time and record the patch in `plugin-intake.md`.
3. Vendoring ledger — `docs/agent/plugin-intake.md` records source repo,
   pinned commit/release, local patches and re-check cadence, mirroring the
   `upstream-intake.md` pattern.
4. MCP bridge — vendored `kritamcp` plugin exposes the HTTP command bridge on
   `127.0.0.1:5678`; FastMCP server and `krita` CLI stay external
   (`uv tool install krita-cli`). Hardening phase adds codex-mcp-style bearer
   auth, per-class timeouts and capability reporting. Stock Krita and
   Solstice cannot run side by side (single-instance key), so the port clash
   with an existing kritamcp is documented rather than engineered around.
5. Builds — `windows-build.yml` on this repo; GitHub Releases for builds
   worth keeping (artifacts expire in 7 days). Optional local `env.bat`
   toolchain later per `development-workflow.md`.
6. Licensing — all selected plugins are GPL-3.0 or MIT; attribution goes in
   `plugins/python/README-vendored.md` / `LICENSES/`.

## Phases

### Phase 0 — fork bootstrap and baseline build — done

- Clone, confirm `main`, dispatch `windows-build.yml`; install artifact ZIP;
  verify visionml and the Python Plugin Manager.
- This document + `plugin-intake.md` skeleton.
- Exit: unsigned ZIP builds from the fork and runs.

### Phase 1 — PyQt6 compat layer and vendor harness — done

- Prototype the PyQt5 alias shim; test against Pigment.O first.
- Vendor procedure doc: pinned source -> normalize desktop file name ->
  `plugins/python/` -> register -> decide default enablement. Check how
  pykrita enablement is seeded for bundled plugins (enabled-by-default may
  need a Solstice default-config seed, not just installation).
- Pilot: Timer Watch end-to-end.
- Exit: one vendored plugin works in a CI-built ZIP.
- Result: `plugins/python/PyQt5/` + `plugins/python/PyQt/` shim packages
  installed next to the plugins; coverage table + real-build fixes
  (`QSignalMapper.mapped`, `QFrame` shape/shadow split) in
  `plugin-intake.md`.

### Phase 2 — vendor Tier-A plugins — done

Repeatable recipe per plugin; order by risk:

1. BuliNotes, BuliBrushSwitch (Codeberg pins)
2. Imagine Board
3. Shortcut Composer (verify under Solstice's input/shortcut stack)
4. Pigment.O (heaviest PyQt surface)
5. Blender Layer (default off; document `blenderPath` setting)
6. Optional cherry-picked Rebecca's tools scripts

Exit: all chosen plugins appear in the Plugin Manager and load without
console errors. Verified on a real build 2026-10-15 — zero plugin
tracebacks at startup after the `wseparator` + `QSignalMapper` fixes.

### Phase 3 — AI Diffusion — done

- Diff local enhanced copy against upstream >= 1.52; port still-missing
  enhancements; vendor as `plugins/python/ai_diffusion` (desktop file +
  `.action`).
- Default to connecting to the existing ComfyUI server (127.0.0.1:8189);
  managed-server mode stays functional.
- Solstice queues jobs on the shared GPU via ComfyUI only — never spawns its
  own inference.
- Exit: generate/inpaint round trip through ComfyUI inside a CI-built ZIP.

### Phase 4 — agent bridge (`kritamcp`) — done

- Vendor edithatogo/krita-cli `krita-plugin/` as `plugins/python/kritamcp`;
  verify resource-path references resolve under the Solstice resource dir.
- External half: `uv tool install krita-cli`; MCP client configs point at it
  (the CascadeProjects/krita-mcp `mcp_config.json` pattern transfers).
- Hardening: bearer token per profile (store under `config\`), main-thread
  dispatch timeouts, capability/security reporting, structured errors.
- Exit: `krita` CLI and an MCP client drive the CI build — create doc, paint,
  save, export under allowed roots; `krita_preview`-equivalent verified.
- **Verified by `tests/e2e/`** (see `e2e-testing.md`): 27 tests green on a
  real build — auth enforcement, document lifecycle, pixels, layers,
  undo/redo, selections, filters, export, path-traversal rejection, soak.
  The suite found + fixed 7 real bugs (startup deadlock, `open_file`
  crash, traversal bypass, token sidecar, silent undo, flattened `.kra`
  saves, PyQt6 API drift). Later additions: `trigger_action` (any action
  by id), `screenshot` (`zoom`, `scope="canvas"`), and docker management
  (`show_docker`, `list_dockers` bringing the command count to 44) for
  display-level and workspace-level automation.

### Phase 5 — Smart Select native port — implemented (built; GUI check pending)

- Shipped as `SmartMattingTool` in `plugins/visionml/src/smartselect/`:
  `KisToolOutlineBase` freehand lasso -> `SegmentationToolHelper::
applyMattingMask` -> padded crop -> `VisionModels::removeBackground`
  (BiRefNet via vision.cpp, in-process; no Python worker) -> soft matte
  clipped to lasso region and layer alpha -> `KisPixelSelection`.
- Reuses the existing sampled-layers modes, all five selection actions, and
  grow/feather/anti-alias post-processing through `KisSelectionToolHelper`.
- Model: uses the `background_removal` task slot (BiRefNet Lite bundled;
  BiRefNet Dynamic or HR-matting GGUFs drop into `visionml/models`);
  HR-matting selectable in the tool-options model dropdown.
- Remaining: GUI verification of lasso/matte quality on a real build,
  optional user doc `docs/smart-matting.md`.

### Phase 6 — packaging, defaults, release — pending

- Bundled-default enablement set (AI Diffusion, kritamcp, Pigment.O on;
  Blender Layer off).
- Optional first-run model fetch for visionml BiRefNet-Dynamic; never bundle
  GB-scale weights into the ZIP.
- GitHub Release on the fork; README "Differences" update; optional website
  note per the release ritual in AGENTS.md.
- Exit: named release ZIP installs to a fully integrated Solstice.

### Phase 7 — AI Diffusion deepening — proposed (2026-10-16)

Audit findings (vendored tree, 2026-10-16):

- **Layered generation is already built.** `Arch.qwen_l` (Qwen-Image-Layered)
  is wired end-to-end: `LayerCountWidget` appears when a layered checkpoint
  is selected (`ui/generation.py:999`), `layer_count` defaults to 4
  (`model/model.py:145`), the workflow builds `1 + n*4` decomposed latents
  (`comfy_workflow.py:594`), and `apply_generated_result` splits the N
  output images into N separate Krita layers named `[Layer N]`
  (`model/model.py:851-855`). What is missing is verification and polish,
  not the feature.
- **RGBA survives the whole path.** `image.py` is `Format_RGBA8888`
  end-to-end; `supports_alpha` (currently `Arch.qwen2` only,
  `resources.py:241`) feeds input alpha into conditioning. LayerDiffuse-
  style SDXL transparency is absent upstream.
- **Model discovery is organic-ish already:** live ComfyUI model lists +
  folder/filename arch and quantization deduction (`client.py:230-248`,
  `resources.py:from_checkpoint_name`), plus a persistent
  `ModelMetadataCache` with mtime/TTL (`files.py:171`) from the local
  enhancement set. Hard limit: workflow support is per-`Arch` enum — a
  new architecture detects but gets no workflows until code is added.
  No safetensors header (`modelspec`) parsing exists. Latent bug fixed
  2026-10-16: `Arch.sd35` (nonexistent member) crashed discovery for
  `sd3.5/` folders → now maps to `Arch.sd3`.
- **Vector precedent exists inside the plugin:** `create_vector` lands
  pose control layers as real vector layers (`model/model.py:890`) —
  the same "result → typed layer" pattern the pipeline chain extends.
- **Catalog is SOTA-current:** 20 builtin styles cover Flux 2 Klein
  (nvfp4/fp8/Q6_K), Krea 2 Turbo, Z-Image Turbo, Anima, ERNIE, Chroma,
  Qwen/Edit/Layered; default style is Anima (alphabetical first,
  `style.py:278-279`). Legacy drag: `models.json.required` pulls ~8
  SD1.5/SDXL ancillaries + 6/11 legacy checkpoints into every managed
  install.

#### 7.1 Safetensors arch detection — implemented

- `backend/modelspec.py` (new, pure stdlib): reads the 8-byte length +
  JSON header only (32 MiB cap, never raises); `arch_keys` priority
  `modelspec.architecture` → `base_model` → `title` → `ss_sd_model_name`
  → `ss_base_model_version`; trigger words from
  `modelspec.trigger_phrase` + `ss_tag_frequency` (freq-sorted, cap 50);
  `extract_metadata_fields` produces the cached subset.
- Consumed in `File.read_model_metadata` (`files.py`) after every hash
  path (`compute_hash`, `compute_hash_async`, `compute_partial_hash`),
  persisted via `ModelMetadataCache`; feeds `lora_triggers` for the
  existing workflow trigger-word collection. `File.update` merges
  metadata so user edits survive re-scans.
- Arch consumption: `deduce_from_filename(filename, metadata)` tries
  `Arch.from_metadata_string` (hint table mapping modelspec/Kohya
  spellings → `from_string` ids, variant detection via filename+hint
  context) before folder/filename heuristics.
- `comfy_client._refresh_models` resolves server-reported filenames to
  local files via `_local_metadata_resolver` (index of `server_path`
  `models/{checkpoints,diffusion_models,unet}` roots, `ModelMetadataCache`
  backed) so headers are read once per file change.
- Tests: `plugins/python/ai_diffusion/tests/test_modelspec.py` (26
  stdlib-only tests, conftest stubs the `ai_diffusion` package import).
- Remaining: the `ui/style.py` LoRA-import path gets metadata for free;
  checkpoint arch from metadata only applies to local/managed servers
  (remote servers have no reachable files — the server-side
  `etn/model_info` endpoint already reports `base_model` there).

#### 7.2 Generic fallback arch — implemented

- `Arch.generic = "Generic"`; `from_checkpoint_name` falls through to it
  instead of assuming sd15; `from_string("generic")` round-trips.
- `is_compatible` treats generic as compatible with everything
  (permissive side); `Arch.list()` includes it so workloads/styles can
  declare it.
- Restrictive capability properties are allow-lists generic is not in:
  no regions, LCM, clip-skip, attention guidance, edit, controlnet —
  only basic txt2img/img2img. `text_encoders` → `clip_l`;
  `workflow.py` encoder dispatch → `stable_diffusion`; default VAE
  search `["vae"]`; universal sampler preset ("Default - DPM++ 2M")
  already applies.
- In `parse_model_info` a model whose `base_model` resolves to nothing
  now deduces to generic instead of being dropped.
- Remaining: `ui/theme.py` `checkpoint_icon(generic)` falls to a warning
  icon — acceptable; a dedicated generic style preset optional.

#### 7.3 CivitAI lookup + model browser — implemented (runtime verify pending)

- `backend/civitai.py` (new): `CivitaiClient` over `RequestManager` —
  `lookup_by_hash` (model-version endpoint → name/version/images/
  trigger words/base-model/downloadUrl/page URL), `search` (public
  model listing). Offline-safe: failures log + return None, never
  block the UI thread. The client takes the hex SHA-256; callers
  convert the plugin's stored base64 hash.
- `ui/model_browser.py` (new, ~1060 lines): `ModelLibraryModel`
  aggregates checkpoint + LoRA collections into a card view —
  thumbnail (`.preview.png`/`png`/`webp`/`jpg` siblings), name,
  architecture badge (modelspec-derived via `Arch.from_metadata_string`,
  raw string fallback), local/remote status, mtime, trigger-word
  snippet. Background scan (`_scan_files_metadata` on the executor)
  reads safetensors headers + mtimes off the UI thread; preview
  decode likewise (`QImage`, not `QPixmap`, for thread safety).
  CivitAI tab: opt-in identify (compute hash → hex → lookup), shows
  name/type/base model/trigger words/preview, "Apply trigger words"
  writes `lora_triggers`/`lora_strength` metadata, download uses
  `network.download` (`.part` resume) into a user-chosen folder and
  registers the result into the matching collection. Browse button
  added to the LoRA list header (`ui/style.py`); setting
  `settings.civitai_lookup` surfaces in the plugin settings page.
- Tests: `tests/plugins/test_civitai.py` (7 tests, needs PyQt6 —
  skipped on bare-Python environments).
- Remaining: runtime verification — delegate painting, metadata-worker
  thread behavior, settings sync, identify/download flows (manual GUI
  matrix item).

#### 7.4 Layered-generation polish — implemented (runtime verify pending)

- Semantic naming: `apply_generated_result` names `[Layer 1]
Background`, `[Layer N] Element M` (slot 0 = merged composite,
  1..N = elements bottom→top — comment cites `comfy_workflow.py:594`
  and `workflow.py:225`).
- Provenance: `_record_layer_provenance` stores prompt/negative/seed/
  checkpoint/style/sampler/strength/workflow_kind as JSON in a dynamic
  `"ai_diffusion_params"` QObject property on the layer node — runtime
  only, not persisted to `.kra` (no Krita API for persistent per-layer
  custom data; documented).
- Style preset added (2026-10-31, user request): `styles/qwen-image-layered.json`
  (`qwen_image_layered_fp8mixed`/`_bf16` checkpoints, Euler beta 50/4.0).
- Weights source found: `Comfy-Org/Qwen-Image-Layered` (public, not gated) —
  `split_files/diffusion_models/qwen_image_layered_{bf16,fp8mixed}.safetensors`
  → `D:\models\diffusion_models\`, and
  `split_files/vae/qwen_image_layered_vae.safetensors` → `D:\models\vae\`
  (exact match for `resource_id(vae, qwen_l)` pattern). Text encoder is the
  existing `qwen_2.5_vl_7b`. No catalog entries (manual download, like MOSS).
- Remaining: download the two weights files (~20-40 GB depending on quant)
  and runtime-verify `[Layer N]` decomposition end-to-end
  (manual GUI matrix item — needs the Qwen-Image-Layered VAE +
  checkpoint in ComfyUI).

#### 7.4a Alpha-layer generation — implemented

- `supports_alpha` widened: `qwen2` + `qwen_l` (layered output is
  inherently RGBA). Z-Image left out — no verified alpha evidence;
  widen per release after a real-generation check.
- **Cutout output mode** (`model.matte_output`, QueuePopup "Cutout"
  checkbox): each generated layer is post-processed in-place with the
  native `background_removal` filter (Vision ML / BiRefNet) — generated
  content lands as a clean cutout layer, no ComfyUI node round-trip.
  Missing filter surfaces a warning instead of failing.
- Still open: LayerDiffuse-class transparent generation workflow;
  GUI verification on a real build.

#### 7.5 Solstice pipeline chain — in-app recipe + E2E

- Generated layer → Smart Matting refine → Trace to Vector. AI asset →
  decomposed layers → vector paths: the workflow no upstream has.
- **In-app**: QueuePopup "Cutout" + "Vectorize" toggles
  (`model.matte_output`, `model.vectorize_output`) chain the two native
  features onto every generated layer — the productized macro, no
  agent needed. `_apply_output_vectorize` triggers the registered
  `layer_to_vector` action on the result layer (activates it first,
  restores the user's active layer afterwards).
- **Agent path** (unchanged): `apply_filter {"name":
"background_removal"}` → `trigger_action {"name": "layer_to_vector"}`.
  Recipe in `docs/agent/mcp-bridge.md` §Pipeline recipes.
- E2E: `test_matting_to_vector_pipeline` in `tests/e2e` drives the
  full chain on a real build.
- Remaining: GUI verification of both toggle paths on a real build.

#### 7.6 ComfyUI first-run installer — present upstream, verify only

- The plugin already ships the full flow: `WelcomeWidget` → server-mode
  chooser (`ui/settings.py` add*option: Online Service / Local Managed
  Server / Custom Server) → managed mode downloads and installs the
  ComfyUI \_application* itself; model weights stay user-managed per the
  Phase 6 no-bundled-weights rule (workloads are opt-in checkboxes).
- External servers reuse is explicit (`ServerMode.external` + custom
  URL); managed defaults point at `127.0.0.1:8189` (local patch).
- Exit: fresh profile with no ComfyUI gets a guided path to a running
  server; skip is frictionless. Verify in the manual GUI matrix.

#### 7.7 Perf/defaults + 7.7a SOTA-first curation

- Curated style presets: steps/CFG/sampler tuned per SOTA arch (the
  style files already isolate sampler choice per arch — adjust values,
  not structure).
- `ModelMetadataCache` exists — extend entries with the 7.1 modelspec
  fields so connect-time stays O(headers) not O(file sizes).
- SOTA-first install profile (implemented): the ~8 SD1.5/SDXL `required`
  ancillaries in `presets/models.json` were demoted to `optional`, and
  the 6 legacy checkpoints copied to a new `legacy_checkpoints` key
  (`checkpoints` keeps the union — `resources.py`, `server.py`, and
  `ui/server.py` hard-index these categories, so an exclusive move would
  strand them). The Workloads tab groups (SD1.5/SDXL/Flux/Flux 2/Z-Image)
  are opt-in checkboxes — selecting a legacy workload now installs its
  checkpoint only; ancillaries are individually selectable packages.
  `show_legacy_styles` filters builtin SD1.5/SDXL styles (mirroring
  `show_builtin_styles`, `style.py`); `settings.recent_styles` factory
  defaults now point at SOTA styles.
- Exit: fresh managed install downloads no SD1.5/SDXL weight by
  default; legacy remains opt-in, not deleted.

#### 7.7b Runtime hardening & Blackwell CUDA 13 verification — verified (2026-10-23)

- **`modelspec` import fix**: `backend/client.py` imports
  `arch_strings_from_metadata` from `.modelspec` for dynamic deduction in
  `deduce_from_filename` without raising `NameError`.
- **`QIcon` serialization guard**: `files.py` `File.to_dict()` replaced
  `asdict(self)` with a field-level comprehension using `fields(self)`
  skipping `f.name == "icon"` — prevents standard library `copy.deepcopy`
  from choking on PyQt6 `QIcon` objects during checkpoint and LoRA database
  JSON serialization.
- **Auto-update freeze**: `settings.py` enforces `auto_update = False`
  factory default to protect Solstice's Krita 6 codebase from being
  overwritten by upstream Krita 5 release packages.
- **Live RTX 5090 Blackwell validation**: Verified against ComfyUI 1.54.0 /
  0.38.0 on CUDA 13 PyTorch (`2.13.0+cu130`) with async weight offloading and
  DynamicVRAM; indexed 200 checkpoints and 10,212 LoRAs cleanly with zero
  exceptions.


#### 7.7c Solstice 0.5.0 base app verification & bridge hardening — verified (2026-10-23)

- **Base app 0.5.0 intake**: Verified Solstice-0.5.0-windows-x64-49697aadce6c.zip
  from upstream GitHub release 0.5.0. Includes native **Remove Dust Tool and Filter**
  (KisToolRemoveDust), QML Quick Access guide, and GGML Vulkan GPU acceleration on RTX 5090.
- **PyQt5/Qt6 shim support**: Patched lib/krita-python-libs/krita/__init__.py in
  the 0.5.0 install directory to permit the pure-Python PyQt5 compatibility package
  under pykrita, avoiding ModuleNotFoundError: This version of Solstice is not compatible with PyQt5!
  when vendored plugins import Qt modules.
- **Bridge initialization hardening**: Started ServerThread(5678) and command queue
  QTimer directly in KritaMCPExtension.__init__, removing reliance on createActions(window)
  which may be deferred during welcome screen display or test executions.
- **E2E ground-truth**: Verified 24 tests PASSED (3 skipped, 1 deselected, 0 failed in 41.24s)
  under Solstice 0.5.0 with full ComfyUI connection on 127.0.0.1:8188.

#### 7.8 Re-vendor at upstream v1.54.0 final

- Tracked in `plugin-intake.md` (~end of Oct 2026): fast-forward
  `_vendor_src/ai_diffusion_upstream`, re-apply `enhancements` branch
  (merge `f628900` is the template), keep the `:8189` patch, re-run
  connect + one generation.
- Order matters: land 7.1–7.4 as _local_ commits in the enhancements
  branch so the re-vendor merge keeps them.

#### 7.9 Borrowed UX — Pixaroma + ComfyUI-LoRA-Manager research (2026-10-16)

Two external projects surveyed for stealable ideas. Both are ComfyUI
**server-side** extensions, which dictates the adoption path.

**ComfyUI-LoRA-Manager** (willmiao, registry `comfyui-lora-manager`):
model browser web UI at `/loras` on the ComfyUI port — CivitAI
download queue, cover previews, base-model badges, trigger words at a
glance, saved "recipes" (LoRA combos), fuzzy search, extra folder
paths, mature-content blur, LoRA randomizer pools, Danbooru+e621 tag
autocomplete.

- Already have: Danbooru+e621 autocomplete ships in `tags/`, extra
  model search paths (local enhancement), per-style LoRA trigger text
  (`ui/style.py:127` — manual entry only), NSFW filter param.
- Worth taking:
  - **Auto trigger-word discovery** — pull activation tags from CivitAI
    by file hash so styles auto-fill the manual trigger field.
    Natural extension of 7.3.
  - **Model browser with previews** — the LoRA picker today is a bare
    filename list (`ui/style.py` LoraItem); cover art + base-model
    badge + trigger words is a large QoL jump. Two paths: (a) bundle
    Lora-Manager in the managed install (7.6) and use its web UI —
    zero plugin work; (b) consume its REST API from the docker —
    native UX, medium work. Start with (a), graduate to (b) if the
    API proves stable.
  - **LoRA recipes** — a style already is a LoRA set; what LM adds is
    share/import. Cheap: export/import style files.

**ComfyUI-Pixaroma** (gitlab `pixaroma/comfyui-pixaroma`): creative-
suite node pack — prompt shortcuts (`@tags`, random picks), Prompt
Each (one image per prompt line, batch), BiRefNet remove-background
node, inpaint crop+stitch, XY plot, image compare slider, Krea LoRA
converter.

- Already beat: BiRefNet remove-bg is a ComfyUI node for them; Solstice
  has it _natively_ in Vision ML (in-process, no server). Our story is
  stronger here.
- Taken (implemented):
  - **Saved prompt snippets / `@tag` expansion** — `text.py`
    `pattern_snippet` + `snippets.json` in `user_data_dir`; expanded
    once in `model/region.py process_regions` so generation, live and
    animation paths all get it; `SnippetsDialog` (raw-JSON editor) on
    the prompt context menu. Unknown `@tokens` pass through; `@@x` and
    `@` inside words don't match.
  - **Per-line batch generation** — `batch_per_line` property + "Per
    Line" checkbox on the batch row; `_enqueue_jobs_per_line`
    re-prepares prompts per line (per-line wildcards/LoRA/metadata)
    and enqueues one job per non-empty line on the same seed schedule
    as regular batches; batch slider disabled in this mode.
  - **Image Compare** — history context menu "Compare Images" (exactly
    2 selected): `CompareDialog`/`_CompareView` split view with a
    draggable divider.
  - **Image Grid** — history context menu "Save Grid as Layer":
    `_compose_grid` builds a sqrt-proportioned labeled grid and lands
    it as `[Grid] <name>` via `layers.create`.
  - **LoRA trigger words from file metadata** — implemented via
    `modelspec.trigger_words_from_metadata` → `lora_triggers` (7.1).
  - **Parameter sweep (XY-lite)** — `backend/sweep.py` `{{steps:…}}` /
    `{{cfg:…}}` prompt spec; `_enqueue_jobs_sweep` runs one job per
    value on the shared seed schedule, results labeled
    `name [axis=value]` — feeds the history grid/compare views for
    preset QA. 9 stdlib tests.
- Done since: model browser with previews + CivitAI identify/download
  (7.3, `ui/model_browser.py`); output post-processing chain —
  "Cutout" (Smart Matting) + "Vectorize" (Trace to Vector) toggles on
  the QueuePopup batch row (7.4a/7.5).
- Still open: LoRA recipe share/import (cheap follow-up).
- Already covered natively: Inpaint/Outpaint "stitch" (paste original
  back, keep only generated area, feather + color-match) is our
  `apply_grow_feather`/`expand_mask` linear-kernel + auto-derived
  blend (`workflow.py:926-934`, `model.py:482-483`); Image Composer /
  Paint / Sketch / Text Overlay — Krita's own canvas beats them;
  Pause Image — our results already land in history for
  review-before-apply.
- **LoRA Loader Pixaroma reads trigger words from each file** —
  validates the 7.1/7.3 modelspec approach (safetensors
  `ss_tag_frequency`/`modelspec` fields); do the same in
  `deduce_from_filename`/`File.metadata`.
- **License: MIT** — the whole pack can ship inside the managed
  ComfyUI install (7.6) with attribution; their custom-workflow nodes
  become available to our custom-workflow feature for free.
- Skip: 3D builder, audio-reactive video, save-mp4, canvas node-tools
  (Node Colors, control panels, set/get wires) — ComfyUI-canvas
  ergonomics with no counterpart in a docker-first UX.

Deployment note: both are server-side extensions — installing them
into the managed ComfyUI (7.6) makes their node-level features
available to custom workflows for free; only the UI ideas need
porting into the docker.

Deferred/not pursued: backend swap off ComfyUI (forfeits the workflow
ecosystem; perf lives server-side anyway), C++ port of the plugin
(Python client + websockets is the right weight for a remote renderer).

#### 7.10 Borrowed UX — OpenFill research — partially shipped (2026-10-26)

Survey of `beckettharriman/openfill` (Apache-2.0; generative fill /
remove / expand / txt2img for GIMP 3 and PhotoCraft over a local
ComfyUI, itself derived from this plugin's design). Full survey and work
items: `docs/agent/openfill-intake.md`.

- Already have (verified): per-mode/per-arch prefill (`detect_inpaint`),
  multi-pass `ScaledExtent` inpaint, `FillMode.inpaint` removal
  (LaMa/MAT), managed ComfyUI, model browser + modelspec metadata,
  kritamcp bridge + e2e. Its PyAV-probe PNG workaround is N/A — uploads
  go through `api/etn/image`, not `/upload/image`.
- Shipped: the `ai_*` action family in kritamcp (`ai_status`, `ai_styles`,
  `ai_generate` modes fill/remove/expand/create/auto, `ai_expand`,
  `ai_jobs`, `ai_apply`, `ai_cancel`) — parameterized agent-driven
  generation on the shared managed ComfyUI (docs: `mcp-bridge.md`); plus
  `license` + `min_vram_gb` metadata on `ModelResource`, annotated
  `presets/models.json` (FLUX dev/Kontext/MAT non-commercial flagged),
  license labels in the package lists, and VRAM-fit gating in the
  Workloads tab fed by the connected client's `device_info`. Tests:
  `ai_diffusion/tests/test_model_metadata.py`, e2e `ai_*` cases.
- Open: `catalog.json` mined as data (low-VRAM inpaint checkpoints,
  RORem, FLUX Fill LoRA). Optional: managed server idle timeout,
  variations picker UI, e2e fill matrix, AMD awareness.
- Skip: OpenFill's engine/supervisor/GIMP+PhotoCraft adapters, A1111
  backend, `task_lama` in visionml (no LaMa arch in vision.cpp),
  PNG tEXt pad, second ComfyUI manager.

Suggested order: 7.1 → 7.2 (cheap, unblocks unknown models) → 7.4/7.4a
(verification + polish) → 7.7/7.7a (defaults) → 7.5 (needs Phase 5 GUI
sign-off) → 7.3, 7.6 (optional UX) → 7.8 (on upstream release).

### Phase 8 — AI Artist Suite — proposed (2026-10-27)

Goal: a state-of-the-art artist suite — Photoshop-parity convenience
plus capabilities Photoshop lacks (local, layered, automatable,
auditable). Local-first inference; upstream's cloud client stays opt-in.
Upstream-tracking is per-feature: cheap cherry-picks land forward; a
full Krita-6-line re-vendor is a separate decision (see 7.8 precedent).

Landscape anchor (verified October 2026): FLUX.3 Image Klein released
2026-10-15 (ComfyUI v0.41.0 native; distilled + base checkpoints;
NVFP4/FP8 quants; bounding-box layout control via prompt layout tables).
Qwen Image 2.1 / 2.1 Turbo (ComfyUI v0.40; plugin 1.54 support
confirmed; convrot int8 weights; RGBA transparency). MOSS-Image and
Fibo 2.0 upstream PRs open (#2640–2646). Krea 3 cloud partner node.
MiniMax-H3 LightVAE, Set Reference Multi Latent, SDR→HDR nodes
(ComfyUI v0.41). Photoshop 2026: Generative Fill with model picker
(Firefly 5 + partner models), Expand/Remove 2K, Harmonize,
Prompt-to-Edit generative layers, AI Assisted Editor, AI Markup,
Reference Image with objects, Generate Similar/Background, Generative
Upscale + Topaz denoise/sharpen; Illustrator adds Text-to-Pattern,
Generative Recolor, Concept-to-Vector. Upscalers: SeedVR2 (Apache-2.0,
one-step, 3B/7B + NVFP4/INT8, native in ComfyUI) is the license-clean
pick; SUPIR is non-commercial. Model tooling: Comfy Desktop downloads
tray (pause/retry), `.metadata.json` sidecars (convention already read
by `extract_sidecar_metadata`), Civitai update polling.

Already have (verified): 21 arches incl. flux2_4b/9b, qwen_e/e_p,
qwen_l, qwen2, zimage, anima, krea2, ernie, chroma; 15 ControlModes;
edit-mode instruction editing; upscale docker (fast model + tiled
diffusion); custom workflows; live mode; animation docker; regions;
autocomplete; tiled VAE; kritamcp `ai_*` family; license/VRAM metadata;
LM `.metadata.json` sidecar reader; LM optional node + per-node
requirements install.

#### 8.1 New-gen models & catalog

- 8.1a FLUX.3 Klein arch — partially shipped (2026-10-27): upstream
  PR #2641 cherry-picked (10 files applied, 3 hand-merged —
  resources/workflow/samplers conflicts resolved against local
  generic-arch + combined-import deltas); `Arch.flux3`, qwen3vl_4b
  encoder wiring, `Flux 3 - Euler Simple` sampler, `flux3-klein`
  style, flux2-VAE mapping, `is_flux2` coverage. Required entries
  added: `text_encoder-qwen_3vl_4b` (flux3+krea2 ids — also unblocks
  krea2), `vae-default-flux3` (reuses verified flux2 VAE).
  Checkpoint files stay manual-download — BFL repo is license-gated
  (no anonymous sha256). Open: layer-bounds → layout-table prompt
  mapping (the unique-on-top feature).
- 8.1b Qwen 2.1 / 2.1 Turbo catalog entries — shipped (2026-10-27):
  `checkpoint-int8-qwen2` + `checkpoint-turbo_int8-qwen2` with
  HF-API-verified sha256 from `Comfy-Org/Qwen-Image-2.1`; required
  `text_encoder-qwen_3vl_8b-all` + `vae-default-qwen2`; styles
  `qwen-image-2.json` + `qwen-image-2-turbo.json`.
- 8.1c Upstream Krita-6 cherry-picks — per-feature pulls from the
  1.54.0-pre+ line: MOSS-Image, Fibo 2.0 as PRs land; each
  evaluated against local-patch surface (kritamcp hooks, ModelResource
  fields, sidecar reader, LM node list). Catalog diff vs upstream
  master done (2026-10-27): identical checkpoint set incl. Flux2
  Q6_K GGUF; only difference is upstream moved 9 control resources
  (Fooocus Inpaint, IP-Adapters, Hyper-SD, CLIP Vision, Easy
  Negative, ControlNet Inpaint/Unblur) into `required` — local keeps
  them `optional` (intentional divergence). First cherry-pick:
  PR #2641 (8.1a) — upstream tests/ files absent locally, skipped.
- 8.1d models.json audit — verified (2026-10-27): all 81 file entries
  carry sha256; **URL liveness sweep complete — 77/77 live** (HEAD
  requests, 200s). Remaining: license/min_vram on any stragglers,
  plus style-only arch gaps (anima, ernie, chroma, krea2 checkpoint,
  flux2_9b) awaiting self-sourced weights.
- 8.1e Qwen Layered → Krita layer stack — `qwen_l` output decomposed
  into real Krita layers (subject/shadow/background separately) via
  the existing apply path. Unique in any editor.
- 8.1f RGBA generation productized — Qwen 2.1 transparency surfaced
  in the docker; model-native alpha wired to the 7.4a alpha-layer path.

#### 8.2 Photoshop-parity generative surface

- 8.2a Prompt-to-Edit generative layers — instruction edits
  (edit_mode exists) land as a new named Krita layer stamped with
  prompt/model/seed; undo = layer delete; re-editable via kritamcp.
  Non-destructive parity with PS — better metadata.
- 8.2b In-context model picker — quick-switch chips/dropdown in the
  docker for per-generation model choice across installed arches
  (Flux2 Klein, Qwen 2.1, Kontext, Z-Image, Illustrious…). PS charges
  per-generation credits; ours is free.
- 8.2c Variations picker UI — results strip with Keep / Keep All /
  Discard wired to existing `apply_generated_result` / `discard`;
  bridge already exposes `ai_apply` / `ai_discard`.
- 8.2d Harmonize lane — composite relighting: instruction relight via
  Qwen Edit/Flux Kontext as primary; IC-Light (kijai nodes, SD1.5) +
  Detail Transfer frequency-separation for detail preservation;
  CDTNet/PCTNet-class color harmonization as cheap fallback.
- 8.2e AI Markup — markup/sketch layer strokes forwarded as visual
  instruction input (multi-ref to Qwen Edit / Flux2) for
  annotate→edit workflows.
- 8.2f Generate Similar — one-click seed-perturbed batch from a
  result layer (entry point on existing job count/seed plumbing).
- 8.2g Seamless-tile generation — wrap-mode/asymmetric tiling for
  texture artists (Text-to-Pattern parity).
- 8.2h Generate Background — native BiRefNet cutout → subject-aware
  background synthesis; better subject isolation than PS's path.

#### 8.3 Model management & automated downloads

- 8.3a Resumable download queue — verified already present
  (2026-10-27): `network.py::_try_download` Range-resumes `.part`
  files, 30s stall-timeout abort, 3-attempt retry with 416 restart,
  cancel-safe abort. Remaining gap: downloads panel UI
  (pause/cancel surface).
- 8.3b Hash verification — shipped (2026-10-27): inline post-download
  sha256 check in `download_models` + `fix_models`
  (`_verify_downloaded`, deletes + fails on mismatch); the manual
  full-tree verify pass already existed.
- 8.3c Update detection — Civitai version polling for tracked models
  (LM already polls its managed set; surface status in our browser);
  catalog drift check for managed resources.
- 8.3d GGUF lane — verified already present (2026-10-27):
  ComfyUI-GGUF in `required_custom_nodes` (UnetLoaderGGUF /
  DualCLIPLoaderGGUF); catalog carries Q6_K/Q4_K/Q5_K_M GGUF entries
  (Flux2 Klein checkpoint, T5/Qwen3 encoders).
- 8.3e VRAM auto-pick — workload/checkpoint recommendation from
  detected `device_info` (floors already in catalog); "why this
  model" tooltip (carried from 7.10).
- 8.3f Installer hardening — shipped (2026-10-27): `_extract_archive`
  now validates every member against the target dir (zip-slip);
  `try_install` rollback already existed. Open: per-node requirements
  conflict reporting on pip failure.
- 8.3g Sidecar write-back — shipped (2026-10-27):
  `_write_metadata_sidecar` emits LM-compatible `.metadata.json`
  (name/license/min_vram/sha256/source_url) after each managed
  download; never overwrites existing sidecars.
- 8.3h LM deep links — "Open in LoRA Manager" affordance
  (`:8189/loras`) for managed models; evaluate recipe-import into
  custom-workflow presets.

#### 8.4 Quality, upscale & native ML

- 8.4a SeedVR2 upscaler — Apache-2.0, one-step diffusion restoration,
  3B/7B + NVFP4/INT8 quants, native ComfyUI. Third upscale mode in
  the docker (fast / tiled diffusion / restoration) + catalog entries.
- 8.4b HDR pipeline — ComfyUI v0.41 SDR→HDR node + LightVAE;
  investigate Solstice HDR canvas hooks.
- 8.4c vision.cpp task expansion — candidates: SAM2/SAM3 encoder (if
  landed upstream), ViTMatte-class matting refinement, DepthAnything
  v3 for faster ControlNet depth/normal maps.
- 8.4d Denoise/sharpen parity — Topaz-parity via ComfyUI utility
  nodes or vision.cpp tasks (Restormer/DnCNN-class), exposed beside
  existing vision filters.
- 8.4e 4K+ fidelity audit — verify multi-pass fills don't downsample
  source detail at 4K+; surface tile-budget knobs in settings.

#### 8.5 Creative suite expansion

- 8.5a Texture/material studio — seamless tiles (8.2g) + normal/
  roughness maps via existing depth/normal control pipeline → Krita
  layer set.
- 8.5b Brush/stamp generator — one-click recipe on the existing
  generate → Smart Matting → Trace to Vector chain: describe a brush
  tip → new brush preset.
- 8.5c Generative recolor — instruction-edit recolor + palette-
  transfer utility (Generative Recolor parity).
- 8.5d Reference-object compositing — geometry-aware object placement
  (PS Reference-Image-with-objects) via IP-Adapter/multi-ref +
  placement heuristics.
- 8.5e Concept→vector recipe — generation → Trace to Vector
  productized as a one-click recipe.
- 8.5f Animation lane (exploratory) — Wan/LTX-class video for Krita
  animation frames, behind the custom-workflow layer first.
- 8.5g AI assist bar — PS AI-Assisted-Editor parity: evaluate a
  contextual prompt strip under the canvas tied to selection state
  (the docker is already the full surface).

#### 8.6 Automation, testing & community release

- 8.6a kritamcp expansion — partially shipped (2026-10-27):
  `ai_catalog` endpoint (archs, per-category models w/ license,
  min_vram_gb, requirements, installed state). Open: batch
  `ai_generate`, recipe runner, markup input.
- 8.6b Fill-quality regression matrix — OpenFill-evidence-style
  model × scenario grid through the e2e harness on a real build;
  results published under `docs/agent/`.
- 8.6c First-run wizard — GPU detect → workload recommendation →
  one-click install. The community-release blocker.
- 8.6d Release engineering — signed-build decision, plugin
  enablement seeding, model preset packs per GPU tier, suite docs.
- 8.6e Upstream sync protocol — per-feature cherry-pick checklist:
  diff upstream arch/workflow/catalog changes → port → re-apply the
  local-patch list from `plugin-intake.md` → run tests.

Suggested order: 8.1d/8.1b (catalog data unblocks models) → 8.3a/8.3b
(download trust) → 8.2a/8.2c (parity UX on existing engine) → 8.4a
(SeedVR2) → 8.1a/8.1e (new-arch features) → 8.5* (suite recipes) →
8.6* (release track throughout).

### Phase 9 — Competitive feature research (2026-10-26)

Synthesis in `docs/agent/feature-research-2026-10-26.md`. Headline
candidates: live co-generation, parametric stroke editing (unique —
`KisBrushStrokeLayer` substrate), wet-media + spectral pigment mixing,
Qwen-Image-Edit/Kontext instruction edits, IC-Light harmonize, generative
upscale, ideation surface, FLF2V/VACE AI inbetweening + stroke tweening,
camera folder/Rhubarb/RIFE, Sub View/lazy-string/QuickShape/Layer-Comps
QoL set, on-device LoRA training. Positioning: local-first/no-telemetry
vs CSP (zero AI) and Affinity ($120/yr AI gate).

### Phase 9 — AI quick wins (plugin-level; models on disk) — proposed

Approved 2026-10-26. Items reuse weights already on `D:\models` or small
public downloads; none need new arches.

#### 9.1 Instruction-based editing — verified e2e (2026-10-26)

- `qwen_image_edit_2511_bf16` in `unet/` + `Qwen Edit` style +
  `Arch.qwen_e_p` wiring already exist; Flux Kontext loras on disk.
- Verified: `ai_generate{mode:fill, style:"Qwen Edit",
  prompt:"make it night time with moonlight"}` over the layered-doc
  canvas → job finished → `[AI Edit]` layer applied. Full path works.
- Remaining polish: surface an "edit instruction" prompt mode in the
  generation dialog; document the flow in the AI feature doc.

#### 9.2 Harmonize (relight) — verified e2e (2026-10-26)

- Nodes: `ICLightAppply` (etn tooling nodes) + `ComfyUI-GGUF`/`UNETLoader`.
  Weights: `huchenlei/IC-Light-ldm` →
  `D:\models\diffusion_models\iclight\iclight_sd15_fc_unet_ldm.safetensors`
  (1.72 GB — use the `_unet_ldm` variant; `iclight_sd15_fc.safetensors` is
  a full ckpt, wrong format, deleted). Base: `Realistic_Vision_V6.0_NV_B1_fp16`
  → `checkpoints/` (no VAE baked) + `vae-ft-mse-840000-ema-pruned` → `vae/`.
- Workflow `harmonize-iclight.json` + style `harmonize-sd15.json` in
  `%APPDATA%\krita\ai_diffusion\{workflows,styles}`: `ETN_KritaStyle` +
  `ETN_KritaImageLayer`(Foreground) + `ICLightAppply`(c_concat=VAEEncode(fg))
  + KSampler on EmptyLatent at fg size → `ETN_SaveImageCache`.
- Verified: `ai_workflow{workflow:"harmonize-iclight", params:{style,
  layer, lighting}}` → relit `[Generated]` layer applied on canvas.
  Direct-ComfyUI test (stalenhag autumn → "cold moonlight"): composition
  preserved, correct night relight. Latent size MUST match fg (use
  GetImageSize→EmptyLatent, not fixed dims).
- New MCP actions `ai_workflows` (list) + `ai_workflow` (run custom
  workflow by id/name with param dict: style=name, image_layer=layer
  name/id, raw values otherwise) in kritamcp. Found+fixed: `generate()`
  is fire-and-forget — restoring `workspace`/`workflow_id` synchronously
  after enqueue made `_generate_custom` silently no-op (it re-checks
  `workspace is Workspace.custom` when the coroutine actually runs).
- flarecore relighting (F:\AIPrograms\_Dev_Projects\flarecore) stays as
  reference for future light-direction inference; IC-Light prompt is the
  shipping path.

#### 9.3 Generative upscale — verified e2e (2026-10-26)

- `seedvr2_7b_int8_convrot` on disk = generative upscaler; runs through
  the existing `prepare_upscale_simple`/`JobKind.upscaling` path via the
  `UpscaleModel` hook in `ui/upscale.py`. New MCP action `ai_upscale`
  (upscaler name, factor, auto_apply) in kritamcp.
- Graph rewritten to native ComfyUI SeedVR2 nodes (old `SeedVR2LoadDiT*
  /SeedVR2VideoUpscaler` pack nodes don't exist): `UNETLoader` +
  `VAELoader` (`seedvr2_ema_vae_fp16`) + `ImageScaleBy` +
  `SeedVR2Preprocess` (1 output in this core) + `VAEEncodeTiled` +
  `SeedVR2Conditioning` + `KSampler`(1 step, euler/simple, cfg 1.0) +
  `VAEDecodeTiled` + `SeedVR2PostProcessing` (color_correction:
  lab/wavelet/adain/none — "linear" is NOT valid).
- Found+fixed: hook ended in `w.save_image` (plain SaveImage → files on
  disk, no result to client → job "cancelled" despite server success).
  Must be `w.send_image` (ETN_ReturnImage/ETN_SaveImageCache).
- Found+fixed: `ComfyWorkflow.seedvr2_upscale` class method existed with
  wrong signature + 2-output SeedVR2Preprocess; `hasattr` guard skipped
  the wrapper entirely. Fixed the class method itself.
- Found+fixed: `upscale_image()` resizes the document synchronously at
  enqueue; a failed/cancelled job left the canvas at the target size.
  `JobParams.metadata["original_extent"]` now recorded at enqueue and
  `_finish_job` reverts on cancel/error when the doc is still at the
  job's target extent (won't stomp a user resize or a later upscale).
- Verified e2e via bridge: 512² → SeedVR2 ×2 → `[Upscale]`+`[Generated]`
  layers applied at 1024²; cancel mid-flight → doc reverts to 512².

#### 9.4 Ideation surface — verified e2e (2026-10-26)

- Batch variations already existed (`count`/`variations` →
  `model.batch_count`, seeds increment by `settings.batch_size`).
  Added two new `ai_generate` params:
  - `similar_to` (job id or index): inherits prompt/negative/strength/
    style from a finished job, new seed — one-call "Generate Similar".
  - `[[a|b|c]]` deterministic variant specs: one job per option
    (product across groups, cap 8 variants / 16 jobs). Single-brace
    `{a|b}` is NOT expanded by the bridge — it passes through to the
    plugin's native `eval_wildcards` (seeded RNG, re-rolled per seed).
- Found+fixed: `QueueMode.replace` inside a multi-`generate()` loop cancels
  the still-queued earlier variant jobs — loop forces `QueueMode.back`
  after the first enqueue.
- Found+fixed (critical): `ai_diffusion/ui/__init__.py` in the test
  install was a stale 2773-line copy of `kritamcp/__init__.py`
  (leftover from an earlier bad deploy). Importing `ai_diffusion.ui`
  executed it → a SECOND bridge bound :5678 (Windows SO_REUSEADDR lets
  both listen) → requests raced between old and new code → phantom
  nondeterminism. Restored the real package init; verify
  `ai_status.build` marker after every kritamcp deploy.
- Verified e2e: `[[red|blue]]` → 2 jobs; `{green|purple}`+count=3 →
  3 seeded-eval jobs; `similar_to:<id>` re-enqueued the source prompt.

#### 9.5 Distraction-detect → fill chain — v1 verified e2e (2026-10-26)

- New bridge action `ai_remove_distraction`: `rect:[x,y,w,h]` or
  `point:[x,y]`+`radius`, padded by `grow` (default 8) → selection →
  `ai_generate mode=remove` → applied `[Generated]` layer. Forwards
  prompt/style/strength/seed/apply.
- Verified e2e: 60px ellipse removed from painted canvas, region
  auto-expanded (grow 16 → 132px square), fill blended to background.
- Noted: visionml segment tools (`tool_select_segment_from_point/rect`)
  are interactive canvas tools — no headless API. Point-and-click SAM
  detection is a Phase 10+ item (needs a visionml selection API or a
  ComfyUI SAM node chain); rect/point-driven removal ships now.
- Noted: `mode=remove` on txt2img-style archs blends acceptably but a
  dedicated inpaint checkpoint gives cleaner edges.

### Phase 10 — Live co-generation — proposed

#### 10.1 Live workspace backend — verified e2e (2026-10-26)

- The plugin's `LiveWorkspace`/`LiveScheduler` already implements the
  full loop (50 Hz input poll, grace-period dispatch, continuous
  regeneration while `is_active`). New bridge action `ai_live`:
  `active` (switch to Workspace.live + toggle), `strength`, `prompt`,
  `negative`, `style`, `apply` (apply_result as a dedicated layer via
  `layer_only=True`), `layer_only`. Response surfaces `has_result`,
  `result_extent`, `model_error`, `doc_active` for agent polling.
- Verified e2e: activate → Z-Image Turbo live job on stroke →
  `has_result` within ~5 s → `apply` → dedicated generated layer.
- Live jobs are removed from the queue on finish (same as upscaling)
  so `ai_jobs` never shows them — poll `ai_live` for `has_result`.
- Noted: sparse-sketch input at high strength mostly preserves the
  sketch — live painting quality is an interactive-tuning matter
  (prompt + strength + progressive painting), mechanism verified.
- Live sampler preset: style `sampler_preset: "live"` (ETN_KritaStyle
  input) routes through the fast path; Z-Image Turbo works as-is.

#### 10.2 Live polish — partially verified (2026-10-26)

- Live results never clobber real layers — they land in
  `LiveWorkspace._result` (docker preview) until `apply_result`;
  bridge `ai_live{apply}` pushes them as a dedicated named layer.
- Strength is exposed (`ai_live{strength}` + docker slider); style-lock
  via LoRA manager unchanged.
- Open for interactive tuning: per-arch live sampler preset
  (`sampler_preset:"live"` in style) + Lightning/Hyper loras for
  non-turbo archs; stroke-quality guidance overlay remains research.

### Phase 11 — Art-side native (highest differentiation) — planned

**Detailed research-backed implementation plan:**
`docs/agent/phase11-implementation-plan.md` (2026-10-27). Key
findings that shaped it: Mixbox is CC BY-NC (GPL-incompatible —
implement our own Burns/spectral.js MIT method via precomputed
RGB→spectral LUT + K-M mix shader); wet media follows the
Curtis-1997 three-layer model on kritagpu compute with a
standalone prototype gate before integration; LookFX/flarecore
ship first as external-worker filters, hot paths ported to
shaders later. Build order: 11.1 → 11.3 → 11.5a → 11.2 gate.

#### 11.1 Spectral pigment mixing

- Mixbox/Kubelka-Munk LUT as opt-in blend mode in the GPU composite
  path (blue+yellow→green, not gray); applies to smudge/color-smudge
  and brush mixing. Small shader, large perceived-quality win.
- **Python slice shipped + verified (2026-10-27)**:
  `plugins/python/kritamcp/spectral_mix.py` is a numpy port of
  spectral.js 3.0.0 (MIT — not mixbox); `ai_pigment {top, bottom,
  opacity, layer_name}` blends two layers into a `[Pigment]` paint
  layer. Verified live: blue+yellow → `[130,144,72]` olive; identity
  and t=0/1 endpoints exact. Remaining for the native slice: move the
  same K/S math into a GPU composite-op blend mode (the Python module
  is the reference implementation + oracle for GPU tests).

#### 11.2 Wet-media engine (Vulkan)

- Per-layer wetness + pigment buffers, diffusion/evaporation pass, wet
  edge darkening; phase 2 impasto height + normal-mapped lighting.
- Reference: `F:\Rebelle 8`. Largest engineering item — write a
  sub-plan + prototype gate before committing; only feasible on the
  Vulkan stroke path (~5 ms strokes).

#### 11.3 Parametric stroke editing — unique in market

- Stroke inspector docker over `KisBrushStrokeLayer` recorded data:
  list strokes, swap preset, change size/color, transform/delete a
  single stroke; deterministic re-render via the existing redraw path.
  No raster competitor can do this.

#### 11.4 Stroke QoL pack

- Lazy-string pulled stabilization, post-stroke smoothing/re-fit on the
  stored point stream, QuickShape (stroke-end hold → fitted editable
  line/ellipse/rect).

#### 11.5 LookFX port → native print-FX — feature port approved

- Source: `F:\lookfx` (`lookfx_core`: halftone screens, 21 paper
  substrates, capillary bleed, plate misregistration, C/M/Y/K
  separations) + `F:\AIPrograms\_Dev_Projects\flarecore` (41 lens-flare
  presets, 182 elements, depth occlusion, linear-space render).
- Ship as generator layer + filter pair. Preferred: port math to
  native filter API; fallback: embedded python GPU worker filter.

#### 11.6 Docker/UI QoL pack

- Sub View reference docker (eyedropper-from-reference), Layer Comps,
  color-mixing scratch palette + image palette extraction, decoration/
  ribbon brushes (seeded image-tip sequences — deterministic under
  brush-stroke layers), vector line suite on `KoPathShape`
  (erase-to-intersection, simplify, weight adjust), word balloons +
  panel clip folders, LT conversion (photo→line+tones), Auto Action
  macro recorder via kritamcp plumbing.

### Phase 12 — Animation (extend ai_diffusion → timeline) — proposed

#### 12.1 Timeline QoL — bridge portion shipped (2026-10-27)

- `ai_anim` action: fps, current time, playback + full clip range
  get/set; `animated_layers` reports per-layer keyframe times.
  Verified live: fps=24, frame=4, clip range 0-100 applied.
- `ai_xsheet` action shipped (2026-10-27): exposure-sheet export.
  Walks every `animated()` node, writes `{fps, layers:[{name,
  keyframes[]}]}` to JSON or `layer,frame` rows to CSV; optional
  `thumbnails` exports a PNG per keyframe via `pixelDataAtTime` +
  `_export_pixels_png` into `<path>_thumbs/`. Params: `path`,
  `layer`, `start`, `end`, `thumbnails`. Verified: JSON + CSV + 7
  real thumbnails on a 31-keyframe lipsync layer.
- Remaining: auto-keyframe on draw; 2D camera folder (keyframed
  pan/zoom/rotate over layer groups via transform masks + curves
  docker); x-sheet view over `KisTimeBasedItemModel` (data export is
  done via `ai_xsheet`; the in-app view is UI work); light-table
  cel references.

#### 12.2 Rhubarb lip-sync — verified e2e (2026-10-27)

- `ai_lipsync` action shipped: `rhubarb.exe -f json` (bundled at
  `D:\krita\tools\rhubarb\Rhubarb-Lip-Sync-1.14.0-Windows\`, also
  PATH fallback) → timed phoneme cues → per-frame PNG sequence from
  mouth-shape paint layers (A-H/X by normalized name, group "Mouths"
  preferred) → `Document.importAnimation` → `[Lipsync]` layer.
- Verified: 14.37 s demo clip → 346 keyframes at 24 fps; pixel check
  confirmed correct shape per frame (X/C/A verified); repeated runs
  and remove→reimport stable after `waitForDone` hardening.
- Optional params: `recognizer` (pocketSphinx|phonetic), `dialog`
  (transcript hint file), `start_frame`, `name`.

#### 12.3 RIFE interpolation — verified e2e (2026-10-27)

- `ai_interpolate`/`ai_interpolate_status` shipped: extracts an
  animated layer's keyframes via `pixelDataAtTime`, chains ImageBatch
  → `FrameInterpolationModelLoader` (rife_v4.26, added
  `frame_interpolation` to krita-managed yaml) → `FrameInterpolate`
  → frames → `importAnimation`. Verified: 9 keys → 17 frames on
  `[Densified]` (0.4 s execution).
- LIMITATION verified: RIFE dissolves objects on large displacement —
  densification only, sparse keys belong to 12.4.

#### 12.4 FLF2V inbetweening — marquee — smoke-tested e2e (2026-10-26)

- Native `WanFirstLastFrameToVideo` node confirmed + verified in this
  ComfyUI core (also `MiniMaxH3ImageToVideo`, `WanVaceToVideo`,
  `WanAnimateToVideo`, `CosmosImageToVideoLatent` — all native).
- Working chain (verified direct `/prompt`): UNETLoader
  `wan\wan2.2_i2v_{high,low}_noise_14B_fp8_scaled` → LoraLoader
  `Wan Video 2.2 I2V-A14B\wan2.2_i2v_lightx2v_4steps_lora_v1_*` →
  CLIPLoader `umt5_xxl_fp16` type=wan → CLIPVisionLoader
  `wanVideo_clipVisionH` + 2×CLIPVisionEncode → WanFirstLastFrameToVideo
  → 2×KSamplerAdvanced (high: steps 0-2 add_noise/return_leftover
  enable; low: 2-4 both disable; uni_pc/simple, cfg 1.0) →
  VAEDecode `wan_2.1_vae` → SaveImage frames + CreateVideo→SaveVideo.
- Verified: 9-frame 480×264 inbetween in ~8 s with warm models
  (~90 s cold incl. 14B load). Real motion interpolation incl.
  squash-and-stretch; SaveImage yields numbered PNGs = exactly what
  `document.import_animation` needs.
- Gotchas: dimensions must be ÷8 (270→264 auto-shrink, silent);
  SaveVideo requires `format`+`codec` args; model/lora names need
  their subfolder prefix in UNETLoader/LoraLoader.
- **Krita integration verified e2e (2026-10-26):** new bridge actions
  `ai_inbetween` (exports two keyframe layers → /upload/image → queues
  the FLF2V graph, returns prompt_id) and `ai_inbetween_status`
  (polls /history, downloads frames via /view, `importAnimation` onto
  a named animation layer). Verified: `keyA`+`keyB` layers →
  `done, imported: 9` → `[Inbetween]` layer `animated=True`,
  keyframe_times [0..8], ~10 s warm.
- Gotchas found: `ai_settings.server_url` lacks a scheme (normalize to
  `http://`); `importAnimation` creates its OWN layer (snapshot node
  uniqueIds, rename the new node — do not pre-create); never put
  `"error": None` in a response — the dispatcher checks key presence
  and would report success as HTTP 500; per-request state must be
  module-global (`_FLF_JOBS`), not instance — duplicate .desktop
  registrations mean any extension object may run a command.

#### 12.5 Motion propagation — verified e2e (2026-10-27)

- `ai_propagate`/`ai_propagate_status` shipped: dense control frames
  extracted from an animated layer via `pixelDataAtTime` (or
  `keyframes_only`), uploaded, batched → `WanVaceToVideo` (native,
  36-ch VACE conditioning) → KSampler → frames → `importAnimation`
  onto `[Propagated]`. Optional `reference_layer` anchors appearance.
- Model: `wan\wan2.1_vace_14B_fp8_e4m3fn.safetensors` (Kamikaze-88,
  16.2 GiB) — loads via plain `UNETLoader`. The earlier
  `Wan2_1-VACE_module_14B_bf16` is a patch module, NOT a UNET
  ("Could not detect model type"); `wan2_2_fun_vace_a14b_high` is
  high-noise-only (no `_low` partner on disk). Verified pairing:
  WanVaceToVideo=36ch vs Wan22FunControlToVideo=48ch.
- Verified: 9-frame control + painted ref → 13 frames on
  `[Propagated]`; reference appearance (green blob) visibly
  propagated. Optional EbSynth fallback unneeded for now.

#### 12.6 Stroke tweening — C++-blocked, native path documented

- **Blocked from Python**: `KisBrushStrokeLayer` lives in
  `libs/ui` (kritaui), not libkis — no Python reach. All pieces
  needed already exist in the fork:
  - `KisRecordedBrushStroke` (`libs/ui/KisBrushStrokeLayer.h`):
    `Job{type: POINT|LINE|CURVE, KisPaintInformation pi1/pi2,
    QPointF control1/control2}` + preset/fg/bg/pattern/gradient
    + `Start` dab-anchor per mirrored copy.
  - `KisPaintInformation::mix(t, pi1, pi2)` +
    `mixOnlyPosition`/`mixWithoutTime`
    (`libs/image/brushengine/kis_paint_information.h:310-316`) —
    the interpolation primitive already exists.
  - `KisBrushStrokeLayer::renderStrokes(strokes, cs, bounds,
    offset, transform)` replays a stroke set into a paint device
    through the brush-tool path with the recorded seeds.
- **Native implementation path**:
  1. `KisBrushStrokeLayer::lerpStroke(a, b, t)` (new static in
     `KisBrushStrokeLayer.cpp`): pairwise job list — `mix(t,
     a.pi1, b.pi1)`, `mix(t, a.pi2, b.pi2)`, `control*` lerped;
     preset/color from the A stroke; requires matching job
     counts/types else returns null. `Start` lerped per mirror
     copy (position + angle only, dab indices reset).
  2. Bake step (an action, not a channel): for two raster
     keyframe times `t1<t2` and N steps, compute
     `lerpStroke` for each `f in (t1,t2)`, `renderStrokes` the
     result, then `KisRasterKeyframeChannel::addKeyframe(f)` +
     copy rendered device into the keyframe's paint device
     (`KisRasterKeyframe::setFrameID` path / `copyFromDevice`).
     Baking keeps existing channel semantics — no new channel
     type, stays a plain animated paint layer, undoable via one
     parent `KUndo2Command`.
  3. UI hook: layer context/animation-docker action
     "Tween Strokes Between Keyframes…" (count + easing).
     Easing = reparameterize `t` before mixing (already trivial
     since mix is parameterized on `t`).
- Optional later: expose to the bridge via a thin libkis wrapper
  (`Node.tweenStrokeKeyframes(count, easing)`) — still C++, but
  then `ai_tween` becomes possible.
- Deferred non-destructive variant: a `KisBrushStrokeKeyframeChannel`
  storing stroke-sets and re-rendering at frame-switch; larger
  design (channel registry, copy/merge semantics, storage in .kra)
  — bake-first is the right order.

#### 12.7 Wan Animate — verified e2e (2026-10-27)

- `ai_animate`/`ai_animate_status` shipped: animated pose layer →
  dense `pixelDataAtTime` frames → `pose_video`; `reference_layer`
  (character) → reference_image + clipVisionH encoding on both;
  `WanAnimate2ToVideo` + `wan\wan2.2_animate_14B_bf16` (single-model,
  no high/low pair needed) → frames → `[Animated]` layer. Verified:
  9 pose frames → 13 output frames, coherent generated content.
- **MiniMax H3 joint AV chain — verified e2e (2026-10-27).**
  Complete on-disk chain, all native `comfy_extras` nodes:
  - UNET `fasth3_omniref_8step_pruned_int8_convrot` (also
    `minimax_h3_fl2va`/`ref2va`, `10Eros_Max_h3_*`,
    `fastvideo_fasth3_8step_v2_nvfp4` on disk)
  - CLIPLoader type=`minimax` ← **requires the 32B encoder**
    (`qwen3vl_32b_minimax_h3_*`, also `_heretic_`/`nvfp4_awq`
    variants): `qwen3vl_8b` yields 4096-dim vs required 5120
    ("mat1/mat2 shapes" error — caught by smoke test).
  - VAE `minimax_h3_video_vae_*` + **audio VAE**
    `minimax_h3_audio_vae_fp32` (`D:\models\vae\`).
  - Graph: `MiniMaxH3ImageToVideo`(clip,vae,prompt,W,H,length)
    → positive + joint AV `NestedTensor` latent (`EmptyMiniMaxH3LatentAV`
    for t2v) → KSampler → `VAEDecode`(video_vae) +
    `VAEDecodeAudio`(audio_vae) → `CreateVideo`+`SaveVideo` /
    `SaveAudio`.
  - Verified: 22-frame 512×288 mp4 (`h3_smoke_00002_.mp4`) +
    real 40 KB FLAC (`h3_smoke_00001.flac`), seed-varied.
  - `MiniMaxH3AddGuide` anchors image/audio guides at arbitrary
    frames — that's the lipsync/foley hook (audio in via
    `audio_vae`, NOT `audio_encoders/` — that folder is for
    `AudioEncoderLoader`, a different feature).
  - Frame grid: length = 17k+5 (22 ≈ 1 s, 124 ≈ 5 s).
  - SaveVideo `format` input takes the bare option key
    (`"auto"`), not the combo object.
  - License: MiniMax commercial — flag in UI/docs before
    shipping a user-facing action.

### Phase 13 — Model ecosystem — proposed

#### 13.1 Train-from-document — SKIPPED (user decision 2026-10-27)

- LoRA fine-tuning on the open document (dataset → adapter → style-
  consistent generation). User has dedicated training tools; not
  needed in-app.

#### 13.2 Per-region reference images — bridge shipped (2026-10-27)

- `ai_region` creates/lists regions (layer→region link via
  `Region.link_target`, `positive` prompt) and `ai_reference` binds a
  named image layer as a `ControlMode.reference` control on the root
  region or a named region, with strength. Verified live: region
  created on `sky-region`, `ref-image` bound to it at 60%.
- Remaining: model-side confirmation that per-region reference
  conditioning fires for a specific arch (needs a generation pass on
  an edit/reference-capable checkpoint).

#### 13.3 Point-drag generative editing — feasibility researched (2026-10-27)

Click-drag a point → object deforms toward it (generative Puppet
Warp). Internet research verdict:

- **LazyDrag** (ICLR 2026, StepFun/HKUST, arXiv 2509.12203) is the
  architectural winner: first drag method on **MM-DiT** (the same
  family as Flux/Qwen backbones), explicit correspondence map
  replaces attention point-matching, full-strength inversion, **no
  test-time optimization**, text guidance resolves ambiguous drags,
  multi-round move+scale. SOTA on DragBench (MD 21.49 vs FastDrag
  31.84; user-study preference 62%). **Blocker: code promised but
  not yet released** — watch for the drop; design `ai_drag`'s
  signature backend-agnostic so it slots in.
- **FramePainter** (ICCV 2025, arXiv 2501.08225) is the **shippable
  path today**: SVD-1.1-based drag+sketch editor with a real
  ComfyUI custom node (`smthemex/ComfyUI_FramePainter`, MIT,
  single-checkpoint variant, ~7–8 GB VRAM at 512²). Requires
  `models/FramePainter/{unet,encoder}_diffusion_pytorch_model.safetensors`
  + an SVD checkpoint. Bonus: it also accepts **sketches** — pairs
  directly with Solstice's stroke layers as an input surface.
- DirectDrag (arXiv 2512.03981) — mask/prompt-free, repo claimed;
  still per-edit latent optimization (SD-era backbone).
- Skip: DragDiffusion/DragLoRA (per-image LoRA, minutes per edit);
  ComfyUI-DragNUWA (video drags, SVD-XT, stale); GDrag/DragNeXt
  (training-free but slow optimization loops); LightningDrag
  (~1 s inference but SD-era backbone, weight availability murky).
- **SHIPPED (2026-10-27)**: `ai_drag {layer, sx, sy, tx, ty, radius,
  width, height, seed, steps, guidance_scale, control_scale,
  layer_name}` + `ai_drag_status` — FramePainter installed
  (`ComfyUI_FramePainter` custom node; weights
  `D:\models\FramePainter\{unet,encoder}_diffusion_pytorch_model.safetensors`
  + `checkpoints\svd_xt.safetensors`; junction
  `ComfyUI\models\FramePainter → D:\models\FramePainter`). Verified
  e2e: drag-track mask → `[Dragged]` layer. Caveat: the output is a
  diffusion *regeneration* (FramePainter repaints the frame through
  SVD priors) — on flat test graphics it reinterpreted rather than
  moved; on real artwork it does semantic deformation. Tune
  `control_scale`/`steps` for fidelity. `points:[…]` multi-drag is a
  mask-render extension of the same path; LazyDrag drops in later
  behind the same signature if its code ships.
- Interactive point-picking UI remains a separate docker/canvas-
  overlay task; the bridge action is agent-usable today.

#### 13.4 ToonCrafter — SKIPPED (user decision 2026-10-27)

- 2024-era cartoon inbetweener, unmaintained, needs a separate ~5 GB
  install + custom nodes. Wan 2.2 FLF2V (12.4, shipped) already covers
  toon-style inbetweening; not worth a stale dependency.

### Phase 14 — Foundation rebase (`Solstice-1.1.1`), native real-time engine, & unified segmentation/layering roadmap (added 2026-11-15)

See also the master session report in
`solstice_rendering_and_art_engine_production_plan.md`.

#### 14.0 Rolling Integration Tally & Architecture Lock (2026-11-15)

**Marked `Integrated` (pruned from pending quick-win backlog per user decision):**

- Backend/attention tuning (`ComfyUI-Easy-Install`, `--use-sage-attention --highvram`, `nvvfx`, `ComfyUI-TwinFlow`) — Integrated.
- `TAEF1` / `TAEF2` + WebSocket JPEG fast live preview (`is_live=True`) — Integrated.
- `FluxKVCache` on `flux2_4b` / `flux2_9b` edit/live conditioning — Integrated (verified wired in `plugins/python/ai_diffusion/backend/workflow.py` and `comfy_workflow.py`).
- Live resolution cap + optional `RTXVideoSuperResolution` post-upscale — Integrated.
- `NitroFusion2` / `TwinFlow` Live style preset — Integrated.

**Explicit Architecture Rule — NO TouchDesigner (`FluxRT-TD`) Bridge:**

- Rejected bridging external TouchDesigner (`FluxRT-TD`). Relevant real-time streaming primitives (prompt-embed caching, cached reference VAE encode, fixed-seed coherence, compact FlowMatch schedules, per-step token-grid inpaint masking, and RIFE display smoothing) are to be **ported directly into Solstice** (principally via `librediffusion` below).

#### 14.1 Foundation upgrade evaluation — `Solstice-1.1.1` vs current `v0.5.0` fork (`deb3ebd6`)

Tree comparison against `C:\Users\richk\Downloads\Solstice-1.1.1` (`ac468d9` Win64 package):

- **`libs/`** (`common=5356`, `diff=4330`, `only_1.1.1=72`, `only_our=19`):
  Upstream `1.1.1` adds `1.0.0` Vulkan GPU passes (`KisGpuColorTransformPass`, `KisGpuCoveragePass`, `KisGpuShiftedCopyPass`, `KisGpuStrokeStylePass`, Blackwell RGBA16F/32F auto-start) and `KisClipStackLayer` (Clipping Masks with KRA/PSD round-trip). Fork-only files are cleanly isolated (`KisGpuPerspectivePass`, `KoSpectralMixing` / `KoCompositeOpPigment`, `KisViewResVectorCompositor` / `Decoration`).
- **`plugins/`** (`common=4536`, `diff=3357`, `only_1.1.1=194`, `only_our=15`):
  Upstream `1.1.1` adds `solsticeeffects` (12 filters + GPU `Color Reference`), G'MIC-Qt `3.7.4.1`, `CompositionDocumentState` (Layer Comps + per-comp saved Fast Image Export), `QuickAccess` gesture/grid dialogs, `through_layer_eraser`, `remove_dust`, and `plugins/extensions/testcontrol`. Fork keeps `kis_svg_export`, `SmartMattingTool`, `DepthMapFilter`, `ESRGANFilter`, `SegmentationMatteUtils`, plus all `plugins/python/` packages (`ai_diffusion`, `kritamcp`, 10 vendored plugins).
- **`krita/`** (`common=1831`, `diff=491`, `only_1.1.1=6`, `only_our=0`):
  Upstream adds icons for clipping masks, remove dust, and through-layer eraser.
- **Decision**: Selective rebase/merge onto `Solstice-1.1.1` preserving all 19 `libs/` and 15 `plugins/` fork-only C++ files plus `plugins/python/`, and reconciling `testcontrol` alongside `kritamcp`.
- **Immediate asset win**: `Solstice-1.1.1-windows-x64\share\krita\visionml\models` already bundles `BiRefNet-lite-F16.gguf` (`84.54 MiB`), `MIGAN-512-places2-F16.gguf` (`14.07 MiB`), and `MobileSAM-F16.gguf` (`25.65 MiB`), whereas `D:\krita\share\krita\visionml\models` and `%APPDATA%\krita\visionml\models` are currently missing.

#### 14.2 Downloads audit ledger (2026-11-15)

| Download Item | Verdict | Notes |
| :--- | :--- | :--- |
| `librediffusion-main.zip` | **Integrate (Native #2 Port)** | C++23 / CUDA / TensorRT 11 (`sm_75`–`sm_120`), MIT/C API (`librediffusion_c.h`). Implements `FLUX.2-klein-4B` streaming (`librediffusion_flux2_stream_*`: Qwen3 prompt cache, ref cache, custom FlowMatch schedules, per-step token-grid `set_mask`, fixed seed `52`), `SDXL Turbo` (26 fps @ 1024²), `SD Turbo` (96 fps @ 512²), `SDXS` (>600 fps), `GaParmar img2img-turbo`, ControlNet/IP-Adapter, and `RIFE` TensorRT display interpolation (`_gpu_sized`). |
| `Solstice-1.1.1` / `Solstice-1.1.1-windows-x64` | **Integrate (Selective Rebase + GGUF copy)** | See §14.1 above; upgrade path over `v0.5.0` fork while retaining custom AI/vector/spectral stack. |
| `ComfyUI_MPEG_Diffusion_Suite` | **Integrate (2K Dual-Res Refinement)** | Paper `2611.12069v1.pdf` + 4 ComfyUI nodes (`MPEGSaliencyProbe`, `MPEGDualResAttentionPatcher`, `MPEGTokenUpscaler`, `MPEGSeamMerger` using 2D Raised-Cosine Hann blend + transition variance lock). Target: Live->2K idle detail refinement without seams. |
| `PixelAlchemist_v1.4.5` | **Reference only (Presets)** | Closed PyInstaller FX pipeline app (`PixelAlchemist.exe`, `pa-vcam.exe`); inspect `presets/qt/*.json` (31 looks/dither configs) only as inspiration alongside `solsticeeffects` & Phase 11.5. |
| `Wicked Engine` | **Reference only** | Heavy C++ DX12/Vulkan 3D game engine; do not embed; reference HDR/CAS/SMAA shaders only if helpful. |
| `glitchVids` | **Asset only** | 20 `.mp4` glitch overlays (CC BY 4.0); optional sample test media only. |
| `S4W-Scanlines-for-Windows v1.4` | **Skip** | Retro emulator DX CRT/bezel overlay injector. |
| `Tiny-Consequences-main` | **Skip** | Standalone Node web simulator game. |
| `Night Shift Setup 2.5.0.exe` | **Skip (External app)** | External creator command-deck installer (`Night Shift` v2.5.0); not a Solstice module. |
| `Paradoxal Creamation Bootstrap 5.1.7z` | **Unverified / Skip** | AES header-encrypted archive requiring password; excluded unless password supplied. |

#### 14.3 Native real-time engine & dual-res 2K refinement — planned

- Embed or host `librediffusion` TensorRT (`sm_120`) pipelines behind an optional `ai_diffusion` Live backend toggle so static-prompt/masked canvas strokes run via cached Qwen3 embeds + `librediffusion_flux2_stream_set_mask` + `frame_cached` with zero HTTP/TouchDesigner overhead.
- Wire `ComfyUI-MPEG-Diffusion` 4-node graph (`MPEGSaliencyProbe` -> `MPEGDualResAttentionPatcher` + `MPEGTokenUpscaler` -> Refine -> `MPEGSeamMerger`) for automatic/on-demand 2K salient detail refinement.

#### 14.4 Segmentation, matting, layered & native RGBA unification — planned

Verified code gates to close against existing `D:\models` & `D:\krita\ComfyUI\comfy_extras` assets:

- **VisionML GGUF provisioning + specialized matting**: Populate base `.gguf`s from `Solstice-1.1.1-windows-x64`; add illustration/soft-edge matting routing via `D:\models\RMBG\BiRefNet\BiRefNet_toonout.safetensors` (`843.9 MiB`) and `D:\models\RMBG\BEN2_Base.pth` (`1082.0 MiB`).
- **`SAM 3.1 Multiplex` -> Auto-Regions -> Masked Live Canvas**: Wire ComfyUI's native `comfy_extras\nodes_sam3.py` (`SAM3_Detect`) with `sam3.1_multiplex_fp16.safetensors` (`1664.7 MiB`) to decompose prompt terms directly into Krita `Region` groups + bounding boxes (feeding `format_flux3_layout_prompt` / `format_moss_layout_prompt` and `_prepare_live_workflow` region-cropped Live Canvas).
- **Post-process cutout/vectorize gate**: Lift `if params.workflow_kind is not WorkflowKind.generate: return` in `plugins/python/ai_diffusion/model/model.py` so `matte_output` (`background_removal`) and `vectorize_output` (`Trace to Vector Layer`) work on edit/refine/inpaint/layered results.
- **`Qwen-Image-Layered` studio & 4-channel Native RGBA**: Wire `setup_latent_layers`/`pack_latent_layers` into `generate()` (not just `refine()`), pair 4/8-step Lightning LoRAs for fast splits, wrap decomposed layers in Krita Group layers with recursive element splitting, and expand `supports_alpha` (`resources.py`) + ComfyUI 4-ch VAE loading across `Qwen 2.1`, `flux2_3_rgba_vae.safetensors`, and `extract_4b`/`9b`.

#### 14.5 ArtCraft Crafting Apps (`storytold/*`, `MIT OR Apache-2.0`) source audit — evaluated 2026-11-15 (Zero-Sidecar Built-In Rule)

All five installed suites under `C:\Program Files\ArtCraft\` (`VectorCraft`, `EffectCraft`, `FigCraft`, `DesignCraft`, `PdfCraft`) were cloned (`--depth 1`) from `https://github.com/storytold/*` into [`storytold-artcraft/`](file:///C:/Users/richk/CascadeProjects/storytold-artcraft). All five carry `MIT OR Apache-2.0` licensing (GPLv3-compatible with Solstice; trademarked brand assets in `docs/brand/` excluded), and share `https://github.com/storytold/craft-libs`. **Per the 2026-11-15 Zero-Sidecar Mandate, do NOT shell out to `<name>-cli.exe` sidecars or embed full Rust desktop UIs (`egui`).** Build thin **static Rust-to-C FFI libraries (`cbindgen`)** linked directly into Solstice plugins/libs or port clean-room WGSL compute shaders directly to Solstice's Vulkan/GLSL pipeline:

- **`VectorCraft` (`storytold/vectorcraft` v0.24.1 — HIGH RELEVANCE, Static FFI):**
  - `crates/trace` (`vectorcraft-trace`, `#![forbid(unsafe_code)]`; deps: `vectorcraft-geom`, `kurbo`, `image`, `serde`, `thiserror`): edge-preserving smoothing, k-means/median-cut palette quantization, noise region merge, sub-pixel crack edge locator, junction-split shared boundaries, corner restoration/arc snapping, **centerline ink tracing** (`centerline.rs`), and **gradient tracing** (`gradient.rs`). Direct upgrade to Solstice's `Trace to Vector Layer` (especially anime ink linework & multi-color cutouts) compiled directly into a static C-FFI library for `TraceToVector`.
  - `crates/pathops` (`vectorcraft-pathops`): exact cubic-Bezier sweep-line booleans via `linesweeper`, 10 Pathfinder ops, `shape_builder`, `live_paint` (open-path planar faces for comic flatting/coloring), `offset_path`, `outline_stroke`, `simplify`, `smooth`.
  - `crates/affinity` (wrapping `craft-affinity` in `craft-libs`): clean-room `.af` (v8-12) / `.afdesign` / `.afphoto` / `.afpub` importer (artboards, live corners, pressure curves `LDeP`, CMYK/Lab/RGBA raster tiles, layer effects).
  - `crates/effects/src/addons/{halftone,duotone}.rs` + `crates/three-d`: vector mono/CMYK halftone screens (up to 100k cells), 1-4 ink spot/CMYK duotone, and 3D Revolve/Extrude/Bevel/Inflate into editable vector geometry.
- **`EffectCraft` (`storytold/effectcraft` v0.24.1 — HIGH RELEVANCE):**
  - `crates/gpu` + `crates/gpu/src/shaders/` (32 `.wgsl` files): highlights include `inpaint.wgsl` (GPU Content-Aware Fill: multi-scale harmonic membrane fill + GPU PatchMatch synthesis for local Remove/Smart Fill acceleration), `fx_lut.wgsl` (1D + 3D tetrahedral/trilinear LUT interpolation + OCIO opcodes), `fx_color.wgsl`, `fx_stylize.wgsl`, `fx_key.wgsl`, `display.wgsl`, plus `context::Pool` texture pooling and `AutoPick` CPU-vs-GPU readback heuristic (aligned with `Solstice-1.1.1` `KisGpu*` + `solsticeeffects`).
  - `crates/sam2` (`kernels.wgsl`, `kernels_ops.wgsl`) + `crates/segment` (`MaskModel` trait): pure-Rust + `wgpu` GPU compute kernels for `sam2.1-hiera-*` and `mobilesam` TinyViT-5M + `mediapipe-face` TFLite (`effectcraft-cli bench --roto --sam2 ... --mobilesam ...`), proving vendor-neutral Vulkan/wgpu inference for VisionML without PyTorch.
  - `crates/lottie` (`craft-lottie` wrapper) + `crates/psd` + `crates/track`: clean-room Lottie import/export (Phase 12 animation synergy), PSD/PSB smart-object (`SoLd`/`SoLE`) bicubic quilt warp reference, and coarse-to-fine NCC + sub-pixel Lucas-Kanade tracker.
- **`FigCraft` (`storytold/figcraft` v0.18.1 — SELECTIVE):** Skip UI/auto-layout framework; mine `crates/render/src/boolean.rs` (rolling-hash prefix-cached live booleans + corner-radius cache), `backdrop.rs` (`Under` stacked backdrop blur/glass pattern), and `crates/format/src/fig` (`.fig` reader) only if Figma vector/backdrop interchange is prioritized.
- **`DesignCraft` (`storytold/designcraft` v0.21.0 — SELECTIVE):** Skip DTP/IDML/EPUB/PPTX core; reference only `crates/pdf/src/gradient_inks.rs` (CMYK / `Separation` / `DeviceN` spot-ink PDF shadings) and `crates/compose` (Knuth-Plass paragraph breaking / hyphenation) if expanding Phase 11 comic word-balloons or print PDF/X-4 output.
- **`PdfCraft` (`storytold/pdfcraft` v0.21.1 — SKIP CORE & SKIP CLI):** Heavy PDF reader/editor/redaction (`cos`, forms, signing, `preflight` PDF/A-2b/3b, `ocr` via `ocrs`). Out of scope for Solstice's painting core; per the Zero-Sidecar Mandate, do NOT ship or shell out to `pdfcraft-cli`.

#### 14.6 Remaining `storytold/*` Ecosystem Audit (`storytold-artcraft/`, `MIT OR Apache-2.0` unless noted)

All 25 repositories under [`storytold-artcraft/`](file:///C:/Users/richk/CascadeProjects/storytold-artcraft) were audited on 2026-11-15. High-value crates and clean-room WGSL compute shaders are compiled via **static C-FFI (`cbindgen`)** or ported directly to Vulkan/GLSL shaders inside Solstice (no `*-cli.exe` sidecars):

- **`photocraft` (`0.23.1`, `MIT OR Apache-2.0`):** `photocraft-tablet` (`WM_POINTER*` / `GetPointerPenInfo` / WinTab 1.4 pattern to eliminate tablet pointer drops), `camera_raw.wgsl`, deterministic `photocraft-paint`.
- **`lightcraft` (`0.21.1`, `MIT OR Apache-2.0`):** `bm3d_*.wgsl` GPU BM3D denoiser + `nn_conv.wgsl` / `nn_pool.wgsl` shader-native neural inference ops (zero PyTorch).
- **`anicraft` (`0.3.0`, `MIT OR Apache-2.0`):** `anicraft-gpu` 16x16 tiled vector binning (`bin.wgsl`, `fine.wgsl`), 2D skeletal/spring/mesh warp, `.fla`/XFL (`craft-xfl`).
- **`materialcraft` (`0.4.1`) & `texturecraft` (`0.6.1`, `MIT OR Apache-2.0`):** DAG procedural material evaluator, 3D surface UV G-buffer painting (`EyeDepth` occlusion), GPU BVH baking (`bake.wgsl` AO/curvature/normal/thickness), `trace.wgsl` path tracer.
- **`filmcraft` (`0.20.1`, `MIT OR Apache-2.0`):** `filmcraft-scopes` (Waveform, RGB Parade, Histogram, Vectorscope), `lut.wgsl` tetrahedral 3D LUT shader, non-destructive timeline edit algebra (`ripple`, `slip`, `roll`, `slide`).
- **`craft-ai-integrations` (`0.1.0`, `MIT OR Apache-2.0`):** `ai-engine-rten` pure-Rust SIMD CPU **SAM 2.1 + BEN2 + ViTMatte** ONNX fallback (works without CUDA/PyTorch) + `ai-engine-candle` CLIP+point **SAM 3**.
- **`craft-libs` (`0.1.0`, `MIT OR Apache-2.0`):** `craft-gainmap` (ISO 21496-1 HDR Gain Map), `craft-jxl`, `craft-psd`/`kra`/`ora`, `craft-lottie`, `craft-materialx`, `craft-cms`.
- **`soundcraft` (`0.20.1`, `MIT OR Apache-2.0`), `spark` (`0.1.10`, `MIT`), `font-preview-service` (`0.1.0`), `cadcraft` (`0.21.0`, `MIT OR Apache-2.0`):** DSP/LUFS/audio envelope helpers, Gaussian splat GLSL shader (`spark`), WGSL HarfBuzz glyph shaders, 2D/3D constraint solver.
- **Skip / Reference-Only:** `deckcraft` (`0.19.0`), `artcraft`/`artcraftx` (keep pre-generation validation gate UX only), `craft-anything` (audit tool only), `craft-extensions`, `craft-skills`, `FineTrainers-Conditioning`, `indd-utils`.

#### 14.7 NightShift, Sentinel, LookFX+Flarecore Native, Lattice, PureRef, Look Engine & Creative/Editing Suite Synthesis (2026-11-15 Zero-Sidecar Build Specs)

- **Unified Built-In `/mcp` Server (No External `krita-mcp/server.py`):** Merge upstream `Solstice-1.1.1`'s native C++ HTTP `/mcp` JSON-RPC 2.0 server (`TestControlPlugin` / `TestControlMcpServer` + `TestControlCanvasTools` / `DocumentTools` / `UiTools`, `26` tools) with our fork's `plugins/python/kritamcp/__init__.py` (`63` `ai_*` / viewport / layer / selection actions) into one in-process endpoint.
- **NightShift Desktop (`v2.7.11`) Native Modules:**
  - `showa\engine.py`: Port the 7-stage analog OVA/film/CRT optical pipeline (`TIMING`, `HANDHELD`, `FILM`, `PRINT`, `SIGNAL`, `CRT`, `CHROMA`, `9` presets: `16mm`, `arcade`, `broadcast`, `crt`, `laserdisc`, `nightshift`, `offair`, `print35`, `vhs`, resolution scaling `L = height / 480`, deterministic seed) to native GLSL/Vulkan `KisFilter` + `showa_*` `/mcp` tools.
  - `camera-blockout` (`blockout.py`): Native pinhole 3D camera staging + colored mannequin bounding rigs + top-down floor plan + contact-sheet lens sweeps rendered to Solstice reference/ControlNet layers (`blockout_*` `/mcp` actions + docker).
  - **Skill/Prompt/Queue Pack:** Integrate the `44` NightShift `vault-seed\.claude\skills\` (plus `137` vault skill entries & `7` `artcraft-mcp` files as local prompt-preset library), node-DAG `@image` composer (`NODES.md`), built-in `gpu-queue` VRAM guard inside `ai_generate`, bundled starter packs + zero-download auto-discovery of `D:\wildcards` (`2,325` files), `D:\models\Poses` (`22,411` files), and `D:\models\LUTs` (`10,383` `.cube`).
- **Sentinel (`v0.6.15`) Clean-Room Patterns:** In-app `shader_worker` TDR timeout crash sandbox for experimental custom shaders, `datamosh_pro.fx` / `dither_mosh.fx` GPU shaders, `blend2d` 2D rasterizer ref, `LaserTraceGpu` shader profiler concept, `nvngx_vsr` RTX VSR + `sm120` FP8 TensorRT packs, 240 Hz decoupled control clock, `capture_at` (`combo_mode: "cartesian"`) parameter sweep, and LFO/WASAPI audio-reactive modulation (`miniaudio`).
- **LookFX (`F:\lookfx`, `Apache-2.0`) + Flarecore (`0.1.2b1`, `Apache-2.0`) 100% Native Override:** Superseding `phase11-implementation-plan.md` §11.5 tier-1 external worker — implement `LookFX`'s `17` halftone screens, `21` paper substrates (`21 .jpg + 8 .png`), `37` print presets, Ben-Day screening, capillary bleed & CMYK 4-plate separation plus `Flarecore`'s `41` optical flare presets & `182` elements purely in Vulkan compute + C++ `KisFilter`/`KisGeneratorLayer` + bundled KTX2 assets.
- **Lattice (`app.asar` 107 MB) Clean-Room Patterns:** Dawn `LatticeSharedImage` Vulkan <-> D3D11 zero-copy NT handle interop (`LibreDiffusion` TRT/CUDA -> canvas), WGSL stdlib helpers (`solver` Navier-Stokes fluid, `pbr`, `rig` 2-bone IK, `destruction`, `erosion`, `cel`), adaptive `panel_set` (`4-10` contextual sliders/knobs per active effect), and multi-aspect (`16:9` / `9:16` / `1:1`) batch deliverables export queue.
- **Built-In Reference, Grading, Glitch & Desktop App UX Concepts (Zero External Binaries / Clean-Room Only):**
  - **`PureRef` Workflow + `.pur` Importer:** Infinite Reference Pinboard Docker + Floating Click-Through Ghost Tracing Overlay (`WS_EX_TRANSPARENT` opacity slider), `Alt+G` value check, `Shift+H`/`V`, crop, `S` color pick, and native SQLite `.pur` 1.x/2.x importer (auto-indexing `E:\AI_Art\PureRefBoards\`).
  - **Solstice Look Engine:** 2D LUT atlas thumbnail strip + live hover preview + draggable split wipe + `filmcraft-scopes` + `EffectCraft`/`filmcraft` tetrahedral 3D LUT shader over `D:\models\LUTs` (`10,383` `.cube`). (`LUTs-Manager-Testing` is `xFxDesigns` proprietary — UX reference only.)
  - **`moshdither-studio` (`MIT`) + `Shader-Art-Studio` (`shaders-data.js` 160 shaders) + `MilkDrop 3` (`469` `.milk`):** Built-in 15+ dithering modes, angled pixel sort, mask datamosh, clean-room procedural generator presets & optional `.milk` loader.
  - **Proprietary App Clean-Room UX Specs (Never Copy Code):** `Stimma` (local FastConformer speech-to-prompt + PNG/WebP/JPEG EXIF workflow inspector/writer), `BorisFX Optics` (halation, mist/silk, frequency-separation beauty studio, DeBand/DeBlock/DeFringe), `CLIP STUDIO` (vector line-width pressure tool, 1/2/3-pt perspective rulers/snap, comic panel divider & word balloons), `Luminar Neo`/`Colourlab`/`Topaz` (3D depth portrait relighting via `DepthAnythingV2`, hero-shot color match, Autopilot noise/blur restore filters via `VisionML`/`BM3D`/`nvngx_vsr`), `Rebelle 8` (wet-on-wet capillary & impasto thickness), `Eagle` (dominant hex + tag Asset Vault docker). Standalone 3D/VFX/NLE/IDE tools (`SynthEyes`, `Sequoia 17`, `Resolve`, `Vegas Pro`, `FreeCAD`, `UE`, `Unity`, `TouchDesigner`, `IrfanView`, `ACDSee`, `PhotoScape X`, `Devin`/`Antigravity`) remain out of scope.

## Risks

- PyQt6 porting is the main per-plugin unknown; Pigment.O is the stress test.
  Worst case a plugin remains a user install instead of vendored.
- ai_diffusion divergence: upstream refactors may collide with the local
  enhancements; diff first, port only what is still absent.
- Smart Select is the only heavy lift (native tool + possible new GGUF
  conversion); deliberately sequenced last so earlier value ships first.
- Bundled pykrita plugins may arrive disabled by default; verify and seed
  enablement in Phase 1.
- Unsigned ZIPs trigger SmartScreen; acceptable for personal builds.
- Phase 8 spans plugin + native + bridge + release — a larger surface
  than any prior phase; keep per-item implemented/proposed status
  discipline or the ledger rots.
- FLUX.3/MOSS/Fibo depend on upstream landing; cherry-pick only once
  upstream stabilizes to avoid porting churn.
- The 8.1d catalog audit is manual verification work (every URL + hash
  by hand) — the largest hidden cost in the phase.
- Dual-lane metadata (our catalog vs LM sidecars) can drift; the 8.3g
  write-back needs the never-touch-LM-files guard enforced.
