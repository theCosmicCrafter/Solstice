# Krita Vision ML

Native Krita tools and filters for promptable selection, background removal,
smart fill, and lasso matting. The implementation uses the lightweight
`vision.cpp`/GGML runtime and does not require Python, PyTorch, or an external
service.

## Tools

- Segment Selection from Point / from Box: MobileSAM promptable segmentation.
- Smart Matting Selection: freehand lasso refined by the BiRefNet matting
  model; produces soft-alpha selections with feathered edges inside the drawn
  region (native successor of the Smart Select plugin; no Python worker).
- Smart Fill Tool: MI-GAN inpainting.
- Filter > Remove Background: BiRefNet layer matting.

## Models

The build downloads and checksum-verifies these default models:

- MobileSAM F16 for point and box selection.
- BiRefNet-lite F16 for background removal and precise box selection.
- MI-GAN 512 F16 for smart fill.

Installed models are stored in `share/krita/visionml/models`. Additional GGUF
models can be placed in the user data directory under `visionml/models`.
BiRefNet Dynamic is the recommended optional model for higher-quality masks on
large images; it is significantly larger than the bundled Lite model. When the
Dynamic model is installed, Krita prefers it by default and retains Lite as the
fallback.

SAM 2 can produce better promptable masks, but it is not currently supported
by `vision.cpp` and its official inference stack adds PyTorch/CUDA. BRIA RMBG
2.0 is not bundled because its model weights restrict commercial use.

## Backends

The CPU backend is always built. The Vulkan backend is enabled when CMake finds
the Vulkan SDK and `glslc`; otherwise the GPU choice is disabled at runtime.
On Windows, a copy-only SDK can be used without changing the machine-wide PATH
by passing `Vulkan_INCLUDE_DIR`, `Vulkan_LIBRARY`, and
`Vulkan_GLSLC_EXECUTABLE` to the Krita CMake configure command.

## Compatibility

Model choices remain in the existing `VisionML` KConfig group. The native
plugin searches the new user and installation model locations first, then the
old `pykrita/vision_tools/models` directory so existing custom models continue
to work.
