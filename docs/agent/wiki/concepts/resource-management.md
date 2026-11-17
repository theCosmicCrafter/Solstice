---
type: concept
updated: 2026-11-16
sources:
  - resource audit of startup, memory ceilings, GPU fallback, AI/ML eager work, thread pools (2026-11-16)
  - libs/image/kis_image_config.cpp, libs/ui/KisApplication.cpp
  - libs/image/gpu/KisGpuTileBackend.cpp, libs/gpu/KisGpuContext.cpp
  - plugins/python/ai_diffusion/eventloop.py, plugins/python/kritamcp/__init__.py
---

# Resource management and graceful degradation

Product contract (user direction, 2026-11-16): DaVinci-Resolve-style — the
app must open and run on modest hardware; advanced features tax the machine
only when used and degrade to CPU otherwise. Minimum requirements must not
grow because a feature exists.

## What already degrades correctly (verified)

- **GPU engine, no runtime hard-fail.** `KisGpuContext::create()` returns
  null at every failure stage and stores `s_unavailableReason`; every
  caller (`KisGpuMergeBatch::tryAdd`, `KisGpuBrushPainter::supports`, tile
  hooks) checks availability and takes the CPU path. The startup probe runs
  once on a `QThreadPool` thread. Device gate: NVIDIA Blackwell
  (`deviceId >= 0x2B00`) is "verified"; anything else needs the user's
  unverified-device opt-in or `KRITA_GPU_ALLOW_UNVERIFIED_DEVICE=1`.
  Build side: `KRITA_GPU_ENGINE=ON` makes configure fail without the SDK —
  *build-time* required, *runtime* graceful. That split is the model.
- **VisionML:** model weights lazy (loaded inside `if (!weights)` at first
  inference), `backend_init` failure retries CPU with a warning; models
  ship at build time, never downloaded at runtime; GPU models unload VRAM
  after compute.
- **Memory ceilings are sane for 8–16 GB:** tile hard limit 50 % RAM,
  soft 2 % of hard, swap ≤ 4096 MiB; preview caches bounded (300 images,
  256 MiB disk prune); asset-library thumbnails 64 MiB cap; GPU tile
  backend budget min(8 GiB, deviceLocal/4), `KRITA_GPU_TILE_BUDGET_MIB`
  override. Undo is count-based (200), not byte-bounded.
- **Threads bounded:** per-image updater pool = idealThreadCount (lazy,
  expires idle); small dedicated pools capped (recent icons 2, stroke
  monitor 1); no unbounded `new QThread` found.
- **G'MIC** instantiates only on first filter invocation.

## Findings — eager work on machines that never use the feature

- **`ai_diffusion` and `kritamcp` are enabled by default**
  (`X-Solstice-EnabledByDefault` + embedded kritarc): a 20 ms asyncio pump,
  ComfyUI connect attempts at startup, an always-on `127.0.0.1:5678` HTTP
  listener + 50 ms command timer, and per-document pollers — all before the
  user ever opens the AI docker. Highest-value lazy-load target; gating is
  a product decision (the bridge is also the dev/test surface).
- **~20 Python plugins import synchronously** at startup; all dock widgets
  (incl. ai_diffusion's 7 stacked sub-widgets) are constructed at
  main-window creation regardless of visibility.
- `KisResourceLocator::synchronizeDb()` scans storages on the GUI thread at
  startup (upstream; incremental, so cost scales with changed files).
- `colorDepthDef=F32` embedded default: every new document is 4× per-pixel
  memory vs U8 — a product decision that sets the memory floor; flagged,
  not changed.
- `ai_diffusion` `local_assets` first-use `rglob("*")` runs on the GUI
  thread — one hitch on huge roots; could be threaded.
- Possible (unverified) port collision: kritamcp `:5678` vs ai_diffusion
  debugpy port — only if `debugpy/src` were shipped (it is not vendored).

## Pitfall note

`KisWeakSharedPtr::data()` on an inconsistent (expired/mid-construction)
ref logs a backtrace + asserts — never use it for liveness. Use
`isValid()`/`toStrongRef()`; for construction-state checks use an
assert-free marker (`KisNodeFacade::root()`), not `rootLayer()` which
asserts.
