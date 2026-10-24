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
- Remaining: runtime-verify `[Layer N]` decomposition end-to-end
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

Suggested order: 7.1 → 7.2 (cheap, unblocks unknown models) → 7.4/7.4a
(verification + polish) → 7.7/7.7a (defaults) → 7.5 (needs Phase 5 GUI
sign-off) → 7.3, 7.6 (optional UX) → 7.8 (on upstream release).

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
