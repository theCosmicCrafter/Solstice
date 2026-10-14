# Vision ML — agent development notes

## Status and locations

- Native implementation: `plugins/visionml/`
- Original reference: `<original-plugin-root>/krita-vision-tools/`
- Native runtime: `plugins/visionml/thirdparty/vision.cpp/`, pinned from
  `Acly/vision.cpp` revision
  `26a752912d49f6c4ff4545b35a1bdf7400d349ed`.

Vision ML is a normal native Krita tool/filter plugin. Do not restore the Python
`ctypes` loader, private `KoToolManager` injection, or installation under
`pykrita`.

## Models and runtime

- Installed defaults live under `share/krita/visionml/models/`.
- User models live under `<resource folder>/visionml/models/`
  (`KoResourcePaths::getAppDataLocation()`; `%APPDATA%\Solstice\resources\visionml\models\`
  by default since the settings moved, `docs/agent/settings-location.md`).
  The user guide is `docs/vision-ml.md` (expanded 2026-10-13).
- `%APPDATA%\krita\pykrita\vision_tools\models\` remains a read-only legacy
  fallback.
- Bundled native models are MobileSAM F16, BiRefNet-lite F16, and MI-GAN 512
  F16.
- Prefer BiRefNet Dynamic for background removal when installed; otherwise use
  bundled BiRefNet Lite.
- Do not bundle BRIA RMBG weights because their license restricts commercial
  use.
- Build Vulkan inference only when the Vulkan SDK and `glslc` are available.
  CPU-only builds must remain functional and show the GPU backend as disabled.
- The development machine has a copy-only LunarG Vulkan SDK 1.4.357.0 at
  `<krita-dev-root>\VulkanSDK\1.4.357.0`; it is intentionally absent from PATH
  and the registry.
- The installed Vulkan runtime has been verified on the NVIDIA RTX PRO 6000
  Blackwell with FP16, BF16, integer dot products, and NVIDIA cooperative
  matrices.

## Build and install

```bat
cmd.exe /d /s /c "call <krita-dev-root>\env.bat && cmake --build <krita-dev-root>\_build --target kritavisionml -j 2"
```

When reconfiguring the build tree, provide the private SDK explicitly:

```bat
cmd.exe /d /s /c "call <krita-dev-root>\env.bat && cmake -S <repository-root> -B <krita-dev-root>\_build -DVulkan_INCLUDE_DIR=<krita-dev-root>\VulkanSDK\1.4.357.0\Include -DVulkan_LIBRARY=<krita-dev-root>\VulkanSDK\1.4.357.0\Lib\vulkan-1.lib -DVulkan_GLSLC_EXECUTABLE=<krita-dev-root>\VulkanSDK\1.4.357.0\Bin\glslc.exe"
```

Install both the module/runtime and models:

```bat
cmake -DCMAKE_INSTALL_LOCAL_ONLY=1 -P <krita-dev-root>\_build\plugins\visionml\src\cmake_install.cmake
cmake -DCMAKE_INSTALL_LOCAL_ONLY=1 -P <krita-dev-root>\_build\plugins\visionml\cmake_install.cmake
```

## Smart Matting Selection (Smart Select native port)

- Tool files: `plugins/visionml/src/smartselect/SmartMattingTool.h/.cpp`,
  registered as `SmartMattingTool` in `VisionMLPlugin.cpp`, action entry in
  `src/vision_tools.action`, toolbox icon `tool_smart_matting` (dashed lasso
  SVGs in `icons/`), cursor PNGs aliased to the selectiontools outline cursors
  in `icons/kritavisionml.qrc`.
- Interaction: `KisToolOutlineBase` freehand outline; `finishOutline` passes a
  `QPolygonF` to `SegmentationToolHelper::applyMattingMask`.
- Pipeline: the helper selects the sampled paint device (current layer / all
  layers / color-labeled layers), pads the polygon bounds by 16 px, crops via
  `VisionMLImage::prepare`, runs `VisionModels::removeBackground`
  (BiRefNet family, `VisionMLTask::background_removal` model slot), then
  intersects the soft matte with the rasterized lasso region and the source
  layer alpha. The result is written into a `KisPixelSelection`, so soft
  (feathered) alpha survives — unlike the point/box tools the byte mask is not
  binarized. Grow/shrink, feather, anti-alias and all selection actions
  (replace/add/subtract/intersect/symdiff) go through the same
  `KisSelectionToolHelper` path as the other tools.
- This replaces the standalone `krita-smart-select-windows` architecture
  (QProcess Python worker + .npy mask exchange). Do not reintroduce the
  external worker; BiRefNet-HR-matting GGUF variants load through the existing
  `vision.cpp` birefnet path — drop them in the `visionml/models` folder to
  upgrade matte quality.
- Model dropdown in the tool options is the shared backend widget only
  (`addOptions(selectionWidget, false)`); the model used is whatever is
  selected for the background removal task.
