# Plugin integration megaplan — agent reference

Status: approved (2026-10-14). Roadmap for shipping a custom Solstice build
with selected third-party Krita plugins integrated — vendored Python plugins,
a bundled MCP agent bridge, and one native port (Smart Select) into
`plugins/visionml`.

Decisions (user, 2026-10-14): mixed integration depth (per-plugin choice),
agent/MCP bridge is core scope, end state is a buildable fork, Smart Select
gets a full native port, builds run via GitHub Actions on this repository,
this document is the canonical plan.

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
| MCP bridge                          | Integrate — `krita-cli` base + codex-mcp hardening              | Bundle edithatogo/krita-cli `krita-plugin/` (54 tools, CLI, typed client, MIT). Graft cyyprezz/krita-codex-mcp patterns: bearer token, capability probing, main-thread dispatcher. codex-mcp stays a documented minimal alternative.                       |
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

### Phase 0 — fork bootstrap and baseline build

- Clone, confirm `main`, dispatch `windows-build.yml`; install artifact ZIP;
  verify visionml and the Python Plugin Manager.
- This document + `plugin-intake.md` skeleton.
- Exit: unsigned ZIP builds from the fork and runs.

### Phase 1 — PyQt6 compat layer and vendor harness

- Prototype the PyQt5 alias shim; test against Pigment.O first.
- Vendor procedure doc: pinned source -> normalize desktop file name ->
  `plugins/python/` -> register -> decide default enablement. Check how
  pykrita enablement is seeded for bundled plugins (enabled-by-default may
  need a Solstice default-config seed, not just installation).
- Pilot: Timer Watch end-to-end.
- Exit: one vendored plugin works in a CI-built ZIP.

### Phase 2 — vendor Tier-A plugins

Repeatable recipe per plugin; order by risk:

1. BuliNotes, BuliBrushSwitch (Codeberg pins)
2. Imagine Board
3. Shortcut Composer (verify under Solstice's input/shortcut stack)
4. Pigment.O (heaviest PyQt surface)
5. Blender Layer (default off; document `blenderPath` setting)
6. Optional cherry-picked Rebecca's tools scripts

Exit: all chosen plugins appear in the Plugin Manager and load without
console errors.

### Phase 3 — AI Diffusion

- Diff local enhanced copy against upstream >= 1.52; port still-missing
  enhancements; vendor as `plugins/python/ai_diffusion` (desktop file +
  `.action`).
- Default to connecting to the existing ComfyUI server (127.0.0.1:8189);
  managed-server mode stays functional.
- Solstice queues jobs on the shared GPU via ComfyUI only — never spawns its
  own inference.
- Exit: generate/inpaint round trip through ComfyUI inside a CI-built ZIP.

### Phase 4 — agent bridge (`solstice-mcp`)

- Vendor edithatogo/krita-cli `krita-plugin/` as `plugins/python/kritamcp`;
  verify resource-path references resolve under the Solstice resource dir.
- External half: `uv tool install krita-cli`; MCP client configs point at it
  (the CascadeProjects/krita-mcp `mcp_config.json` pattern transfers).
- Hardening: bearer token per profile (store under `config\`), main-thread
  dispatch timeouts, capability/security reporting, structured errors.
- Exit: `krita` CLI and an MCP client drive the CI build — create doc, paint,
  save, export under allowed roots; `krita_preview`-equivalent verified.

### Phase 5 — Smart Select native port — implemented (pending real-build test)

- Shipped as `SmartMattingTool` in `plugins/visionml/src/smartselect/`:
  `KisToolOutlineBase` freehand lasso -> `SegmentationToolHelper::
applyMattingMask` -> padded crop -> `VisionModels::removeBackground`
  (BiRefNet via vision.cpp, in-process; no Python worker) -> soft matte
  clipped to lasso region and layer alpha -> `KisPixelSelection`.
- Reuses the existing sampled-layers modes, all five selection actions, and
  grow/feather/anti-alias post-processing through `KisSelectionToolHelper`.
- Model: uses the `background_removal` task slot (BiRefNet Lite bundled;
  BiRefNet Dynamic or HR-matting GGUFs drop into `visionml/models`).
- Remaining: compile in the real build, GUI verification of lasso/matte
  quality, optional user doc `docs/smart-matting.md`.

### Phase 6 — packaging, defaults, release

- Bundled-default enablement set (AI Diffusion, kritamcp, Pigment.O on;
  Blender Layer off).
- Optional first-run model fetch for visionml BiRefNet-Dynamic; never bundle
  GB-scale weights into the ZIP.
- GitHub Release on the fork; README "Differences" update; optional website
  note per the release ritual in AGENTS.md.
- Exit: named release ZIP installs to a fully integrated Solstice.

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
