# GPU Engine (experimental)

The GPU Engine is an ongoing rewrite of Solstice's image engine so that layer
compositing, painting, transforms and filters run on the graphics card with
Vulkan instead of on the CPU. It is on by default for documents in RGBA 32-bit
or 16-bit float. Whatever it does not support runs on the CPU as in Krita.

## What runs on the GPU

On RGBA 32-bit and 16-bit float layers:

- **Layer compositing and the canvas display**, including alpha lock and
  layers with individual RGB channels turned off.
- **Blend modes**: every mode except Dissolve, Marker, Destination In,
  Destination Atop and Divisive Modulo (and its Continuous variants). Modes
  based on powers, exponentials or trigonometry (for example Gamma, Easy
  Dodge or Interpolation) match the CPU closely rather than exactly; the
  others give the CPU's result.
- **Pixel-brush strokes** (the GPU brush, on by default) in Buildup and Wash,
  with selections, alpha lock, channel locks, mirror painting, Erase and the
  default Alpha Darken painting. Brushes with a masking brush use it too: the
  mask itself is painted on the CPU and combined with the stroke on the GPU.
- **Brush dabs** are generated on the GPU instead of uploaded:
  - circle, Gaussian and soft auto-brush tips;
  - pattern textures, in every texturing mode except Lightness and Gradient;
  - image tips (Predefined tips and imported Photoshop brushes) used as an
    alpha mask, Image Stamp, Lightness Map or Gradient Map, at any size and
    rotation. Image Stamp tips use the GPU when the document's color profile
    converts each channel separately, as the default float profiles do.
- **Transform Tool**: applying scale, rotate and shear (Free Transform
  without perspective), Liquify and Puppet Warp. The results are identical
  to the CPU results; Puppet Warp's parts are combined on the CPU.
- **Gaussian Blur**, also inside Unsharp Mask and Gaussian High Pass, for
  filter applications, filter layers and filter masks that update large
  areas. The result matches the CPU within one rounding step.

To draw image tips the same way on the CPU and the GPU, Solstice transforms
them with its own code instead of Qt's painter. The result can differ from
Krita's by at most one step of 8-bit precision, and the CPU takes about a
third longer than Krita for each image tip on processors with AVX2 (twice as
long without AVX2).

## What stays on the CPU

- Other filters, and the sharpening step of Unsharp Mask. Filter areas
  smaller than about 180 x 180 pixels, the reduced-resolution Instant Preview,
  integer layers, wrap-around mode, layer styles and the Colorize Mask.
- Perspective, Warp, Cage and Mesh transforms, older Puppet Warp transforms,
  and the preview while editing a transform.
- Painting the masking brush's mask; animated (`.gih`) and text brush tips;
  the Lightness and Gradient texture modes; the brush Sharpness option.
  Large or densely spaced strokes using them can take time to catch up.
- Alpha Darken with some channels turned off, and the blend modes listed
  above.
- Soft proofing, channel selection in the Channels docker, and some display
  color profiles (LUT-based profiles, absolute colorimetric intent) use the
  CPU for the canvas display.

## Current benchmarks

These numbers measure specific engine operations, not overall application
performance. Measured with an AMD Ryzen 9 9950X and NVIDIA RTX PRO 6000
Blackwell (driver 596.86); each value is the median of three fresh processes,
with GPU completion included.

| Workload | CPU | GPU |
| --- | ---: | ---: |
| Resident full layer projection, 4096 x 4096 RGBA32F, 16 layers | 270 ms | 60.6 ms |
| Canvas data preparation, 4096 x 4096 RGBA32F, 8 layers | 1215 ms | 1.81 ms |
| Four mirror passes, 32 dabs of 256 x 256 pixels | 51.1 ms | 2.08 ms |
| Transform Tool apply, scale + rotate (bicubic), 2480 x 3508 RGBA32F | 502 ms | 61.5 ms |
| Liquify apply, 20 strokes, 2480 x 3508 RGBA32F | 253 ms | 63.5 ms |
| Gaussian Blur apply, radius 30, 2480 x 3508 RGBA32F | 212 ms | 68.0 ms |
| Puppet Warp apply, 3 pins, 2480 x 3508 RGBA32F | 750 ms | 212 ms |

Timing a complete short stroke (brush generation, final merge and
projection, without input and display) can still favor the CPU for small
brushes, while larger, textured, masked or mirrored strokes favor the GPU:

| Queued stroke, 1024 x 1024 document, four layers | CPU only | GPU projection and brush |
| --- | ---: | ---: |
| RGBA32F, 64px Normal Wash | 6.19 ms | 10.88 ms |
| RGBA32F, 256px Normal Buildup | 8.15 ms | 10.70 ms |
| RGBA16F, 128px Soft Light (SVG) Wash, selection and both mirrors | 73.60 ms | 11.46 ms |
| RGBA32F, 300px textured (pattern) Buildup | 53.35 ms | 18.42 ms |
| RGBA32F, 300px textured with a masking brush, Wash | 80.51 ms | 37.45 ms |

From pen input to the displayed frame, the GPU brush is faster (next
section). Results vary with workload and hardware.

## Input to display

How long it takes from a pen movement until the painted pixels are handed to
the screen, with hand-drawn strokes (`b) Basic-4 Flow Opacity`, RGBA32F
2480x3508, one Solstice process per setting, three strokes per condition;
median of the strokes' medians, ms):

| Condition | CPU | GPU canvas, CPU brush | GPU brush |
| --- | --- | --- | --- |
| 64px Buildup | 17.8 | 18.8 | 5.0 |
| 64px Wash | 19.4 | 15.5 | 5.6 |
| 256px Buildup | 20.6 | 17.3 | 4.8 |
| 256px Wash | 22.5 | 21.2 | 5.6 |

The end point is Qt's frame swap, not light leaving the screen, and the
strokes were drawn by hand. The CPU brush has since been changed to start its
updates sooner; its current times have not been measured.

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
  current driver, the only tested GPU. Other GPUs are untested.
- Windows. The canvas must use desktop OpenGL, not ANGLE/Direct3D;
  otherwise compositing still runs on the GPU, but the canvas display does
  not.
  - With the GPU engine on, the default renderer (**Settings → Configure
    Solstice → Display → Preferred Renderer: Auto**) picks desktop OpenGL,
    unless the driver is known to be unreliable.
  - A renderer you chose explicitly is kept.

## Limitations

- The GPU tile cache has a memory limit. Idle tiles are copied back to RAM
  before their GPU memory is reused, and can then use the normal disk swap.
  If a processing job cannot fit, it continues through the CPU path; Undo
  data is preserved. Display buffers and temporary processing buffers use
  additional memory.
- With large brushes (around 200-300px and above), mirror painting with
  Alpha Lock may continue catching up after pen release.
- Brush size, spacing and preset complexity can still make drawing lag
  behind input; the GPU brush does not guarantee lower latency in every case.
  A single oversized dab is painted on the CPU.
- Brushes with a masking brush are painted on the CPU on a GPU without 64-bit
  float shader support.
- In RGBA 16-bit float documents, half-precision rounding is kept after each
  dab; small rounding differences from the CPU remain possible.
- With mirroring and blend modes such as Overlay or Dodge, where mirrored
  dabs overlap the stroke, the result can differ slightly from a CPU stroke
  because the dabs are grouped differently; the CPU brush shows the same
  kind of variation between strokes.
- If queued display updates fill the canvas transfer buffers, new updates use
  the CPU upload path until space is available.
- Before using accelerated canvas uploads, Solstice checks that pixels written
  by Vulkan can be read by OpenGL. If this check fails, the canvas reads the
  image through the CPU for the rest of the session; layer compositing can
  continue on the GPU.
- The warning after the engine stops cannot tell which open document was
  affected.

Development notes and the full measurement history are kept for contributors
in [`agent/gpu-engine.md`](agent/gpu-engine.md).
