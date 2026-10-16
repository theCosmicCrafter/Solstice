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
is hardened and covered by `tests/e2e/` (18 tests, all green);
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
| MCP bridge                          | Integrate — nanayax3/krita-mcp base + bearer auth               | nanayax3/krita-mcp based in-app HTTP bridge (42 POST actions + GET /health, /info, bearer token, MIT). krita-cli remains a compatible external client; codex-mcp stays a documented minimal alternative.                                                   |
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
- **Verified by `tests/e2e/`** (see `e2e-testing.md`): 18 tests green on a
  real build — auth enforcement, document lifecycle, pixels, layers,
  undo/redo, selections, filters, export, path-traversal rejection, soak.
  The suite found + fixed 7 real bugs (startup deadlock, `open_file`
  crash, traversal bypass, token sidecar, silent undo, flattened `.kra`
  saves, PyQt6 API drift). Later additions: `trigger_action` (any action
  by id) and `screenshot` (`zoom`, `scope="canvas"`) for display-level
  verification.

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

#### 7.1 Safetensors arch detection — cheap, do first

- Safetensors headers are length-prefixed JSON; read first 8 bytes +
  header, parse `__metadata__.modelspec.architecture`/`title` —
  stdlib-only, no deps, no GB-scale reads.
- Where: populate `File.metadata` inside `compute_hash_async`
  (`files.py:79`, already async + progress-callbacked) and consume in
  `deduce_from_filename` (`client.py:230`) as a higher-priority source
  than folder/filename heuristics.
- Exit: a checkpoint in an unlabeled folder with a garbage filename is
  still arch-detected correctly; heuristics remain as fallback.

#### 7.2 Generic fallback arch

- Unknown `Arch.from_string` results currently mean "unsupported". Add
  `Arch.generic` mapped to a minimal workflow (txt2img + img2img, no
  controlnet/inpaint/regions) so newly released models are usable day
  one instead of dead.
- Where: `Arch` enum + `from_string` fallback (`resources.py:101-144`),
  `is_compatible`, and the style-checkpoint matcher.
- Exit: unknown checkpoint selectable and generates a plain image;
  no control/region UI offered for `generic`.

#### 7.3 CivitAI lookup + model browser — optional, network-gated

- Hash-prefix query against CivitAI's model-version API for files that
  survive 7.1+7.2 unidentified → name/arch/cover preview. Download
  writes into ComfyUI's model folders, never Solstice's.
- Opt-in setting, offline by default; reuse `ModelMetadataCache` for
  lookup results.
- Exit: identify an unknown checkpoint by hash with UI showing the
  resolved card; nothing happens without the toggle.

#### 7.4 Layered-generation polish

- Runtime-verify `[Layer N]` decomposition lands correctly (manual GUI
  matrix item — needs the Qwen-Image-Layered VAE + checkpoint in
  ComfyUI).
- Semantic naming: Qwen-Layered emits background + ordered elements —
  name `Background`, `Element N` instead of `[Layer N]`
  (`model.py:853-855`, where `prefix=` is built).
- Reproducibility: write prompt/seed/model/checkpoint into layer props
  (Krita layer names are long-lived; use `setProperty`/name suffix
  consistent with how `create_result_layer` already names results).
- Exit: a 4-layer generation produces 4 named layers carrying their
  provenance.

#### 7.4a Alpha-layer generation

- Widen `supports_alpha` (`resources.py:241`) as RGBA archs land
  (Qwen 2.1 variants, Z-Image alpha) — small enum/property edit per
  upstream release.
- LayerDiffuse-class transparent generation is absent upstream —
  candidate local workflow on top of the existing SDXL graph.
- Asset UX: a "cutout" generation mode that lands an RGBA layer ready
  to composite — chains into Smart Matting (edge refine) and
  alpha-aware Trace to Vector.
- Exit: RGBA layer with real transparency lands from a supported arch;
  checkerboard shows through.

#### 7.5 Solstice pipeline chain — the differentiator

- Generated layer → Smart Matting refine → Trace to Vector. AI asset →
  decomposed layers → vector paths: the workflow no upstream has.
- Mechanically: the kritamcp `trigger_action` bridge already drives
  `layer_to_vector` and Smart Matting is a native tool — the chain is
  scriptable today; productize as a docker shortcut or menu macro
  rather than new engine code.
- Prerequisite: Smart Matting GUI verification (Phase 5 remaining item).
- Exit: one-click (or one-command) path from a generated layer to a
  vector layer, verified on a real build.

#### 7.6 ComfyUI first-run installer

- Optional managed-server _application_ install on first use (detect
  missing server → offer download). Application only — model weights
  stay user-managed per the Phase 6 no-bundled-weights rule.
- Exit: fresh profile with no ComfyUI gets a guided path to a running
  server; skip is frictionless.

#### 7.7 Perf/defaults + 7.7a SOTA-first curation

- Curated style presets: steps/CFG/sampler tuned per SOTA arch (the
  style files already isolate sampler choice per arch — adjust values,
  not structure).
- `ModelMetadataCache` exists — extend entries with the 7.1 modelspec
  fields so connect-time stays O(headers) not O(file sizes).
- SOTA-first install profile: move the ~8 SD1.5/SDXL `required`
  ancillaries + legacy checkpoints in `presets/models.json` to
  `optional` under a "full/legacy" profile; default managed install
  pulls only current-arch weights. Add a `show_legacy_styles` filter
  for builtin SD1.5/SDXL styles (mirroring `show_builtin_styles`,
  `style.py:319-322`).
- Exit: fresh managed install downloads no SD1.5/SDXL weight by
  default; legacy remains opt-in, not deleted.

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
- Worth taking:
  - **Saved prompt snippets / `@tag` expansion** — small UI add on top
    of the existing autocomplete.
  - **Per-line batch generation** — check whether the batch widget
    accepts a prompt list; if not, a simple loop.
  - **XY/parameter sweep** — the job system already batches; an XY
    grid over seed/steps/CFG feeding the history view is a moderate
    add with real QA value for tuning curated presets (7.7).
- Skip: 3D builder, audio-reactive video, save-mp4 — outside scope.

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
