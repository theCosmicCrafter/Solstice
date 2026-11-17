# DevOps: environments & toolchains — agent reference

Master doc: `devops.md`. CI specifics: `devops-ci.md`,
`github-actions.md`. Build/test commands: `development-workflow.md`.

## Environment matrix

| Environment | Where | Purpose | Toolchain |
| --- | --- | --- | --- |
| Local dev | `<repository-root>` (source), `<krita-dev-root>\_build`, `<krita-dev-root>\_install`, `<krita-dev-root>\env.bat` | Day-to-day compile/test/install; e2e and manual checks | LLVM-MinGW, CMake/ninja, Vulkan SDK, Python 3.13 — pinned in `build-tools/toolchains/windows-dev.lock.json` |
| CI hosted | GitHub `windows-2022` runner, isolated `RUNNER_TEMP\solstice` root | Reproducible ZIP builds; sanity gate; dep-mirror check | Same lock file's CI subset: KDE dep packages (87, lock-pinned), LLVM-MinGW 20251118, Vulkan SDK 1.4.357.0, Python 3.13, hash-pinned build modules |
| Local GPU / release | The RTX 5090 machine (this dev box; also `D:\krita` runtime deps, `D:\models` etc.) | CUDA/TRT builds, GPU e2e, live-engine work, release packaging for GPU features | Local set **plus** CUDA Toolkit, TensorRT, Rust stable + `cbindgen`, glslc/dxc (Vulkan SDK), optional FFmpeg dev |

Phases 1-2 of `rendering-art-engine-megaplan.md` need only the base local
set. CUDA/TensorRT gate Phase 3; Rust+cbindgen gates Phase 4 C-FFI work.
`build-tools/toolchains/check-local-toolchains.py` reports
installed/missing/mismatched versions against the lock — run it before
starting a gated phase, **inside an `env.bat` shell** (LLVM-MinGW and the
Vulkan SDK are scoped to the dev environment's PATH, not global; a global
shell reports them missing).

## Toolchain pins

Canonical list: `build-tools/toolchains/windows-dev.lock.json`.

| Tool | Pinned | Needed for |
| --- | --- | --- |
| LLVM-MinGW | 20251118 (ucrt-x86_64) | All C++ builds (matches `krita-windows-clang21` dep image) |
| Vulkan SDK | 1.4.357.0 | `kritagpu` glslc shader compile, VisionML Vulkan runtime |
| Python | 3.13 | pykrita, build scripts, e2e |
| CMake / ninja | floor versions in lock | Configure/build |
| CUDA Toolkit | 13.x | Phase 3 `librediffusion` (`sm_120`) |
| TensorRT | 11.x (Enterprise, cuda-13.x; engines TRT-major-locked — 10.x too old) | Phase 3 `librediffusion` engines, Sentinel `nvngx_vsr` concepts |
| Rust stable + `cbindgen` | pin in lock | Phase 4 static C-FFI crates (`vectorcraft`, `craft-ai-integrations`, `craft-libs`) |
| glslc / dxc | ships with Vulkan SDK | WGSL->GLSL/Vulkan shader ports (Phases 3-5) |

The CI dep lock (`build-tools/github-actions/windows-dependencies.lock.json`)
covers only the hosted KDE package set — the local lock is additive, not a
replacement.

## Machine-local asset roots (optional, auto-discovered)

Documented defaults on this machine; nothing downloads them and the app
must work without them (zero-sidecar rule, megaplan decision #4):

| Path | Contents | Consumer |
| --- | --- | --- |
| `D:\krita\share\krita\visionml\models` (+ `%APPDATA%\krita\visionml\models`) | VisionML GGUFs (SAM/MIGAN/BiRefNet/DepthAnything/ESRGAN) | `plugins/visionml` |
| `D:\krita\ComfyUI` | External ComfyUI install (optional heavyweight backend) | `ai_diffusion` ComfyUI path |
| `D:\models` | Checkpoints/LoRAs (`sam3.1_multiplex_fp16`, `BiRefNet_*`, `BEN2`, `qwen_image_2.1*`, …), `Poses` (22,411), `LUTs` (10,383 `.cube`) | `ai_diffusion`, segmentation, Look Engine |
| `D:\wildcards` | 2,325 prompt wildcard files | Prompt composer |
| `E:\AI_Art\PureRefBoards` | `.pur` 1.x/2.x reference boards | Phase 4 `.pur` importer |
| `F:\lookfx`, `F:\AIPrograms\_Dev_Projects\flarecore` | LookFX + Flarecore sources (`Apache-2.0`) | Phase 5 port |

Keep these paths out of committed code; reference them only in docs as
"documented local defaults" or via the `<...>` placeholder style already
used by `AGENTS.md`.

## Environment variables

| Variable | Set by | Purpose |
| --- | --- | --- |
| `KRITAMCP_TOKEN` | user (kritarc `[kritamcp] token` or sidecar fallback) | kritamcp bridge bearer auth |
| `SOLSTICE_E2E_ATTACH` | test runner | e2e drives an already-running instance |
| `SOLSTICE_EXE` | test runner | override binary path for e2e |
| `SOLSTICE_SOAK_ITERS` | test runner | e2e soak loop scale (default 5) |
| `SOLSTICE_TEST_CONTROL_PORT` / `_TOKEN` / `_ROOT` | user/test | upstream `TestControlMcpServer` config (Phase 2 merge target) |
| `PYTHONPATH` | `env.bat` / CI `build-windows.ps1` | pykrita SIP/PyQt6 discovery (`<install>\lib\site-packages`) |

## Rules

- A running `solstice.exe` locks DLLs — check `tasklist /FI "IMAGENAME eq
  solstice.exe"` before install; never kill the app (`development-workflow.md`).
- CI never uses the developer's `env.bat`, `_build`, `_install`, or active
  `kritarc` — same rule applies to any local automation
  (`package-local.ps1` resolves its own paths).
- New machine prerequisites land in `windows-dev.lock.json` first, then in
  this table.
