---
type: history
topic: gpu-engine
status: archived record (moved from docs/agent/gpu-engine.md on 2026-10-07)
sources: [docs/agent/gpu-engine.md, git history]
---

# GPU engine history: filters and transforms (phases 4.93-)

CPU filter/transform baseline, GPU affine passes, GPU Liquify grid warp, GPU Gaussian blur, GPU Puppet Warp rendering. Moved verbatim from `docs/agent/gpu-engine.md`; current state, decisions and
invariants stay there ([gpu-engine.md](../../gpu-engine.md)). Each section records what was true
when it was written; later sections and the current document take precedence.

## Priority 4 baseline: CPU filter and transform costs (phase 4.93)

The user chose priority 4 (filters and transforms) after phase 4.92.
`KisGpuPaintDeviceTest::benchmarkFiltersAndTransforms`, opt-in with
`KRITA_GPU_BENCHMARK_FILTERS=1`, measures a 2480x3508 RGBA32F layer (the
user's document size) with random content, as the median of 3 runs on 32
hardware threads.

- **Filter rows** time one `KisFilter::process` call over the layer, and the
  layer split into 32 bands on a `QThreadPool` (source to destination, like a
  filter stroke's parallel patches).
- **Transform rows** time one worker call, as the Transform Tool does:
  - `InplaceTransformStrokeStrategy::reapplyTransform` renders the live
    preview with `KisTransformUtils::transformDevice`, at a preview level of
    detail (`forceLodMode`).
  - The final apply renders at full resolution.

| Operation | Single call | 32 bands |
| --- | --- | --- |
| Readback of a GPU-written layer | 14.9ms | - |
| Gaussian blur r5 | 1021.7ms | 143.5ms |
| Gaussian blur r30 | 1065.9ms | 199.3ms |
| Levels (lightness curve) | 173.5ms | 229.2ms |
| Curves (all channels) | 0.1ms | 24.3ms |
| HSV adjust s+20 | 175.8ms | 47.6ms |
| Unsharp mask | 6155.6ms | 389.8ms |
| Affine scale 0.9 + rotate 10°, bicubic | 440.5ms | - |
| Puppet Warp (rigid MLS, 9 points) | 601.6ms | - |
| Liquify (20 moves, sigma 200) | 217.0ms | - |

Notes:

- **Readback is not the bottleneck.** About 15ms for the whole layer, after
  phases 4.54-4.57 batched it.
- **The transform workers are single-threaded.** Their full call is the
  user's wait on apply. Their preview cost scales with the preview level of
  detail.
- **Filters are already parallel.** About 140-390ms per full-layer
  application on this machine.
- **Suspect rows:**
  - Curves: the single in-place call returned in 0.1ms, so the properties set
    by name may not have produced a non-identity transfer. This row needs a
    configuration built through the filter's own API before it is used.
  - Levels: slower in bands, probably because each `process` call builds the
    color transformation.
- **Proposed order by these numbers:**
  1. Transform Tool rendering: affine bicubic first, as the most common
     operation, then the warp/Puppet Warp and Liquify workers.
  2. The blur family: Gaussian, then Unsharp, which is built on it.
  3. Per-pixel color adjustments, where parallel CPU code is already fast.

  This needs the user's choice.

## GPU affine transform passes (phase 4.94)

The user chose the Transform Tool, starting with the affine transform.

**How the CPU worker works.** `KisTransformWorker::runPartial` resamples with
two in-place passes, `transformPass<H>` then `transformPass<V>`. The general
branch uses `a, b, c` / `e, d, f`; the shear branch uses
`xscale, yscale*xshear, 0` / `yscale, yshear, 0`.

- Each line runs `KisFilterWeightsApplicator::processLine`. For every
  destination pixel, `calculateBlendSpan` uses `KisFixedPoint` (24.8)
  arithmetic plus one double `dstToSrc()` division to pick one of the 256
  `KisFilterWeightsBuffer` entries (`qint16` weights summing to 255).
- `KoMixColorsOpImpl::mixColors` blends with double accumulators
  (`mixtype` is double for float and half), divides, clamps to
  +-FLT_MAX / +-HALF_MAX, and stores; half stores go double -> float -> half.
- Source pixels outside the line come from the line's first/last pixel when
  `shear == 0` (clamp to edge); otherwise from the device default pixel.
- The source line is cleared to the default pixel, then the destination range
  is written.

**GPU implementation.**

- **`libs/gpu/shaders/transform_pass.comp`** (RGBA32F/F16):
  - One invocation per pixel of the destination tile grid.
  - Resampled pixels inside each line's `[dstStart, dstEnd)`; the default
    pixel elsewhere, which is what the in-place CPU passes leave there.
  - The span lookup is the same integer fixed-point code with `precise`
    doubles and a correctly rounded division (fma residual over neighbouring
    doubles), plus `int()` truncation.
  - Accumulation follows the CPU's order without contraction. Divisions are
    correctly rounded, and double -> float uses nearest-even with neighbour
    checks; F16 then rounds like Imath (`roundToHalf`).
  - A non-positive total alpha gives zeros, as the CPU's `memset`.
- **`KisGpuTransformPass`** (`libs/gpu`) records a pass from line ranges,
  weights and tile tables. It requires `shaderFloat64`, which `KisGpuContext`
  now enables when supported (`KisGpuDeviceInfo::supportsFloat64`).
- **`KisFilterWeightsApplicator::setupLine()`** now holds the line setup from
  `processLine()` (destination range and source borders); `processLine()`
  calls it. Both the CPU and the GPU plan use the same code.
- **`kis_transform_worker.cc::planGpuPass()/runGpuPasses()`** plan both
  passes with the CPU classes. The weights headers define non-inline
  functions and can only be included in this translation unit.
- **`KisGpuTransformWorker::runPlannedPasses()`** (`libs/image/gpu`):
  - The x pass goes from the device (`ReadOnly`) into a temporary device
    (`WriteOnly`).
  - The y pass goes from the temporary device into the device, `WriteOnly`
    over the tile grid of source | x rect | y rect.
  - Each pass is its own submission and waits; on failure the device is
    unchanged and the CPU passes run.
  - The written rect is then downloaded in one batch, because the following
    CPU `purgeDefaultPixels()` downloaded the tiles one by one (about 250ms
    instead of about 15ms).
- **Scope.**
  - Only `KisTransformWorker::run()` (`m_wholeDevice`): `runPartial()` callers
    may have pixels outside the rect, which the in-place CPU passes leave
    untouched.
  - RGBA32F/F16, with LOD 0, no wrap-around, and a line setup that is never
    empty (an empty CPU line keeps its source pixels).
  - `KRITA_GPU_TRANSFORM=0` disables it; it is also off when the GPU engine
    is disabled.
  - The 90-degree quadrant pre-rotation, simple translations and flips, and
    the single-pass shear stay on the CPU, as do the perspective worker, warp,
    Puppet Warp, Liquify, cage and mesh.
  - The Transform Tool's live preview renders at a preview level of detail,
    so it stays on the CPU. The final apply (`transformDevice` ->
    `KisTransformWorker::run()`) uses the GPU. Free Transform's perspective
    pass returns at once for an identity perspective.
- **Cached bounds.** GPU writes did not invalidate `KisPaintDevice`'s cached
  bounds, whereas writable CPU iterators invalidate them when created. The
  first parity run returned the pre-transform `exactBounds()`. New
  `KisPaintDevice::invalidateCachedBounds()` is now called by
  `KisGpuTileAccess` for write accesses at construction and on publish.
- **Debug.** `KRITA_GPU_TRANSFORM_DEBUG=1` prints stage times; trace scope
  `transform.gpu_passes`.

**Tests** (Vulkan validation):

- New `KisGpuPaintDeviceTest::testGpuTransformMatchesCpu`, 58 rows, all
  bit-identical to the CPU, with exact bounds and Undo/Redo:
  - F32 and F16;
  - scale+rotate, upscale, downscale, rotate only, sub-pixel scale,
    non-uniform, flip+scale, 100 degrees (quadrant), shear+scale, two-axis
    shear and tiny scale;
  - plain, offset device and opaque default pixel variants;
  - every filter strategy.
- `KisGpuPaintDeviceTest` 209 (1 skipped: the opt-in benchmark),
  `KisGpuProjectionTest` 199, `KisGpuBrushTest` 4451, `KisGpuStrokeTest` 360,
  `KisGpuBrushJobsTest` 41, `KisGpuEngineTest` 11, `KisGpuCanvasUploadTest` 38,
  `KisGpuGLInteropTest` 3.
- `kis_transform_worker_test` 40, also with `KRITA_GPU_PROJECTION=1`; its
  devices are mostly 8-bit, which the GPU path does not handle.
- `kis_paint_device_test` 51.

**Benchmark** (2480x3508 RGBA32F, scale 0.9, rotate 10 degrees, bicubic,
CPU-resident source): CPU 432ms, GPU 63ms (6.8x).

- The GPU passes take 33-56ms. The compute is about 3ms per pass; the rest is
  uploading a CPU-resident source, allocating the destination tiles and the
  batched download.
- Planning takes 0.1ms.

Installed gpu/image/ui/defaultpaintops; hashes match. The user reported the
manual checks OK on 2026-10-06:
- Free Transform scale and rotate on normal and Background layers: speed,
  result, Undo/Redo;
- flips, large down- and upscales;
- RGBA16F documents;
- unchanged perspective, warp, Puppet Warp and Liquify;
- image resize and rotate.

## GPU Liquify grid warp (phase 4.97)

The user chose Liquify next (task order 1, 3, 4, 6 of 2026-10-07). Phase
4.96 stays reserved for Puppet Warp's mesh rendering.

**How the CPU worker paints.** `KisLiquifyTransformWorker::run()` clears the
destination and calls `GridIterationTools::iterateThroughGrid()` over the
sub-grid touched by strokes (`calculateCorrectSubGrid()`), then copies the
rest of the source rect (`cutOutSubgridFromBounds()`). For every cell,
`PaintDevicePolygonOp`:

- skips an empty bound rect (`clipDstPolygon.boundingRect().toAlignedRect()`);
- copies a pixel-aligned identity cell with `KisPainter::copyAreaOptimized()`,
  postponed and merged (`KisRegion::mergeSparseRects()`) when
  `canProcessRectsInRandomOrder()` found every cell convex;
- otherwise first flushes the postponed copies, then visits every pixel of the
  bound rect inside the polygon (`QPolygonF::containsPoint`, odd-even) and
  writes `KisRandomSubAccessor::sampledOldRawData()` at
  `KisFourPointInterpolatorBackward::getValue()` (or at
  `fallbackSourcePoint()` when the interpolator is not valid).

A later cell overwrites the pixels of an earlier one where they overlap
(folds, and the one-pixel overlap from `adjustAlignedPolygon()`).

**GPU implementation.**

- **`KisGpuGridWarpWorker::Recorder`** (`libs/image/gpu`) is passed to
  `iterateThroughGrid()` in place of `PaintDevicePolygonOp` and mirrors its
  decisions, recording operations in order:
  - copies (split into 128x128 chunks);
  - warps, with the CPU's bound rect, the polygon edges' values that do not
    depend on the pixel (`qFuzzyCompare` skip, direction,
    `(x2 - x1) / (y2 - y1)`, the implicit closing edge), the interpolator's
    coefficients (new `KisFourPointInterpolatorBackward::coefficients()`),
    `isValid(0.1)` and the fallback point.
- **`libs/gpu/shaders/grid_warp.comp`**, through **`KisGpuGridWarpPass`**:
  - *claim* (`-DCLAIM`): one workgroup per operation; each pixel it writes
    stores `atomicMax(owner, index + 1)` in a device-local owner buffer over
    the union of the bound rects;
  - *resolve* (RGBA32F/F16): one invocation per destination tile-grid pixel;
    the owner computes it, the default pixel elsewhere.
  - `getValue()` in doubles with `precise`, correctly rounded division and
    square root (fma residuals over neighbouring doubles), and CPU branch order,
    including `yMu2` computed with `xBasedMu()`.
  - Sampling: `qRound()` is `int(d + 0.5)` for these non-negative values (Qt
    6.8 on x86-64 with SSE2); the `qint16` weights are mixed like
    `KoMixColorsOpImpl::mixColors()` with their sum as the alpha divisor,
    with the double -> float (-> half) rounding of phase 4.94.
  - A sample outside the source tile grid sets a flag. The source access
    covers the source polygons plus 8 pixels and the copy rects. The worker
    then returns false, and `run()` clears the destination and paints on the
    CPU.
- **`KisLiquifyTransformWorker::run()`** records and runs the GPU path when
  `KisGpuGridWarpWorker::canRun()`; the trailing copies outside the sub-grid
  stay on the CPU (`copyAreaOptimized()` shares whole tiles). The written rect
  is downloaded in one batch first.
- **Scope** (`canRun()`):
  - RGBA32F/F16, LOD 0, no wrap-around, `shaderFloat64`;
  - distinct devices with the same color space and default pixel;
  - **the same offset.** With different offsets `bitBlt()` cannot use
    `fastBitBlt()`. The Copy composite op (`KoOptimizedCompositeOpCopy128`)
    then clears fully transparent pixels in whole SIMD batches, and the
    color of transparent pixels depends on the batch grouping. The first test
    run failed on exactly those pixels. The Transform Tool's source is a copy
    of the node's device, so the offsets match.
  - `KRITA_GPU_LIQUIFY=0` or `KRITA_GPU_TRANSFORM=0` disables it.
  - The Liquify preview while painting (`runOnQImage()` on the 8-bit
    thumbnail) and level-of-detail previews stay on the CPU.
- **Debug.** `KRITA_GPU_TRANSFORM_DEBUG=1` prints the operation count and
  stage times; trace scope `transform.gpu_grid_warp`.

**Tests** (Vulkan validation):

- New `KisGpuPaintDeviceTest::testGpuLiquifyMatchesCpu`, 44 rows. All rows
  are bit-identical to the CPU, with exact bounds, Undo/Redo, and a
  destination that held pixels before `run()`:
  - F32 and F16;
  - push, wash, grow, shrink, rotate;
  - fold: large moves with small radii, giving crossing, non-convex cells and
    unmerged copies;
  - far: content pushed outside the source rect;
  - the undo brush, and no stroke;
  - pixel precision 1, 4, 8 and 16;
  - plain, offset and opaque-default variants.
- `KisGpuPaintDeviceTest` 251 (1 skipped: the opt-in benchmark),
  `kis_liquify_transform_worker_test` 15 (also with `KRITA_GPU_PROJECTION=1`),
  `KisGpuProjectionTest` 199, `KisGpuEngineTest` 11.

**Benchmark** (2480x3508 RGBA32F, 20 moves with sigma 200, about 46,000
operations; `benchmarkFiltersAndTransforms`): CPU 219ms, GPU 56ms (3.9x).
Temporary instrumentation, since removed, gave this breakdown:

| Stage | Time |
| --- | --- |
| Clear and `canProcessRectsInRandomOrder()` | about 13ms |
| Recording through `iterateThroughGrid()` | about 18ms |
| GPU path | about 20ms |
| Remaining CPU copies | about 2ms |

Inside the GPU path:

| Stage | Time |
| --- | --- |
| Prepare (source upload, 1216 tiles) | about 5ms |
| Table write (about 15MB of operations) | about 3.5ms |
| GPU wait | about 1.8ms |
| Batched download | about 7.6ms |

Further gains would need a recorder that walks the grid without the per-cell
`QVector`/`QPolygonF` allocations of `iterateThroughGrid()`, or a GPU
convexity check.

Installed gpu/image; hashes match. The user reported the manual checks OK on 2026-10-07:

- Liquify move, scale, rotate and undo strokes on RGBA32F and RGBA16F layers;
  apply, Undo and Redo;
- a large Liquify over most of a layer: apply time;
- 8-bit layers unchanged (CPU);
- the preview while painting unchanged.

## GPU Gaussian blur family (phase 4.98)

The user chose the blur family next (task order 2, 3, 4, 6, 7, 8, 9 of
2026-10-07) and approved the design on 2026-10-07: parity within a
tolerance, because bit identity is impossible, with Unsharp Mask and
Gaussian High Pass included.

**How the CPU computes it.** `KisGaussianKernel::applyGaussian()` uses the
FFT convolution when FFTW is available, as in this build
(`KisConvolutionPainter::supportsFFTW()`):

- `KisConvolutionWorkerFFT` with `createUniform2DKernel()`: the outer
  product `v * h` of the vertical and horizontal Gaussian vectors, divided
  by its sum (`factor`).
- The cache holds color times alpha, and alpha, in doubles from
  `oldRawData()`. It covers the rect plus the kernel's half sizes; the
  padding (4 x half width, 2 x half height) keeps the circular convolution
  from wrapping into the result.
- Writing (`writeResultToDevice()`):
  - alpha = sum x fftScale (offset 0), clamped to +-FLT_MAX / +-HALF_MAX,
    NaN to the lower bound, then stored;
  - when the stored alpha is below the type's epsilon (FLT_EPSILON,
    HALF_EPSILON) or alpha <= DBL_EPSILON, the convolved colors become 0;
  - otherwise color = sum x fftScale / alpha, clamped and stored.
  - Without alpha in the channel flags, colors are convolved without
    premultiplication. Channels that are not convolved keep their value.
- Borders: `BORDER_REPEAT` clamps source coordinates into
  `rect | defaultBounds()->bounds()` (repeat iterators); `BORDER_IGNORE`
  reads the device as it is.
- Callers: Gaussian Blur, Unsharp Mask and Gaussian High Pass (RGBA float
  candidates); layer styles and the Colorize Mask (8-bit selections, CPU).
- Filter strokes split the rect into `KritaUtils::optimalPatchSize()`
  patches (512x512 by default). The patches run concurrently, in place,
  under one transaction; each patch reads its neighbours' old data.

The FFT's rounding cannot be reproduced, so the GPU computes the same
convolution directly, in doubles.

**GPU implementation.**

- **`libs/gpu/shaders/separable_convolution.comp`**, through
  **`KisGpuSeparableConvolutionPass`**. The destination tile grid is
  processed in bands of tile rows; the intermediate buffer stays within
  64MB per band, and the pass refuses above 1GB.
  - Horizontal pass: premultiplied sums in doubles into a device-local
    intermediate (the band's rows plus the half height above and below;
    source rows clamped for `BORDER_REPEAT`).
  - Vertical pass (`-DVERTICAL`): every pixel of the band's destination
    tiles. Inside the rect it follows the FFT worker's write rules above;
    elsewhere, and in channels that are not convolved, it writes the source
    pixel. Doubles become float (and half) with the rounding of phase 4.94.
- **`KisGpuConvolutionWorker::applySeparable()`** (`libs/image/gpu`):
  - a snapshot of the read rect: `KisPainter::copyAreaOptimizedOldData()`
    into a clone (whole tiles are shared), as the CPU reads `oldRawData()`;
  - one submission: the snapshot `ReadOnly`, a temporary device
    `WriteOnly` over the rect;
  - a batched download of the result, then `KisPainter::copyAreaOptimized()`
    into the rect (the fast path: same offsets and color space).
  - Calls on disjoint rects of one device may therefore run concurrently,
    like a filter stroke's patches.
  - Below 32,768 source pixels (the rect plus the margins, about 180x180)
    the CPU is used: a GPU call costs about 1.7ms (see the benchmark).
- **`KisGaussianKernel::applyGaussian()`** tries it first (`runsOnGpu()`:
  FFTW and `KisGpuConvolutionWorker::canRun()`); on false the FFT runs.
- **`KisFilter::prefersSingleCall()`**, a new virtual (default false), makes
  `KisFilterStrokeStrategy` call `processImpl()` once over the process rect.
  `KisGaussianBlurFilter` returns `KisGaussianKernel::runsOnGpu(device)`.
  - Concurrent patches serialize their GPU calls: 114-200ms for 32 bands
    against 50-110ms for one call.
  - Unsharp Mask and Gaussian High Pass keep the patches, because their CPU
    steps (sharpening, grain extract) need the threads.
  - The new virtual changes `KisFilter`'s vtable: every filter plugin must
    be rebuilt and installed with `kritaimage`.
- **Scope** (`canRun()`):
  - RGBA32F/F16 with RGBA channel order, at least one convolved channel;
  - LOD 0: the Instant Preview at a lower level of detail stays on the CPU;
  - no wrap-around, `shaderFloat64`, the GPU engine on;
  - `KRITA_GPU_CONVOLUTION=0` disables it.
- **Debug.** `KRITA_GPU_CONVOLUTION_DEBUG=1` prints stage times; trace scope
  `filter.gpu_convolution`.

**Tests** (Vulkan validation):

- New `KisGpuPaintDeviceTest::testGpuGaussianMatchesCpu`, 30 rows:
  - F32 and F16;
  - radii 1, 5, 30, 100, 12x3, 0x7 and 2.5x40;
  - an offset device, a partial rect, `BORDER_IGNORE`, RGB only, alpha only,
    without green, an opaque default pixel, HDR colors (x40);
  - tolerance: relative 1e-5 (F32) or 1e-3 (F16, about one half ulp);
    colors are skipped where either alpha is near the null threshold;
  - pixels outside the rect are bit-identical; Undo/Redo.
  - Worst relative difference: F32 6e-8 (one rounding step), F16 identical.
- New `testGpuGaussianPatchesMatchCpu`: concurrent patches under one
  transaction; the narrow edge patches stay on the CPU (size threshold), so
  GPU and CPU patches mix.
- New `testGpuGaussianFiltersMatchCpu`, 6 rows: the three filters through
  `KisFilter::process()`, with and without a selection.
- New `libs/ui/tests/KisGpuFilterStrokeTest`, 6 rows: the three filters
  through `KisFilterStrokeStrategy` on an image, with Undo/Redo. Gaussian
  Blur makes one GPU call; the others one per patch.
- `KisGpuPaintDeviceTest` 288 (1 skipped: the opt-in benchmark),
  `KisGpuProjectionTest` 199, `KisGpuEngineTest` 11,
  `kis_convolution_painter_test` 20, `kis_filter_test` 8,
  `kis_filter_mask_test` 5, `kis_adjustment_layer_test` 6,
  `kis_lazy_brush_test` 13, `kis_async_merger_test` 12.
- `kis_all_filter_test` (an upstream broken test, not registered in ctest)
  fails on 8-bit reference images of many filters. The GPU path does not
  run for 8-bit images.

**Found while testing.**

- Unsharp Mask with a selection first differed in F32 by up to 0.11 at a few
  pixels. The selected copy uses `KoOptimizedCompositeOpCopy128`, whose SIMD
  path clamps colors to 1 for a whole batch only when some pixel of the
  batch is not opaque. A one-ulp alpha difference therefore changes the
  clamping of HDR colors in neighbouring pixels. The test keeps Unsharp
  Mask results below 1 (see
  [Krita copy and sampling semantics](../concepts/krita-copy-semantics.md)).
- The first run after adding the virtual crashed `kis_filter_test` and other
  filter tests with 0xc0000005: tests load the installed filter plugins,
  which still had the old vtable. A full install fixed it.

**Benchmark** (2480x3508 RGBA32F, `benchmarkFiltersAndTransforms`, medians
of three processes, the range of the process results in parentheses):

| Filter | CPU single call | CPU 32 bands | GPU single call | GPU 32 bands |
| --- | ---: | ---: | ---: | ---: |
| Gaussian blur r5 | 1015 (1012-1023) | 150 (149-152) | 50.0 (46.1-54.1) | 114 (113-115) |
| Gaussian blur r30 | 1085 (1079-1099) | 213 (210-214) | 62.0 (59.2-64.5) | 124 (124-126) |
| Gaussian blur r100 | 1759 (1745-1769) | 398 (394-400) | 110 (108-110) | 200 (200-215) |
| Unsharp mask | 6345 (6321-6399) | 408 (406-421) | 5011 (4965-5011) | 378 (372-379) |

- The Filter dialog's apply corresponds to "CPU 32 bands" (patches) before
  and "GPU single call" now for Gaussian Blur: 150-398ms -> 50-110ms.
- Unsharp Mask gains little: its sharpening step on the CPU dominates.

One `applyGaussian()` call on a small square (median of 9 per process,
medians of three processes; before the size threshold, the GPU took
1.7-1.9ms for the 64 and 128 rows):

| Square | r5 CPU | r5 GPU | r30 CPU | r30 GPU |
| --- | ---: | ---: | ---: | ---: |
| 64 | 0.41 | CPU | 1.21 | CPU |
| 128 | 1.03 | CPU | 2.95 | 1.91 |
| 256 | 4.89 | 1.92 | 10.82 | 2.15 |
| 512 | 35.5 | 2.94 | 33.1 | 3.46 |
| 1024 | 147 | 7.04 | 122 | 8.99 |

Installed with `cmake --install` (every filter plugin, because of the new
virtual). The user reported the manual checks OK on 2026-10-07: Gaussian
Blur, Unsharp Mask and Gaussian High Pass on RGBA32F and RGBA16F layers
(apply time, result, selection, Undo/Redo, preview, filter mask), 8-bit
unchanged.

## GPU Puppet Warp mesh rendering (phase 4.96)

Reserved since phase 4.95 and started after phase 4.98 (task order of
2026-10-07). The folded-joint issue of the mesh model stays open, as the user
decided on 2026-10-07 (`docs/agent/puppet-warp.md`); this phase only moves
the final rendering of the current model to the GPU.

**How the CPU renders.** `KisPuppetTransformWorker::run()` clears the
destination and calls `GridIterationTools::processGrid()` at 8 px precision
with `GridIterationTools::PaintDevicePolygonOp` behind `OrderFilterOp`
(skip cells without artwork; with several stacking groups, keep only the
group's cells). With one stacking group it paints straight into the
destination. With several, each group paints into its own layer, and the
layers are composited bottom to top with `COMPOSITE_OVER`.

**GPU implementation.** The polygon op is the one Liquify uses, so the
phase 4.97 recorder and pass are reused unchanged:

- One group: `KisGpuGridWarpWorker::Recorder` behind the same
  `OrderFilterOp`, then `KisGpuGridWarpWorker::run()` into the destination.
- Several groups: one grid pass with `GroupRecordersOp` records every group
  (the CPU walks the grid once per group), then each group is painted on the
  GPU into its layer and composited with `COMPOSITE_OVER` on the CPU as
  before.
- The layers now take the destination's offset (`moveTo()`), on the CPU path
  too; `KisGpuGridWarpWorker::canRun()` requires equal offsets.
- On failure the target is cleared and painted on the CPU.
- Scope: that of `KisGpuGridWarpWorker::canRun()` (RGBA32F/F16, LOD 0, equal
  color space, default pixel and offset). The preview while editing
  (`runOnQImage()`) and legacy MLS transforms stay on the CPU.
- `KRITA_GPU_PUPPET=0` disables it, as do `KRITA_GPU_LIQUIFY=0` and
  `KRITA_GPU_TRANSFORM=0` (they disable the shared grid warp).

**The "over" composite is not reproducible, even on the CPU.** The first
parity run differed by one ulp in one or two channels for several groups in
F32. Running the CPU path 20 times against itself differed the same way in
about half the runs. RGBA32F tile data is `malloc()`ed (16 bytes per pixel,
no pool; `KisTileData::allocateData()`), so its address modulo the SIMD
width varies, and `KoStreamedMath::genericComposite()` splits each row into
a scalar head, a vector body and a scalar tail by the address. The vector
and scalar "over" round differently (up to about 5 ulps at low alpha).
Therefore:

- one group (painted straight into the destination) and all F16 rows are
  compared bit for bit;
- several F32 groups are compared with the phase 0 compositing rule (relative
  1e-5).

**Tests** (Vulkan validation):

- New `KisGpuPaintDeviceTest::testGpuPuppetMatchesCpu`, 18 rows: F32 and F16;
  move (one group), elbow at 90 degrees, fold (137 degrees), orders front and
  back, squeeze; an offset variant for F32. Exact bounds, Undo/Redo, a
  destination that held pixels before `run()`. Stable over three runs.
- `KisGpuPaintDeviceTest` 306 (1 skipped: the opt-in benchmark),
  `KisPuppetTransformWorkerTest` 14, `test_animated_transform_parameters` 4,
  `kis_warp_transform_worker_test` 7.

**Benchmark** (2480x3508 RGBA32F, a full-layer mesh, three pins, the middle
one turned by 0.5 rad, three stacking groups; `benchmarkFiltersAndTransforms`,
medians of three processes): CPU 713 ms (690-754), GPU 190 ms (189-200).

- Recording each group in its own grid pass first gave 247 ms; one pass for
  all groups saved about 57 ms.
- Per group on the GPU: preparation 5-11 ms, wait 2-6 ms, download 7-15 ms.
  The rest is the grid pass (`map()`, `touchesArtwork()`, `ownerAt()` per
  cell) and the CPU composites.

**Reverted and reapplied.** The user's first check found Puppet Warp
misbehaving and the work was reverted (2026-10-07). The cause was older: with
the Accurate preview settings the Transform Tool made no thumbnail, so
Puppet Warp never built its mesh (legacy MLS, no mesh overlay, an empty
layer before the first pin). That was fixed (`docs/agent/puppet-warp.md`,
"Mesh mask and overlay"), and this phase was reapplied unchanged at the
user's request.

Installed `libs/image` and `tool_transform2`; hashes match. The user reported
the manual checks OK on 2026-10-07: Puppet Warp with the Accurate and Fast
previews (mesh shown, image kept before the first pin), pins, orders, apply
and Undo/Redo on RGBA32F and RGBA16F layers.

## Re-baseline of input to display after phases 4.81-4.92 (phase 4.99)

2026-10-10, user-run captures with the controlled protocol, one process per
mode (run sheet: [paint-trace-baseline-runs.md](../../paint-trace-baseline-runs.md),
"Re-baseline after phases 4.81-4.92"). Median of per-stroke medians, ms:

| Condition | CPU (PID 44148) | Projection-requested (PID 50752) | Brush-requested (PID 51524) |
| --- | --- | --- | --- |
| 64px Buildup | 17.8 (17.7–18.8), 56 inputs | 18.8 (17.1–19.5), 58 | 5.0 (3.9–5.3), 52 |
| 64px Wash | 19.4 (17.5–19.5), 59 | 15.5 (15.1–18.6), 53 | 5.6 (5.1–6.8), 50 |
| 256px Buildup | 20.6 (19.0–22.5), 54 | 17.3 (16.9–19.9), 58 | 4.8 (3.7–5.1), 51 |
| 256px Wash | 22.5 (20.2–23.1), 61 | 21.2 (17.7–24.4), 63 | 5.6 (5.3–5.9), 59 |

- With the GPU brush, input to display is about 5 ms in every condition,
  against 18–22 ms on the CPU; the per-stroke ranges do not overlap. This is
  the first software-timing comparison in which the GPU path is lower than
  the CPU in every condition (phase 4.72 had the GPU brush slower at 256px).
- GPU projection with the CPU brush stays at the CPU's level: the CPU brush
  modes spend 12–21 ms before the last upload. Phases 4.88 (brush minimum
  update period) and 4.92 (canvas compressor) apply only to the GPU paths.
- The GPU brush is still opt-in (`KRITA_GPU_BRUSH=1`); the user's normal
  launcher does not set it, so everyday painting runs at the CPU figures.
- Limits as before: Qt command-swap acknowledgement, not photons; trace on;
  hand drawing; one process per mode.

## GPU masking brush composite (phase 4.101)

2026-10-10, user request (follow-up of the masked-brush lag fix). The masking
brush composite now runs on the GPU for RGBA32F strokes in the ten modes
without strength, so masked strokes can use the GPU brush. Details and
measurements: [gpu-engine.md](../../gpu-engine.md), "Masking brush".

- Exact parity needed float arithmetic, not double: the CPU's
  `compositetype` for float is float. Products and sums use `precise float`.
  Divisions are computed in double and cast, which gives a correctly rounded
  float quotient.
- Masked strokes keep the CPU's adaptive update period. With the GPU minimum,
  textured masked strokes split into about seven times more batches, each
  paying for a masking composite and a projection merge.
- Tests: `testMaskingCompositeMatchesCpu` (ten modes) and the masked rows of
  `testTexturedMaskedStroke`.

## GPU textured dabs (phase 4.102)

2026-10-11, user request. RGBA32F circle dabs with a pattern texture in the
14 alpha-only texturing modes are generated and textured on the GPU; once a
mask kind is verified, the CPU generation and texturing are skipped.
Details and measurements: [gpu-engine.md](../../gpu-engine.md), "Textured
dabs".

- The texture is a postprocessing step; the description had to separate the
  brush tip's reflection (before texturing) from canvas mirroring (after).
- The CPU fallback (materialization after a refused batch) runs the real
  `KisTextureOption` code through a small applier, so the C++ side needs no
  copy of the texture math; the shader's copy is covered by the stroke tests
  of every mode.

## GPU image tip dabs (phase 4.103)

2026-10-11, user request (decision "A2"). RGBA32F dabs of predefined image
tips in alpha-mask mode are generated on the GPU; the CPU transforms image
tips with the same deterministic integer code (`KisImageTipSampler`)
instead of QPainter. Details and measurements:
[gpu-engine.md](../../gpu-engine.md), "Image tip dabs".

- QPainter's result depends on the CPU (Qt's run-time SIMD dispatch), so a
  bit-exact GPU copy of it was impossible; the user chose to replace it
  (accepting one 8-bit step from upstream) over a tolerance.
- Rotated rows must restart the 16.16 position at each row's first covered
  pixel, as `QRasterizer` spans do; accumulating from x = 0 drifted by up to
  4 steps.
- The first CPU version was about 5 times slower than QPainter; SSE2, a
  factor table for the unpremultiply and a separable vertical blend for
  scale-only rows brought it to about 2 times.
- The shader computes only red and alpha (the alpha mask needs no more).

## GPU colour, Lightness and Gradient image tips (phase 4.104)

2026-10-11, user request. The image stamp, Lightness map and Gradient map
applications of predefined image tips are generated on the GPU too.
Details: [gpu-engine.md](../../gpu-engine.md), "Image tip dabs",
Applications.

- Each application's output channel depends on a few 8-bit channels of the
  sampled tip pixel, so a 256-entry table made with the CPU code replaces a
  shader copy of the color math (Lightness's HSL arithmetic, LCMS for the
  stamp, the gradient cache); the GPU only samples and looks up.
- The stamp's table is valid only when the color conversion is per
  channel; that is checked once per color space instead of assumed.
- The gradient branch carried the previous pixel's color into transparent
  pixels; it was made per pixel (invisible) so that the dab is a function
  of the tip.
- Colour dabs do not commute: the stroke tests compare colour strokes
  without mirrors, because the paths order the reflections differently.

## RGBA16F image tips and textures (phase 4.105)

2026-10-11, user request. Image tips (every application) and textures are
described on RGBA16F layers too. Details:
[gpu-engine.md](../../gpu-engine.md), "RGBA16F image tips and textures".

- The image tip tables carry over unchanged: they are made with the CPU code
  in the F16 color space and hold exact halves.
- The texture composite needed a half copy of every mode: the C++ types
  decide the rounding (double for KoColorSpaceMaths, float for Imath
  `half +- half`, double -> float -> half for every half result).
- An exact unit test (every dab alpha against every texel, three
  strengths) is what checks it; the stroke tests' RGBA16F tolerance (0.002)
  would hide a wrong rounding.

## RGBA16F masking brush composite (phase 4.106)

2026-10-12, user request. The masking brush composite runs on RGBA16F tiles
too, so masked RGBA16F strokes use the GPU brush. Details:
[gpu-engine.md](../../gpu-engine.md), "Masking brush".

- The half rules found for the textured dabs (phase 4.105) applied as is to
  the ten modes without strength; the exact test with alphas at the half
  fuzzy thresholds passed at the first build.

## Remaining blend modes (phase 4.107)

2026-10-12, user request ("6"). RGBA16F brushes in every GPU blend mode, and
29 new HSX modes (HSY Tint and increase/decrease modes, the HSI, HSL and HSV
families) for layers and brushes. Details:
[gpu-engine.md](../../gpu-engine.md), "Remaining blend modes (phase 4.107)".

- The new brush test rows showed that RGBA32F brushes in Vivid Light, Hard
  Mix Softer and Lighter Color, enabled long ago without brush tests,
  drifted from the CPU over repeated dabs; fixed with `precise` and IEEE
  divisions.
- `KoColorSpaceMathsTraits<float>::compositetype` is float; emulating
  double for RGBA32F made the results worse.
- The NVIDIA driver fused `l + t * s` into an fma despite `NoContraction`;
  `roundedProduct()` works around it.
