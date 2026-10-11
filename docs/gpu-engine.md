# GPU Engine (experimental)

The GPU Engine is an ongoing rewrite of Solstice's image engine so that layer
compositing, filters, transforms, and painting run on the graphics card with
Vulkan instead of on the CPU. It is developed on the `krita-sol-gpu` branch.

## Current status

Layer compositing and the canvas display run on the GPU for RGBA floating
point documents. Pixel-brush strokes are painted on the GPU by default (since
2026-10-10); the GPU brush composites supported blend modes in RGBA32F
documents, including the temporary painting buffer used by Wash mode.
RGBA16F pixel brushes also support major GPU blend modes in Buildup and Wash,
including selections, channel locks and mirrors. These are Normal, Erase,
Multiply, Screen, Addition / Linear Dodge, Subtract, Darken, Lighten,
Difference, Overlay, Hard Light, Exclusion, Linear Burn, Linear Light and
Pin Light, Soft Light (SVG), Color Dodge, Color Burn and HSY Color, Hue,
Saturation and Luminosity. Wash's Alpha Darken painting buffer, preview and
final merge can stay on the GPU. Other RGBA16F brush blend modes continue
through the CPU path. Dab generation remains on
the CPU in both formats.
Most filters and transforms still run on the CPU; the exceptions are
described below (Transform Tool, Liquify, Puppet Warp, Gaussian Blur). The engine is on by default in
new profiles and in profiles that never changed the option (since 2026-10-07).

Accelerated layer blend modes include Normal, Multiply, Screen, Addition /
Linear Dodge, Subtract, Darken, Lighten, Difference, Overlay, Hard Light,
Exclusion, Linear Burn, Linear Light and Pin Light in RGBA32F and RGBA16F.
The same formats also accelerate Soft Light (SVG and Photoshop), Color Dodge,
Color Burn, Divide, Vivid Light, Hard Mix (including Photoshop and Softer
Photoshop), Grain Merge/Extract, Negation, Allanon, and the HSY Hue, Saturation,
Color, Luminosity, Darker Color and Lighter Color modes. The separately named
HDR variants of Dodge, Vivid Light and Hard Mix still use the CPU, as do the
HSI/HSL/HSV families.
Other layer blend modes continue through the CPU path.

Layer compositing also stays on the GPU when individual RGB channels are
disabled in the layer properties, with or without alpha locked, in both
RGBA32F and RGBA16F documents. The supported blend modes retain the CPU's
channel-preservation behavior. For unrestricted Normal blending, fully
transparent pixels retain their original hidden RGB; CPU SIMD processing
can produce different hidden RGB at zero alpha, without changing appearance.

Bulk pixel reads from GPU-backed documents now transfer tiles in batches,
including channel-by-channel reads used by some CPU consumers. This reduces
transfer waits when those paths need current GPU pixels. Filters and file
encoding still run on the CPU.

CPU filters also batch their input transfers, including surrounding pixels
needed for Gaussian Blur and destination pixels used with selections. FFT
convolution batches its padded reads and the extra row span held by its
border iterator. This reduces transfer waits; filter calculations still run
on the CPU, and large filters can remain expensive.

Affine transforms and horizontal/vertical layer flips also batch GPU pixel
readbacks before their CPU processing. Scaling, rotation, shear and move
operations retain their existing interpolation and Undo behavior. This
reduces transfer submissions; it does not move transform calculations to
the GPU or change canvas mirroring.

Before applying a transform, Ctrl+Z steps back through the Transform Tool's
edits: flip, rotate, then Undo returns to the flipped image. Continuing a
previously applied transform can combine it with the next transform in the
document's Undo history; this is the existing Transform Tool behavior.

Whole-tile copies can share GPU-resident pixels without downloading them.
Partial clears batch the edge tiles that need CPU pixels, while fully cleared
tiles need no readback. GPU copy-on-write also avoids filling a CPU buffer
before replacing it with the retained snapshot. Undo and CPU access retain
their existing behavior.

CPU-to-GPU transfers share temporary buffers across layers and use memory
suited to CPU snapshots. Completed transfers also release resources without
holding the shared cleanup lock. This reduces waits when an image first
enters the GPU path or returns to it after CPU processing.

Layer and Wash compositing reuse completed work buffers before waiting for
busy ones. A small cache allows consecutive updates to be queued without
waiting after every submission. Busy buffers remain protected until their
GPU work completes, preserving selections, layer updates and Undo/Redo.

Large textured brushes now reuse available GPU transfer buffers when the
memory limit prevents keeping all three buffers. The masking brush's mask
is combined with the stroke on the GPU on RGBA 32-bit float layers (see the
GPU brush notes below). Round auto-brush dabs with a pattern texture are
generated and textured on the GPU on RGBA 32-bit float layers, for every
texturing mode except Lightness and Gradient (which change the brush
color); for example, a large textured stroke takes about a third of the
CPU time. Painting the masking brush's mask, textures of image-based
tips, the Lightness and Gradient modes and the brush Sharpness option
remain CPU operations; large or densely spaced strokes using them can
still take time to catch up.

Automated checks compare complete strokes and their layer projections with
CPU drawing, including Buildup/Wash, mirror painting, selections, alpha lock
and Undo/Redo. Supported Wash previews and final merges, including
selections and channel locks, can stay on the GPU with aligned tiles. Other Wash
previews and final merges batch GPU readbacks
to reduce transfer waits. Short-stroke measurements still show cases where the CPU is
faster. Tablet input and the time until a stroke appears on screen still need
separate measurement.

The [current benchmarks](#current-benchmarks) distinguish layer compositing,
canvas preparation and complete queued strokes. Faster compositing alone
does not establish lower pen-to-screen latency.

## Current benchmarks

The nine-process manual comparison completed on October 6, 2026 measures
**Qt input receipt to the last required command-swap acknowledgment**, not
physical pen-to-screen latency. Three fresh processes per configuration used
RGBA32F, a 2480x3508 document and the Basic-4 Flow Opacity preset. After excluding
one warm-up stroke per condition/process, 108 strokes and 2,078 timed inputs
remain. All request-producing inputs passed the recorded checks; no trace events
were dropped.

Each table value is the median of three process summaries; each process summary
is the median of its three measured stroke medians. Parentheses show the range
of process summaries, not confidence intervals. All values are milliseconds.

| Condition | CPU brush + CPU-pixel upload | CPU brush + shared-buffer upload | GPU brush + shared-buffer upload |
| --- | ---: | ---: | ---: |
| 64px Buildup | 19.91 (19.43–21.94) | 19.06 (18.00–20.58) | 19.77 (19.05–20.10) |
| 64px Wash | 19.41 (18.60–20.75) | 18.79 (17.98–20.45) | 19.73 (19.22–19.95) |
| 256px Buildup | 21.65 (21.35–22.66) | 32.79 (26.92–38.99) | 25.11 (21.95–29.28) |
| 256px Wash | 18.38 (17.48–21.00) | 26.85 (24.29–29.13) | 24.20 (23.83–25.16) |

There is no clear GPU advantage at 64px; process ranges overlap. CPU has lower
summary medians at 256px, although GPU-brush Buildup overlaps CPU across processes.
This supports investigating overhead before adding more GPU features; it does
not identify the cause or establish a general slowdown.

Reanalysis of the same captures places most of the 256px difference before the
last required upload is issued, rather than in the trailing Qt swap acknowledgment
interval. That earlier interval combines scheduling, drawing, preparation and
transfer work; it does not isolate a GPU-transfer bottleneck. No new runtime
optimization or additional manual captures were needed for this analysis.

A further split of the same measured strokes points to different investigation
areas for Buildup and Wash. At 256px, the interval from projection completion to
prepared canvas update was about 0.72ms for CPU and 5.03ms for the GPU-brush
setting. GPU-brush Wash instead showed about 2.02ms in the host projection span
and 0.25ms afterwards. These are diagnostic stage statistics, not GPU execution
times or additive portions of input latency. Preparation and synchronization
still need to be separated before selecting an optimization.

Opt-in diagnostic tracing now separates context reuse, source/target preparation
and submission on these paths. This adds measurement detail, not a speedup;
normal runs do not record these events.
The submission diagnostics also distinguish lock acquisition, transfer-command
recording, queue submission and state updates. Tile lookup/COW and CPU staging
have separate scopes; no synchronization policy has been changed.
Lock-holder diagnostics record holds of at least 10 microseconds to keep traces
bounded. Overlap with another thread is evidence for investigation, not a
guarantee that the recorded holder caused all of the delay.

Main command-buffer recording now ends before the shared residency lock is
acquired, reducing work inside that lock while preserving upload and submission
order. Correctness regression tests pass, but the first focused real-app
comparison did not demonstrate a latency improvement; lock contention remains.
The latest optional diagnostics distinguish the queue's own lock wait from
the driver submission call, with timestamps logged after the measured locks
are released. These changes support investigation; they do not claim a speedup.

Those diagnostics showed that most of the waiting was between canvas updates
from different worker threads, each submitting its own small GPU transfer.
Canvas updates that arrive at the same time are now prepared together in a
single GPU submission. Each update still covers exactly its own area, and
distant areas (for example mirrored strokes) are not enlarged into one large
rectangle. Soft proofing, channel selection and the CPU canvas path are
unchanged. In a first real-app capture this cut canvas GPU submissions from
834 to 241 and the summed lock waiting from about 460ms to 73ms. The measured
input-to-display times were longer, but those strokes were much shorter and
faster than in the earlier capture, and the extra time was spent before the
canvas stage, while brush dabs waited for processing. Neither a speedup nor
a slowdown of input-to-display time is claimed from this comparison.

Those waiting dabs were limited by the GPU brush's per-update data budget. The
measured brush turns its dabs with the stroke direction, so a 256px dab needs a
larger, about 362px square, area on diagonal strokes, and only about 16 dabs
fit into one update. The brush then waited at least 10ms before the next
update. Now, when an update is cut short only by that budget, the next update
starts as soon as the current one finishes. Slower strokes and the CPU brush
path are unchanged. In the following capture, dabs again waited about 10ms
for processing, as before the slowdown, and input-to-display times were
similar to the first capture. The strokes differed in speed and direction, so
this is not claimed as a speedup.

GPU dab generation has started with the simplest case. With the GPU brush
enabled, dabs of the default round auto brush on RGBA 32-bit float images are
now drawn by the GPU from a short description (position, size, shape, fade
and color) instead of uploading the dab's pixels. The CPU still prepares each
dab as before, so fallbacks remain exact. Textured, sharpened and image-based
brushes still upload pixels.
The Gaussian round mask (used by the Basic-4 presets) is now generated on the
GPU as well, and so are the Soft round mask (including edited curves and
softness) and RGBA 16-bit float documents. Its GPU result is identical to the CPU result, pixel for pixel.
In a first capture with the Basic-4 preset at 256px, every GPU brush update used
generated dabs. The CPU time to send each update to the GPU fell from about
1.8ms to 0.4ms. Buildup strokes reached the screen faster than in the earlier
captures, and Wash strokes were similar. Hand-drawn strokes vary, so this is
reported as an observation, not a guaranteed speedup.

For these generated dabs the CPU no longer computes the dab image at all once
the first dabs of a session have been checked against the CPU result. If a CPU
path still needs the pixels (for example when the GPU cannot take a batch),
they are computed then, identically to before. In the first capture this cut
the CPU time spent preparing dabs during strokes by about eight times. The
time to the screen stayed in the same range; the brush's own update interval
is now the largest remaining wait.

With the GPU brush, the brush no longer waits for its minimum update interval
(10ms or more): a new GPU update starts as soon as new dabs are ready and the
previous update has finished. Painting without the GPU brush is unchanged. In a
capture of six Basic-4 strokes at 256px, the median time from input to the
screen fell from about 18-24ms to 7-13ms per stroke. Hand-drawn strokes vary,
so treat these numbers as an observation. Dabs that become ready while
an update is still running are now picked up as soon as it ends, instead of at
the next pen movement; in Wash strokes this lowered the median time to the
last upload from about 9.8ms to 7.8ms. During Wash strokes the layer's own
pixels are now also copied into the live preview on the GPU, in the same step
as the stroke preview, instead of by the CPU. In a capture on the Background
layer, Wash strokes then reached the screen as fast as Buildup strokes (about
6ms median instead of about 11ms). The first Wash stroke after opening a
document was noticeably slower: the GPU programs it needs were prepared on
first use, separately for each parallel task. They are now prepared once,
in the background shortly after Krita starts. In a capture, the first Wash
stroke then reached the screen in about 11ms instead of about 36ms, and the
first Buildup stroke in about 5ms instead of about 14ms. With the GPU engine,
finished canvas updates are now sent to the display texture immediately
instead of after the frame-rate limiter's interval; Wash strokes reached the
screen in about 5.7ms (median) instead of about 8.2ms, while Buildup strokes
stayed at about 5.5ms. Most of the remaining time is waiting for the screen's
next refresh.

The Transform Tool's scale, rotate and shear (Free Transform without
perspective) are now applied on the GPU for RGBA 32/16-bit float layers. The
result is identical to the CPU result, pixel for pixel. In a measurement on a
2480x3508 layer, applying a scale and rotation took about 63ms instead of
about 430ms. Applying Liquify is also done on the GPU for these layers, again
identical to the CPU result: about 56ms instead of about 220ms for 20 strokes
over a large part of the same layer. Applying Puppet Warp is done on the GPU
for these layers as well: about 190ms instead of about 710ms with three pins
on the same layer. Where its parts are stacked by pin order, they are
combined on the CPU, which can differ from a CPU-only result by tiny rounding
amounts (repeated CPU-only results differ the same way). The preview while
editing, perspective, warp, legacy Puppet Warp transforms, cage and mesh
transforms still run on the CPU, as does
Liquify when the layer's position differs from its original copy (an internal
case the Transform Tool does not use).

Gaussian Blur is computed on the GPU for RGBA 32/16-bit float layers, and so
is the blur inside Unsharp Mask and Gaussian High Pass. The result matches
the CPU result within rounding: in 32-bit float layers the difference is at
most one rounding step, and in the tests 16-bit float results were identical.
On a 2480x3508 32-bit float layer, applying Gaussian Blur took about 50ms
(radius 5), 62ms (radius 30) and 110ms (radius 100) instead of about 150ms,
213ms and 398ms. Unsharp Mask gains little (about 378ms instead of 408ms),
because its sharpening step still runs on the CPU. Filter masks and filter
layers with these filters use the GPU when they update large areas. Areas
smaller than about 180x180 pixels, the reduced-resolution Instant Preview,
8-bit and 16-bit integer layers, wrap-around mode, layer styles and the
Colorize Mask stay on the CPU. With mirroring and
blend modes such as Overlay or Dodge, where mirrored dabs overlap the stroke,
the result can differ slightly from a CPU stroke because the dabs are grouped
differently; the CPU brush shows the same kind of variation between strokes.

The labels describe observed paths: this scene reused child images and skipped
extra layer composition, so the middle column is **not a GPU layer-compositing
benchmark**. GPU brush submissions and shared-buffer transfers were recorded;
GPU-brush Wash also recorded compositor submissions during its processing.

The user kept the same requested conditions, but hand motion/pressure and all
unsaved brush settings were not replayed or fully recorded. Refresh rate, zoom
and smoothing were not independently captured. Samples sharing batches/frames
are correlated, tracing overhead is uncorrected, and physical pixel visibility
is not established. These results are an observational software-timing baseline.
See the [run records, sample counts, tail summaries and limitations](agent/paint-trace-baseline-runs.md#completed-comparison-october-6-2026).

The following earlier benchmarks measure separate workloads and remain unchanged.

Measured on October 4, 2026, in the local development build: Windows 11,
AMD Ryzen 9 9950X, RTX PRO 6000 Blackwell, driver 596.86 and Qt 6.8.
Vulkan validation was disabled for timing. Three fresh processes were run
sequentially for each workload. Values below are the median of their results;
ranges in parentheses show variation between processes, not latency percentiles.

| Engine operation | CPU | GPU |
| --- | ---: | ---: |
| Resident projection, 4096x4096 RGBA32F, 16 layers | 247 ms (242-248) | 56.6 ms (55.4-57.1) |
| Full canvas preparation, 4096x4096 RGBA32F, 8 layers | 1060 ms (1038-1115) | 1.62 ms (1.43-1.80) |
| 256x256 canvas update in that image | 5.36 ms (5.23-5.84) | 0.156 ms (0.133-0.156) |
| Four mirror passes, 14 dabs of 73x73 pixels | 0.958 ms (0.956-1.007) | 0.381 ms (0.352-0.384) |
| Four mirror passes, 32 dabs of 256x256 pixels | 45.14 ms (43.63-48.07) | 2.02 ms (1.96-2.04) |

Projection and canvas use five-sample medians per process. Mirror results
average five updates after warming all three brush staging slots. All GPU
times include completion. Canvas preparation starts with fresh GPU-resident
projection pixels for both paths, includes display color conversion and any
CPU download, and excludes projection work and final OpenGL texture copies.
Mirror timings include reflection and uploads, but exclude brush generation,
scheduling and display; their CPU reference is serial painting.

For the projection workload, initial GPU preparation takes 0.61-0.64 seconds,
the first refresh after CPU projection work takes 58-72 ms, and reading the
full projection back to CPU adds 46-49 ms. These are distinct operations and
are not included in the resident projection row.

Complete queued strokes show the remaining overhead. These tests use Normal
pixel brushes, a 1024x1024 RGBA32F document with four layers, four image workers
and 24 queued line segments, without mirroring or selections. Each process
takes ten samples after warmup, alternating path order. The stroke results
below include the subsequent tile-copy and partial-clear optimizations;
the projection, canvas and mirror table above retains the phase 4.42 baseline.
Times include brush
generation, scheduling, Wash final merging and completed projection; tablet
events, canvas presentation and verification readback are excluded.

| Stroke | CPU only | GPU projection, CPU brush | GPU projection and brush |
| --- | ---: | ---: | ---: |
| 64px Buildup | 4.27 ms | 7.81 ms | 7.18 ms |
| 64px Wash | 5.14 ms | 8.85 ms | 10.49 ms |
| 256px Buildup | 8.92 ms | 12.41 ms | 11.61 ms |
| 256px Wash | 11.82 ms | 15.76 ms | 18.18 ms |

In a controlled same-session comparison against the preceding image library,
GPU projection-and-brush times fell from 8.28, 11.74, 13.87 and 19.91 ms
respectively (about 9-16%). Each version used three fresh processes, with
before/after order alternated. These results measure the combined changes;
they do not assign a separate speedup to each optimization. See the
[comparison and regression checks](agent/wiki/history/gpu-phases-4.17-4.57.md#ordinary-stroke-tile-costs-phases-443-445).

A subsequent work-buffer reuse change removes an avoidable wait between
consecutive projection submissions. In the same-session comparison, complete
GPU projection-and-brush times changed from 10.75 to 10.61 ms for 64px Wash,
10.74 to 10.55 ms for 256px Buildup, and 13.66 to 13.79 ms for a mirrored,
selected F16 Soft Light Wash stroke. Process ranges overlap, so these results
do not establish a substantial drawing speedup. See the
[context-reuse checks and measurements](agent/wiki/history/gpu-phases-4.17-4.57.md#projection-context-reuse-phase-453).

The CPU is faster for these short strokes when the whole stroke is timed;
from pen input to the displayed frame the GPU brush is now faster (see
[Input to display](#input-to-display)). These measurements do not claim a
fix for the deferred large mirrored Alpha Lock case. All measured paths
passed their CPU image comparisons. The canvas benchmark also compares the
actual OpenGL textures after timing. See the
[reproduction notes](agent/wiki/history/gpu-phases-4.17-4.57.md#current-build-benchmark-baseline-phase-442)
for exact tests and measurement boundaries.

The new RGBA16F Normal/Erase Buildup path was measured separately on the same
hardware and date, using 128px brushes and the same four-layer, 24-segment
stroke fixture. These are five-sample medians from three fresh processes
(median across processes), with validation off and GPU completion included:

| RGBA16F Buildup stroke | CPU only | GPU projection, CPU brush | GPU projection and brush |
| --- | ---: | ---: | ---: |
| Normal | 12.15 ms | 10.27 ms | 7.42 ms |
| Erase | 12.52 ms | 9.97 ms | 7.23 ms |
| Normal, soft selection + Alpha Lock + both mirrors | 32.77 ms | 18.94 ms | 10.67 ms |
| Erase, soft selection + Alpha Lock + both mirrors | 42.11 ms | 29.46 ms | 9.87 ms |

The Erase operation ignores Alpha Lock, as on the CPU. Process-to-process
variation was larger for the combined selection/mirror rows; these results
do not predict tablet latency or large-brush behavior.
See the [F16 Buildup measurement details](agent/wiki/history/gpu-phases-4.17-4.57.md#rgba16f-normalerase-dabs-phase-446).

The F16 Wash extension was measured with the same 128px fixture, five samples
per path in each of three fresh processes, validation off. Median across
processes, including final merging and GPU completion:

| RGBA16F Wash stroke | CPU only | GPU projection, CPU brush | GPU projection and brush |
| --- | ---: | ---: | ---: |
| Normal | 15.29 ms | 12.30 ms | 10.72 ms |
| Erase | 16.13 ms | 11.90 ms | 10.13 ms |
| Normal, soft selection + Alpha Lock + both mirrors | 45.82 ms | 34.09 ms | 15.72 ms |
| Erase, soft selection + Alpha Lock + both mirrors | 47.44 ms | 35.99 ms | 13.78 ms |

These measurements exclude input and canvas display and do not resolve the
deferred large-brush mirrored Alpha Lock case.
See the [F16 Wash test and measurement details](agent/wiki/history/gpu-phases-4.17-4.57.md#rgba16f-alpha-darken-and-wash-phases-447-448).

Representative F16 blend-brush cases were also measured with a 128px
brush, soft selection and both mirrors (all channels enabled), using the
same five-sample/three-process protocol:

| RGBA16F stroke | CPU only | GPU projection, CPU brush | GPU projection and brush |
| --- | ---: | ---: | ---: |
| Multiply Buildup | 69.29 ms | 41.53 ms | 12.17 ms |
| Overlay Wash | 69.07 ms | 55.64 ms | 15.27 ms |
| HSY Color Buildup | 107.50 ms | 101.30 ms | 12.61 ms |
| Soft Light (SVG) Wash | 69.20 ms | 55.25 ms | 15.38 ms |

These include completed GPU work but exclude input and canvas display;
other modes and workloads can behave differently. See the
[basic blend-brush measurements](agent/wiki/history/gpu-phases-4.17-4.57.md#rgba16f-basic-blend-brushes-phases-449-450)
and [extended major-mode measurements](agent/wiki/history/gpu-phases-4.17-4.57.md#rgba16f-extended-major-blend-brushes-phases-451-452).

## Input to display

How long it takes from a pen movement until the painted pixels are handed to
the screen, measured on 2026-10-10 with hand-drawn strokes (`b) Basic-4 Flow
Opacity`, RGBA32F 2480x3508, one Solstice process per setting, three strokes
per condition; median of the strokes' medians, ms):

| Condition | CPU | GPU canvas, CPU brush | GPU brush |
| --- | --- | --- | --- |
| 64px Buildup | 17.8 | 18.8 | 5.0 |
| 64px Wash | 19.4 | 15.5 | 5.6 |
| 256px Buildup | 20.6 | 17.3 | 4.8 |
| 256px Wash | 22.5 | 21.2 | 5.6 |

The end point is Qt's frame swap, not light leaving the screen, and the
strokes were drawn by hand. Since the same day the CPU brush also starts
its updates sooner (it used to wait at least 10 ms between them); its new
times have not been measured yet. Details:
[the run sheet](agent/paint-trace-baseline-runs.md).

## Turning it on or off

The engine is on by default. To change it:

1. Open **Settings → Configure Solstice → Performance → General**.
2. In **GPU Engine (Vulkan)**, check or uncheck **Use the GPU engine for RGBA
   float documents**. **Paint brush strokes on the GPU** (on by default)
   chooses whether pixel-brush strokes are painted on the GPU too; strokes
   the GPU brush does not support are always painted on the CPU.
3. Restart Solstice. With the engine on, the status line in the same place
   shows the GPU in use once an RGBA float document is open.

Without a usable Vulkan GPU the engine is not used, and Solstice works on the CPU.
Software Vulkan drivers that run on the processor (common in virtual
machines) are not used either, because they are slower than Solstice's CPU
paths.

## Documents in other color spaces

The GPU engine only accelerates documents in **RGBA 32-bit or 16-bit
float**. When you open a document in another color space (for example an
8-bit PNG, a 16-bit integer, CMYK, Lab, or grayscale document), Solstice asks
whether to convert it:

- **Convert** changes the document to RGBA 32-bit float. RGB documents keep
  their color profile, so they look the same. The conversion can be undone,
  and saving keeps the new color space.
- **Keep** leaves the document as it is. It works exactly as before, without
  GPU acceleration.

Check **Do not ask again** to remember the answer. You can change it later
in **Opening other documents** next to the GPU engine option (Ask, Convert,
or Keep). New documents default to RGBA 32-bit float, which the GPU engine
accelerates; the New Document dialog offers the other color spaces too and
remembers the last choice.

File layers are not converted, and in a document that is already RGBA float,
layers in other color spaces stay as they are; such layers are composited
on the CPU.

## If the GPU engine stops

If image data cannot be read back from the graphics card (for example after a
driver reset), the GPU engine stops and Solstice continues on the CPU. A
message tells you how many image tiles were affected: the most recent changes
in those areas are lost and show older content.

Afterwards, every save (Save, Save As, Export, and saving when closing; not
autosave) asks first. Choose **Save as New File** to keep your previous file
untouched, **Save Anyway** to overwrite it, or **Cancel** to check your
documents first. Restart Solstice to use the GPU engine again.

If the data loss happens while a document is being saved, that save fails
with a writing error and the existing file is left unchanged. Save again to
choose how to continue. (This covers most formats, including `.kra`; a few
export formats write the file directly and are not covered.)

## Requirements

- An NVIDIA Blackwell GPU (GeForce RTX 50 series or RTX PRO Blackwell) with a
  current driver. Other GPUs are not supported.
- Windows. The canvas must use desktop OpenGL, not ANGLE/Direct3D;
  otherwise compositing still runs on the GPU, but the canvas display does
  not.
  - With the GPU engine on, the default renderer (**Settings → Configure
    Solstice → Display → Preferred Renderer: Auto**) picks desktop OpenGL,
    unless the driver is known to be unreliable.
  - A renderer you chose explicitly is kept.

## Limitations

- The GPU tile cache has a memory limit. Idle tiles are copied back to RAM
  in batches before their GPU memory is reused, and can then use the normal
  disk swap. Busy tiles are skipped; if a transfer fails, their GPU copy is
  kept instead of being discarded to make room.
  If a processing job cannot fit, it continues through the CPU path. Undo
  data is preserved. This limit covers image tiles; display buffers and
  temporary processing buffers use additional memory.
- After a stroke, background preparation of CPU tile copies skips tiles whose
  latest pixels are only on the GPU. Undo and later CPU operations still read
  the current pixels when needed. With large brushes (around 200-300px and
  above), mirror painting with Alpha Lock may still continue catching up after
  pen release in both Wash and Buildup. Further optimization of this rare case
  is deferred; the delay is a known limitation.
- Canvas transfer buffers also have a separate limit. If queued display
  updates fill it, new updates use the CPU upload path until space is
  available. Unused shared buffers are reclaimed without discarding pending
  display updates.
- Before using accelerated canvas uploads, Solstice checks that pixels written
  by Vulkan can actually be read by OpenGL. If this check fails, the canvas
  automatically reads the projection through the CPU for the rest of the
  session. Layer compositing can continue on the GPU; no setting is changed.
- The GPU brush needs the GPU engine and its own option (on by default);
  `KRITA_GPU_BRUSH=1` or `0` overrides the option. Strokes of brushes with a
  masking brush (two-tip brushes) also use the GPU brush on RGBA 32-bit float
  layers: the masking brush's mask is still painted on the CPU, but it is
  combined with the stroke on the GPU, with the same result as the CPU. This
  covers the masking blend modes Multiply, Darken, Overlay, Color Dodge,
  Color Burn, Linear Burn, Linear Dodge, Hard Mix (Photoshop), Hard Mix
  Softer (Photoshop) and Subtract. Masked strokes on RGBA 16-bit float layers,
  with the masking modes that have a strength value, or on a GPU without
  64-bit float shader support are painted on the CPU.
  Large groups of brush dabs are split by their pixel-data size so they can
  stay within the GPU upload budget. A single oversized dab can still use
  the CPU. Up to three brush batches can be queued without waiting after each
  submission. Their reusable pixel, selection and table buffers share a
  64 MiB limit; buffer reuse and CPU pixel access wait for pending work when
  necessary. Tile uploads and other engine allocations are separate.
  Brush size, spacing and preset complexity can still cause drawing
  to lag behind input; GPU painting does not yet guarantee lower latency.
  It covers RGBA32F pixel brushes with matching dab/layer profiles. GPU blend
  modes are Normal, Multiply, Screen, Addition/Linear Dodge, Subtract, Darken,
  Lighten, Difference, Overlay, Hard Light, Exclusion, Linear Burn, Linear Light,
  Pin Light, the additional layer modes listed above, and Erase. They work in
  Buildup and in Wash's preview and final merge with aligned tiles, including
  selections, Alpha Lock and individual RGB locks. Alpha Darken dab compositing,
  including Wash's temporary painting buffer, is also supported with all
  channels enabled. Soft, inverted and moved
  selections are supported; their coverage is read on the CPU and applied
  on the GPU. Oversized selection snapshots use the CPU path. Channel locks
  follow the existing CPU behavior, including clearing hidden color values in
  fully transparent pixels for the added blend modes. Dab generation remains on
  the CPU. Horizontal and vertical mirror painting can also composite on
  the GPU. Both nearby and widely separated mirror passes can share one GPU
  submission, without allocating tiles in the empty space between reflections.
  Batches that exceed memory limits retain separate GPU/CPU passes. Reflected edges are clipped to the same
  painting regions as the CPU, including when a mirror axis lies between
  pixels. Performance still depends on the
  stroke and brush; the prototype does not yet establish full input latency.
  Erase supports selections, mirror painting and channel-flag combinations on
  the GPU. It reproduces the existing CPU eraser behavior: the Erase operation
  itself ignores channel flags and changes alpha while preserving RGB.
  Restricted channels with Alpha Darken, wrap-around,
  instant-preview LOD and other blend modes use the CPU path. RGBA16F supports
  the major modes listed above in Buildup and Wash with matching dab/layer
  profiles, selections, channel locks and mirrors. Alpha Darken is supported with all channels
  enabled, including Wash's internal buffer. F16 also supports Soft Light
  (SVG), Color Dodge/Burn and HSY Color/Hue/Saturation/Luminosity. Other F16
  brush modes, including Soft Light (Photoshop), Divide and HSI/HSL/HSV
  color-component modes, still use the CPU.
  Half-precision storage and intermediate rounding are preserved after each
  dab; small rounding differences from CPU arithmetic remain possible.
  It is still under development; filters and transforms are not accelerated,
  except the Transform Tool's affine transforms, Liquify, Puppet Warp and
  Gaussian Blur (described above).
- Soft proofing, channel selection in the Channels docker, and some display
  color profiles (LUT-based profiles, absolute colorimetric intent) use the
  CPU for the canvas display.
- The warning after a stop cannot tell which open document was affected.
