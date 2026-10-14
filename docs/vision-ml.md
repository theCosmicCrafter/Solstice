# Vision ML

Vision ML adds AI-assisted selection, filling and background removal. The
models run inside Solstice (the embedded `vision.cpp` runtime) on the CPU or,
with a supported graphics card, on the GPU through Vulkan. Nothing is sent
over the network, and no Python installation is needed.

For details, see the original plugin repository, [Krita Vision
Tools](https://github.com/Acly/krita-vision-tools). This guide covers
Solstice's native version and where it differs.

## Segment Selection

Two selection tools in the toolbox select an object in the active layer:

- **Segment Selection from Point**: click on the object.
- **Segment Selection from Box**: drag a box around the object.

They work like the other selection tools: the usual selection modes (replace,
add, subtract, intersect) and anti-aliasing apply, and the result is an
ordinary selection. In Tool Options:

- **Mode**: **Fast** or **Precise**. Precise refines the edge and takes
  longer.
- **Backend** and **Model** (see [Models and backend](#models-and-backend)).

The model used is MobileSAM.

## Smart Fill Tool

**Smart Fill Tool** in the toolbox removes something from a picture and fills
the area from its surroundings. Paint over the area with the tool's brush;
when you release the pen, the painted area is filled. With an active
selection, only the selected part is filled. The tool works on paint layers,
and the fill can be undone as one step. The model used is MI-GAN.

## Background Removal

**Filter > Other > Background Removal...** keeps the foreground objects of
the active layer and makes the background transparent. Options:

- **Model**: **BiRefNet Dynamic** gives higher-quality masks on large
  images; **BiRefNet Lite**, included with Solstice, is faster and uses less
  memory. Dynamic is used by default once it is installed.
- **Estimate pixel foreground contribution** also corrects the colors of
  semi-transparent edge pixels, such as hair.

Like other filters, it can be previewed and applied to a selection. It is
not available as a filter mask or filter layer, and it computes in 8-bit RGBA
whatever the layer's color depth.

## Models and backend

Solstice includes MobileSAM, MI-GAN and BiRefNet Lite. More models go into
the models folder, `%APPDATA%\Solstice\resources\visionml\models`, in a
subfolder per kind: `sam`, `migan` or `birefnet`. The folder button next to
**Model** in the tool or filter options opens it. For example, BiRefNet
Dynamic is the file `BiRefNet-dynamic-F16.gguf` from the
[BiRefNet GGUF models](https://huggingface.co/Acly/BiRefNet-GGUF), placed in
the `birefnet` subfolder.

**Backend** chooses where the models run: **GPU** (Vulkan) is much faster
when available; **CPU** works everywhere. A backend that cannot run on this
computer is shown disabled. The choice is shared by all Vision ML tools.

## Limitations

- The first use of a model in a session loads it, which takes a moment.
- Large images take longer and use more memory, especially on the CPU.
- Results depend on the image; check the selection or the filled area and
  adjust it with the usual tools.

The implementation and model files are in
[`../plugins/visionml`](../plugins/visionml/).
