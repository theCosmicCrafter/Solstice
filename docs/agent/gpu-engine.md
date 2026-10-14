# GPU Engine — agent development notes

Design document and technical reference for moving Solstice's image engine
(compositing, filters, transforms, and painting) from the CPU to Vulkan
compute on the GPU. User-facing status is in `docs/gpu-engine.md`.

## Status and locations

Where this document says "below" or "details below" about a phase, the
details are in the phase records, now in the wiki history pages (see
"Phase history").

- Branch: `krita-sol-gpu`, created from `krita-sol` at `890aadc2ba`; since
  2026-10-13 development continues on `main`, created from it.
  Upstream Krita synchronization is intentionally abandoned on this branch.
- GPU brush on by default since 2026-10-10 (`Solstice/GpuBrush`, default
  true, read once per process; `KRITA_GPU_BRUSH` 1/0 overrides), after the
  input-to-display re-baseline (about 5 ms against 18–22 ms on the CPU;
  `gpu-work-priorities.md`). The CPU brush's minimum update period went from
  10 ms to 0 (adaptive period kept; `KRITA_CPU_BRUSH_MIN_UPDATE_MS`), and the
  stroke's first update and the after-batch re-check (4.88, 4.89) apply to
  every path. GPU dab descriptions are decided once per stroke
  (`KisBrushOpResources::gpuDabs`) instead of reading the environment for
  every dab (see `gpu-work-priorities.md`, 2026-10-10).
- Masking brush on the GPU (phase 4.101, 2026-10-10; section "Masking
  brush"): RGBA32F strokes with a masking brush in one of the ten modes
  without strength use the GPU brush; the masking composite runs on the GPU
  (`KisGpuMaskingWorker`), bit-identical to the CPU; RGBA16F strokes too
  since phase 4.106 (2026-10-12). The ten modes are all the masking brush
  offers (`KisMaskingBrushCompositeOpFactory::supportedCompositeOpIds()`;
  the strength modes are for textures only). Other masked strokes
  (no shaderFloat64, `KRITA_GPU_MASKING=0`, LOD) stay on
  the CPU brush with a CPU Wash preview and merge
  (`KisIndirectPaintingSupport::temporaryTargetPaintedOnCpu()`).
- Textured dabs on the GPU (phase 4.102, 2026-10-11; section "Textured
  dabs"): RGBA32F circle dabs with a pattern texture in one of the 14
  alpha-only modes are generated and textured on the GPU; Lightness,
  Gradient, sharpness and RGBA16F stay on the CPU (RGBA16F: phase 4.105).
- RGBA16F image tips and textures on the GPU (phase 4.105, 2026-10-11;
  section "RGBA16F image tips and textures").
- Every other blend mode except Dissolve, Marker, Destination In/Atop and
  Divisive Modulo (and its Continuous variants) on the GPU (phase 4.108, 2026-10-13; section "All
  other blend modes").
- Remaining blend modes on the GPU (phase 4.107, 2026-10-12; section
  "Remaining blend modes"): RGBA16F brushes in every GPU blend mode, and the
  HSI/HSL/HSV families plus HSY Tint and Increase/Decrease
  Saturation/Luminosity for layers and brushes in both formats.
- Image tip dabs on the GPU (phase 4.103, 2026-10-11; section "Image tip
  dabs"): RGBA32F dabs of predefined (GBR, PNG, SVG, ABR) tips in alpha-mask
  mode are generated on the GPU, also textured; the CPU now transforms
  image tips with the same deterministic code instead of QPainter (at most
  one 8-bit step from upstream Krita). Phase 4.104 (2026-10-11): the image
  stamp (colour), Lightness map and Gradient map applications too.
- Phase: **4 (brush prototype), opt-in; phases 4.1–4.5 manually checked,
  phases 4.6 and 4.8 add brush-job and full stroke-strategy coverage.** Phase
  4.7 is also manually confirmed, with combined four-pass painting recorded
  for Normal/Alpha Darken, selection and alpha lock. Phase 4.8 batches Wash
  readbacks; the user confirmed correctness but reported Wash mirror lag
  (Buildup appeared unaffected). Phase 4.9 accelerates unrestricted Wash
  preview; the user confirmed improved response near the mirror intersection,
  but distant mirror lag remains in both Wash and Buildup. Phase 4.10 combines
  distant passes using sparse destination tiles; the user confirmed large
  improvements in ordinary Wash and Buildup. Alpha Lock still causes a long
  delay after pen release in both modes. Phase 4.11 addresses speculative CPU
  clone readbacks after stroke commits, but did not resolve the reported lag.
  The user clarified that strokes keep catching up after release at 200-300px
  and above. Phase 4.12 bounds brush batches by actual source bytes to avoid
  GPU staging refusals. The user confirmed residual lag and explicitly deferred
  further large-brush mirror optimization on 2026-10-03 as a rare use case.
  Retain the tested improvements; do not resume this investigation without a
  new request. This deferral applies only to this performance issue. Phase 4.13
  adds GPU Erase dab compositing for unrestricted-channel RGBA32F pixel brushes;
  the user confirmed the manual eraser check. Phase 4.14 extends unrestricted
  RGBA32F Wash eraser preview compositing to the GPU; the user confirmed the
  manual check without issues. Phase 4.15 adds GPU final Wash merging for
  unrestricted Normal/Erase RGBA32F paint layers; the user confirmed the manual
  test passed. Phase 4.16 adds selection coverage to Normal/Erase Wash previews
  and final merges; the user confirmed the manual selection check passed.
  Phases 4.17-4.19 are implemented together: Wash Alpha Lock, Wash RGB channel
  restrictions with selections, and CPU-compatible Erase channel handling in
  both direct and Wash painting. The user confirmed the combined manual check
  passed. Phases 4.20-4.22 extend the existing separable layer blend modes to
  direct brush dabs and Wash preview/final merging, including selections and
  channel restrictions. The user confirmed their combined manual test passed.
  Phases 4.23-4.25 pipeline brush submissions through three reusable contexts,
  reuse upload allocations in size buckets and cap their total at 64 MiB.
  Their combined manual check passed, as confirmed by the user. Phases 4.26-4.28
  add Linear Burn, Linear Light and Pin Light to float layer compositing,
  RGBA32F Buildup dabs and Wash preview/final merging; the user confirmed their
  combined manual check passed. Phases 4.29-4.31 add the separable and HSY blend
  families described below as one bundle; the user confirmed the combined
  manual check passed.
  Phases 4.32-4.33 batch ordinary bulk CPU reads and distinguish initial,
  CPU-to-GPU transition and resident projection timings (details below).
  The subsequent manual report identified drawing catch-up with textured
  brushes at 150px and above; the screenshot also has Masked Brush enabled.
  Phases 4.34-4.35 address masked-stroke readbacks and staging reallocation
  pressure, with a new textured/masked full-stroke regression matrix.
  The user confirmed almost no lag at 150px and acceptable gradual lag above
  about 250px. Further tuning of that reported case is not currently needed.
  Phases 4.36-4.37 add per-channel layer projection in F32/F16 and align the
  established F16 generic blend math with CPU half arithmetic (details below).
  The user confirmed the combined real-app check passed.
  Phases 4.38-4.40 reduce CPU-to-GPU transition costs with shared upload
  allocations, bounded retirement outside the shared lock, and host-cached
  transfer-only staging. The user confirmed their real-app check passed.
  Phase 4.41 batches voluntary tile eviction readbacks while retaining busy
  tiles and GPU-only data after failed transfers. The user confirmed its
  real-app check passed. Details and checks are below.
  Phase 4.42 refreshes all README benchmark workloads and ordinary complete
  strokes on the current build, with a strengthened canvas benchmark.
  It changes measurement coverage only; no application DLL changes.
  Phases 4.43-4.45 reduce ordinary-stroke tile costs: GPU COW snapshot copying,
  whole-tile sharing without readback, and batched partial-clear edge reads.
  Controlled short-stroke results and regression coverage are documented below;
  the user confirmed this bundle's real-app test passed.
  Phase 4.46 adds RGBA16F Normal/Erase Buildup with selections and channel
  locks; the user confirmed its real-app test passed. Phases 4.47-4.48 add
  F16 Alpha Darken and Normal/Erase Wash preview/final merge; the user confirmed
  this bundle passed. Phases 4.49-4.50 extend F16 Buildup/Wash to basic generic
  modes through Pin Light; the user confirmed this bundle passed. Phases
  4.51-4.52 add Soft Light SVG, Color Dodge/Burn and HSY color modes in F16
  Buildup/Wash; the user confirmed the real-app checks passed. Other extended
  F16 brush modes retain CPU fallback. Phase 4.53 reuses completed projection
  work contexts first and permits three pending serial submissions; details below.
  The user confirmed phase 4.53 and the CPU filter/FFT readback bundle
  (4.54-4.55). Phases 4.56-4.57 batch affine transform and layer-flip readbacks;
  details and verification are below. Filters and transforms then still
  executed their calculations on the CPU; phases 4.94, 4.96, 4.97 and 4.98
  later moved the affine passes, Puppet Warp's mesh rendering, Liquify and the
  Gaussian convolution to the GPU.
  Phases
  0–3 are implemented, including real-app canvas and memory-budget checks;
  the phase 3.3 dialog checklist remains documented separately. Layer stacks of RGBA float
  images are composited on the GPU inside `KisAsyncMerger`, directly into
  GPU-resident projection tiles (3.1), the canvas textures are filled from
  those tiles on the GPU through a buffer shared with OpenGL (3.2), and the
  engine has a user setting, document conversion, and failure warnings
  (3.3). The GPU engine is **on by default** since 2026-10-07: the embedded
  `krita/data/kritarc` sets `Solstice/GpuEngine=true`, and the code fallback
  stays off, so tests are unaffected (`docs/agent/settings-location.md`,
  phase 4). It is set in Preferences → Performance → GPU Engine;
  `KRITA_GPU_PROJECTION=1/0` overrides it. With it off, Krita's runtime
  behavior is unchanged.
- Implementation:
  - `libs/gpu/` (target `kritagpu`): Vulkan foundation, shaders in
    `libs/gpu/shaders/`, tests in `libs/gpu/tests/`;
  - `libs/image/gpu/` (part of `kritaimage`): `KisGpuTileBackend`,
    `KisGpuTileAccess`, `KisGpuProjectionCompositor`, `KisGpuMergeBatch`;
    tests `libs/image/tests/KisGpuPaintDeviceTest.cpp`,
    `KisGpuProjectionTest.cpp`;
  - `libs/image/tiles3/KisTileGpuState.h`, `KisTileGpuHooksStub.cpp`.
  - `plugins/paintops/defaultpaintops/brush/tests/KisGpuBrushJobsTest.cpp`:
    real brush generation, cache and asynchronous update jobs on the image
    worker scheduler, linked with `kritadefaultpaintops_static`.
- Hooks in upstream files: `libs/CMakeLists.txt` (`add_subdirectory( gpu )`),
  `libs/image/CMakeLists.txt` (sources, `kritagpu` link,
  `HAVE_KRITA_GPU_ENGINE`), `libs/image/tests/CMakeLists.txt`, and the tile
  engine: `tiles3/kis_tile_data_interface.h`, `kis_tile_data.h`,
  `kis_tile_data.cc`, `kis_tile.h`, `kis_tile.cc`, `kis_tile_data_store.cc`,
  `kis_tiled_data_manager.cc` (automatic interleaved/planar bulk readback),
  `libs/ui/tool/strokes/kis_painter_based_stroke_strategy.cpp` (batch prefetch
  before CPU masking-brush patch jobs),
  the merger: `kis_async_merger.h`, `kis_async_merger.cpp`, the scheduler:
  `kis_updater_context.cpp`, and the canvas: `libs/ui/opengl/KisOpenGLUpdateInfoBuilder.{h,cpp}`,
  `kis_texture_tile.{h,cpp}`, `kis_texture_tile_update_info.h`,
  `kis_opengl_image_textures.cpp`, `libs/ui/CMakeLists.txt`,
  `libs/ui/tests/CMakeLists.txt`, and the user interface (3.3):
  `libs/ui/KisMainWindow.cpp` (`KisGpuEngineUi::install()` in the
  constructor, `offerConversion()` in `slotLoadCompleted()`,
  `confirmSave()` in `saveDocument()`), `libs/ui/dialogs/kis_dlg_preferences.{h,cc}`
  (`KisGpuEngineSettingsWidget` in the Performance tab),
  `libs/ui/KisImportExportManager.cpp` (`mayFinishExport()` in
  `doExportImpl()`), `plugins/impex/libkra/tests/CMakeLists.txt`, and the
  Gaussian filters (phase 4.98): `libs/image/kis_gaussian_kernel.{h,cpp}`
  (`runsOnGpu()`, the GPU branch of `applyGaussian()`),
  `libs/image/filter/kis_filter.{h,cc}` (`prefersSingleCall()`),
  `libs/ui/tool/strokes/kis_filter_stroke_strategy.cpp`,
  `plugins/filters/blur/kis_gaussian_blur_filter.{h,cpp}`.

## Decisions approved by the user

| Topic | Decision |
| --- | --- |
| Direction | Move the engine as a whole to the GPU. Upstream sync is given up on this branch. |
| GPU API | Vulkan. |
| Working color space | RGBA float only (32-bit, optionally 16-bit storage). Other color models (8/16-bit integer, CMYK, Lab, gray) are converted on import/export, not painted in. |
| Hardware | NVIDIA Blackwell (RTX 50 series, RTX PRO Blackwell) is the only tested target. Other GPUs are untested and have no compatibility guarantee, but nothing Blackwell-specific is required: device selection accepts any GPU with the Vulkan 1.3 features in "Phase 0 technology decisions", and `isBlackwell()` only logs a warning. Docs say "untested", never that other GPUs fall back to the CPU (user decision, 2026-10-14). |
| Priorities | All of brush latency, many-layer documents, and filters matter; the phase order below is chosen by dependency, not by priority. |
| Implementation workflow | Bundle several related implementation tasks and their automated checks into one manual-test handoff, as requested by the user. |

## Current CPU architecture (what is being replaced)

Line numbers refer to `890aadc2ba`.

- **Tiles.** 64x64 pixels (`libs/image/tiles3/kis_tile_data_interface.h:24`).
  `KisPaintDevice` → `KisPaintDeviceData` → `KisDataManager`
  (= `KisTiledDataManager`) → `KisTile` → shared `KisTileData` (raw bytes,
  `pixelSize` from the color space). `KisTileDataStore`, `KisTileDataPooler`
  (pre-cloning), `KisTileDataSwapper` (LZF swap to a memory-mapped file).
  Animation frames and LOD planes are separate `KisPaintDeviceData` objects.
- **Undo.** `KisTransaction` → `KisMementoManager`. Tile-level copy-on-write:
  `KisTile::lockForWrite` clones shared tile data and registers the old data as
  a memento (`kis_tile.cc:221`). Undo cost is O(changed tiles).
- **Projection.** `KisImage` update → `KisUpdateScheduler` →
  `KisSimpleUpdateQueue` (splits into 512x512 patches) → `KisUpdaterContext`
  thread pool → `KisAsyncMerger::startMerge` → `KisLayerProjectionPlane::apply`
  → `KisPainter::bitBlt` → `KoColorSpace::bitBlt` → `KoCompositeOp::composite`.
  Adjustment layers and filter masks call `KisFilter::process` inside merge
  jobs.
- **Composite ops.** `libs/pigment/compositeops`. Over, alpha-darken, and copy
  for RGBA F32 use xsimd kernels (`KoOptimizedCompositeOpOver128.h`).
- **Painting.** `FreehandStrokeStrategy` / `KisPainterBasedStrokeStrategy`
  open a transaction, paint ops render dabs (`KisDabRenderingExecutor`), and
  `KisPainter::bltFixed` composites them into the indirect-painting temporary
  target, which is merged into the layer at stroke end.
- **Filters.** `KisFilter::processImpl(device, rect, config)`, in place on one
  device; parallelism comes only from patch splitting.
- **Canvas.** `KisOpenGLImageTextures::updateCache` reads projection bytes per
  256x256 texture tile, converts them with LCMS to the display profile on the
  CPU, and uploads them with `glTexSubImage2D`. OCIO runs in the fragment
  shader. The Windows renderer is desktop OpenGL or ANGLE
  (`KisOpenGL::OpenGLRenderer`); this machine uses `OpenGLRenderer=desktop`.

## Target architecture

```
krita (canvas, tools, dockers)            libs/ui
   │  OpenGL canvas samples shared textures (KisGpuGLSharedImage)
   ▼
GPU image backend (planned)               libs/image  (phase 1+)
   │  GPU-resident paint devices, undo, projection, strokes, filters
   ▼
kritagpu                                   libs/gpu    (phase 0, exists)
      device, queue, timeline, buffers, tile pool, pipelines, interop
```

### Data model (implemented in phase 1)

GPU residency is a property of `KisTileData`, not a separate GPU device. The
original plan (a parallel sparse *tile → slot* map per device with its own undo)
was replaced because Krita's tile engine already provides everything a GPU
device needs: sparse tiles, a shared default tile, copy-on-write, mementos,
frames, and LOD planes. Attaching a GPU copy to each tile data reuses all of
it unchanged.

- A GPU tile is a 64x64 slot in `KisGpuTilePool` (RGBA32F 64 KiB, RGBA16F
  32 KiB). The size matches `KisTileData`, so a CPU tile maps 1:1 onto a slot.
- `KisTileData::gpuState()` is `nullptr` until the tile data is first used on
  the GPU; then it points to a `KisTileGpuState` (`tiles3/KisTileGpuState.h`):
  slot, one atomic word holding the `CpuValid`/`GpuValid`/`ContentLost`
  flags and the CPU write generation, and the timeline value of the last GPU
  use. Non-GPU tile data pays one pointer and one predictable branch.
- The generation is bumped by `KisTile::lockForWrite` **and**
  `KisTile::unlockForWrite`, so a snapshot taken while a CPU write was in
  progress (a stroke painting into a layer that a merge reads) is never
  published as the tile's GPU copy.
- Flags and generation are updated only by compare-and-swap on that word:
  `notifyCpuWrite()` bumps the generation and clears `GpuValid` in one step,
  and an upload is published with `markGpuValidIfGeneration()`, which fails
  if any CPU write happened since the upload was recorded. A check followed
  by a separate store (the earlier implementation) could publish stale GPU
  content.
- Synchronization rules:
  - CPU read or write lock (`KisTile::lockForRead/Write` →
    `KisTileData::blockSwapping`) downloads the GPU copy first if the CPU copy
    is stale (`KisTileGpuHooks::ensureCpuValid` → `KisGpuTileBackend`);
  - `KisTile::lockForWrite` marks the GPU copy stale
    (`KisTileData::notifyCpuWrite`);
  - a GPU write (`KisGpuTileAccess::finish`) marks the CPU copy stale.

  All existing CPU code (iterators, `readBytes`, painters, `.kra` saving,
  scripting) therefore keeps working without modification.
- GPU-resident tile data keeps its CPU buffer even when stale. An idle slot
  can be downloaded and evicted; `KisTileDataStore::trySwapTileData` does
  this before ordinary disk swapping. Prepared or in-flight GPU accesses
  prevent eviction (see
  [Tile memory budget and eviction](wiki/history/gpu-reviews-and-memory.md#tile-memory-budget-and-eviction)).
- Only RGBA F32 (pixel size 16) and RGBA F16 (pixel size 8) devices are
  eligible (`KisGpuTileAccess::isSupported`). The tile engine itself stays
  color-space agnostic; eligibility is checked at the access level.
- Converting documents in other color models to RGBA float moves to phase 3,
  together with the user-visible GPU mode switch.

### GPU access API

`KisGpuTileAccess(device, rect, ReadOnly | ReadWrite | WriteOnly)`:

1. `prepare(commands)` fetches the tiles covering the rect, performs
   copy-on-write for writers (`KisTile::detachForExternalWrite`, the GPU twin
   of the COW branch of `lockForWrite`, including memento registration),
   allocates slots, records GPU slot-to-slot copies for COW clones into the
   caller's command list, and **snapshots** the CPU content of tiles that
   need an upload into a staging buffer (it does not record the upload).
   A COW clone for a read-write access is always made by a GPU slot copy;
   a source that is not GPU-current is uploaded once by the access (many
   cleared tiles share one default tile data, so clearing a projection costs
   one upload, not one per tile).
2. The caller records compute work on `addresses()` (one device address per
   tile, row-major over `tileGrid()`).
3. `KisGpuTileAccess::submitAndFinish(commands, {accesses...})`, under the
   backend's residency mutex: decides which snapshots are still uploaded
   and records them into `commands.preamble()` (a second command buffer
   submitted before the main one in the same batch), submits, and publishes
   flags. A snapshot is **skipped** when the tile is already GPU-valid
   (GpuValid means the slot holds the current generation, which is at least
   as new as the snapshot); a snapshot that overlapped a CPU write is used
   for this submission but **not published** (`markGpuValidIfGeneration`
   fails). Outside the mutex it then releases replaced tile data and retires
   the staging buffer. This replaced an earlier rule that refused to submit
   when an uploaded tile had changed: during painting that rule sent most
   merges back to the CPU.
4. The caller calls `device->setDirty(rect)`, as CPU writers do.

Failure handling:

- `prepare()` fails if any tile is locked by a CPU user (shared or not), if a
  slot cannot be allocated, or if staging memory cannot be allocated. Fresh
  copy-on-write clones whose content only the GPU would have produced get the
  previous content on the CPU (`restorePendingContent`), so a failed or
  refused GPU write never changes the device.
- Readback buffers are sized for the tiles actually downloaded, so the
  tile-by-tile retry of a failed batch needs one tile of memory (64 KiB
  for RGBA F32), not a full batch.
- A failed download is retried tile by tile. If it still fails (device loss,
  out of memory), the latest content of that tile is **lost**. The tile then
  shows the content the CPU last had, which can be older than several GPU
  writes, but it is never presented as normal data: the tile is marked
  `ContentLost`, `contentLossCount()` is incremented, an error is logged,
  and the backend enters the failed state (`hasFailed()`): no further GPU
  work is accepted. `isSupported()` and `prepare()` fail, and
  `submitAndFinish()` re-checks the state under the residency mutex (which
  the backend also holds while entering the failed state), so work prepared
  before the failure is not submitted and its written tiles keep their
  previous content. The merger and all other users fall back to the CPU. Phase 3 must surface the
  loss to the user (and warn before saving).
- Copy-on-write clones made for GPU writes copy the source's CPU buffer, so
  their CPU buffer is never meaningless (it holds what the CPU last had).

`KisGpuTileAccess::syncToCpu(device, rect)` downloads stale tiles in batches
of 256 per submission (use before bulk CPU reads such as saving).

### Undo

GPU writes reuse `KisMementoManager` unchanged: `detachForExternalWrite`
clones shared tile data exactly like `lockForWrite` and registers the change,
so a GPU write inside a `KisTransaction` is undone and redone by swapping
tile data, with no GPU-specific undo code. A clone of GPU-only data is made
by a GPU slot copy; a clone of CPU-valid data is a normal CPU clone that is
uploaded. Slots are returned to the pool when their tile data is destroyed,
deferred until the timeline passes the slot's `lastUse`
(`KisGpuTileBackend::destroyState` / `collectGarbage`).

Note for tests: `KisTransactionData` ignores its first `redo()` (the undo
stack calls it on push). Call `redo()` once after `endAndTake()` before
testing undo/redo.

### Projection (implemented in phase 2)

`KisAsyncMerger` composites the children of a group one by one onto
`m_currentProjection` (`compositeWithProjection`). Phase 2 batches those
composites:

- `KisGpuMergeBatch::tryAdd()` defers a leaf when it is GPU-composable: plain
  `KisLayerProjectionPlane` (no layer style), a supported blend mode, channel
  flags either empty or RGB with the alpha channel locked, the same RGBA
  F32/F16 color space as the projection, and a device offset that differs
  from the projection's by a multiple of 64 pixels, inside an image whose
  own color space is RGBA F32/F16 (`mayCompositeOnGpu`; a float group in an
  RGBA8 image stays on the CPU because the scheduler below would not keep
  its jobs apart). Anything else flushes the batch and is composited on the
  CPU exactly as before.
- The merger flushes before every step that reads the projection composited
  so far: root and `N_EXTRA` leaves, leaves that depend on lower nodes
  (adjustment layers), `writeProjection()`, and the end of `startMerge()`.
  Adjustment layers and filter masks are therefore split points, and the
  result equals immediate compositing.
- A flush runs `KisGpuProjectionCompositor::composite()`: one dispatch of
  `composite_layers.comp` iterates over all deferred layers per pixel, inside
  the clip rect only. Layer tiles are read through `KisGpuTileAccess`
  (resident; uploaded only when stale); tiles outside a layer's extent are
  skipped, like `KisLayerProjectionPlane` clips to the extent.
- **Since 3.1 the projection is GPU-authoritative.** The batch composites
  in place into the projection's tiles (`KisGpuTileAccess` read-write, clip
  rect), which become GPU-valid and CPU-stale; nothing is read back by the
  merge. CPU readers (writeProjection's partial copies, adjustment layers,
  the canvas, thumbnails, saving) download on demand, ordered after the
  merge's submission. (Phase 2 composited into scratch tiles and wrote the
  result back with `writeBytes()`.)
- Concurrent merge jobs used to be allowed when their rects were disjoint,
  even if they shared a 64x64 tile. In-place GPU writes of whole tiles make
  that unsafe, so `KisUpdaterContext::walkerIntersectsJob` compares
  tile-aligned rects (`KisGpuMergeBatch::tileAligned`) when
  `KisGpuMergeBatch::mayCompositeOnGpu(startNode)` (GPU projection enabled
  and an RGBA F32/F16 image). `tryAdd()` uses the same condition, so GPU
  merges never run without this exclusivity. The GPU path only composites
  into projections whose offset is a multiple of 64, so the image grid is
  the tile grid.
- When the canvas cannot use the GPU path (below), it reads the projection
  on the CPU: `KisOpenGLUpdateInfoBuilder::buildUpdateInfo` calls
  `KisGpuTileAccess::syncToCpu` for the update rect first, so stale tiles
  are downloaded in batches instead of one submission per tile.

### Canvas from the GPU (implemented in phase 3.2)

Before enabling uploads, `checkGLInterop()` runs
`KisGpuGLSharedBuffer::testGLInterop()` once in the actual current canvas
context. A temporary 1 MiB buffer follows the production allocation, lazy
import, and semaphore cycle. Vulkan fills it with the bit patterns of 1.0f,
then 0.5f; GL copies it to an ordinary buffer and reads back every word.
Failure (including wrong bytes with GL error 0) disables canvas interop for
the session, so the builder uses its existing batched CPU download path.
Projection compositing remains enabled and kritarc is untouched. Probe GL
objects are deleted with their context current, and pixel-unpack/copy
bindings are restored. The persistent canvas pool is not used for the probe.

The canvas keeps its OpenGL textures (`KisTextureTile`, 256 px with a
16 px border, mipmaps, filters, OCIO shaders) unchanged. Only where the
texture data comes from changes:

1. `KisOpenGLUpdateInfoBuilder::buildUpdateInfo(rect, image, …)` (the live
   canvas; the animation frame cache keeps the CPU path because it stores
   CPU pixel data) builds the tile infos as before, then asks
   `KisGpuCanvasUploader::upload()` to produce their pixels.
2. The uploader reads the projection tiles (`KisGpuTileAccess` read-only,
   current LOD plane), and `canvas_patches.comp` writes every patch into a
   `KisGpuGLSharedBuffer`: a Vulkan buffer whose memory is imported into GL
   as a pixel unpack buffer. Each patch has exactly the layout that
   `KisTextureTile::update()` uploads (`KisTextureTileUpdateInfo::
   uploadGeometry()`, now shared by both paths): at image edges the side
   margins repeat the edge rows/columns, and the corner blocks are zero,
   like the CPU code.
3. On the GUI thread `KisOpenGLImageTextures::recalculateCache()` acquires
   the shared buffers (GL waits for the Vulkan semaphore), each tile's
   `update()` calls `glTexSubImage2D`/`glTexImage2D` with the buffer bound
   as `GL_PIXEL_UNPACK_BUFFER`, and the buffers are released (GL signals a
   semaphore that the next Vulkan write of that buffer waits for).
4. Shared buffers come from a bounded pool (normally power-of-two sizes,
   ≥ 8 MiB, exact-sized when rounding would exceed the budget) and return to
   it when the last tile info referencing them dies. Idle buffers can be
   reclaimed with the importing GL share group current; see the memory
   management sections below.
   A buffer whose content GL never consumed (the update was compressed
   away) is reused after Vulkan waits for its own previous signal.
5. If GL cannot import a shared buffer (`KisGpuGLSharedBuffer::glAcquire()`
   returns false), `KisGpuCanvasUploader::acquire()` fails: the GPU canvas
   path is disabled for the session, the buffer is kept out of the pool,
   and no tile reads it through GL (`KisTextureTile::update()` asserts
   `isAcquired()`). `recalculateCache()` then calls
   `readBackFailedUploads()`: each such buffer is copied to the CPU through
   Vulkan (the copy waits for the Vulkan write and consumes its signal,
   `finishVulkanRead()`), and its tiles get the patch centers as CPU pixels
   (`KisTextureTileUpdateInfo::replaceGpuUploadWithPixels()`), so the same
   update is applied on the CPU upload path. This reads neither the
   projection (which a merge may be writing; the GUI thread must never
   read it) nor needs any rect or level-of-detail conversion: the patches
   are exactly the ones computed for this update. Only if the readback
   fails too (the GPU engine has failed) is the update dropped and the
   whole image refreshed (`refreshGraphAsync()`; the full-resolution
   bounds cover the dropped area at any level of detail).

Display conversion on the GPU (`KisGpuCanvasUploader::conversionFor`):

- equal profiles: only the storage format changes (F32/F16);
- two RGB matrix-shaper profiles (colorants and TRCs, no LUT tags, intent
  other than absolute colorimetric): linearize with the source TRC,
  multiply by `inverse(dst colorants) · src colorants`, delinearize with
  the destination TRC. The colorants must be the PCS (D50) values LCMS
  uses: `LcmsColorProfileContainer` stores them Bradford-adapted to the
  profile's media white point, so `colorantMatrix()` undoes that
  adaptation, and refuses the profile if the columns then do not sum to
  the D50 white (±0.01). Without this, profiles with different white
  points (LargeRGB D50, CIE RGB E, sRGB D65) were off by up to 0.32. Non-linear TRCs are sampled with
  `KoColorProfile::linearizeFloatValue/delinearizeFloatValue` (LCMS
  `cmsEvalToneCurveFloat`) into 4096-entry tables over [0, 1] (input
  clamped); linear TRCs are exact and unbounded. Conversion flags
  (HighQuality, black point compensation) do not change matrix-shaper
  results for these profiles.

Everything else uses the CPU path: soft proofing, channel selection,
non-RGBA-float projections or displays, profiles whose ICC tag table has
any AToB*/BToA*/DToB*/BToD* tag (LCMS would use the LUT; checked on
`rawData()`, unreadable data counts as LUT), absolute
colorimetric intent, no GL interop (ANGLE, another GPU), or any GPU
failure.

Limitation: values outside [0, 1] with a non-linear TRC are clamped by the
tables, while LCMS extends parametric curves. Linear-light documents (the
usual RGBA float case) and HDR displays with linear profiles are exact.
- If the GPU fails, the batch falls back to the CPU composites.

Parity criteria (`KisGpuProjectionTest`):

- RGBA F32: every channel within 2e-5 of the CPU projection (Krita computes
  intermediates in double, the shader in float). Colors of pixels with alpha
  exactly 0 are not compared (they are unspecified in Krita itself).
- RGBA F16: the shader rounds the result to half after every layer, like
  Krita compositing into an F16 device. Every channel within 4 half ULPs at
  max(|value|, 1), i.e. 4/1024 for normalized values; measured maximum 3.

Supported blend modes (`KisGpuBlendOp`, parity with Krita's RGBA float ops):
Normal (`KoOptimizedCompositeOpOver128`), Multiply, Screen, Addition / Linear
Dodge, Subtract, Darken, Lighten, Difference, Overlay, Hard Light, Exclusion,
Linear Burn, Linear Light, Pin Light, Soft Light SVG/Photoshop, Color Dodge,
Color Burn, Divide, Vivid Light, Hard Mix/default/Photoshop/Softer Photoshop,
Grain Merge/Extract, Negation and Allanon
(`KoCompositeOpGenericSC` with the `cf*` functions and their clamp policies,
including the alpha-locked branch and `KoCompositeOpBase`'s clearing of
alpha-0 destination pixels). HSY Hue, Saturation, Color, Luminosity, Darker
Color and Lighter Color use `KoCompositeOpGenericHSLFunctor` semantics, and
so do (phase 4.107) HSY Tint and Increase/Decrease Saturation/Luminosity and
the HSI, HSL and HSV families (Color, Hue, Saturation, Increase/Decrease
Saturation, Intensity/Lightness/Value, Increase/Decrease of it).
Phase 4.108 adds every other mode of Krita's RGBA float spaces except
Dissolve, Marker, Destination In/Atop, Divisive Modulo, Divisive Modulo
Continuous and Modulo Continuous ("All other blend modes").

### Canvas

- The Qt build has **no Vulkan support** (`QT_FEATURE_vulkan -1` in
  `_install/include/QtGui/qtgui-config.h`), so `QVulkanInstance`/QRhi-Vulkan
  are unavailable. The OpenGL canvas stays, and Vulkan shares images with it
  through `GL_EXT_memory_object_win32` / `GL_EXT_semaphore_win32`
  (`KisGpuGLSharedImage`).
- The projection is copied (GPU→GPU) into shared canvas textures instead of
  `readBytes` + LCMS + `glTexSubImage2D`. The LCMS display conversion moves to
  the GPU (3D LUT or matrix+TRC shader); OCIO stays in the fragment shader.
- LOD/mipmaps are produced by a GPU downsample pass.
- Requirement: desktop OpenGL on the same GPU. ANGLE (D3D11) is not supported
  in GPU mode. Since 2026-10-07 the Windows "Auto" renderer prefers desktop
  OpenGL when `KisGpuMergeBatch::isEnabled()`
  (`KisOpenGL::selectSurfaceConfig()`, `preferredAutoRenderer`; ranked after
  the driver blacklist). `krita/main.cc` falls back to "auto" instead of the
  upstream "angle" when no renderer is set.

### Threading and submission

- `KisGpuContext` owns one graphics+compute queue. Submission is serialized by
  a mutex; every submission signals the context timeline semaphore.
- Stroke and merge jobs keep running on `KisUpdaterContext` threads, but they
  *record* GPU work (per-thread `KisGpuCommandList`) instead of touching
  pixels. Completion is tracked by timeline values, not CPU waits.
- The GUI thread only performs GL work (acquire/release shared images).

### Feature gate and user interface (implemented in phase 3.3)

- `Solstice/GpuEngine` (bool; embedded default on, code fallback off; `KisGpuEngineSettings`) enables
  the engine for the whole process. `KisGpuMergeBatch::isEnabled()` decides
  once (`std::call_once`; a value set by `setEnabled()` before the first
  call wins), so a change applies after a restart: switching while merges
  run would mix GPU merges with CPU-style job exclusivity. The environment
  variable `KRITA_GPU_PROJECTION` (`1` or `0`) overrides the setting.
  Documents that are not RGBA F32/F16 always use the CPU path.
- Opening a document (`KisMainWindow::slotLoadCompleted()`, deferred until
  the view is shown) calls `KisGpuEngineUi::offerConversion()`: if the
  engine is enabled and usable (Vulkan context created, not failed) and the
  image is not RGBA float, `Solstice/GpuEngineConvertDocuments` decides:
  Ask (default; Convert / Keep, with "Do not ask again" storing the
  answer), Convert, or Keep. The conversion is
  `KisImage::convertImageColorSpace()` to
  `KisGpuEngineSettings::conversionTarget()`: RGBA F32 with the same profile
  for RGB documents (same values, same appearance), RGBA F32 with the
  default profile otherwise; internal rendering intent and flags. It is
  undoable and marks the document modified; saving keeps the new color
  space. New documents are not affected (choose RGBA float in the New
  Document dialog). Not converted: file layers (they have no color space
  conversion and stay on the CPU path through the color space mismatch),
  and layers of another color space inside an image that is already RGBA
  float (no unification).
- When the engine enters the failed state (GPU content lost),
  `KisGpuTileBackend` calls its failure listener once per process;
  `KisGpuEngineUi::install()` registers one that shows a non-modal warning
  on the GUI thread (the number of lost tiles, that the engine stopped, and
  to check documents before saving).
- `KisMainWindow::saveDocument()` calls `KisGpuEngineUi::confirmSave()`
  after the busy-image check: after a failure in this session it warns
  before every manual save, Save As, export, and save on close (not
  autosave), offering Save as New File (default), Save Anyway, or Cancel.
  Agreeing records the loss count the user accepted. The loss count is
  process-wide; the warning cannot say which document was affected.
- The background save itself can lose content: the tile compressor locks
  tiles of the cloned document, which downloads GPU-only tiles, and a
  failed download leaves the old CPU content without telling the saver.
  So `KisImportExportManager::doExportImpl()` asks
  `KisGpuEngineUi::mayFinishExport()` after `filter->convert()` and before
  the temporary file replaces the target (`QSaveFile::commit()`, or the
  copy on Windows/macOS): if more tiles were lost than the user accepted,
  the export fails with `ErrorWhileWriting` and the target is untouched;
  the next save asks first. Autosaves (`isAutosaving()`) are exempt.
  Filters that write the target themselves (`supportsIO()` false) are not
  covered.
- Preferences → Performance → General → GPU Engine
  (`KisGpuEngineSettingsWidget`): the two settings and the current status
  (active device, stopped, enabled but not started, not used, or not built
  with Vulkan).

### Masking brush (phase 4.101)

A brush with a masking brush paints two devices per stroke
(`KisMaskingBrushRenderer`): the stroke device (the main brush, same color
space as the target) and a GrayA8 mask device (the masking brush). Each
update copies the stroke into the target and composites the mask into the
target's alpha (`KisMaskingBrushCompositeOp`). Until phase 4.101 this
composite ran on the CPU, so masked strokes stayed on the CPU brush.

- `libs/gpu/shaders/masking_composite.comp` + `KisGpuMaskingCompositePass.*`:
  one workgroup per 64x64 tile; inside the rect each pixel becomes the
  stroke pixel (zero for an absent stroke tile) with
  `a = composite(mode, uint8ToFloat[mask], a)`. Modes 0-9 follow
  `KisMaskingBrushCompositeFuncTypes` without strength: Multiply, Darken,
  Overlay, Color Dodge, Color Burn, Linear Burn, Linear Dodge, Hard Mix
  (Photoshop), Hard Mix Softer (Photoshop), Subtract.
- Parity: `KoColorSpaceMathsTraits<float>::compositetype` is float, so the
  shader uses `precise float` for products and sums (no double chains:
  Overlay, Linear Burn and Hard Mix Softer differed by 1 ulp with double).
  Divisions are computed in double and cast, which rounds them correctly.
  Dodge/Burn reproduce the CPU's NaN/inf handling; the fuzzy tests follow
  `qFuzzyIsNull` (<= 0.00001) and `qFuzzyCompare`. The mask byte is
  `multiply(gray, alpha)` computed on the CPU, and the byte-to-float table is
  uploaded from `KoColorSpaceMaths<quint8, float>::scaleToA` (`KoLuts`).
  Requires `shaderFloat64`.
- `libs/image/gpu/KisGpuMaskingWorker.*`: `supports(colorSpace, opId)` (GPU
  brush enabled, RGBA32F, a supported mode, float64, `KRITA_GPU_MASKING` not
  `0`); `apply(stroke, mask, dst, rect, opId)` reads the mask bytes, takes
  the stroke ReadOnly and the target ReadWrite (their tile grids must match),
  records, submits and waits. It returns false without changing the devices
  when it cannot run; the caller then composites on the CPU. `runCount()`
  for tests.
- `KisPainterBasedStrokeStrategy`: `m_gpuMasking` is decided once per stroke
  (LOD 0, `paintbrush` preset, `supports()`); it marks the temporary target
  as GPU-painted (`setTemporaryTargetPaintedOnCpu(!m_gpuMasking)`, also on
  resume). `doMaskingBrushUpdates()` then runs one sequential job over the
  union of the rects; on failure it falls back to
  `updateProjection(rc)` per rect.
- `KisBrushOp`: a masked stroke keeps the GPU brush only when `supports()`
  holds for the masking composite op. Masked strokes keep the adaptive update
  period instead of the GPU minimum (4.88): each batch also costs a masking
  composite and a projection merge, and with CPU-generated (textured) dabs the
  GPU minimum made about seven times more batches (300px textured masked:
  113 -> 89 ms; CPU 78 ms, GPU projection only 90 ms).
  Re-measured after phase 4.102 (textured dabs on the GPU, 2026-10-11):
  with the GPU minimum period for masked strokes too, masked and dense
  mirrored rows finished within noise of the adaptive period (for example
  300px masked textured 36.9 / 35.8 ms, 80px dense mirrored textured
  30.5 / 29.9 ms) with the same submission counts. The adaptive period
  stays: it still protects masked strokes whose dabs are generated on the
  CPU (Lightness or Gradient textures, sharpness, RGBA16F textures).
- Measurements (`KisGpuStrokeTest`, median of 5, no validation, ms; CPU /
  GPU projection / GPU brush): masked 300px 51 / 56 / 30; masked 300px
  textured 78 / 90 / 89; masked 150px 11 / 16 / 13; temporary dense rows
  (spacing 0.03, both mirrors) 40px 25 / 32 / 29, 40px textured
  29 / 36 / 36, 80px 39 / 47 / 27, 80px textured 49 / 55 / 41.
- Tests: `KisGpuStrokeTest::testMaskingCompositeMatchesCpu` (all ten modes,
  bit-exact against `KisMaskingBrushCompositeOpFactory`, pixels outside the
  rect kept) and the masked rows of `testTexturedMaskedStroke` (RGBA32F masked
  rows on the GPU brush path must run the worker; others must not).
- RGBA16F (phase 4.106, 2026-10-12, user request): the `TILE_F16` build
  `masking_composite_rgba16f` (half tiles) computes
  `KisMaskingBrushCompositeOp<half, mode>` with the half rules of the
  textured dabs ("RGBA16F image tips and textures"): `mul` and `div` in
  double, `half +- half` in float, every half result double -> float ->
  half, the half fuzzy thresholds (0.002, 0.001); the mask byte is scaled
  with `KoColorSpaceMaths<quint8, half>` (the worker passes that table).
  `KisGpuMaskingWorker::supports()` accepts RGBA F16, so masked RGBA16F
  strokes use the GPU brush. Tests: RGBA16F rows of
  `testMaskingCompositeMatchesCpu` (every mode bit-identical, including
  alphas at the half thresholds) and of `testTexturedMaskedStroke`.
  Measurements (median of 5, no validation, ms; CPU / GPU projection / GPU
  brush; before, RGBA16F masked strokes had only the CPU brush): 150px
  23.8 / 19.0 / 13.6, 150px textured 25.7 / 24.2 / 14.3, 300px
  91.7 / 91.5 / 35.9, 300px textured 129.5 / 132.4 / 36.4.
- Not covered: LOD previews. (The strength modes, Height, Linear Height and
  their Photoshop variants, are texture modes only: the masking brush never
  offers them, `supportedCompositeOpIds()`; textures run on the GPU since
  phases 4.102 and 4.105.) Painting the mask itself stays on
  the CPU (an alpha device; GPU painting was measured and deferred on
  2026-10-11, `gpu-work-priorities.md`).
- Manual check reported OK by the user on 2026-10-10.

### Textured dabs (phase 4.102)

Before phase 4.102 a dab with a pattern texture (`KisTextureOption`) was
generated and textured on the CPU and uploaded as pixels: the texture made
the dab "postprocessed" (`DabGenerationInfo::needsPostprocessing`), which
excluded it from GPU descriptions. Textured RGBA32F strokes now describe the
circle and its texture, and the GPU evaluates both.

- Scope: RGBA32F circle auto-brush dabs (default, Gaussian, Soft) whose only
  postprocessing is a texture in one of the 14 alpha-only modes (Multiply,
  Subtract, Darken, Overlay, Color Dodge, Color Burn, Linear Dodge, Linear
  Burn, Hard Mix and Hard Mix Softer (Photoshop), Height, Linear Height, Height
  and Linear Height (Photoshop)), hard and soft texturing, any strength
  (pressure). Lightness and Gradient (they change the dab color), sharpness,
  RGBA16F dabs and predefined (image) tips stay on the CPU (image tips:
  phase 4.103; RGBA16F: phase 4.105).
- Description: `KisProceduralDabTexture` (`libs/image/KisProceduralCircleDab.h`),
  attached as `KisProceduralCircleDab::texture`: the alpha8 pattern texels
  (`KisTextureMaskInfo::alphaBytes()`, computed once in `recalculateMask()`),
  the patch origin `offset % size - effectiveOffset` of `apply()`, the
  `KisMaskingBrushCompositeFuncTypes` mode, the soft flag, the composite
  function constants as their constructors compute them in float (strength,
  `0.99 * strength` for Height and Linear Height, its inverse, the
  10x/9x Photoshop weights), and the brush tip's reflection flips.
  `KisTextureOption::gpuDescription()` builds it; `apply()` now shares
  `applyAlphaMask()` with the description's CPU applier.
- Jobs (`KisDabRenderingJob.cpp`): `buildCircleDab()` accepts a dab whose
  postprocessing is a GPU texture (`gpuTexture()`). While a mask kind is
  being verified (phase 4.85), textured dabs are generated and textured on the
  CPU, and the circle is verified against the original (untextured) pixels.
  Once the kind is verified, a textured dab skips both the CPU generation and
  the CPU texturing: the postprocess branch replaces the job's description
  with the circle plus that dab's texture (`texturedDescriptionCount()`).
  Postprocess jobs (repeated dab shapes) build their own texture description.
- Reflections: the brush tip's mirroring (`mirrorFlips(di)`) happens before
  texturing, canvas mirroring after it. `proceduralFlips` holds both; the
  shader samples the texture at the generation coordinate reflected by
  `textureBrushFlips`. `KisRenderedDab::materialize()` renders the circle with
  the brush flips, applies the CPU texture (`CpuApplier`, the real
  `KisTextureOption` code), then mirrors by the remaining flips.
- GPU: `KisGpuDabCompositor::Circle` grows to 176 bytes (texture and table
  addresses, origin, size, mode, soft, strength, inverted strength, weight,
  brush flips). The pattern bytes and the 256-entry byte-to-float table
  (`KoColorSpaceMaths<quint8, float>::scaleToA`) are uploaded once per batch
  and distinct pointer. `paint_dabs.comp`: `textureTexel()` wraps like
  `KisFillPainter::fillRect()` (positive modulo), `texturedAlpha()` repeats
  the strength composite functions of `KisMaskingBrushCompositeOp.h` with
  `precise` float operations and double divisions, including qMin/qMax/
  kisBoundFast NaN behaviour.
- Separate shader builds: the texture code (and the 176-byte Circle
  fields) is compiled only into `paint_dabs_textured` and
  `paint_dabs_textured_extended` (`DAB_TEXTURE`, RGBA32F); the compositor
  uses them only for batches with a textured dab, and the untextured
  variants keep their previous source. Pitfall: with the (unused) texture
  code in the existing variants, RGBA16F dabs changed by one half ULP
  (`KisGpuBrushTest::testHalfDabs` and others) and the RGBA32F Soft circle
  stopped being bit-exact (`testGeneratedCircleDabsExact`): extra code
  changes how the driver compiles the existing exact paths.
- Tests: `KisGpuStrokeTest::testTextureModes` (14 modes x hard/soft at
  strength 0.6 with a texture offset, some mirrored, plus Lightness as the
  CPU case; layer and projection against the CPU path, generated, skipped and
  textured-description counters) and the textured rows of
  `testTexturedMaskedStroke` (now expected on the GPU).
- Measurements (`KisGpuStrokeTest`, median of 5, no validation, ms; CPU /
  GPU projection / GPU brush): 300px textured Buildup 54 / 62 / 16 (GPU brush
  57 before); 300px textured Wash 65 / 76 / 19 (53 before); 300px masked
  textured 85 / 99 / 38 (106 before); temporary dense rows (spacing 0.03,
  both mirrors, masked, textured) 40px 30 / 38 / 31 (36 before), 80px
  51 / 57 / 27 (41 before).

### Image tip dabs (phase 4.103)

Before phase 4.103 every dab of a predefined (image) brush tip (GBR, PNG,
SVG, ABR) was transformed with QPainter on the CPU and uploaded as pixels.
QPainter could not be repeated on the GPU: Qt replaces its pixel
conversions at run time with AVX2/SSE4 versions that round differently
(the SSE4 unpremultiply uses `rcpps`, whose result differs between CPU
vendors), so even the CPU output depended on the machine.

- Decision (user, 2026-10-11, "A2"): replace QPainter in
  `KisQImagePyramid::createImage()` with a deterministic implementation
  shared by the CPU and the GPU brush. Up to one 8-bit step per channel of
  difference from upstream Krita is accepted.
- Sampler: `KisImageTipSampler` (`libs/image/KisImageTipSampler.{h,cpp}`,
  so that the dab description in kritaimage can use it) with the
  description `KisImageTipGeometry` (bordered ARGB32 pyramid level, its
  premultiplied QRgba64 copy computed once per level, dab size, identity
  flag, 16.16 increments and one row per dab row: first pixel, length and
  the source position at the first pixel). The arithmetic follows Qt 6.8's
  raster engine for `drawImage()` into ARGB32 (KDE qtbase fe3936871d):
  `QSpanData::setupMatrix()` with its 1/65536 delta,
  `fetchTransformedBilinear64_uint32()` with Qt's edge clamping,
  `QRgba64::premultiplied()`, the 16-bit `_mm_mulhi_epu16` interpolation of
  `interpolate_4_pixels_rgb64()`, `unpremultiplied_64bit()` (a 65536-entry
  factor table on the CPU) and `toArgb32()`. Scale-only transforms use
  Qt's rounded target rectangle; rotated dabs cover whole rows of pixels
  whose centres map into the bordered level, with the position computed at
  each row start like `QRasterizer` spans (accumulating from x = 0 gave
  differences of 4).
- `KisQImagePyramid`: `describeImage()` builds the geometry (false outside
  Qt's fast fixed-point range; `createImage()` then keeps QPainter, now
  `createImageWithQPainter()`), `renderDescribed()` and `describedPixel()`
  forward to the sampler. The CPU path is separable for scale-only rows
  (the vertical blend once per source column) and uses SSE2.
- Accuracy against QPainter on this machine
  (`KisQImagePyramidDescribeTest`, about 100 million pixels): at most one
  step of the alpha-mask value `UINT8_MULT(255 - red, alpha)` and of the
  alpha; no coverage differences for scale-only dabs, 2 pixels in 13.6
  million for rotated dabs.
- CPU speed (2026-10-11, after phase 4.103): `render()` has AVX2 kernels
  (four pixels per step, chosen at run time with
  `__builtin_cpu_supports("avx2")`, functions compiled with
  `__attribute__((target("avx2")))`; the SSE2 path remains the fallback and
  the reference). They pair-load each row's two neighbouring texels instead
  of gathering (edge pixels keep a gather path). Gray levels
  (`KisImageTipSampler::isGray()`, every visible texel red == green ==
  blue, stored per pyramid level as `KisImageTipGeometry::gray`)
  unpremultiply one channel for all three, which gives the same bytes.
  Only the pixels outside each row are cleared. Pitfall: interpolating an
  opaque tip gives alpha 0xfffe (both mulhi terms round down, as in Qt), so
  nearly every pixel takes the unpremultiply path; that arithmetic, not
  the factor table lookup, was the cost (an exact double division for the
  factor was slower than the table on Zen 5).
  `KisQImagePyramidDescribeTest` checks AVX2 against SSE2 and the gray path
  against three channels on every case, and its benchmark reports both.
  400 dabs of a 256px tip (Ryzen 9 9950X): scaled QPainter 28-34 ms, AVX2
  40-46, SSE2 53-59; rotated QPainter 53-63, AVX2 66-75, SSE2 96-107
  (before: 66-80 and 110-131 ms).
- Description: `KisProceduralCircleDab::ImageTip` (kind 3) with
  `imageTip` (the geometry); the alpha is
  `Uint8ToFloat(UINT8_MULT(255 - red, alpha))` of the sampled ARGB32 pixel,
  as `KoColorSpaceTrait::fillGrayBrushWithColor()` computes it for RGBA F32
  (no SIMD specialisation exists for float). `render()` uses
  `KisImageTipSampler::render()`. RGBA32F only.
- `KisBrush::proceduralImageDab()` describes the dab of
  `generateMaskAndApplyMaskOrCreateDab()` (the same pyramid and
  `KisDabShape(scale * d->scale, ratio, -normalizeAngle(rotation +
  d->angle))`) for alpha-mask brushes without preserve-lightness or a
  gradient; auto, image pipe (`.gih`) and text brushes are refused (they
  generate their dabs differently).
- Jobs: `buildCircleDab()` tries the image tip when the brush is not an auto
  brush; the description's size must equal the dab bounds. The kind gets its
  own verification counter (`s_verifiedDabs[3]`), so CPU generation is
  skipped after 16 verified image tip dabs; textures (phase 4.102),
  mirroring and materialization work unchanged.
- GPU: `KisGpuDabCompositor::Circle` grows to 224 bytes (level and row
  table addresses, level size, increments, identity flag). The ARGB32 level
  and each dab's row table are uploaded once per batch and distinct pointer,
  with the byte-to-float table. `paint_dabs.comp` evaluates kind 3 only in
  the `DAB_TEXTURE` builds (the compositor selects them for batches with an
  image tip); it computes only the red and alpha channels in 32- and 64-bit
  integers (the unpremultiply divides in uint64).
- Memory and transfer: each pyramid level keeps its premultiplied copy
  (8 bytes per texel, besides the 4-byte ARGB32 level), and every GPU batch
  uploads the levels its dabs use. A level larger than the 64 MiB staging
  budget makes the batch fall back to materialized CPU pixels.
- Tests: `KisQImagePyramidDescribeTest` (accuracy against QPainter, the
  described image equals `createImage()`, a benchmark),
  `KisGpuBrushTest::testGeneratedImageTipDabs` (identity, scaled, rotated and
  out-of-level rows; GPU alpha equals the CPU alpha bit for bit, and
  `render()` equals `alphaAt()`), `KisGpuStrokeTest::testImageTipStroke`
  (a GBR tip at 48 and 160px, rotation, Wash and Buildup, mirrors, a
  textured row, scale 1; layer and projection against the CPU path,
  generated and skipped counters).
- Measurements (`KisGpuStrokeTest::testImageTipStroke`, median of 5, no
  validation, ms; CPU / GPU projection / GPU brush; short 24-segment
  strokes): 160px Buildup 9.6 / 13.7 / 11.0, rotated 9.9 / 15.8 / 12.5;
  160px rotated Wash with mirrors 26.2 / 32.2 / 21.8; 120px textured with
  mirrors 17.9 / 22.0 / 17.4, rotated 19.1 / 26.1 / 17.7; 48px 5.1 / 9.1 /
  9.3 (no gain at small sizes, like the round dabs).
- Applications (phase 4.104, 2026-10-11, user request): the image stamp,
  Lightness map and Gradient map tips are described as well
  (`KisProceduralCircleDab::imageApplication`). Each output channel depends
  on a few 8-bit channels of the sampled tip pixel only, so
  `KisBrush::proceduralImageDab()` captures the CPU's per-pixel work in a
  table made with the CPU code itself, and the GPU looks it up:
  - Lightness map: `fillGrayBrushWithColorAndLightnessWithStrength()` on
    256 pixels `qRgba(v, 0, 0, v)` (the color depends on red, the alpha on
    alpha); made per dab (paint color, strength).
  - Gradient map: the 256 colors `cachedAt(qGray / 255.0)` and the 256
    doubles `alpha / 255.0`; the alpha is
    `float(double(gradient alpha) * maskOpacity)` (a double product on the
    GPU, correctly rounded). Upstream change (`kis_brush.cpp`, gradient
    branch of `generateMaskAndApplyMaskOrCreateDab()`): fully transparent
    tip pixels now take their own gradient color instead of the previous
    pixel's, so that a pixel depends on its tip pixel only (their alpha
    stays 0; invisible).
  - Image stamp (`paintDevice()`, `KisFixedPaintDevice::convertFromQImage()`
    from 8-bit sRGB): the conversion per channel, from a gray ramp, used
    only if the color space's conversion is per channel (checked once per
    color space on a 16-level grid with four alphas and 65536 random
    pixels; true for the default float profiles); otherwise the dabs keep
    their pixels.
  - Verification and skipping (phase 4.85) count each application
    separately (kind index 3 + application).
  - GPU: `Circle.imageApplication` and `Circle.imageTable` (in the former
    padding, still 224 bytes); the shader samples all four channels
    (`imageTipPixel()`) and applies the table (`imageTipValue()`); the alpha
    mask keeps its own path (`imageTipAlpha()`; the unused channels are dead
    code).
  - Tests: `KisImageTipDescriptionTest` (every application, color and gray
    tips, 60 shapes: the description renders exactly the CPU dab of
    `mask()` or `paintDevice()`), `KisGpuBrushTest::
    testGeneratedImageTipDabs` (every application on every geometry, all
    channels bit for bit), `KisGpuStrokeTest::testImageTipStroke` (stamp and
    Lightness rows; the Gradient map needs the canvas gradient, which the
    stroke harness does not provide).
  - Measurements (`KisGpuStrokeTest::testImageTipStroke`, median of 5, no
    validation, ms; CPU / GPU projection / GPU brush; 120px, rotated):
    stamp textured Buildup 26.3 / 29.5 / 10.4, stamp Wash 25.6 / 28.4 /
    12.8 (the CPU converts every dab's colors), Lightness textured Buildup
    11.1 / 14.9 / 11.1, Lightness Wash 13.8 / 16.2 / 12.5.
  - Mirrors: dabs of different colors do not commute, and the CPU and GPU
    paths order the reflections of a batch differently, so mirrored colour
    strokes may differ slightly between the paths where dabs overlap
    (unchanged: colour dabs were already painted by the GPU brush as
    uploaded pixels).
- Not covered: image pipe (.gih) and text brushes (RGBA16F: phase 4.105), image stamps in
  color spaces whose conversion is not per channel, Gradient maps without a
  gradient, and dabs with other postprocessing (sharpness,
  Lightness/Gradient textures) keep their pixels.

### RGBA16F image tips and textures (phase 4.105)

2026-10-11, user request. Image tip dabs (every application) and textured
dabs are described on RGBA16F layers too; before, only their RGBA32F dabs
were (phases 4.102-4.104), and RGBA16F dabs with an image tip or a texture
kept their pixels.

- Image tips: `KisBrush::proceduralImageDab()` accepts 8-byte RGBA F16 color
  spaces; the tables are made with the same CPU calls in the F16 color space
  and hold exact half values as floats (`channelValue()`). The alpha mask
  uses `KoColorSpaceMaths<quint8, half>` (`imageTipHalfAlpha()`; on the GPU
  `uint8ToHalfTable()` as the 256-entry table). The Gradient map's opacity
  goes through `KoColor::setOpacity()`: double -> float -> half
  (`roundToHalf(float(alpha))` in the shader). `render()` and
  `matchesPixel()` write and compare halves.
- Textures: `KisTextureOption::gpuDescription()` accepts RGBA F16 dabs and
  computes the constants with the half instantiation
  (`StrengthCompositeFunctionBase<half>`, `Arithmetic::inv<half>`, the
  double weight, exact in float). The texels go through
  `KoColorSpaceMaths<quint8, half>` (the same 256-entry table). The shader's
  `texturedAlphaHalf()` repeats `KisMaskingBrushCompositeOp<half, ...>`:
  `KoColorSpaceMathsTraits<half>::compositetype` is double, and Imath's
  `half` has no arithmetic operators, so `half +- half` is computed in float
  (`inv`, the soft Linear Burn and Linear Dodge sums), `mul`, `div` and
  `unionShapeOpacity` in double, and every value returned as a half rounds
  double -> float -> half (`hd()`); the fuzzy checks are the half
  specializations (0.002 and 0.001 thresholds).
- Shaders: `paint_dabs_textured_rgba16f` and
  `paint_dabs_textured_extended_rgba16f` (`TILE_F16` + `DAB_TEXTURE`); the
  compositor uses them for RGBA16F batches with an image tip or a texture.
  The existing RGBA16F variants are unchanged.
- Tests: `KisImageTipDescriptionTest` (RGBA16F rows: the description is
  the CPU dab, byte for byte), `KisGpuBrushTest::testGeneratedImageTipDabs`
  (RGBA16F runs: all channels as halves), `KisGpuStrokeTest::
  testHalfTextureModesExact` (14 modes x hard/soft x 3 strengths, the GPU
  alpha equals `KisMaskingBrushCompositeOp<half>` on every dab alpha and
  texel bit for bit), and RGBA16F rows of `testImageTipStroke` and
  `testTextureModes`.
- Measurements (median of 5, no validation, ms; CPU / GPU projection / GPU
  brush; RGBA16F): image tip 120px Wash with mirrors 55.4 / 43.3 / 19.9,
  colour stamp 41.8 / 41.4 / 11.6, Lightness map 20.2 / 17.2 / 11.8, 80px
  textured round (Multiply, hard) 14.7 / 13.1 / 10.7.
- Still on the CPU: Lightness and Gradient textures, sharpness (RGBA16F
  masked strokes: phase 4.106, "Masking brush").

### Remaining blend modes (phase 4.107)

2026-10-12, user request ("6", priority 5 of `gpu-work-priorities.md`).

- RGBA16F brushes: `kisGpuSupportsHalfBrushBlend()` accepts every
  `KisGpuBlendOp`, and `KisGpuDabCompositor::record()` no longer refuses F16
  modes, so Soft Light (Photoshop), Divide, Vivid Light, Hard Mix (all
  three), Grain Merge/Extract, Negation, Allanon, Darker/Lighter Color and
  the new modes below use the GPU brush on RGBA16F too.
- New modes (`KisGpuBlendOp` 34-62, dab modes +2): HSY Tint, Increase/
  Decrease Saturation and Luminosity; HSI, HSL and HSV Color, Hue,
  Saturation, Increase/Decrease Saturation, Intensity/Lightness/Value and
  Increase/Decrease of it. `composite_blend.glsl` decodes HSI/HSL/HSV as
  `OpColorHsi + 8 * family + function` (`blendHSY()`), and one generic
  implementation (`hsxLightness`, `hsxSaturation`, `hsxSetSaturation` with
  the CPU's sort, `hsxToneMapping` with HSV's per-channel fallback) now
  serves the HSY modes too.
- Arithmetic, following `KoColorSpaceMathsTraits<T>::compositetype`
  (float for float, double for half):
  - `exactBlend()` modes (Color Burn, Vivid Light, Hard Mix Softer, every
    HSX mode) use `precise` alpha, weighted colour and interpolation and an
    IEEE division (`exactQuotient()`);
  - RGBA16F: `inv()` results round to half before Vivid Light's and Hard
    Mix Softer's arithmetic; Vivid Light's `1 - a / b` is rounded once
    (`oneMinusQuotient()`); the weighted colour of `exactBlend()` modes
    rounds each `mul(a, b, c)` once (`mul3()`);
  - `roundedProduct()` for every product that feeds an addition in these
    paths: the NVIDIA driver fused `l + t * s` into an fma despite
    `NoContraction`, which changed HSL results at rounding ties
    ([wiki](wiki/concepts/cpu-gpu-bit-parity.md), techniques 9-11).
- Fixed on the way (already on the GPU, not covered before): RGBA32F
  brushes in Vivid Light, Hard Mix Softer and Lighter Color differed from
  the CPU over repeated dabs (up to 4e-5, 0.009 for Lighter Color).
- Tests: the brush lists of `KisGpuBrushTest` (F32 matrices with
  selections, channel flags, mirrors; `testHalfDabs`; `testHalfWashBlendModes`),
  `KisGpuStrokeTest::testBlendModes`/`testHalfBlendModes` and the two lists
  of `KisGpuProjectionTest` include every mode; the existing tolerances
  (2e-5 for F32, 1/1024 for F16 dabs) hold. Darker Color with alpha locked
  leaves the stroke test's darker background unchanged on every path.
- Still on the CPU after this phase: see "All other blend modes (phase
  4.108)".

### All other blend modes (phase 4.108)

2026-10-13, user request ("4 のブレンドモード変更はすべてまとめて").

- New `KisGpuBlendOp` 63-116 (dab modes +2), mapped in
  `KisGpuProjectionCompositor::blendOpForCompositeOp()`:
  - separable (`blendChannelExtra()`, clamp policies in
    `extraClampsSource/Destination/Bottom()`): the HDR variants of Hard Mix,
    Color Dodge and Vivid Light, Hard Overlay (and HDR), Geometric Mean,
    Parallel, Interpolation (and 2x), Penumbra A-D, Soft Light (IFS
    Illusions, Pegtop-Delphi), Gamma Light/Dark/Illumination, Flat Light,
    P-Norm A/B, Super Light, Tint/Shade/Fog Lighten/Fog Darken (IFS
    Illusions), Easy Dodge/Burn, Inverse Subtract, Modulo, Modulo Shift (and
    Continuous), Arc Tangent, Equivalence, Additive-Subtractive, Reflect,
    Glow, Freeze, Heat, Glow-Heat, Heat-Glow, Reflect-Freeze,
    Freeze-Reflect, Heat-Glow-Freeze-Reflect;
  - HSL functors (`blendHslExtra()`, through `compositeHSY()`): Lambert
    Lighting (and Gamma 2.2), Tangent Normal Map, Combine Normal;
  - own composite functions (`compositeSpecial()`): Luminosity SAI
    (`KoCompositeOpGenericSCAlpha`, no normalization), Behind, Greater,
    Copy Red/Green/Blue.
- Kept on the CPU: Dissolve (random), Marker (needs Alpha Darken's flow
  parameters), Divisive Modulo, Divisive Modulo Continuous and Modulo
  Continuous (their quotient's fractional part, and the source-0 branch of
  `KoColorSpaceMaths<double>::modulus()` at 2^52 scale, turn any one-ulp
  difference into an O(1) error), and Destination In/Atop: they change pixels
  where the source has no tiles (`KisLayerProjectionPlane` applies them
  beyond the layer's extent, and a Wash merge beyond the stroke), which the
  tile-based GPU compositor skips. Logical modes exist only for integer
  spaces.
- CPU semantics reproduced:
  - `clamp<T>()` is the identity for float and half (`KoColorSpaceMaths`
    specializations); `kisBoundFast()` gives the lower bound for NaN
    (`kbound()`); `T::max` is FLT_MAX or HALF_MAX (`FltMax`), epsilon
    FLT_EPSILON or HALF_EPSILON;
  - std::pow() edge cases (`cpow()`: negative bases, zero) and accurate
    pow (`accurateLog()` with an atanh series and split ln2,
    `accurateExp()` with range reduction): GLSL `log2()` has an absolute
    error that ruined large exponents near 1;
  - double modulus near integers (`modOne()` on an exact hi + lo sum);
  - RGBA16F: Arithmetic results round to half at every step where the CPU
    returns a T (`tv()`), so half overflows to infinity as on the CPU;
  - infinities survive `roundedProduct()`, `mul3()` and `exactQuotient()`.
- Accuracy: modes whose CPU functions compute pow/exp/cos/atan2/sqrt in
  double are tolerance-based (`modeTolerance()` in `KisGpuBrushTest`: 1e-3
  F32, 4 half ulps; Greater and Easy Burn 5e-3, they amplify). Glow, Heat
  and their combinations divide by 1 - dst: on exact inputs
  (`testExtendedBlendBoundaries`) they hold 2e-5, after other layers
  (`testBlendModesMatchCpu`) 1e-2. F32 comparisons are relative above 1.
- Tests: the mode lists of `KisGpuBrushTest`, `KisGpuStrokeTest` and
  `KisGpuProjectionTest` include the new modes; NaN and equal infinities
  compare equal (`sameNonFinite()`, bitwise `PixelValues`), since several
  modes give NaN for negative HDR channels on the CPU too; boundary failures
  print the source, both destinations and both results.

### Precise contraction audit (phase 4.109)

2026-10-13, user request (task 1 after phase 4.108).

- `KisGpuEngineTest::testPreciseContraction` (`libs/gpu/tests/
  precise_probe.comp`, built optimized and not, float, double and with the
  float16 extensions) runs `a + b * c` and longer shapes on triples whose
  fused and unfused results differ: on driver 596.86 every precise form
  keeps the unfused result; it records the counts per driver (qInfo) and
  checks `roundedProduct()` and an explicit fma.
- `KisGpuEngineTest::testBlendProbe` (`blend_probe.comp`, the real
  `composite_blend.glsl`) reproduces phase 4.108's case: with the plain
  precise multiply-add in `hsxToneMapping()` the Increase Saturation (HSL)
  green came out one float above the CPU's tie (0.980712890625), with every
  input bit-identical and NoContraction on every operation; an inline copy
  of the same code was exact. The test now checks the value bit for bit.
- Changed to `roundedProduct()`: the half lerps of
  `composite_half_brush.glsl` (`halfLerp()`, `overHalf()`), the RGBA16F
  Alpha Darken colour of `paint_dabs.comp`, and in `grid_warp.comp`
  (`roundedProductD()`) the quadratic of `backwardMap()`, the mapped
  coordinates, the fallback distance and the integer weights (truncated
  after + 0.5, where one bit can change a weight). `transform_pass.comp`
  has no precise product feeding a sum; `separable_convolution.comp` is
  tolerance-based; `masking_composite.comp` rounds double results to half.
- Not changed: precise products that feed a multiplication, a division or
  a comparison only, and explicit `fma()` calls that reproduce the CPU's
  AVX2 kernels (`softFade()`).

## Phase 0 technology decisions

| Topic | Decision | Reason |
| --- | --- | --- |
| API level | Vulkan 1.3 core: synchronization2, timeline semaphores, buffer device address, shaderInt64, shaderFloat16, 16-bit storage. | All present on Blackwell (driver 596.86 reports Vulkan 1.4.329). |
| Loader | `vulkan-1.dll` loaded at runtime (`KisGpuVulkanFunctions`, X-macro tables). No link-time dependency. | Krita still starts without a Vulkan driver. The SDK has no volk. |
| Memory | One dedicated `VkDeviceMemory` per buffer; tiles sub-allocated from 256 MiB chunks (4096 RGBA32F tiles). No VMA. | Tiles are fixed-size, so a slab allocator is trivial; the SDK does not ship VMA. |
| Tile storage | Storage buffers addressed by 64-bit device address (`GL_EXT_buffer_reference`), not images. | Any number of tiles per dispatch through address tables; no descriptor sets, layouts, or image transitions; sub-allocation is trivial. |
| Shader input | Push constants only (addresses + counts). | No descriptor management in the engine. |
| Shaders | GLSL compute, compiled at build time by the SDK's `glslc` (`--target-env=vulkan1.3 -O -mfmt=num`), included as word arrays (`kis_gpu_add_shader()` in `libs/gpu/CMakeLists.txt`). | No runtime compiler dependency; build fails on shader errors. |
| Precision | RGBA32F is the default working format; RGBA16F storage is supported with float32 arithmetic. | Parity with Krita's F32 color space; F16 halves bandwidth (measured below). |
| Display | GL/Vulkan interop with exported memory and binary semaphores, `GL_LAYOUT_GENERAL_EXT`, dedicated allocation, device UUID check. | Qt has no Vulkan; interop keeps the existing canvas, OCIO, and decorations. |
| Device choice | Highest score: discrete GPU, then NVIDIA vendor; requires a graphics+compute queue with timestamps. `KisGpuDeviceInfo::isBlackwell()` (NVIDIA device id ≥ 0x2B00) is informational and logs a warning when false. | Blackwell-only target without hard-failing development on other GPUs. |
| Validation | `KRITA_GPU_VALIDATION=1` enables `VK_LAYER_KHRONOS_validation` plus a debug messenger that counts errors (`validationErrorCount()`). | Tests assert zero validation errors. |

### Parity rule found in phase 0

The color of a pixel whose alpha is exactly 0 is **unspecified** in Krita:
`KoOptimizedCompositeOpOver128` copies the source color into lanes that stay
transparent, while the scalar paths keep the old color. GPU parity tests
compare such pixels by alpha only. Everything else must match `KoCompositeOp`
within 1e-5 (F32).

## Phase 0 measurements

RTX PRO 6000 Blackwell Workstation Edition, driver 596.86, 32-thread CPU,
RelWithDebInfo. Normal blend of 8 layers of 4096x4096 (16.8 MPix per layer,
10% of tiles empty, random alpha), `KisGpuEngineTest::benchmarkCompositeStack`:

| Path | Time |
| --- | --- |
| CPU `KoCompositeOp` (xsimd), 1 thread | 290 ms |
| CPU `KoCompositeOp`, 32 threads | 82.8 ms |
| GPU RGBA32F kernel | **1.55 ms** (×53 vs. 32 threads; ≈1.5 TB/s, close to the memory bandwidth limit) |
| GPU RGBA16F kernel | **0.74 ms** (×112) |
| Upload of the whole stack (2.1 GB F32, staging → slots) | 39 ms |
| Readback of the result (268 MB F32) | 6.8 ms |
| Vulkan → GL shared image round trip | functional, no CPU wait on the GL side |

Conclusions:

1. Compositing is memory-bound; the GPU wins by 50–100× once data is resident.
2. Upload/readback cost far more than the kernel. The engine must keep pixel
   data **resident on the GPU** and transfer only on load, save, and
   unported CPU access. A hybrid that uploads per operation would lose most of
   the gain.
3. RGBA16F storage halves the kernel time; decide per document later (default
   F32 for parity).

## Phase 1 measurements

`KisGpuPaintDeviceTest::benchmarkTransfers`, 4096x4096 RGBA F32 device
(4096 tiles, 256 MiB), wall-clock times including submission and wait:

| Operation | Time |
| --- | --- |
| First GPU read (uploads every tile) | 63 ms |
| GPU read when already resident | 0.7 ms |
| GPU fill, write-only, in place | 1.6 ms |
| `readBytes` of stale tiles (one download submission per tile) | 406 ms |
| `syncToCpu` (256 tiles per submission) | 26 ms |
| `readBytes` after `syncToCpu` | 20 ms |

Conclusions: residency works as intended (re-reading resident data costs
almost nothing), and per-tile synchronous downloads are ~15x slower than
batched ones. Any bulk CPU consumer of GPU-written data must download in
batches; the canvas must eventually not download at all (phase 3).

## Phase 2 measurements

`KisGpuProjectionTest::benchmarkRefresh`, 4096x4096 RGBA F32 image, 16 full
layers (Normal/Multiply/Screen/Overlay, 86% opacity), layers already
GPU-resident, 32 CPU threads:

| Operation | CPU | GPU |
| --- | --- | --- |
| Full refresh (`refreshGraphAsync`) | 284 ms | 66 ms |
| 256x256 update of one layer (`setDirty`), average of 20 | 12.7 ms | 0.88 ms |

The full refresh is bounded by the projection round trip (fill, download,
`writeBytes` of 16.8 Mpixels): the compositing kernel itself takes a few
milliseconds. Phase 3 removes that round trip.

## Phase 3.1 measurements

Same benchmark as phase 2 (4096x4096 RGBA F32, 16 layers, 32 CPU threads):

| Operation | CPU | Phase 2 | Phase 3.1 |
| --- | --- | --- | --- |
| Full refresh | 253 ms | 66 ms | 42 ms (+ 51 ms if all of it is then read back on the CPU) |
| 256x256 update of one layer + reading the updated rect (what the canvas does) | 12.0 ms | — | 2.3 ms |

## Phase 3.2 measurements

`KisGpuCanvasUploadTest::benchmarkCanvasUpdate`: producing the canvas
texture data (`buildUpdateInfo`) for a 4096x4096 RGBA F32 image with
8 layers, linear sRGB image profile, sRGB display profile. The GPU time
includes waiting for the GPU to finish; the GL-side texture copies are not
included (GPU-to-GPU).

| Update | CPU path (download + LCMS) | GPU path |
| --- | --- | --- |
| Whole image | 1025 ms | 1.6 ms |
| 256x256 rect | 4.5 ms | 0.39 ms |

## Phase plan

The order of work after phase 4.57 is set in
`docs/agent/gpu-work-priorities.md`; record measurements and phase details
here and keep that document as the ordering reference.

Each phase ends with parity tests against the CPU implementation, validation
clean runs, and a benchmark entry in this document.

| Phase | Scope | Exit criteria |
| --- | --- | --- |
| 0 | `kritagpu` foundation, compositing spike, GL interop spike. | Done (this document). |
| 1 | GPU residency of `KisTileData`, download on CPU access, GPU copy-on-write with memento registration, deferred slot free, `KisGpuTileAccess`. | Done: `KisGpuPaintDeviceTest` (fill, composite parity, undo/redo, COW isolation, CPU-after-GPU writes, save/load with and without prior sync, F16, slot release, deferred release of in-flight slots, locked tiles, download failure retry and fallback, stale-upload refusal) and the existing tile/paint device/transaction/iterator tests pass. |
| 2 | Projection on the GPU: batched layer compositing in `KisAsyncMerger`, 11 blend modes, alpha lock, adjustment layers and filter masks as split points, CPU fallback. | Done: `KisGpuProjectionTest` (every blend mode with and without alpha lock, full refresh, partial unaligned updates, adjustment layer, filter mask, F16) matches the CPU projection within the parity criteria, validation-clean with exit code 0; existing merger/projection/walker/layer tests pass with `KRITA_GPU_PROJECTION=1`. |
| 3.1 | GPU-authoritative projection: in-place GPU compositing, tile-aligned merge job exclusivity, uploads decided at submission, COW clones by GPU copies, batched canvas downloads. | Done: `KisGpuProjectionTest` (incl. projection residency and concurrent updates sharing tiles) and all `libs/image` tests except four unrelated ones pass with and without `KRITA_GPU_PROJECTION=1`. |
| 3.2 | Canvas from the GPU: projection tiles → display-converted pixels in a Vulkan buffer shared with GL (PBO) → existing `KisTextureTile` uploads without CPU copies; GPU display conversion for matrix-shaper profiles. | Done: `KisGpuCanvasUploadTest` writes the same texture tiles through the CPU and the GPU path and compares them: identical for equal profiles, ≤ 1 half ULP for F32→F16, ≤ 1.2e-4 for matrix-shaper conversions (sRGB/Rec.2020 linear ↔ sRGB TRC, and different white points: LargeRGB/CIE RGB ↔ sRGB; F16 image to an sRGB TRC F16 texture). `testLutProfilesUseCpu` covers the LUT fallback; `testGLImportFailureFallsBackToCpu` runs the GL-import recovery for all those conversion rows (one failed upload, an edge update, and a merged update where one upload is read back and the other uploaded by GL) with the same tolerances. |
| 3.3 | `Solstice/GpuEngine` gate and Preferences UI; conversion of documents in other color spaces to RGBA float when opened; failure warning, pre-save warning, and no file replacement after a loss during the save. | Code done: `KisGpuProjectionTest::testConvertedDocumentUsesGpu` (conversion targets; an RGBA8 document converted to the target composites on the GPU with an identical 8-bit appearance), `KisGpuPaintDeviceTest::testPersistentDownloadFailureIsReported` (the failure listener fires once for two losses), `KisGpuSaveTest::testLossDuringSaveKeepsTarget` (a `.kra` save that loses the GPU-only projection fails and leaves the target file unchanged; after acknowledging, the save succeeds; fails without the guard). The dialogs themselves are not unit-tested. Open: the manual checklist below (first user-visible build). |
| 3.4 | Bounded image tile pools, idle tile eviction, and normal disk swap for evicted GPU tile data. | Done: low-budget, rollback, concurrency, disk swap, and Undo tests; the user confirmed painting, Undo/Redo, zoom, save and reopen under a 16 MiB tile budget. |
| 3.5 | Bounded canvas transfer buffers and safe GL/Vulkan retirement; cap idle canvas work contexts. | Done: automated budget/parity/lifecycle tests; the user confirmed correct operation under a 16 MiB canvas budget. Its log confirms budget fallback with interop still enabled. Other transient allocations and stale CPU snapshots remain separate work. |
| 4.1 | Opt-in RGBA32F Normal/Alpha Darken pixel-brush dab compositing, retaining CPU dab generation. | Sequential brush update job. GPU/CPU parity, COW/Undo/Redo, CPU-after-GPU writes, failed submit, and low-budget rollback tests. Normal and Alpha Darken/Wash manually confirmed, with successful GPU batches in both logs. Full stroke latency remains unverified. |
| 4.2 | Selection coverage for direct GPU dab compositing. | Soft/inverted/empty selection, translated masks, deselection, CPU fallback, Undo/Redo, and both Alpha Darken variants tested. User confirmed operation; follow-up log confirms Normal with selection on GPU. |
| 4.3 | Normal-mode alpha and RGB channel locks on GPU dabs. | All 16 channel masks, with/without selection, compared with CPU; transparent RGB, COW, Undo/Redo and fallback covered. User confirmed operation; log confirms Normal with alpha locked on GPU. |
| 4.4 | GPU compositing for the existing horizontal/vertical mirror passes. | CPU parity for reflection order, shared dab pixels, masks, alpha lock, fractional-axis clipping fallback and mixed CPU/GPU passes. User confirmed operation; logs confirm reflected Normal and Alpha Darken passes, including Normal with selection and alpha lock together. |
| 4.5 | Combine nearby mirror passes into one submission with immutable shared source pixels. | CPU parity, source immutability, COW/Undo/Redo and preparation/submit fallback tested. Warm benchmarks improved; user confirmed operation. Log confirms combined single-axis painting; four-pass combination remains automatically tested only. |
| 4.6 | Exercise actual brush generation/cache/update jobs on the image worker scheduler. | 15 integration rows plus init/cleanup passed: CPU/GPU pixels, dirty regions, exact submission counts, mixed fallback, owning-image gate, COW and Undo/Redo. No runtime behavior change. |
| 4.7 | Clip GPU dabs to the CPU painting regions, including fractional mirror axes. | CPU parity, gaps/empty regions, COW/Undo/Redo, rollback and exact combined submission counts tested. User confirmed operation; real-app log confirms four-pass combination. |
| 4.8 | Measure complete queued strokes and batch Wash preview/final-merge readbacks. | CPU, GPU projection only, and GPU projection+brush compared through FreehandStrokeStrategy and completed projection. Input/display latency remains separate; see measurements and validation below. |
| 4.9 | Keep unrestricted Normal RGBA32F Wash preview compositing on GPU. | CPU parity/fallback checks; user reports improved response near mirror intersection, but distant Wash/Buildup lag remains. |
| 4.10 | Combine distant mirror passes using sparse destination tiles. | CPU parity, no gap allocation, rollback, Undo/Redo and complete distant Wash/Buildup strokes; user confirmed major improvement without Alpha Lock. |
| 4.11 | Avoid speculative CPU-clone downloads of GPU-only tiles after stroke commits. | Residency/COW and Alpha Lock stroke tests pass; manual lag persisted, later clarified as large-brush drawing catch-up. |
| 4.12 | Bound ready GPU brush batches by source pixel bytes. | Automated parity, queue and Undo/Redo checks passed. User confirmed residual large-brush mirror lag; further optimization explicitly deferred on 2026-10-03. |
| 4.13 | Erase dab compositing on GPU for RGBA32F pixel brushes. | CPU parity including hidden RGB, selections, mirrored passes, rollback and Undo/Redo; real brush jobs and complete eraser strokes. User confirmed the manual eraser check. |
| 4.14 | Keep unrestricted RGBA32F Wash Erase previews on GPU. | CPU preview/final parity including hidden RGB, channel/selection fallback, failed submit, readback limits and complete eraser strokes; user confirmed the manual check without issues. |
| 4.15 | Final Wash merge on GPU for unrestricted RGBA32F Normal/Erase painting. | CPU parity, COW/Undo/Redo, failed submit, low-budget fallback and tile-aligned parallel final jobs; user confirmed the manual test passed. |
| 4.16 | Selection coverage for RGBA32F Normal/Erase Wash preview and final merge. | Soft/inverted/moved/empty selections, changing/removing selections, submit fallback and full selected/mirrored strokes; user confirmed the manual check passed. |
| 4.17 | Normal Wash Alpha Lock on GPU, including selected painting. | Bundled with 4.18-4.19; all channel masks, CPU parity, COW/Undo/Redo and full strokes; user confirmed the combined manual check passed. |
| 4.18 | Normal Wash RGB channel locks, alone or with Alpha Lock/selection. | Bundled with 4.17/4.19; hidden RGB and channel preservation tested; user confirmed the combined manual check passed. |
| 4.19 | CPU-compatible Erase channel handling on GPU in Buildup and Wash. | Bundled with 4.17-4.18; CPU Erase ignores channel flags, including Alpha Lock; user confirmed the combined manual check passed. |
| 4.20 | Separable GPU blend modes for RGBA32F Buildup dabs. | Bundled with 4.21-4.22; CPU pixel parity, ordered mirrors, COW/Undo/Redo and failure tests; user confirmed the combined manual test passed. |
| 4.21 | The same blend modes for GPU Wash preview and final merge. | Shared shader math, full strokes and GPU-path counters; user confirmed the combined manual test passed. |
| 4.22 | Selection/channel-lock parity for the added brush modes, including sparse dab tile regions. | All 16 channel masks, hidden RGB, empty/moved/inverted selection, source-region parity and failed-submit recovery; user confirmed the combined manual test passed. |
| 4.23 | Pipeline brush submissions without a completion wait after each batch. | Three contexts retain in-flight source/mask/table bytes; real blocked-queue parity and CPU-read tests; combined manual check passed (user confirmed). |
| 4.24 | Reuse brush upload allocations in 256 KiB buckets. | Variable-size dabs reuse existing capacity; completed-work benchmarks include explicit waits; combined manual check passed (user confirmed). |
| 4.25 | Bound the whole brush staging ring to 64 MiB. | Drain before slot reuse or reclamation; pressure, oversized refusal, failure/CPU fallback and Undo/Redo tests; combined manual check passed (user confirmed). |
| 4.26 | Linear Burn, Linear Light and Pin Light for RGBA32F/RGBA16F layer compositing. | CPU parity and direct GPU clamp-boundary tests; bundled validation below. |
| 4.27 | The same three blend modes for RGBA32F Buildup dabs. | Selections, all channel masks, mirrors, COW/Undo/Redo and submit fallback; bundled validation below. |
| 4.28 | The same three blend modes for RGBA32F Wash previews and final merging. | Selected/restricted preview/final parity and complete strokes; bundled validation below. |
| 4.29 | Soft Light, Dodge/Burn, Divide, Vivid Light, Hard Mix and related separable modes. | Shared F32/F16 layer and F32 brush shaders; bundled with 4.30-4.31. |
| 4.30 | HSY Hue/Saturation/Color/Luminosity and Darker/Lighter Color. | Dedicated non-separable alpha/clamp behavior; bundled with 4.29/4.31. |
| 4.31 | Selection, channel-lock and complete stroke coverage for the new major modes. | Representative Buildup/Wash parity and Undo/Redo; user confirmed the combined manual check passed. |
| 4.32 | Automatically batch bulk CPU pixel reads and bound explicit prefetch storage. | F32/F16 interleaved and planar reads, padding, sparse/negative/offset regions, batch boundaries and failed-transfer retry. |
| 4.33 | Separate initial, CPU-to-GPU transition and resident projection measurements. | GPU completion, repeated CPU/GPU medians, actual GPU-path use and full pixel parity checked. |
| 4.34 | Batch masked-brush stroke readback before parallel CPU mask patches. | Complete masked/textured 150px and 300px strokes match CPU layers/projections and Undo/Redo. |
| 4.35 | Reuse fitting brush staging contexts under budget pressure. | Two large buffers remain reusable within 64 MiB instead of rotating through repeated third-slot allocations; deterministic context-count and pixel regression. |
| 4.36 | Individual RGB channel flags for F32/F16 layer projection. | All 16 channel masks, full refresh and partial updates; eight representative blend modes. User confirmed the combined manual check passed. |
| 4.37 | CPU-compatible half alpha tests and intermediate rounding in established F16 blend modes. | Transparent RGB, near-zero/near-one alpha and Hard Light/Overlay midpoint boundaries; bundled with 4.36. User confirmed the combined manual check passed. |
| 4.38 | Share projection upload buffers across layers in one submission. | Geometric growth up to 16 MiB chunks; F32/F16 offsets, rollover, oversized uploads, lifetime and failed-submit parity. |
| 4.39 | Bound automatic retired-resource reclamation and destroy outside its shared lock. | Slow destructors cannot block other retirement; new tile allocation reclaims at most 16 resource entries, explicit flush drains completed entries. |
| 4.40 | Host-cached memory for transfer-only CPU snapshots. | Original direct shader Upload memory stays unchanged; three-process transition/initial timing and CPU parity checks. |
| 4.41 | Batch GPU tile eviction under memory pressure. | Up to 256 tiles per batch; F32/F16, mixed sizes, busy locks, failed transfers, actual released bytes and Undo/disk-swap/concurrent eviction checks. |
| 4.42 | Refresh the performance baseline and validate measured canvas paths. | Three fresh processes per workload, completed GPU work, CPU parity and rejection of silent CPU fallback; ordinary short strokes remain slower than CPU. |
| 4.43 | Copy retained CPU snapshots directly during GPU COW. | F32/F16 ReadWrite/WriteOnly, submission failure, shared source and Undo/Redo coverage. |
| 4.44 | Share whole current/old tiles without CPU synchronization in exact/rough copies. | F32/F16 no-download assertions, partial-copy parity, independent later writes and Undo/Redo. |
| 4.45 | Batch partial-clear edge downloads; discard fully cleared tiles without reading. | Aligned, negative-coordinate, one-row and failed-batch cases; combined stroke comparison below. |
| 4.46 | RGBA16F Normal/Erase Buildup with per-dab half rounding. | Selections, channel flags, mirrors, async ring lifetime, rollback and complete-stroke checks; F16 Wash remains CPU. |
| 4.47 | RGBA16F hard/creamy Alpha Darken dabs. | CPU variant parity, fractional flow and masks, async ring lifetime. |
| 4.48 | RGBA16F Normal/Erase Wash preview and final merge. | Scalar half arithmetic, selection/channel flags, fallback and complete-stroke checks. |
| 4.49 | RGBA16F basic generic dab modes through Pin Light. | Per-dab half rounding, Exclusion intermediate correction, all channel masks, alpha boundaries and async batches. |
| 4.50 | RGBA16F basic generic Wash preview/final merge. | Soft selections, all channel locks, CPU fallback, Undo/Redo and actual brush jobs. |
| 4.51 | RGBA16F extended major dab modes. | Soft Light SVG, Color Dodge/Burn and HSY color modes, with CPU half intermediates. |
| 4.52 | RGBA16F extended major Wash preview/final merge. | Selection/channel/lifecycle matrix and actual mirrored strokes. |
| 4.53 | Reuse completed projection contexts before waiting for busy ones. | Three pending serial submissions, oldest-first reuse, queue-gated lifetime/fallback/Undo tests. |
| 4.54-4.55 | Batch CPU filter inputs/destinations and FFT cache reads. | 24 actual-filter rows, CPU parity and failure/Undo checks; user confirmed real-app operation. |
| 4.56-4.57 | Batch affine transform and layer-flip readbacks. | F32/F16 exact CPU parity, full/partial transforms, transfer counts, failed download and Undo checks. |
| 4.58 | Opt-in CPU timeline from input receipt through Qt frame swaps. | Infrastructure in progress; event-to-pixel attribution and real-app baseline remain open. |
| 4.59 | Stable input/update IDs and per-widget upload/frame command coverage. | Partial lineage only; compressed-input/job/projection attribution remains open. |
| 4.60 | Scheduled job identities and explicit creation ancestry across worker threads. | Unit/scheduler/stroke regressions pass; batch input sets and projection lineage remain open. |
| 4.61 | Logical dab requests, cache-hit identity and explicit paint-batch membership. | Queue and stroke regressions pass; dirty/projection/frame dependency chain remains open. |
| 4.62 | Dirty-group IDs through projection splitting/merging to canvas preparation. | All 28 regression batches reach executed projection; all 21 input-linked real-app batches reach swapped command descendants. |
| 4.63 | Per-input request audit, stroke-start condition snapshots and tracing-cost check. | Automated checks pass; conditions installed, real-app condition capture pending. Full-region coverage and interactive baseline remain open. |
| 4.64 | Audit every recorded projection/presentation branch, not only a successful descendant. | Python tests pass; existing real-app capture passes for 21 batches/64 request-producing inputs. Geometric coverage remains open. |
| 4.65 | Record image-space projection and canvas-notification rectangles; exact LOD-0 containment checks. | 100 regression requests covered by executed walker request regions; no-canvas stage explicitly unverified. Upload/presentation geometry remains open. |
| 4.66 | Record uploaded patches and logical-widget regions; invalidate pending coverage on view changes. | Automated checks pass; combined real-app capture verifies four condition records, 33 input-linked batches and all 450 upload geometries/swap chains. |
| 4.67 | Explicit tool input-to-stroke membership and joined per-input recorded checks. | Installed; analysis tests and stroke regressions pass. Combined real-app capture verifies membership for all 106 accepted inputs and joined checks for all 94 request-producing inputs; full latency baseline remains open. |
| 4.68 | Dirty collection/submission and compressed-update-to-upload geometry. | Installed; 28 ordinary and six masked dirty groups pass; analysis tests 24/24. Combined real-app capture passes for 94 request-producing inputs, 40 input-linked batches and all 472 uploads. |
| 4 | Brush engine: GPU dab rendering and compositing for the pixel brush (mask generation, alpha darken, indirect painting), then color smudge. | Stroke parity tests; input-to-pixel latency measured lower than CPU. |
| 5 | Filters and transforms: blur family, levels/curves, Liquify, Transform Tool, Puppet Warp (preview/final parity). Decide fate of remaining paint ops and color models. | Per-filter parity tests; Puppet Warp invariants from `docs/agent/puppet-warp.md` hold. |

Per the user's priority change on 2026-10-06, remaining Blend Mode extensions
are deferred until after phase 5, as the final item in
`docs/agent/gpu-work-priorities.md`. Current overhead analysis and GPU dab
generation retain their earlier priority; existing phase numbers are unchanged.

## Build, test, and install

`kritagpu` is built only when CMake finds the Vulkan headers and `glslc`
(the private SDK is passed as described in `docs/agent/vision-ml.md`).

```bat
cmd.exe /d /s /c "call <krita-dev-root>\env.bat && cmake --build <krita-dev-root>\_build --target kritagpu KisGpuEngineTest KisGpuGLInteropTest -j 8"
cmd.exe /d /s /c "call <krita-dev-root>\env.bat && ctest --test-dir <krita-dev-root>\_build -R libs-gpu --output-on-failure"
cmake -DCMAKE_INSTALL_LOCAL_ONLY=1 -P <krita-dev-root>\_build\libs\gpu\cmake_install.cmake
```

- Known failures unrelated to the GPU engine (identical with and without
  `KRITA_GPU_PROJECTION=1`; not re-checked against a pre-GPU build):
  `kis_pattern_test` (PNG pattern load), `kis_transform_mask_test` (1 px
  rect difference), `kis_cage_transform_worker_test` (reference images),
  `KisPaintOpPresetTest` (embedded pattern load).
- Do not run the whole `libs-ui-*` suite on a desktop session: GUI tests
  such as `kis_view_signals_test` create canvases without the default
  surface format and trigger the `g_sanityDefaultFormatIsSet` safe assert
  in `KisOpenGL::initialize()`, which opens blocking "Internal Error"
  dialogs (observed with and without `KRITA_GPU_PROJECTION=1`; not caused
  by the GPU engine, not checked against a pre-GPU build). Run the GPU
  tests and specific UI tests individually instead. The same applies to
  `plugins-impex-libkra-kis_kra_saver_test`: its documents have an empty
  MIME type, so its saves fail, and without batch mode a failed load or
  save opens a blocking dialog ("File emptytest.kra does not exist").
- Phase 3.3 save test: target `KisGpuSaveTest`, ctest name
  `plugins-impex-libkra-KisGpuSaveTest` (run it from
  `<krita-dev-root>\_build\plugins\impex\libkra\tests`, as ctest does;
  it uses batch mode and the native MIME type, so it opens no dialogs).
- Phase 3.2 test: target `KisGpuCanvasUploadTest`, ctest name
  `libs-ui-KisGpuCanvasUploadTest` (needs a desktop OpenGL context on the
  Vulkan GPU; skips otherwise).
- Phase 2 test: target `KisGpuProjectionTest`, ctest name
  `libs-image-KisGpuProjectionTest`. Changes to the merger must also pass
  `kis_async_merger_test`, `kis_projection_test`, `kis_update_scheduler_test`,
  `kis_walkers_test`, `kis_*layer*_test`, and `kis_filter_mask_test`, both
  without and with `KRITA_GPU_PROJECTION=1`.
- Phase 1 test: target `KisGpuPaintDeviceTest`, ctest name
  `libs-image-KisGpuPaintDeviceTest`. Changes to the tile engine must also
  pass `libs-image-tiles3-*`, `kis_paint_device_test`,
  `kis_transaction_test`, `kis_iterator*_test`, `kis_painter_test`, and
  `kis_low_memory_tests`.
- Test output (including benchmark numbers) is only visible with
  `-o <file>,txt` when running `<krita-dev-root>\_build\bin\KisGpuEngineTest.exe`
  directly.
- Validation run: set `KRITA_GPU_VALIDATION=1` and
  `VK_LAYER_PATH=<krita-dev-root>\VulkanSDK\1.4.357.0\Bin`.
- Benchmark size: `KRITA_GPU_BENCH_SIZE` (pixels per side, default 4096) and
  `KRITA_GPU_BENCH_LAYERS` (default 8).
- Tests skip (not fail) when no suitable Vulkan device or desktop GL context
  exists.

## Source map (`libs/gpu`)

| File | Role |
| --- | --- |
| `KisGpuVulkanFunctions.*` | Runtime loader and function tables. Add every new Vulkan function to an X-macro list. Platform functions are stored untyped so headers never include `<windows.h>`. |
| `KisGpuContext.*` | Instance, device selection, queue, timeline semaphore, memory type lookup, submission, validation messenger. |
| `KisGpuBuffer.*` | Buffer + dedicated memory; `Device`, `Upload` (prefers ReBAR), `Readback` (prefers cached). |
| `KisGpuCommandList.*` | Reusable command buffer, barriers, buffer copies, timestamp queries. Tracks its recording state: `submit()` ends recording even when the submission fails; `abandon()` ends a recording that will not be submitted. |
| `KisGpuComputePipeline.*` | Push-constant-only compute pipelines. |
| `KisGpuTilePool.*` | 64x64 tile slots, chunk growth, batched upload/readback per chunk. |
| `KisGpuLayerStackCompositor.*` + `shaders/composite_over_stack.comp` | Phase-0 normal-blend stack flattening. |
| `KisGpuGLSharedImage.*` (Windows) | Vulkan image shared with a GL texture plus exported semaphores. |
| `KisGpuTileFill.*` + `shaders/fill_tiles.comp` | Fills whole tiles with one color (tests, projection background). |
| `KisGpuLayerCompositor.*` + `shaders/composite_layers.comp` | Phase-2 layer compositing: per-layer blend op, opacity, alpha lock, clip rect. Defines `KisGpuBlendOp`. |
| `KisGpuCanvasPatchWriter.*` + `shaders/canvas_patches.comp` | Phase-3.2 canvas patches: edge-extended texture blocks, display conversion (identity / matrix-shaper), F32/F16 in and out. |
| `KisGpuGLSharedBuffer.*` (Windows) | Vulkan buffer shared with a GL pixel unpack buffer, with the Vulkan/GL semaphore cycle. |
| `KisGpuGLInterop_p.h` (private) | GL_EXT_memory_object / GL_EXT_semaphore entry points and the device-UUID check shared by the interop classes. |
| `KisGpuSeparableConvolutionPass.*` + `shaders/separable_convolution.comp` | Phase-4.98 separable convolution in bands: horizontal pass into a double intermediate, vertical pass with the FFT worker's write rules. |
| `KisGpuMaskingCompositePass.*` + `shaders/masking_composite.comp` | Phase-4.101 masking brush composite: stroke pixels into the target with the mask composited into alpha, ten modes, CPU-exact. |

## Source map (`libs/image`)

| File | Role |
| --- | --- |
| `tiles3/KisTileGpuState.h` | Per-tile-data GPU state and the `KisTileGpuHooks` entry points. |
| `tiles3/kis_tile_data*.{h,cc}` | `gpuState()`, `installGpuState()`, `notifyCpuWrite()`, `dropClones()`; download hook in `blockSwapping()`; state release in the destructor. |
| `tiles3/kis_tile.{h,cc}` | `detachForExternalWrite()`; `lockForWrite()` calls `notifyCpuWrite()`. |
| `tiles3/kis_tile_data_store.cc` | The swapper downloads and evicts idle GPU slots before disk swapping; prepared/in-flight slots are skipped. |
| `gpu/KisGpuTileBackend.*` | Process-wide context, RGBA32F/RGBA16F pools, downloads, deferred slot release, retired resources. Never destroyed. |
| `gpu/KisGpuTileAccess.*` | Public GPU access to device tiles (see above). |
| `gpu/KisGpuMergeBatch.*` | Batches composites inside `KisAsyncMerger`; eligibility checks; CPU fallback. Always compiled; inert without `HAVE_KRITA_GPU_ENGINE`. |
| `gpu/KisGpuEngineSettings.*` | kritarc keys, the convert policy, GPU color spaces and the conversion target. Always compiled. |
| `gpu/KisGpuProjectionCompositor.*` | Runs a batch: scratch tiles, layer accesses, dispatch, write-back; per-thread work contexts. |
| `kis_async_merger.{h,cpp}` | `m_gpuBatch` member, `tryAdd` in `compositeWithProjection`, flush points. |
| `kis_updater_context.cpp` | Tile-aligned job exclusivity when `KisGpuMergeBatch::mayCompositeOnGpu`. |
| `gpu/KisGpuConvolutionWorker.*` | Phase-4.98 Gaussian convolution: old-data snapshot, one GPU submission into a temporary device, CPU copy into the rect. Called by `KisGaussianKernel::applyGaussian()`. |
| `gpu/KisGpuMaskingWorker.*` | Phase-4.101 masking brush composite for `KisPainterBasedStrokeStrategy::doMaskingBrushUpdates()`; `supports()` is also used by `KisBrushOp`. |

## Source map (`libs/ui`)

| File | Role |
| --- | --- |
| `opengl/KisGpuCanvasUploader.*` (Windows, `HAVE_KRITA_GPU_CANVAS`) | GL interop check, display conversion setup, shared-buffer pool, Vulkan submission of canvas patches, acquire/release on the GUI thread. |
| `opengl/kis_texture_tile_update_info.h` | `uploadGeometry()` (shared CPU/GPU layout), `setGpuUpload()`. |
| `opengl/kis_texture_tile.{h,cpp}` | GPU branch of `update()` (upload from the bound shared buffer); class exported for tests. |
| `opengl/KisOpenGLUpdateInfoBuilder.{h,cpp}` | GPU path for the live canvas (`allowGpuUpload`), CPU fallback with batched downloads; `buildUpdateInfos()` shares one upload between rects. |
| `canvas/KisCanvasUpdateBatcher.*`, `canvas/kis_canvas2.cpp` | Group commit of concurrent projection updates into shared builds (phase 4.81). |
| `canvas/kis_canvas_updates_compressor.*` | `putUpdateInfos()`: atomic put of one batch. |
| `canvas/kis_abstract_canvas_widget.h`, `canvas/kis_canvas_widget_base.*`, `opengl/kis_opengl_canvas2.*`, `opengl/KisOpenGLCanvasRenderer.*`, `opengl/kis_opengl_image_textures.*` | `sharesProjectionUploads()` / `startUpdateCanvasProjections()`; batch-wide GL hold in `KisOpenGLCanvas2::updateCanvasProjection(QVector)`. |
| `opengl/kis_opengl_image_textures.cpp` | `checkGLInterop()` in `initGL()`; acquire/release around tile updates; readback of failed GL imports. |
| `KisGpuEngineUi.*` | Conversion offer, failure message, pre-save warning, `KisGpuEngineSettingsWidget`. |
| `KisMainWindow.cpp` | `install()`, `offerConversion()`, `confirmSave()` hooks. |
| `KisImportExportManager.cpp` | `mayFinishExport()` before the target file is replaced. |
| `dialogs/kis_dlg_preferences.{h,cc}` | The settings widget in the Performance tab. |
| `tiles3/KisTileGpuHooksStub.cpp` | Hooks for builds without `kritagpu`. |

## Configuration and compatibility

- kritarc (default group): `Solstice/GpuEngine` (bool; the embedded default
  `krita/data/kritarc` sets true since 2026-10-07, the code fallback is
  false) and
  `Solstice/GpuEngineConvertDocuments` (int: 0 Ask, 1 Convert, 2 Keep;
  default 0). See "Feature gate and user interface".
- Environment `KRITA_GPU_PROJECTION=1` or `0` overrides `Solstice/GpuEngine`
  (development switch; `KisGpuMergeBatch::setEnabled()` in tests).
- Device selection refuses `VK_PHYSICAL_DEVICE_TYPE_CPU` devices (software
  Vulkan such as lavapipe or SwiftShader, typical in virtual machines; they
  are slower than the CPU paths), 2026-10-11 at the user's request:
  `KisGpuContext::acceptsDeviceType()`, rejection reason "software (CPU)
  device" in the "GPU engine unavailable" log line. `KRITA_GPU_ALLOW_SOFTWARE_DEVICE=1`
  accepts them (diagnostics). Integrated and virtual GPUs are still
  accepted when they meet the feature requirements (discrete GPUs are
  preferred). Test: `KisGpuEngineTest::testSoftwareDevicesRefused`.
- With the engine on, `KRITA_GPU_TRANSFORM=0`, `KRITA_GPU_LIQUIFY=0`,
  `KRITA_GPU_PUPPET=0` and `KRITA_GPU_CONVOLUTION=0` keep the affine passes,
  Liquify (and with it Puppet Warp, which shares the grid warp), Puppet Warp
  alone and the Gaussian convolution on the CPU (development switches).
- No `.kra` changes or persisted ids: `.kra` files stay readable by the CPU
  path and upstream Krita; GPU mode stores RGBA F32/F16 layers with the
  normal Krita color space ids. Saving works through the CPU path: the tile
  compressor locks tiles, which downloads stale data.

## Invariants

- `kritagpu` depends only on `kritaglobal` and Qt Core/Gui. Never link the
  Vulkan loader at build time; never include `<windows.h>` from its headers.
- `kritaimage` builds and works without `kritagpu` (`HAVE_KRITA_GPU_ENGINE`
  off): no tile data then ever gets a GPU state.
- At least one of a tile data's CPU and GPU copies is valid, except between
  `KisGpuTileAccess::prepare()` and `submitAndFinish()` for tiles the access
  writes. No CPU thread may lock those tiles in that window (stroke
  discipline). A failed download never leaves both copies invalid; lost
  content is marked `ContentLost` and stops the engine instead of being
  treated as valid.
- `detachForExternalWrite` refuses (sets `isLocked`) when a CPU user holds
  the tile, whether or not its data is shared. This is a snapshot check;
  GPU writers must still not run concurrently with CPU users of the same
  tiles.
- Uploads are published only through `submitAndFinish()`, never with a
  stale CPU write generation.
- Process-wide Vulkan objects (`KisGpuTileBackend`, the projection
  compositor's work contexts) are never destroyed: static destruction can
  run after the Vulkan loader and layers are torn down (this crashed the
  tests with `0xC0000409` under the validation layer).
- Every command buffer of a command list (the main one and the preamble)
  begins with a full memory barrier; the engine's only
  GPU dependency model is submission order on the single queue. Flags change
  only after the submission exists (`finish(value)`), so a later download is
  always ordered after the write it depends on.
- Lock order: `KisGpuTileBackend::residencyMutex()`, then
  `KisTileGpuState::mutex` (several: sorted by address), then
  `KisGpuTileBackend::m_transferMutex`.
- The GPU merge batch must be flushed before anything reads
  `KisAsyncMerger::m_currentProjection`; when adding a new reader to the
  merger, add a flush before it.
- Canvas patches written by the GPU must keep exactly the layout of
  `KisTextureTileUpdateInfo::uploadGeometry()` (including zero corners);
  change both paths together.
- A `KisGpuGLSharedBuffer` is never written by Vulkan while GL may still
  read it: it returns to the pool only when no tile info references it.
- Canvas updates sharing one upload (a `KisCanvasUpdateBatcher` build) are
  put into the compressor together and applied in one GUI pass under one GL
  hold. Do not split them across passes or release the hold between them.
- A batched canvas update's caller returns only after its build submitted and
  put it, preserving the order relative to later conflicting walkers.
- The GPU writes projection tiles in place only while merge jobs that
  share a tile are kept apart (`KisUpdaterContext::walkerIntersectsJob`)
  and only for projections on the 64 px image grid.
- Krita must start and work without Vulkan; GPU features degrade to the CPU
  path.
- Blend results match the `KoCompositeOp` RGBA float implementation (alpha-0
  color excepted). Preview and final render must stay identical (Transform
  Tool, Puppet Warp).
- Tile slots are released only after all GPU work using them has completed.
- The GPU Gaussian convolution reads a snapshot of the device's old data and
  writes the device only through a CPU copy of its own rect. Concurrent
  filter-stroke patches rely on this; do not make it write the device's
  tiles from the GPU directly.
- `KisFilter` has the Solstice virtual `prefersSingleCall()`. Changing
  `KisFilter`'s virtual functions requires rebuilding and installing every
  filter plugin; tests load the installed plugins.
- GL objects of a shared image are created and destroyed with its GL context
  current; the image stays in `VK_IMAGE_LAYOUT_GENERAL`.

## Manual regression checklist

Phase 0 has no user-visible behavior. From phase 3 on:

1. Open, display, zoom, rotate, and mirror a multi-layer document in GPU mode.
2. Paint, undo, redo; compare with the CPU mode result.
3. Save, reopen in CPU mode and in upstream Krita.
4. Enable Preferences → Performance → GPU Engine, restart: the status shows
   the active GPU after opening an RGBA float document.
5. Open an RGBA 8-bit PNG: the conversion question appears; Convert keeps
   the appearance and undo restores 8-bit; Keep leaves it on the CPU path;
   "Do not ask again" is honored on the next open and changeable in
   Preferences.
6. Disable the setting and restart: no question, no GPU use
   (`KRITA_GPU_PROJECTION=0` likewise).
7. Gaussian Blur, Unsharp Mask and Gaussian High Pass on RGBA32F and RGBA16F
   layers (phase 4.98): apply time and result, with a selection, Undo/Redo,
   the dialog preview, and a filter mask while painting below it.
8. Puppet Warp apply on RGBA32F and RGBA16F layers (phase 4.96): one pin,
   several pins with a turned joint and pin orders, Undo/Redo; the result
   matches the preview.
9. Masking brush on the GPU brush (phase 4.101) on an RGBA32F layer: a
   textured masked preset (e.g. `z) 线稿 纹理`) with both mirrors at about
   40px and at a large size, a masked preset in Wash and Buildup, another
   masking mode (e.g. Darken or Subtract), Undo/Redo; the stroke looks like
   the CPU brush (GPU brush off) and does not lag behind it.
10. Textured dabs on the GPU brush (phase 4.102) on an RGBA32F layer: a
    textured round brush (Multiply, then Subtract and a Height mode, hard and
    soft texturing, strength on pressure) at small and large sizes, with
    mirrors, in Wash and Buildup; the texture lines up between dabs and with
    the CPU brush (GPU brush off); Undo/Redo; a Lightness-mode texture
    still paints (CPU).
11. Image tip dabs on the GPU brush (phase 4.103) on an RGBA32F layer: a
    predefined GBR or PNG tip and an imported ABR tip at small and large
    sizes, with rotation (fixed angle and Drawing Angle), mirrors, a texture,
    Wash and Buildup; the stroke matches the CPU brush (GPU brush off) and
    the brush outline; Undo/Redo. Phase 4.104: a colour tip as Image
    Stamp, as Lightness Map (change the color and the Lightness strength)
    and as Gradient Map (change the gradient), with rotation, a texture,
    Wash and Buildup; each looks like the CPU brush. On an 8-bit layer image
    tips look as before.
12. RGBA16F image tips and textures (phase 4.105) on an RGBA 16-bit float
    layer: a gray tip, a colour tip as Image Stamp and Lightness Map, a
    textured round brush (Multiply, Overlay, a Height mode, hard and soft)
    and a textured image tip, Wash and Buildup; each looks like the CPU
    brush (GPU brush off); Undo/Redo.
13. RGBA16F masked strokes (phase 4.106) on an RGBA 16-bit float layer: a
    masked preset (e.g. `z) 线稿 纹理`) at small and large sizes, another
    masking mode (Darken or Subtract), with a texture, Wash and Buildup;
    the stroke looks like the CPU brush and does not lag; Undo/Redo.
14. Blend modes (phase 4.107): on an RGBA 16-bit float layer, a brush in
    Soft Light (Photoshop), Vivid Light, Hard Mix (Softer Photoshop) and
    Lighter Color; on both float formats, HSV Hue, HSL Saturation, HSI
    Intensity, Tint and Decrease Value, as brush modes (Buildup, Wash,
    mirrors, alpha lock) and as layer blend modes; each looks like the CPU
    (GPU brush off, or GPU Engine off); Undo/Redo.
15. Other blend modes (phase 4.108), on RGBA 32-bit and 16-bit float layers,
    as layer modes and brush modes (Buildup, Wash): Glow, Heat, Gamma Light,
    Hard Overlay, Penumbra A, Easy Dodge, Modulo Shift, Lambert Lighting,
    Behind, Greater, Luminosity SAI and Copy Red; each looks like the CPU;
    Destination In still works (CPU); Undo/Redo.

## Phase history

The review notes and phase records (phases 4.1 and later) are in the wiki history pages,
moved verbatim on 2026-10-07. Add new phase records to the newest page, and summaries of
cross-cutting findings to `docs/agent/wiki/` (see `wiki/index.md`).

- [Reviews and memory budgets](wiki/history/gpu-reviews-and-memory.md): Canvas interop investigation, earlier phase reviews, tile memory budget and eviction, canvas transfer buffers.
- [Pixel brush and Wash (phases 4.1-4.16)](wiki/history/gpu-phases-4.01-4.16.md): GPU brush prototype, selections, channel locks, mirrors, Wash preview and final merge.
- [Blend modes, RGBA16F and transfers (phases 4.17-4.57)](wiki/history/gpu-phases-4.17-4.57.md): Blend-mode coverage, asynchronous submissions, RGBA16F brushes, context reuse, batched readbacks.
- [Paint trace and latency analysis (phases 4.58-4.81)](wiki/history/gpu-phases-4.58-4.81.md): Paint pipeline trace, capture protocol, real-app stage and lock analysis, shared canvas builds.
- [Generated dabs and brush latency (phases 4.82-4.92)](wiki/history/gpu-phases-4.82-4.92.md): GPU circle dabs, update period, Wash latency, shared pipelines, immediate canvas uploads.
- [Filters and transforms (phases 4.93-)](wiki/history/gpu-phases-4.93-.md): CPU filter/transform baseline, GPU affine passes, GPU Liquify grid warp, GPU Gaussian blur, GPU Puppet Warp rendering.

Section index:

- [Canvas interop investigation (2026-10-03)](wiki/history/gpu-reviews-and-memory.md#canvas-interop-investigation-2026-10-03)
- [Earlier phase reviews](wiki/history/gpu-reviews-and-memory.md#earlier-phase-reviews)
- [Tile memory budget and eviction](wiki/history/gpu-reviews-and-memory.md#tile-memory-budget-and-eviction)
- [Canvas transfer buffer budget and retirement](wiki/history/gpu-reviews-and-memory.md#canvas-transfer-buffer-budget-and-retirement)
- [Pixel brush prototype (phase 4.1)](wiki/history/gpu-phases-4.01-4.16.md#pixel-brush-prototype-phase-41)
- [Selection coverage for pixel brushes (phase 4.2)](wiki/history/gpu-phases-4.01-4.16.md#selection-coverage-for-pixel-brushes-phase-42)
- [Normal brush channel locks (phase 4.3)](wiki/history/gpu-phases-4.01-4.16.md#normal-brush-channel-locks-phase-43)
- [Mirrored pixel-brush compositing (phase 4.4)](wiki/history/gpu-phases-4.01-4.16.md#mirrored-pixel-brush-compositing-phase-44)
- [Combined mirror submissions (phase 4.5)](wiki/history/gpu-phases-4.01-4.16.md#combined-mirror-submissions-phase-45)
- [Brush job integration coverage (phase 4.6)](wiki/history/gpu-phases-4.01-4.16.md#brush-job-integration-coverage-phase-46)
- [GPU clipping to CPU paint rectangles (phase 4.7)](wiki/history/gpu-phases-4.01-4.16.md#gpu-clipping-to-cpu-paint-rectangles-phase-47)
- [Queued stroke measurements and Wash readback batching (phase 4.8)](wiki/history/gpu-phases-4.01-4.16.md#queued-stroke-measurements-and-wash-readback-batching-phase-48)
- [GPU Wash preview (phase 4.9)](wiki/history/gpu-phases-4.01-4.16.md#gpu-wash-preview-phase-49)
- [Sparse mirror destinations (phase 4.10)](wiki/history/gpu-phases-4.01-4.16.md#sparse-mirror-destinations-phase-410)
- [Post-stroke speculative CPU clones (phase 4.11)](wiki/history/gpu-phases-4.01-4.16.md#post-stroke-speculative-cpu-clones-phase-411)
- [Byte-bounded brush updates (phase 4.12)](wiki/history/gpu-phases-4.01-4.16.md#byte-bounded-brush-updates-phase-412)
- [Erase dab compositing (phase 4.13)](wiki/history/gpu-phases-4.01-4.16.md#erase-dab-compositing-phase-413)
- [Wash Erase GPU preview (phase 4.14)](wiki/history/gpu-phases-4.01-4.16.md#wash-erase-gpu-preview-phase-414)
- [GPU final Wash merge (phase 4.15)](wiki/history/gpu-phases-4.01-4.16.md#gpu-final-wash-merge-phase-415)
- [Selected Wash compositing (phase 4.16)](wiki/history/gpu-phases-4.01-4.16.md#selected-wash-compositing-phase-416)
- [Channel-locked Wash and Erase bundle (phases 4.17-4.19)](wiki/history/gpu-phases-4.17-4.57.md#channel-locked-wash-and-erase-bundle-phases-417-419)
- [Separable brush blend bundle (phases 4.20-4.22)](wiki/history/gpu-phases-4.17-4.57.md#separable-brush-blend-bundle-phases-420-422)
- [Bounded asynchronous brush submissions (phases 4.23-4.25)](wiki/history/gpu-phases-4.17-4.57.md#bounded-asynchronous-brush-submissions-phases-423-425)
- [Extended blend coverage (phases 4.26-4.28)](wiki/history/gpu-phases-4.17-4.57.md#extended-blend-coverage-phases-426-428)
- [Blend-family bundle (phases 4.29-4.31)](wiki/history/gpu-phases-4.17-4.57.md#blend-family-bundle-phases-429-431)
- [Bulk readback and projection measurements (phases 4.32-4.33)](wiki/history/gpu-phases-4.17-4.57.md#bulk-readback-and-projection-measurements-phases-432-433)
- [Textured and masked brush catch-up (phases 4.34-4.35)](wiki/history/gpu-phases-4.17-4.57.md#textured-and-masked-brush-catch-up-phases-434-435)
- [Layer channel flags and F16 blend arithmetic (phases 4.36-4.37)](wiki/history/gpu-phases-4.17-4.57.md#layer-channel-flags-and-f16-blend-arithmetic-phases-436-437)
- [CPU-to-GPU transition preparation (phases 4.38-4.40)](wiki/history/gpu-phases-4.17-4.57.md#cpu-to-gpu-transition-preparation-phases-438-440)
- [Batched voluntary tile eviction (phase 4.41)](wiki/history/gpu-phases-4.17-4.57.md#batched-voluntary-tile-eviction-phase-441)
- [Current-build benchmark baseline (phase 4.42)](wiki/history/gpu-phases-4.17-4.57.md#current-build-benchmark-baseline-phase-442)
- [Ordinary-stroke tile costs (phases 4.43-4.45)](wiki/history/gpu-phases-4.17-4.57.md#ordinary-stroke-tile-costs-phases-443-445)
- [RGBA16F Normal/Erase dabs (phase 4.46)](wiki/history/gpu-phases-4.17-4.57.md#rgba16f-normalerase-dabs-phase-446)
- [RGBA16F Alpha Darken and Wash (phases 4.47-4.48)](wiki/history/gpu-phases-4.17-4.57.md#rgba16f-alpha-darken-and-wash-phases-447-448)
- [RGBA16F basic blend brushes (phases 4.49-4.50)](wiki/history/gpu-phases-4.17-4.57.md#rgba16f-basic-blend-brushes-phases-449-450)
- [RGBA16F extended major blend brushes (phases 4.51-4.52)](wiki/history/gpu-phases-4.17-4.57.md#rgba16f-extended-major-blend-brushes-phases-451-452)
- [Projection context reuse (phase 4.53)](wiki/history/gpu-phases-4.17-4.57.md#projection-context-reuse-phase-453)
- [Batched CPU filter and FFT readback (phases 4.54-4.55)](wiki/history/gpu-phases-4.17-4.57.md#batched-cpu-filter-and-fft-readback-phases-454-455)
- [Batched affine transform and layer-flip readback (phases 4.56-4.57)](wiki/history/gpu-phases-4.17-4.57.md#batched-affine-transform-and-layer-flip-readback-phases-456-457)
- [Closed Transform Tool Undo investigation (October 4, 2026)](wiki/history/gpu-phases-4.17-4.57.md#closed-transform-tool-undo-investigation-october-4-2026)
- [Paint pipeline trace foundation (phase 4.58)](wiki/history/gpu-phases-4.58-4.81.md#paint-pipeline-trace-foundation-phase-458)
- [Paint trace identity and frame coverage (phase 4.59)](wiki/history/gpu-phases-4.58-4.81.md#paint-trace-identity-and-frame-coverage-phase-459)
- [Scheduled job trace lineage (phase 4.60)](wiki/history/gpu-phases-4.58-4.81.md#scheduled-job-trace-lineage-phase-460)
- [Logical dab and paint-batch trace membership (phase 4.61)](wiki/history/gpu-phases-4.58-4.81.md#logical-dab-and-paint-batch-trace-membership-phase-461)
- [Dirty groups and projection lineage (phase 4.62)](wiki/history/gpu-phases-4.58-4.81.md#dirty-groups-and-projection-lineage-phase-462)
- [Input audit and workload conditions (phase 4.63)](wiki/history/gpu-phases-4.58-4.81.md#input-audit-and-workload-conditions-phase-463)
- [Recorded branch completeness (phase 4.64)](wiki/history/gpu-phases-4.58-4.81.md#recorded-branch-completeness-phase-464)
- [Image-space rectangle coverage (phase 4.65)](wiki/history/gpu-phases-4.58-4.81.md#image-space-rectangle-coverage-phase-465)
- [Upload and widget geometry (phase 4.66)](wiki/history/gpu-phases-4.58-4.81.md#upload-and-widget-geometry-phase-466)
- [Explicit stroke membership and joined checks (phase 4.67)](wiki/history/gpu-phases-4.58-4.81.md#explicit-stroke-membership-and-joined-checks-phase-467)
- [Dirty and compressed-update geometry bridges (phase 4.68)](wiki/history/gpu-phases-4.58-4.81.md#dirty-and-compressed-update-geometry-bridges-phase-468)
- [Qt command-presentation timing (phase 4.69)](wiki/history/gpu-phases-4.58-4.81.md#qt-command-presentation-timing-phase-469)
- [Explicit execution-path evidence (phase 4.70)](wiki/history/gpu-phases-4.58-4.81.md#explicit-execution-path-evidence-phase-470)
- [Projection reuse and skip evidence (phase 4.71)](wiki/history/gpu-phases-4.58-4.81.md#projection-reuse-and-skip-evidence-phase-471)
- [Nine-process software-timing baseline (phase 4.72)](wiki/history/gpu-phases-4.58-4.81.md#nine-process-software-timing-baseline-phase-472)
- [Upload-boundary wall-interval analysis (phase 4.73)](wiki/history/gpu-phases-4.58-4.81.md#upload-boundary-wall-interval-analysis-phase-473)
- [Direct update-ready to upload-issue analysis (phase 4.74)](wiki/history/gpu-phases-4.58-4.81.md#direct-update-ready-to-upload-issue-analysis-phase-474)
- [Projection-end to prepared-update analysis (phase 4.75)](wiki/history/gpu-phases-4.58-4.81.md#projection-end-to-prepared-update-analysis-phase-475)
- [Native preparation stage probes (phase 4.76)](wiki/history/gpu-phases-4.58-4.81.md#native-preparation-stage-probes-phase-476)
- [Focused real-app stage capture (phase 4.76 follow-up)](wiki/history/gpu-phases-4.58-4.81.md#focused-real-app-stage-capture-phase-476-follow-up)
- [Tile preparation and submission substage probes (phase 4.77)](wiki/history/gpu-phases-4.58-4.81.md#tile-preparation-and-submission-substage-probes-phase-477)
- [Real-app submission substage findings (phase 4.77 follow-up)](wiki/history/gpu-phases-4.58-4.81.md#real-app-submission-substage-findings-phase-477-follow-up)
- [Residency lock-holder tracing (phase 4.78)](wiki/history/gpu-phases-4.58-4.81.md#residency-lock-holder-tracing-phase-478)
- [Real-app residency overlap (phase 4.78 follow-up)](wiki/history/gpu-phases-4.58-4.81.md#real-app-residency-overlap-phase-478-follow-up)
- [End main command recording before residency acquisition (phase 4.79)](wiki/history/gpu-phases-4.58-4.81.md#end-main-command-recording-before-residency-acquisition-phase-479)
- [Early-finish real-app result: no demonstrated improvement](wiki/history/gpu-phases-4.58-4.81.md#early-finish-real-app-result-no-demonstrated-improvement)
- [Queue-lock and driver-call timestamps (phase 4.80)](wiki/history/gpu-phases-4.58-4.81.md#queue-lock-and-driver-call-timestamps-phase-480)
- [Real-app driver-call overlap (phase 4.80 follow-up)](wiki/history/gpu-phases-4.58-4.81.md#real-app-driver-call-overlap-phase-480-follow-up)
- [Shared canvas update builds (phase 4.81)](wiki/history/gpu-phases-4.58-4.81.md#shared-canvas-update-builds-phase-481)
- [Real-app capture of shared canvas builds (phase 4.81 follow-up)](wiki/history/gpu-phases-4.58-4.81.md#real-app-capture-of-shared-canvas-builds-phase-481-follow-up)
- [Byte-limited GPU brush batches continue at once (phase 4.82)](wiki/history/gpu-phases-4.82-4.92.md#byte-limited-gpu-brush-batches-continue-at-once-phase-482)
- [GPU-evaluated circle dabs (phase 4.83, priority 3 step 1)](wiki/history/gpu-phases-4.82-4.92.md#gpu-evaluated-circle-dabs-phase-483-priority-3-step-1)
- [Exact Gaussian and fused default circle dabs (phase 4.84)](wiki/history/gpu-phases-4.82-4.92.md#exact-gaussian-and-fused-default-circle-dabs-phase-484)
- [Skipped CPU generation of described dabs (phase 4.85)](wiki/history/gpu-phases-4.82-4.92.md#skipped-cpu-generation-of-described-dabs-phase-485)
- [RGBA16F generated dabs (phase 4.86)](wiki/history/gpu-phases-4.82-4.92.md#rgba16f-generated-dabs-phase-486)
- [Soft (curve) circle generated dabs (phase 4.87)](wiki/history/gpu-phases-4.82-4.92.md#soft-curve-circle-generated-dabs-phase-487)
- [GPU brush update period (phase 4.88)](wiki/history/gpu-phases-4.82-4.92.md#gpu-brush-update-period-phase-488)
- [Wash latency breakdown and chained brush updates (phase 4.89)](wiki/history/gpu-phases-4.82-4.92.md#wash-latency-breakdown-and-chained-brush-updates-phase-489)
- [GPU base copy in the Wash preview (phase 4.90)](wiki/history/gpu-phases-4.82-4.92.md#gpu-base-copy-in-the-wash-preview-phase-490)
- [Shared and precompiled compute pipelines (phase 4.91)](wiki/history/gpu-phases-4.82-4.92.md#shared-and-precompiled-compute-pipelines-phase-491)
- [Immediate canvas uploads on the shared GPU path (phase 4.92)](wiki/history/gpu-phases-4.82-4.92.md#immediate-canvas-uploads-on-the-shared-gpu-path-phase-492)
- [Priority 4 baseline: CPU filter and transform costs (phase 4.93)](wiki/history/gpu-phases-4.93-.md#priority-4-baseline-cpu-filter-and-transform-costs-phase-493)
- [GPU affine transform passes (phase 4.94)](wiki/history/gpu-phases-4.93-.md#gpu-affine-transform-passes-phase-494)
- [GPU Liquify grid warp (phase 4.97)](wiki/history/gpu-phases-4.93-.md#gpu-liquify-grid-warp-phase-497)
- [GPU Gaussian blur family (phase 4.98)](wiki/history/gpu-phases-4.93-.md#gpu-gaussian-blur-family-phase-498)
- [GPU Puppet Warp mesh rendering (phase 4.96)](wiki/history/gpu-phases-4.93-.md#gpu-puppet-warp-mesh-rendering-phase-496)
- [GPU masking brush composite (phase 4.101)](wiki/history/gpu-phases-4.93-.md#gpu-masking-brush-composite-phase-4101)
- [GPU textured dabs (phase 4.102)](wiki/history/gpu-phases-4.93-.md#gpu-textured-dabs-phase-4102)
- [GPU image tip dabs (phase 4.103)](wiki/history/gpu-phases-4.93-.md#gpu-image-tip-dabs-phase-4103)
- [GPU colour, Lightness and Gradient image tips (phase 4.104)](wiki/history/gpu-phases-4.93-.md#gpu-colour-lightness-and-gradient-image-tips-phase-4104)
- [RGBA16F image tips and textures (phase 4.105)](wiki/history/gpu-phases-4.93-.md#rgba16f-image-tips-and-textures-phase-4105)
- [RGBA16F masking brush composite (phase 4.106)](wiki/history/gpu-phases-4.93-.md#rgba16f-masking-brush-composite-phase-4106)
- [Remaining blend modes (phase 4.107)](wiki/history/gpu-phases-4.93-.md#remaining-blend-modes-phase-4107)
- [All other blend modes (phase 4.108)](wiki/history/gpu-phases-4.93-.md#all-other-blend-modes-phase-4108)
- [Precise contraction audit (phase 4.109)](wiki/history/gpu-phases-4.93-.md#precise-contraction-audit-phase-4109)

## Risks and open questions

- Interop requires desktop OpenGL. With the engine on, "Auto" picks it on
  Windows unless the driver is blacklisted; an explicit ANGLE choice still
  disables interop.
- Memory management now bounds and evicts tile pools, including a path to
  disk swap. Canvas transfer buffers have a separate cap. GPU-resident tiles
  still retain CPU snapshots, other temporary allocations and GL textures
  have no shared budget. Voluntary tile eviction downloads are now batched;
  disk swap still uses the single-tile hook.
- Individual iterator reads of stale tiles still download synchronously per
  tile; bulk `readBytes`, planar reads, CPU filter inputs/destinations and FFT
  cache reads now batch automatically. Other large
  iterator-based consumers should call `KisGpuTileAccess::syncToCpu` first.
  Host-cached staging brought the tested CPU-to-GPU transition close to
  resident update time; other GPUs/drivers and larger documents still need
  measurements. Initial preparation/upload costs remain higher than resident
  updates.
- `KisGpuLayerStackCompositor`, `KisGpuLayerCompositor`, and
  `KisGpuTileFill` reuse one host-visible table buffer, so callers must wait
  between recordings (the projection compositor keeps one per work context);
  replace with a per-submission ring before overlapping submissions.
- Projection work contexts prefer completed buffers and permit three pending
  serial submissions; they still wait under pressure before reusing busy
  command/table buffers. Successful submissions return without an end wait.
  The separate bounded upload ring added in 4.23-4.25 is specific to brushes.
- Unsupported in the GPU projection (CPU fallback): layer styles, blend modes
  outside the list above, layers whose offset is
  not a multiple of 64 relative to the projection, color space mismatches.
- The whole-stack upload measured 55 GB/s with one `vkCmdCopyBuffer` region per
  tile; evaluate a compute scatter from ReBAR memory for document load.
- Every GPU blend mode has F16 brush and layer parity coverage since phase
  4.107; phase 4.108 added the rest except the modes listed in "All other
  blend modes", whose results the GPU cannot reproduce.
- Python scripting (`libkis`) and file export need CPU pixel access; every such
  call becomes a synchronous download.
